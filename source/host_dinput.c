/* host_dinput.c -- minimal usable fake IDirectInput8 object
 *
 * ==========================================================================
 * 1. Why this file must exist
 *
 * At startup the game does:
 *     DirectInput8Create(..., &g_pDI)          // g_pDI @ 0x803010
 *     ...
 *     mov byte [0xc007de], 1                   // <- unconditionally set to 1
 *     call sub_0059d3b0
 *        └─ if ([0xc007de]) { eax = [0x803010];
 *                             ecx = [eax];            // <- NULL dereference
 *                             call [ecx+0x10]; }      // EnumDevices
 *
 * In other words: **the game assumes DirectInput8Create always succeeds** and sets the failure flag anyway.
 * That is the game's assumption; it is not ours to change (gen/ is read-only).
 *
 * On real hardware the assumption holds: DINPUT8.dll is a Windows system component,
 * and DirectInput8Create fails only in extreme cases. So the right move is to **provide this object**,
 * let creation succeed, and have EnumDevices honestly report "no devices".
 *
 * That is exactly what real hardware does with a gamepad unplugged: the interface exists, enumeration is empty.
 *
 * (Note: gen/ actually has an `#if defined(__vita__)` branch that forces 0xc007de
 *  to 0 to work around this. We do not define __vita__, and we will not change
 *  a build macro just to dodge a missing host piece -- that would enable Vita-only paths elsewhere.)
 *
 * ==========================================================================
 * 2. Object layout (everything below the 4GB line)
 *
 *   [g_pDI] ──► obj:  obj[0] = &vtable[0]
 *                     obj[4] = reference count
 *
 *   vtable (standard COM order; each slot holds a token the guest can call):
 *     +0x00 QueryInterface   +0x10 EnumDevices
 *     +0x04 AddRef           +0x14 GetDeviceStatus
 *     +0x08 Release          +0x18 RunControlPanel
 *     +0x0c CreateDevice     +0x1c Initialize
 *
 * Every method is x86 stdcall (the COM convention): pop the return address and clear the arguments.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "guest.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ---- memory below 4GB ---- */
#define DI_BASE   0x3A400000u
#define DI_SIZE   0x2000u
#define DI_OBJ    (DI_BASE + 0x0000u)      /* object */
#define DI_VT     (DI_BASE + 0x0100u)      /* vtable */
#define DI_DEV_OBJ (DI_BASE + 0x0400u)     /* keyboard device object */
#define DI_DEV_VT  (DI_BASE + 0x0500u)     /* keyboard device vtable */

/* ---- token range (does not collide with GL 0x7D or kernel32 0x7E) ---- */
#define DI_TOKEN_BASE 0x7F000000u
/* device methods get their own range so they never mix with IDirectInput8 */
#define DI_DEV_TOKEN_BASE 0x7F010000u
#define DI_DEV_T_COUNT 32u

enum {
    DI_T_QI = 0, DI_T_ADDREF, DI_T_RELEASE, DI_T_CREATEDEVICE,
    DI_T_ENUMDEVICES, DI_T_GETDEVSTATUS, DI_T_RUNCP, DI_T_INIT,
    DI_T_COUNT
};

static int g_di_state;      /* 0 uninitialised, 1 ready, -1 failed */
static uint32_t g_di_obj;

/* HRESULT / DI return codes */
#define S_OK                    0x00000000u
#define S_FALSE                 0x00000001u
#define E_NOINTERFACE           0x80004002u
#define DIERR_DEVICENOTREG      0x80070032u   /* ERROR_NOT_REGISTERED */
#define DI_NOTATTACHED          S_FALSE

/* IID_IUnknown {00000000-0000-0000-C000-000000000046}
 * IID_IDirectInput8A {BF798030-483A-4DA2-99BD-576B4C0BAD}}
 * The game only checks S_OK vs. failure, so accepting IUnknown is enough; everything else gets E_NOINTERFACE. */
static int iid_is_unknown(uint32_t p)
{
    static const uint8_t unk[16] = {
        0x00,0x00,0x00,0x00, 0x00,0x00, 0x00,0x00,
        0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46
    };
    if (!p) return 0;
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = ld8(p + (uint32_t)i);
    return memcmp(buf, unk, 16) == 0;
}

/* ---- method implementations ---- */

/* QueryInterface(this, riid, ppvObject)  12 bytes */
static void di_QueryInterface(CPU *c)
{
    (void)A(c, 0);
    uint32_t riid = A(c, 1), ppv = A(c, 2);
    if (ppv) {
        if (iid_is_unknown(riid)) {
            st32(ppv, g_di_obj);
            c->eax = S_OK;
        } else {
            st32(ppv, 0);
            c->eax = E_NOINTERFACE;
        }
    } else {
        c->eax = E_NOINTERFACE;
    }
    R(c, 12);
}

/* AddRef(this)  4 bytes */
static void di_AddRef(CPU *c)
{
    uint32_t self = A(c, 0);
    uint32_t n = (self ? ld32(self + 4u) : 0u) + 1u;
    if (self) st32(self + 4u, n);
    c->eax = n;
    R(c, 4);
}

/* Release(this)  4 bytes */
static void di_Release(CPU *c)
{
    uint32_t self = A(c, 0);
    uint32_t n = self ? ld32(self + 4u) : 0u;
    if (n) n--;
    if (self) st32(self + 4u, n);
    c->eax = n;
    R(c, 4);
}

/* ==========================================================================
 * Fake IDirectInputDevice8 keyboard device (2026-11-05)
 *
 * Why this is required
 * ------------
 * On real hardware the gamepad/keyboard did nothing at all. The logs showed:
 *   DirectInput8Create   called 1 time (succeeded, returned the fake IDirectInput8)
 *   GetAsyncKeyState    called 0 times
 *   PeekMessage         called 0 times
 * The game neither reads GetAsyncKeyState nor runs a window message loop -- it only trusts
 * DirectInput device objects. This stub used to return DIERR_DEVICENOTREG unconditionally,
 * so the game never got an input device and the whole input chain was dead.
 *
 * Therefore: CreateDevice must succeed and return a working
 * IDirectInputDevice8 whose GetDeviceState copies the host-side key table out in one go.
 *
 * The vtable order follows dinput.h's IDirectInputDevice8A exactly (32 methods).
 * One slot off and you call a different method; the symptoms get truly bizarre.
 * ========================================================================== */

extern const unsigned char *host_key_table(void);

#define DI_OK        0x00000000u
#define DIERR_INPUTLOST 0x8007001Eu

/* commonly used methods */
/* QueryInterface(this, riid, ppvObject)  12 bytes
 *
 * ★ 2026-11-05 fixed a fatal argument bug: this used to read A(c,1),
 * which is **riid** (the input GUID pointer, aimed at a read-only constant in the guest),
 * and writing there segfaults immediately. The out parameter is the third one, A(c,2).
 * Crash log: host_dinput_dynamic+0xa0, dead right after dev-method #1. */
static void dev_QueryInterface(CPU *c)
{
    uint32_t ppv = A(c, 2);
    if (ppv) st32(ppv, DI_DEV_OBJ);
    c->eax = S_OK;
    R(c, 12);
}
static void dev_AddRef(CPU *c)         { c->eax = 2; R(c, 4); }
static void dev_Release(CPU *c)        { c->eax = 1; R(c, 4); }

/* GetCapabilities(this, lpDIDevCaps)  8 bytes */
static void dev_GetCapabilities(CPU *c)
{
    uint32_t p = A(c, 1);
    if (p) {
        /* DIDEVICEOBJECTINSTANCE / DIDEVCAPS structure: just write
         * dwDevType as keyboard (DIDEVTYPE_KEYBOARD = 3) plus plausible
         * dwAxes/dwButtons/dwPOVs, and the game treats it as a keyboard. */
        for (int i = 0; i < 44; i++) st8(p + (uint32_t)i, 0);
        st32(p + 0x00, 44);          /* dwSize */
        st32(p + 0x04, 3);           /* dwDevType = DI8DEVTYPE_KEYBOARD */
    }
    c->eax = S_OK; R(c, 8);
}

/* EnumObjects(this, cb, ref, flags)  16 bytes -- enumerate nothing, return an empty success */
static void dev_EnumObjects(CPU *c) { c->eax = S_OK; R(c, 16); }

/* GetProperty / SetProperty are 16 / 16 bytes -- always succeed */
/* GetProperty(this, rguidProp, pdiph) -- this+2 = 12 bytes. 16 here would clear 4 bytes too many,
 * tipping the stack over (guest stack pointer outside bounds -> SIGABRT). */
static void dev_GetProperty(CPU *c) { c->eax = S_OK; R(c, 12); }
/* SetProperty(this, rguidProp, pdiph) -- this+2 = 12 bytes. Same as above, already fixed. */
static void dev_SetProperty(CPU *c) { c->eax = S_OK; R(c, 12); }

/* Acquire / Unacquire */
static void dev_Acquire(CPU *c)   { c->eax = S_OK; R(c, 4); }
static void dev_Unacquire(CPU *c) { c->eax = S_OK; R(c, 4); }

/* ★ GetDeviceState(this, cbData, lpvData)  12 bytes
 *
 * This is the heart of the whole chain: copy the host-side 256-byte key table into the guest buffer.
 * The DirectInput keyboard convention is one byte per key, 0x80 = pressed, 0 = released.
 * Our u_key_state stores 0/1, so multiply by 0x80 here to get DInput semantics. */
static void dev_GetDeviceState(CPU *c)
{
    uint32_t cb  = A(c, 1);
    uint32_t buf = A(c, 2);
    if (buf && cb) {
        const unsigned char *keys = host_key_table();
        uint32_t n = cb < 256u ? cb : 256u;
        for (uint32_t i = 0; i < n; i++)
            st8(buf + i, keys[i] ? 0x80u : 0x00u);
        /* zero the part beyond 256 so the game never reads uninitialised memory */
        for (uint32_t i = n; i < cb && i < 1024u; i++) st8(buf + i, 0u);
    }
    c->eax = S_OK; R(c, 12);
}

/* GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags)  20 bytes */
/* ★ GetDeviceData(this, cbObjectData, rgdod, pdwInOut, dwFlags)  20 bytes
 *
 * 2026-11-05: the game reads input in **buffered event mode** (index 10), using the standard
 * two-step probe:
 *     GetDeviceData(cb=20, buf=NULL,     cap=0xFFFFFFFF)  <- "how many events?"
 *     GetDeviceData(cb=20, buf=<valid>,  cap=<return value from the first step>)
 *
 * So a real **queue** is required: diff the current key state into the queue, then drain by cap.
 * The old code compared against a "last frame snapshot" and never queued, so the first step
 * always returned 0 and the game concluded there was no input.
 *
 * DIDEVICEOBJECTDATA (x86, 20 bytes):
 *   +0x00 dwOfs   key code   +0x04 dwData 0x80 pressed / 0 released
 *   +0x08 dwTimeStamp     +0x0C dwSequence     +0x10 dwAppData
 *
 * When the queue is full, **drop the newest**: losing an event beats writing out of bounds.
 * Keyboard event volume is tiny (a few keys at a time), so depth 64 is plenty.
 */
#define EVQ_MAX 64

typedef struct {
    unsigned char ofs;
    unsigned char data;
    unsigned long seq;
} din_ev;

static din_ev        g_evq[EVQ_MAX];
static unsigned      g_evq_head, g_evq_tail;
static unsigned char dev_prev_keys[256];
static unsigned long dev_seq;

/* diff the current key state against the last snapshot and enqueue it */
/* ★★★ 2026-11-06: VK -> DIK scancode mapping (the real reason the gamepad did nothing) ★★★
 *
 * The logs showed events being **delivered successfully** to the game (queue of 32, delivery lines),
 * yet the game did not react at all. The cause was the meaning of dwOfs:
 *
 *   · we sent **VK codes** (space = 0x20 = 32)
 *   · DirectInput's data format uses **DIK scancodes** (space = 0x39 = 57)
 *
 * The game hands over the standard DIK layout in SetDataFormat and then indexes its own
 * key table with dwOfs as a DIK index, so "0x20" is read as DIK 0x20 = the 'D' key
 * instead of space. That is why A did nothing and every key was shifted.
 *
 * This is a trimmed table covering every default binding (WASD/arrows/space/E/Q/Tab/ESC).
 * Unlisted keys pass through unchanged (at worst wrong, never negative or memory-corrupting).
 */
static unsigned char vk_to_dik(uint32_t vk)
{
    switch (vk) {
        case 0x1B: return 0x01;   /* ESC      */
        case 0x09: return 0x0F;   /* Tab      */
        case 0x0D: return 0x1C;   /* Enter    */
        case 0x20: return 0x39;   /* Space    */
        case 0x10: return 0x2A;   /* Shift    */
        case 0x11: return 0x1D;   /* Ctrl     */
        case 0x12: return 0x38;   /* Alt      */

        /* letters: DIK order is scrambled, so each one needs its own mapping */
        case 'Q': return 0x10;  case 'W': return 0x11;
        case 'E': return 0x12;  case 'R': return 0x13;
        case 'T': return 0x14;  case 'Y': return 0x15;
        case 'U': return 0x16;  case 'I': return 0x17;
        case 'O': return 0x18;  case 'P': return 0x19;
        case 'A': return 0x1E;  case 'S': return 0x1F;
        case 'D': return 0x20;  case 'F': return 0x21;
        case 'G': return 0x22;  case 'H': return 0x23;
        case 'J': return 0x24;  case 'K': return 0x25;
        case 'L': return 0x26;
        case 'Z': return 0x2C;  case 'X': return 0x2D;
        case 'C': return 0x2E;  case 'V': return 0x2F;
        case 'B': return 0x30;  case 'N': return 0x31;
        case 'M': return 0x32;

        /* digits 1-9,0 */
        case '1': return 0x02;  case '2': return 0x03;
        case '3': return 0x04;  case '4': return 0x05;
        case '5': return 0x06;  case '6': return 0x07;
        case '7': return 0x08;  case '8': return 0x09;
        case '9': return 0x0A;  case '0': return 0x0B;

        /* arrow keys (extended keys, DIK 0xC8..0xCD) */
        case 0x26: return 0xC8;   /* Up    */
        case 0x28: return 0xD0;   /* Down  */
        case 0x25: return 0xCB;   /* Left  */
        case 0x27: return 0xCD;   /* Right */

        default:   return (unsigned char)(vk & 0xFF);
    }
}

static void evq_fill(void)
{
    const unsigned char *cur = host_key_table();
    for (uint32_t i = 0; i < 256u; i++) {
        unsigned char now = cur[i] ? 0x80u : 0x00u;
        if (now == dev_prev_keys[i]) continue;
        dev_prev_keys[i] = now;

        unsigned nt = (g_evq_tail + 1u) % EVQ_MAX;
        if (nt == g_evq_head) return;      /* full: drop, never overwrite unread events */
        g_evq[g_evq_tail].ofs  = (unsigned char)i;
        g_evq[g_evq_tail].data = now;
        g_evq[g_evq_tail].seq  = ++dev_seq;
        g_evq_tail = nt;
    }
}

static unsigned evq_count(void)
{
    return (g_evq_tail + EVQ_MAX - g_evq_head) % EVQ_MAX;
}

static void dev_GetDeviceData(CPU *c)
{
    uint32_t cbObjectData = A(c, 1);
    uint32_t rgdod       = A(c, 2);
    uint32_t pInOut      = A(c, 3);

    evq_fill();                 /* pick up new changes into the queue on every call */

    /* 2026-11-05 diagnostics: print only when "the space key has a value".
     * A call-count limit could not show the state after injection (injection happens at 45 s,
     * by which time the call count was long past the limit). Trigger on content instead. */
    {
        const unsigned char *tb = host_key_table();
        if (tb[32]) {
            static unsigned d2;
            if (d2++ < 10u) {
                fprintf(stderr, "[dinput] injection effective: u_key[32]=%u prev[32]=%u queue=%u\n",
                        tb[32], dev_prev_keys[32], evq_count());
                fflush(NULL);
            }
        }
    }

    uint32_t cap = 0;
    if (pInOut) cap = ld32(pInOut);
    unsigned avail = evq_count();

    {
        static unsigned dbg;
        if (dbg++ < 4u) {
            fprintf(stderr, "[dinput] GetDeviceData(cb=%u, buf=%08x, cap=%u) queue=%u\n",
                    cbObjectData, rgdod, cap, avail);
            fflush(NULL);
        }
    }

    /* count-only query: buf is null, or cap == INFINITE(0xFFFFFFFF) */
    if (!rgdod || cap == 0xFFFFFFFFu) {
        if (pInOut) st32(pInOut, avail);
        c->eax = S_OK; R(c, 20); return;
    }

    /* normal event retrieval */
    uint32_t n = 0;
    if (cbObjectData >= 16u) {
        while (n < cap && g_evq_head != g_evq_tail) {
            uint32_t off = rgdod + n * cbObjectData;
            st32(off + 0x00u, (uint32_t)vk_to_dik(g_evq[g_evq_head].ofs));
            st32(off + 0x04u, (uint32_t)g_evq[g_evq_head].data);
            st32(off + 0x08u, 0u);
            st32(off + 0x0Cu, (uint32_t)g_evq[g_evq_head].seq);
            st32(off + 0x10u, 0u);
            g_evq_head = (g_evq_head + 1u) % EVQ_MAX;
            n++;
        }
    }

    if (pInOut) st32(pInOut, n);

    static unsigned shown;
    if (n && shown++ < 12u) {
        fprintf(stderr, "[dinput] -> delivered %u key events\n", n);
        fflush(NULL);
    }

    c->eax = S_OK;
    R(c, 20);
}

/* SetDataFormat / SetEventNotification / SetCooperativeLevel */
static void dev_SetDataFormat(CPU *c)        { c->eax = S_OK; R(c, 8); }
static void dev_SetEventNotification(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_SetCooperativeLevel(CPU *c)  { c->eax = S_OK; R(c, 12); }

static void dev_GetObjectInfo(CPU *c) { c->eax = S_OK; R(c, 16); }
static void dev_GetDeviceInfo(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_RunControlPanel(CPU *c) { c->eax = S_OK; R(c, 12); }
static void dev_Initialize(CPU *c) { c->eax = S_OK; R(c, 16); }

/* force feedback / effects -- not supported by a keyboard, report failure honestly */
/* CreateEffect(this, rguid, lpeff, ppeff, punkOuter) -- this+4 = 20 bytes. */
static void dev_CreateEffect(CPU *c) { c->eax = 0x80004005u; R(c, 20); }
static void dev_EnumEffects(CPU *c) { c->eax = S_OK; R(c, 16); }
static void dev_GetEffectInfo(CPU *c) { c->eax = 0x80004005u; R(c, 12); }
static void dev_GetForceFeedbackState(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_SendForceFeedbackCommand(CPU *c) { c->eax = S_OK; R(c, 8); }
static void dev_EnumCreatedEffectObjects(CPU *c) { c->eax = S_OK; R(c, 16); }
/* Escape(this, pdie) -- this+1 = 8 bytes. */
static void dev_Escape(CPU *c) { c->eax = S_OK; R(c, 8); }

/* ★ Poll(this) -- the game calls Poll before every input read. A keyboard is instantaneous: succeed. */
static void dev_Poll(CPU *c) { c->eax = S_OK; R(c, 4); }

static void dev_SendDeviceData(CPU *c) { c->eax = S_OK; R(c, 20); }
static void dev_EnumEffectsInFile(CPU *c) { c->eax = S_OK; R(c, 20); }
/* WriteEffectToFile(this, szFileName, dwEntries, rgdife, dwFlags) -- this+4 = 20 bytes. */
static void dev_WriteEffectToFile(CPU *c) { c->eax = S_OK; R(c, 20); }
static void dev_BuildActionMap(CPU *c) { c->eax = S_OK; R(c, 16); }
/* SetActionMap(this, pdiam, szUserName, dwFlags) -- this+3 = 16 bytes. */
static void dev_SetActionMap(CPU *c) { c->eax = S_OK; R(c, 16); }
/* GetImageInfo(this, pdii) -- this+1 = 8 bytes. */
static void dev_GetImageInfo(CPU *c) { c->eax = S_OK; R(c, 8); }

/* vtable: strictly the IDirectInputDevice8A declaration order */
static void (*const s_dev_fns[DI_DEV_T_COUNT])(CPU *) = {
    dev_QueryInterface,        /*  0 */
    dev_AddRef,                /*  1 */
    dev_Release,               /*  2 */
    dev_GetCapabilities,       /*  3 */
    dev_EnumObjects,           /*  4 */
    dev_GetProperty,           /*  5 */
    dev_SetProperty,           /*  6 */
    dev_Acquire,               /*  7 */
    dev_Unacquire,             /*  8 */
    dev_GetDeviceState,        /*  9 ★ */
    dev_GetDeviceData,         /* 10 */
    dev_SetDataFormat,         /* 11 */
    dev_SetEventNotification,  /* 12 */
    dev_SetCooperativeLevel,   /* 13 */
    dev_GetObjectInfo,         /* 14 */
    dev_GetDeviceInfo,         /* 15 */
    dev_RunControlPanel,       /* 16 */
    dev_Initialize,            /* 17 */
    dev_CreateEffect,          /* 18 */
    dev_EnumEffects,           /* 19 */
    dev_GetEffectInfo,         /* 20 */
    dev_GetForceFeedbackState, /* 21 */
    dev_SendForceFeedbackCommand, /* 22 */
    dev_EnumCreatedEffectObjects, /* 23 */
    dev_Escape,                /* 24 */
    dev_Poll,                  /* 25 ★ */
    dev_SendDeviceData,        /* 26 */
    dev_EnumEffectsInFile,     /* 27 */
    dev_WriteEffectToFile,     /* 28 */
    dev_BuildActionMap,        /* 29 */
    dev_SetActionMap,          /* 30 */
    dev_GetImageInfo           /* 31 */
};

/* CreateDevice(this, rguid, lplpDevice, pUnkOuter)  16 bytes
 *
 * ★ 2026-11-05: changed from "always fails" to returning a usable fake keyboard device.
 *   This is the prerequisite for the gamepad/keyboard to work at all. */
static void di_CreateDevice(CPU *c)
{
    uint32_t ppDev = A(c, 2);

    /* build the device object and vtable (idempotent) */
    for (unsigned i = 0; i < DI_DEV_T_COUNT; i++)
        st32(DI_DEV_VT + i * 4u, DI_DEV_TOKEN_BASE + i * 4u);
    st32(DI_DEV_OBJ + 0u, DI_DEV_VT);   /* lpVtbl */
    st32(DI_DEV_OBJ + 4u, 0x12345678u); /* occupancy marker, handy when debugging */

    if (ppDev) st32(ppDev, DI_DEV_OBJ);

    fprintf(stderr, "[dinput] ✓ CreateDevice -> fake keyboard device @ %08x\n", DI_DEV_OBJ);
    fflush(NULL);
    c->eax = S_OK;
    R(c, 16);
}

/* EnumDevices(this, dwDevType, callback, pvRef, dwFlags)  20 bytes
 * ★ Returns S_OK here and **does not invoke the callback** -- i.e. "enumeration succeeded, 0 devices".
 *   That is exactly what real hardware yields when no gamepad is present. */
/* DIDEVICEINSTANCEW used by the enumeration callback (x86 32-bit, 1100 bytes total)
 *   +0x00 DWORD dwSize
 *   +0x04 GUID  guidInstance (16)
 *   +0x14 GUID  guidProduct  (16)
 *   +0x24 DWORD dwDevType
 *   +0x28 WCHAR tszInstanceName[260]
 *   +0x230 WCHAR tszProductName[260]
 *   +0x438 GUID  guidFFDriver
 *   +0x448 WORD  wUsagePage
 *   +0x44A WORD  wUsage */
#define DI_INST_BUF (DI_BASE + 0x0800u)

/* GUID_DEVCLASS_KEYBOARD = {4D36E96B-E325-11CE-BFC1-08002BE10318}
 * The memory layout is little-endian: Data1(4) Data2(2) Data3(2) Data4(8) */
static const unsigned char kbd_guid[16] = {
    0x6B, 0xE9, 0x36, 0x4D,   /* Data1 */
    0x25, 0xE3,               /* Data2 */
    0xCE, 0x11,               /* Data3 */
    0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18  /* Data4 */
};

/* EnumDevices(this, dwDevType, callback, pvRef, dwFlags)  20 bytes
 *
 * ★ 2026-11-05: from here on we report **one keyboard device**.
 *
 * Measurement showed the game only ever calls EnumDevices(wType=4 = DIDEVTYPE_KEYBOARD),
 * and after getting "0 devices" it never calls CreateDevice again -- the whole input layer is dead.
 * That is the root cause of "gamepad/keyboard completely unresponsive".
 *
 * Now we build a DIDEVICEINSTANCEW and call the guest callback, so the game believes
 * a keyboard exists, calls CreateDevice, and then gets keys through GetDeviceState.
 *
 * Callback signature: BOOL CALLBACK cb(LPCDIDEVICEINSTANCE, LPVOID) -- cdecl.
 * Return TRUE(1) to continue enumerating, FALSE(0) to stop. */
static void di_EnumDevices(CPU *c)
{
    uint32_t wType  = A(c, 1);
    uint32_t cbAddr = A(c, 2);
    uint32_t pvRef  = A(c, 3);

    static int shown;
    if (shown++ < 3) {
        fprintf(stderr, "[dinput] EnumDevices(wType=%08x) -> reporting 1 keyboard device\n", wType);
        fflush(NULL);
    }

    if (!cbAddr) { c->eax = S_OK; R(c, 20); return; }

    /* --- build DIDEVICEINSTANCEW --- */
    uint32_t inst = DI_INST_BUF;
    for (int i = 0; i < 0x44C; i++) st8(inst + (uint32_t)i, 0u);
    st32(inst + 0x00, 0x44Cu);              /* dwSize */
    for (int i = 0; i < 16; i++) st8(inst + 0x04u + (uint32_t)i, kbd_guid[i]);
    for (int i = 0; i < 16; i++) st8(inst + 0x14u + (uint32_t)i, kbd_guid[i]);
    st32(inst + 0x24, 3u);                  /* DI8DEVTYPE_KEYBOARD */
    /* name is written as UTF-16LE */
    const char *nm = "Keyboard";
    for (int i = 0; nm[i]; i++) {
        st16(inst + 0x28u + (uint32_t)i * 2u, (uint16_t)(unsigned char)nm[i]);
        st16(inst + 0x230u + (uint32_t)i * 2u, (uint16_t)(unsigned char)nm[i]);
    }

    /* --- call the guest callback: cdecl, push pvRef then inst --- */
    {
        uint32_t saved = c->esp;
        gpush(c, pvRef);
        gpush(c, inst);
        gpush(c, 0xabc0d1u);        /* placeholder return address; guest_call does not use it */
        guest_call(c, cbAddr);
        c->esp = saved;
    }

    c->eax = S_OK;
    R(c, 20);
}

/* GetDeviceStatus(this, rguidInstance)  8 bytes */
static void di_GetDeviceStatus(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    c->eax = DI_NOTATTACHED;
    R(c, 8);
}

/* RunControlPanel(this, hwndOwner, dwFlags)  12 bytes */
static void di_RunControlPanel(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = S_OK;
    R(c, 12);
}

/* Initialize(this, hinst, dwVersion)  12 bytes */
static void di_Initialize(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = S_OK;
    R(c, 12);
}

static void (*const s_di_fns[DI_T_COUNT])(CPU *) = {
    di_QueryInterface, di_AddRef, di_Release, di_CreateDevice,
    di_EnumDevices, di_GetDeviceStatus, di_RunControlPanel, di_Initialize
};

/* build the object and vtable (idempotent) */
uint32_t host_dinput_obj(void)
{
    if (g_di_state == 1) return g_di_obj;
    if (g_di_state == -1) return 0;

    void *p = mmap((void *)(uintptr_t)DI_BASE, DI_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)DI_BASE) {
        fprintf(stderr, "[dinput] ✗ cannot construct object at 0x%08x\n", DI_BASE);
        g_di_state = -1;
        return 0;
    }

    /* vtable: each slot holds a token the guest can call */
    for (unsigned i = 0; i < DI_T_COUNT; i++)
        st32(DI_VT + i * 4u, DI_TOKEN_BASE + i * 4u);

    /* object: first field is the vtable pointer, second is the reference count */
    st32(DI_OBJ + 0u, DI_VT);
    st32(DI_OBJ + 4u, 1u);

    g_di_obj = DI_OBJ;
    g_di_state = 1;
    fprintf(stderr, "[dinput] ✓ IDirectInput8 fake object @ %08x  vtable @ %08x\n",
            DI_OBJ, DI_VT);
    return g_di_obj;
}

/* guest_call dispatch entry. Returns 1 = handled */
int host_dinput_dynamic(CPU *__restrict c, uint32_t addr)
{
    /* ---- device vtable (32 methods) ---- */
    if (addr >= DI_DEV_TOKEN_BASE && addr < DI_DEV_TOKEN_BASE + DI_DEV_T_COUNT * 4u) {
        uint32_t off = addr - DI_DEV_TOKEN_BASE;
        if (off % 4u) return 0;
        uint32_t i = off / 4u;
        if (i >= DI_DEV_T_COUNT) return 0;
        /* for the first 24 device method calls, print the method number to see what the game uses */
        static unsigned ndev;
        if (ndev++ < 24u) {
            fprintf(stderr, "[dinput] dev-method #%u (index %u)\n", ndev, i);
            fflush(NULL);
        }
        s_dev_fns[i](c);
        return 1;
    }

    /* ---- IDirectInput8 vtable ---- */
    if (addr < DI_TOKEN_BASE || addr >= DI_TOKEN_BASE + DI_T_COUNT * 4u) return 0;
    uint32_t off = addr - DI_TOKEN_BASE;
    if (off % 4u) return 0;
    uint32_t i = off / 4u;
    if (i >= DI_T_COUNT) return 0;
    {
        static unsigned nroot;
        if (nroot++ < 12u) {
            fprintf(stderr, "[dinput] root-method #%u (index %u)\n", nroot, i);
            fflush(NULL);
        }
    }
    s_di_fns[i](c);
    return 1;
}

int host_dinput_count(void) { return (int)DI_T_COUNT; }
