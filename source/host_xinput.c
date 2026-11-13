/* host_xinput.c -- minimal working XInput 1.4 / 1.3 fake implementation
 *
 * ==========================================================================
 * 1. Why it has to exist
 *
 * At startup the game probes for a pad:
 *     LoadLibraryA("XInput1_4.dll")   -> fails
 *     LoadLibraryA("XInput1_3.dll")   -> fails
 *     LoadLibraryA("bin\XInput1_3.dll") -> fails
 *     "Gamepad_init couldn't load XInput1_4.dll or XInput1_3.dll;
 *      proceeding with DInput only"
 *
 * then falls back to DirectInput. We had been fixing DInput keyboards, but the game's primary path
 * is XInput -- only when LoadLibraryA succeeds does the game GetProcAddress the
 * functions below and genuinely start polling the pad.
 *
 * Measured (see run.log lines 280-284), the game takes only three exports:
 *     XInputGetState             by name
 *     XInputGetStateEx          by ordinal 100   <- the undocumented XPAD variant
 *     XInputGetCapabilities     by name
 * XInputSetState / XInputEnable / XInputGetKeystroke /
 * XInputGetBatteryInformation / XInputGetAudioDeviceIds are also provided,
 * so that some branch getting 0 does not call 0 outright.
 *
 * ==========================================================================
 * 2. Where the state comes from
 *
 * Besides the original keyboard lines, the input bridge (TCP:7137 in host_user32.c) also accepts:
 *
 *     G <buttons-hex> <lx> <ly> <rx> <ry> <lt> <rt>\n
 *
 *     lx/ly/rx/ry : -32768..32767 (XInput semantics: Y axis positive upwards)
 *     lt/rt       : 0..255
 *
 * With no pad line received the state is all zero but it still reports "connected" -- otherwise the game
 * thinks the pad was unplugged and returns to the DInput fallback, wasting all this work.
 *
 * ==========================================================================
 * 3. Structure layout (identical to the Windows SDK, pinned by _Static_assert)
 *
 *   XINPUT_STATE        16 bytes
 *     +0x00 DWORD dwPacketNumber
 *     +0x04 WORD  wButtons
 *     +0x06 BYTE  bLeftTrigger
 *     +0x07 BYTE  bRightTrigger
 *     +0x08 SHORT sThumbLX
 *     +0x0A SHORT sThumbLY
 *     +0x0C SHORT sThumbRX
 *     +0x0E SHORT sThumbRY
 *
 *   XINPUT_CAPABILITIES 20 bytes
 *     +0x00 BYTE Type      +0x01 BYTE SubType  +0x02 WORD Flags
 *     +0x04 XINPUT_GAMEPAD (12)  +0x10 XINPUT_VIBRATION (4)
 *
 * Calling convention: x86 stdcall; the argument count cleared follows the real signature (see each function's end).
 * ========================================================================== */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

/* ---- stdcall arguments / returns ---- */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ---- token space ----
 * 0x7D = GL, 0x7E = kernel32 dynamic exports, 0x7F00/0x7F01 = DirectInput.
 * XInput starts at 0x7F020000; none of them overlap. */
#define XI_TOKEN_BASE 0x7F020000u

enum {
    XI_T_GETSTATE = 0,
    XI_T_GETSTATEEX,
    XI_T_SETSTATE,
    XI_T_GETCAPS,
    XI_T_ENABLE,
    XI_T_GETKEYSTROKE,
    XI_T_GETBATTERY,
    XI_T_GETAUDIODEVICEIDS,
    XI_T_COUNT
};

/* return codes */
#define XI_SUCCESS                 0u
#define XI_BAD_ARGUMENTS           160u
#define XI_DEVICE_NOT_CONNECTED    1167u
#define XI_EMPTY                   4306u

/* button bits */
#define XB_DPAD_UP        0x0001u
#define XB_DPAD_DOWN      0x0002u
#define XB_DPAD_LEFT      0x0004u
#define XB_DPAD_RIGHT     0x0008u
#define XB_START          0x0010u
#define XB_BACK           0x0020u
#define XB_LEFT_THUMB     0x0040u
#define XB_RIGHT_THUMB    0x0080u
#define XB_LEFT_SHOULDER  0x0100u
#define XB_RIGHT_SHOULDER 0x0200u
#define XB_A              0x1000u
#define XB_B              0x2000u
#define XB_X              0x4000u
#define XB_Y              0x8000u

/* ---- state provided by the input bridge (host_user32.c) ---- */
/* Weak symbol: if the input bridge is not compiled in, this still works (all-zero state). */
extern int u_pad_get(unsigned *buttons, int *lx, int *ly, int *rx, int *ry,
                     int *lt, int *rt) __attribute__((weak));

typedef struct {
    uint32_t packet;
    uint16_t buttons;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
} xi_gamepad;

static int g_enabled = 1;          /* the XInputEnable switch */
static int g_last_connected = -1;  /* for logging only */
static uint32_t g_packet;          /* dwPacketNumber, incremented when the state changes */
static uint32_t g_prev_raw;        /* fingerprint of the previous raw state */
static uint32_t g_nstate_calls;    /* log the first few GetState calls */
static uint32_t xi_n_getstate, xi_n_getstateex, xi_n_getcaps;  /* probe counters */

static int16_t clamp16(int v)
{
    if (v < -32768) return -32768;
    if (v >  32767) return  32767;
    return (int16_t)v;
}

static uint8_t clamp8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Fetch one frame of state from the input bridge. Returns 1 on success. */
static int xi_sample(xi_gamepad *g)
{
    unsigned buttons = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;

    if (u_pad_get)
        (void)u_pad_get(&buttons, &lx, &ly, &rx, &ry, &lt, &rt);

    g->buttons = (uint16_t)(buttons & 0xFFFFu);
    g->lt = clamp8(lt);
    g->rt = clamp8(rt);
    g->lx = clamp16(lx);
    g->ly = clamp16(ly);
    g->rx = clamp16(rx);
    g->ry = clamp16(ry);
    return 1;
}

/* State fingerprint: decides whether dwPacketNumber should be incremented.
 * Note a plain sum will not do (A+B versus B+A being equal is fine, but distinct combinations must differ),
 * something like FNV-1a works; a multiplicative mix is enough here. */
static uint32_t xi_fingerprint(const xi_gamepad *g)
{
    uint32_t h = 2166136261u;
    h = (h ^ g->buttons) * 16777619u;
    h = (h ^ (uint32_t)(uint16_t)g->lx) * 16777619u;
    h = (h ^ (uint32_t)(uint16_t)g->ly) * 16777619u;
    h = (h ^ (uint32_t)(uint16_t)g->rx) * 16777619u;
    h = (h ^ (uint32_t)(uint16_t)g->ry) * 16777619u;
    h = (h ^ ((uint32_t)g->lt << 8 | g->rt)) * 16777619u;
    return h;
}

/* Write XINPUT_GAMEPAD into guest memory (12 bytes, no embedded pointers). */
static void xi_store_gamepad(uint32_t out, const xi_gamepad *g)
{
    st16(out + 0x0u, g->buttons);
    st8 (out + 0x2u, g->lt);
    st8 (out + 0x3u, g->rt);
    st16(out + 0x4u, (uint16_t)g->lx);
    st16(out + 0x6u, (uint16_t)g->ly);
    st16(out + 0x8u, (uint16_t)g->rx);
    st16(out + 0xAu, (uint16_t)g->ry);
}

/* ---------------- XInputGetState / XInputGetStateEx ----------------
 *
 * DWORD XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState);   stdcall 8
 * GetStateEx has the same signature but a 20-byte structure (one extra reserved DWORD).
 *
 * * Must return 0 (ERROR_SUCCESS). Returning 1167 makes the game treat the pad as unplugged,
 *   returning to the DInput fallback -- exactly what we are trying to leave. */
/* * 2026-11-06: only user 0 is a "real pad". Every other index must return 1167.
 * Measured: the game probes 0..3 one by one; if all return 0 it believes four pads are connected. */
#define XI_USER_IS_PRESENT(u) ((u) == 0u)

static void xi_get_state(CPU *__restrict c, int extended)
{
    uint32_t user = A(c, 0);
    uint32_t out  = A(c, 1);
    xi_gamepad g;

    if (user > 3u) { c->eax = XI_BAD_ARGUMENTS; R(c, 8); return; }
    if (!out)      { c->eax = XI_BAD_ARGUMENTS; R(c, 8); return; }
    if (!XI_USER_IS_PRESENT(user)) {
        c->eax = XI_DEVICE_NOT_CONNECTED;
        R(c, 8);
        return;
    }

    if (extended) xi_n_getstateex++; else xi_n_getstate++;
    if (xi_n_getstate + xi_n_getstateex == 1u) {
        extern void host_boot_mark(const char *) __attribute__((weak));
        if (host_boot_mark) host_boot_mark("first XInput poll (the game starts reading the pad)");
    }

    memset(&g, 0, sizeof g);
    if (g_enabled)
        xi_sample(&g);

    {
        int changed = 0;
        uint32_t fp = xi_fingerprint(&g);
        if (fp != g_prev_raw) {
            g_prev_raw = fp;
            g_packet++;
            changed = 1;
        }
        /* Always log the first two calls (proving the game really polls), then only on state change.
         * Then a single G line sent locally with nc verifies the whole "input bridge -> XInput" chain. */
        if (g_nstate_calls < 2u || (changed && g_nstate_calls < 60u)) {
            g_nstate_calls++;
            /* * 2026-11-10: the log used to print g.packet, which is not assigned until
             * after this block (g.packet = g_packet), so it always printed 0.
             * That caused the misdiagnosis "dwPacketNumber does not increment" -- the value actually written to the guest was
             * the correctly incrementing one. It now prints g_packet. */
            fprintf(stderr, "[xinput] GetState%s(user=%u) -> 0  btn=%04x "
                    "L=%d,%d R=%d,%d LT=%u RT=%u pkt=%u%s\n",
                    extended ? "Ex" : "", user, (unsigned)g.buttons,
                    g.lx, g.ly, g.rx, g.ry, g.lt, g.rt, g_packet,
                    changed ? "  <- changed" : "");
            fflush(NULL);
        }
    }
    g.packet = g_packet;

    st32(out + 0x0u, g.packet);
    xi_store_gamepad(out + 0x4u, &g);
    if (extended)
        st32(out + 0x10u, 0u);   /* reserved; the caller may copy only 4..15 */

    c->eax = XI_SUCCESS;
    R(c, 8);
}

static void xi_GetState(CPU *__restrict c)   { xi_get_state(c, 0); }
static void xi_GetStateEx(CPU *__restrict c) { xi_get_state(c, 1); }

/* DWORD XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration); */
static void xi_SetState(CPU *__restrict c)
{
    uint32_t user = A(c, 0), p = A(c, 1);
    if (user > 3u) { c->eax = XI_BAD_ARGUMENTS; R(c, 8); return; }
    if (p) {
        uint32_t left = (uint32_t)ld16(p + 0u);
        uint32_t right = (uint32_t)ld16(p + 2u);
        static unsigned shown;
        if (shown++ < 4u) {
            fprintf(stderr, "[xinput] SetState vibration L=%u R=%u\n", left, right);
            fflush(NULL);
        }
    }
    c->eax = XI_SUCCESS;
    R(c, 8);
}

/* DWORD XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
 *                             XINPUT_CAPABILITIES *pCapabilities);  stdcall 12 */
static void xi_GetCapabilities(CPU *__restrict c)
{
    uint32_t user  = A(c, 0);
    uint32_t flags = A(c, 1);
    uint32_t out   = A(c, 2);
    xi_n_getcaps++;

    if (user > 3u) { c->eax = XI_BAD_ARGUMENTS; R(c, 12); return; }
    if (!out)      { c->eax = XI_BAD_ARGUMENTS; R(c, 12); return; }
    if (!XI_USER_IS_PRESENT(user)) {
        /* No pad plugged in: do not write the output structure, report 1167 honestly. */
        c->eax = XI_DEVICE_NOT_CONNECTED;
        R(c, 12);
        return;
    }
    /* XINPUT_FLAG_GAMEPAD = 1; 0 means "any device". Other flags are not accepted. */
    if (flags != 0u && flags != 1u) {
        c->eax = XI_BAD_ARGUMENTS;
        R(c, 12);
        return;
    }

    st8 (out + 0x00u, 1u);    /* XINPUT_DEVTYPE_GAMEPAD */
    st8 (out + 0x01u, 1u);    /* XINPUT_DEVSUBTYPE_GAMEPAD */
    st16(out + 0x02u, 0u);    /* Flags */
    /* supported button bits */
    st16(out + 0x04u, (uint16_t)(XB_DPAD_UP | XB_DPAD_DOWN | XB_DPAD_LEFT |
                                 XB_DPAD_RIGHT | XB_START | XB_BACK |
                                 XB_LEFT_THUMB | XB_RIGHT_THUMB |
                                 XB_LEFT_SHOULDER | XB_RIGHT_SHOULDER |
                                 XB_A | XB_B | XB_X | XB_Y));
    st8 (out + 0x06u, 255u);  /* bLeftTrigger  full scale */
    st8 (out + 0x07u, 255u);  /* bRightTrigger */
    /* Stick resolution/precision: real XInput fills 0 meaning "no hardware precision requirement" */
    st16(out + 0x08u, 0u);
    st16(out + 0x0Au, 0u);
    st16(out + 0x0Cu, 0u);
    st16(out + 0x0Eu, 0u);
    /* XINPUT_VIBRATION supported */
    st16(out + 0x10u, 0u);
    st16(out + 0x12u, 0u);

    {
        static unsigned caps_shown;
        if (caps_shown++ < 8u) {
            fprintf(stderr, "[xinput] GetCapabilities(user=%u, flags=%u) -> 0\n",
                    user, flags);
            fflush(NULL);
        }
    }
    c->eax = XI_SUCCESS;
    R(c, 12);
}

/* void XInputEnable(BOOL enable);  stdcall 4 */
static void xi_Enable(CPU *__restrict c)
{
    g_enabled = (A(c, 0) != 0u);
    fprintf(stderr, "[xinput] Enable(%d)\n", g_enabled);
    fflush(NULL);
    R(c, 4);
}

/* DWORD XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserved,
 *                          PXINPUT_KEYSTROKE pKeystroke);  stdcall 12
 * We implement only the pad and have no keyboard event source -> return ERROR_EMPTY honestly. */
static void xi_GetKeystroke(CPU *__restrict c)
{
    uint32_t user = A(c, 0), out = A(c, 2);
    if (user > 3u) { c->eax = XI_BAD_ARGUMENTS; R(c, 12); return; }
    if (out) {
        st16(out + 0u, 0u);
        st16(out + 2u, 0u);
        st16(out + 4u, 0u);
        st8 (out + 6u, 0u);
        st8 (out + 7u, 0u);
    }
    c->eax = XI_EMPTY;
    R(c, 12);
}

/* DWORD XInputGetBatteryInformation(DWORD dwUserIndex, BYTE devType,
 *                                   XINPUT_BATTERY_INFORMATION *pBattery); */
static void xi_GetBatteryInformation(CPU *__restrict c)
{
    uint32_t user = A(c, 0), out = A(c, 2);
    if (user > 3u) { c->eax = XI_BAD_ARGUMENTS; R(c, 12); return; }
    if (out) {
        st8(out + 0u, 1u);   /* BATTERY_TYPE_WIRED */
        st8(out + 1u, 3u);   /* BATTERY_LEVEL_FULL */
    }
    c->eax = XI_SUCCESS;
    R(c, 12);
}

/* DWORD XInputGetAudioDeviceIds(DWORD dwUserIndex, LPWSTR pRenderDeviceId,
 *                               UINT *pRenderCount, LPWSTR pCaptureDeviceId,
 *                               UINT *pCaptureCount);  stdcall 20
 * No matching audio device -> 1167 (the game takes its default audio path). */
static void xi_GetAudioDeviceIds(CPU *__restrict c)
{
    uint32_t user = A(c, 0), rc = A(c, 2), cc = A(c, 4);
    if (user > 3u) { c->eax = XI_BAD_ARGUMENTS; R(c, 20); return; }
    if (rc) st32(rc, 0u);
    if (cc) st32(cc, 0u);
    c->eax = XI_DEVICE_NOT_CONNECTED;
    R(c, 20);
}

/* ---------------- name / ordinal resolution ---------------- */

static const char *const xi_names[XI_T_COUNT] = {
    "XInputGetState",
    "XInputGetStateEx",          /* exposed only by ordinal 100; the name is for logging only */
    "XInputSetState",
    "XInputGetCapabilities",
    "XInputEnable",
    "XInputGetKeystroke",
    "XInputGetBatteryInformation",
    "XInputGetAudioDeviceIds",
};

static void (*const xi_fns[XI_T_COUNT])(CPU *) = {
    xi_GetState,
    xi_GetStateEx,
    xi_SetState,
    xi_GetCapabilities,
    xi_Enable,
    xi_GetKeystroke,
    xi_GetBatteryInformation,
    xi_GetAudioDeviceIds,
};

/* GetProcAddress lookup by name. Returns 1 and writes the token. */
int host_xinput_lookup(const char *name, uint32_t *out_addr)
{
    int i;
    if (!name || !out_addr) return 0;
    for (i = 0; i < (int)XI_T_COUNT; i++) {
        if (strcmp(xi_names[i], name) == 0) {
            *out_addr = XI_TOKEN_BASE + (uint32_t)i * 4u;
            return 1;
        }
    }
    return 0;
}

/* GetProcAddress lookup by ordinal. Only 100 = XInputGetStateEx. */
int host_xinput_lookup_ordinal(uint32_t ordinal, uint32_t *out_addr)
{
    if (ordinal != 100u || !out_addr) return 0;
    *out_addr = XI_TOKEN_BASE + (uint32_t)XI_T_GETSTATEEX * 4u;
    return 1;
}

/* guest_call dispatch entry. 1 = handled. */
int host_xinput_dynamic(CPU *__restrict c, uint32_t addr)
{
    if (addr < XI_TOKEN_BASE || addr >= XI_TOKEN_BASE + XI_T_COUNT * 4u)
        return 0;
    if ((addr - XI_TOKEN_BASE) % 4u) return 0;
    {
        uint32_t i = (addr - XI_TOKEN_BASE) / 4u;
        if (i >= XI_T_COUNT) return 0;
        xi_fns[i](c);
    }
    return 1;
}

int host_xinput_count(void) { return (int)XI_T_COUNT; }

/* After XInputSetState writes a vibration value, might the game expect the next GetState to reflect it?
 * No -- the packet number in real XInput is unrelated to vibration.
 * This deliberately does nothing. */

/* ==========================================================================
 * Diagnostic probe (called once every 600 frames from the SwapBuffers heartbeat in host_gl_audio.c)
 *
 * Background: in the device log GetStateEx was called only twice and then stopped for good, while GetCapabilities
 * was called 4+ times. sub_0059d720 (the polling root, coverage 11488) opens with three switches:
 *
 *   0059d73a  cmp byte  [0x3c803040], 0   ; pad system initialised
 *   0059d744  je   0x59f18a               ; 0 -> the whole function returns immediately
 *   0059d74a  cmp byte  [0x3c8090d8], 0   ; reentrancy guard
 *   0059d751  jne  0x59f18a               ; non-zero -> return (= permanently disabled)
 *   0059d764  cmp dword [0x3c803044], 0   ; number of registered devices
 *   0059d76a  jbe  0x59f183               ; <= 0 -> skip sampling
 *   while iterating devices:
 *   0059d78f  cmp byte  [ebx], 0          ; whether that device is "available"
 *   0059d792  je   0x59e5a4               ; 0 -> skip; do not call GetState
 *
 * If any of these four goes bad, input goes completely silent with nothing visible in the log.
 * The probe is read-only and changes no state; matching its values one-to-one localises the break.
 * ========================================================================== */

/* Guest base after relocation (image.py always moves it to 0x3c000000) */
#define XI_G(off) ((uint32_t)(0x3c000000u + (uint32_t)(off)))


/* Safe read: only look at pointers inside the image data segment or the heap */
static int xi_safe_ptr(uint32_t p)
{
    return p > 0x10000u && p < 0xFFF00000u;
}

void host_xinput_probe(void)
{
    uint32_t init_b      = xi_safe_ptr(XI_G(0x803040u)) ? ld8(XI_G(0x803040u)) : 0xEEu;
    uint32_t reentry     = ld8(XI_G(0x8090d8u));
    uint32_t ndev        = ld32(XI_G(0x803044u));
    uint32_t arr         = ld32(XI_G(0x803024u));
    uint32_t fn_state    = ld32(XI_G(0x803020u));
    uint32_t fn_stateex  = ld32(XI_G(0x803028u));

    fprintf(stderr, "[xprobe] init=%u reentry=%u ndev=%u arr=%08x "
            "fnState=%08x fnStateEx=%08x | calls: GetState=%u GetStateEx=%u "
            "GetCaps=%u\n",
            init_b, reentry, ndev, arr, fn_state, fn_stateex,
            xi_n_getstate, xi_n_getstateex, xi_n_getcaps);

    /* Per device: the array holds device object pointers; dev[0x20] is the substructure */
    if (ndev > 0u && ndev < 16u && xi_safe_ptr(arr)) {
        for (uint32_t i = 0; i < ndev; i++) {
            uint32_t dev = ld32(arr + i * 4u);
            if (!xi_safe_ptr(dev)) {
                fprintf(stderr, "[xprobe]   dev[%u] = %08x (invalid pointer)\n", i, dev);
                continue;
            }
            uint32_t sub = ld32(dev + 0x20u);
            uint8_t  flag = xi_safe_ptr(sub) ? ld8(sub) : 0xEEu;
            uint32_t user = xi_safe_ptr(sub) ? ld32(sub + 0x2Cu) : 0xFFFFFFFFu;
            fprintf(stderr, "[xprobe]   dev[%u]=%08x sub=%08x flag=%u user=%u%s\n",
                    i, dev, sub, (unsigned)flag, user,
                    flag ? "" : "  <- ★ this device is skipped; GetState will not be called");
        }
    }
    fflush(NULL);
}
