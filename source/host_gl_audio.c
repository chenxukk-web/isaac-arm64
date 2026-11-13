/* ==========================================================================
 * host_gl_audio.c -- OPENGL32.dll(5) + GDI32.dll(6) + OpenAL32.dll(24) host layer
 *
 * Goal: the statically recompiled build of "The Binding of Isaac: Repentance" (repentogxm) on Android arm64.
 * This layer exists so guest calls into these imports **do not crash, do not spin, and get correctly
 * shaped values** -- not to really render or play audio. The real GL backend (vitaGL / EGL) is gl_bridge's job later.
 *
 * ---------------------------------------------------------------------------
 * 1. Import list (all verified with grep on ~/tmp/rgxm_build/gen/guest_table.c)
 *
 *   GDI32.dll (6)  0x00606010 SetPixelFormat   0x00606014 GetDeviceCaps
 *                  0x00606018 DeleteDC         0x0060601c DescribePixelFormat
 *                  0x00606020 SwapBuffers      0x00606024 CreateDCA
 *   OPENGL32.dll(5) 0x006062f0 wglDeleteContext 0x006062f4 wglCreateContext
 *                  0x006062f8 wglShareLists    0x006062fc wglGetProcAddress
 *                  0x00606300 wglMakeCurrent
 *   OpenAL32.dll(24) 0x00606308..0x00606364, 24 consecutive IAT slots (see the table below)
 *
 *   Note: the game imports **no** gl* functions at all (glClear/glTexImage2D...).
 *   Every GL entry point is resolved dynamically through wglGetProcAddress -- so the
 *   "5 OPENGL32 imports" are all WGL context management + the dynamic resolver, not
 *   draw calls. That fact determines the shape of the second tier (see section 5).
 *
 * ---------------------------------------------------------------------------
 * 2. Calling conventions (verified, not guessed)
 *
 *   OPENGL32/GDI32: x86 **stdcall** (WINAPI/APIENTRY).
 *       Evidence: the comment on wglGetProcAddress in recomp/runtime/host_win32.c
 *       "`PROC WINAPI wglGetProcAddress(LPCSTR)` -- x86 stdcall, one argument."
 *       plus the Win32 header declarations for GDI32/WGL. So a return = pop the return address + clean the arguments.
 *
 *   OpenAL32: x86 **cdecl**.
 *       Evidence: recomp/runtime/host_vita_audio.h, lines 8-11, verbatim:
 *         "All entries use the Win32 OpenAL cdecl ABI, so an adapter removes
 *          only the return address; the translated caller removes its own
 *          arguments."
 *       which matches vita_audio_cdecl_return() in host_vita_audio.c (gpop only).
 *       → here we **only** gpop and never clean the arguments; the guest cleans them with its own add esp,N.
 *       Cost of guessing wrong: the stack shifts by N bytes on every call, so after a few hundred
 *       calls every return address is wrong and it crashes far away from the actual scene. This one is verified, no need to gamble.
 *
 * ---------------------------------------------------------------------------
 * 3. Low 4GB constraint
 *
 *   Guest pointers are uint32_t. Any **pointer** handed to the guest must be a low-4GB address.
 *   This file uses a bump allocator starting at 0x3C000000 (another agent uses 0x3B000000 for user32,
 *   the stack is at 0x40000000, the image at 0x3c000000, the TIB at 0x32xxxxxx), with the upper bound at
 *   0x3FF00000 (stop 1 MiB below the stack), so it never collides with any of those regions.
 *
 *   Only ALCdevice* / ALCcontext* have "real pointer" shape (real OpenAL returns heap
 *   pointers), so they are allocated from the low 4GB range and stamped with a magic; destroy invalidates it.
 *   HDC / HGLRC are small integer kernel handles in Win32 anyway and the guest never dereferences them,
 *   so we use tagged small integers (0x00DCxxxx / 0x00ACxxxx): they never mix with image addresses
 *   and are never treated as a dereferenceable pointer that would corrupt memory.
 *
 * ---------------------------------------------------------------------------
 * 4. Risk points (what whoever wires this up needs to know)
 *
 *   R1  wglGetProcAddress currently returns 0. The game resolves every GL entry through it; returning 0 makes
 *       its loader store 0 into the function pointer table, so on the first real draw the guest side raises
 *       a **named fault** of "indirect call to untranslated address" (controlled and diagnosable).
 *       That is the honest answer for now: with no GL backend, any non-zero value would be a fabrication.
 *       To wire a real implementation later: define the weak symbol host_gl_proc_resolve() (see section 5)
 *       and route the token it returns through guest_host_dynamic.
 *   R2  The 110..121 indices of GetDeviceCaps (PHYSICAL / VREFRESH / SHADEBLENDCAPS...)
 *       are written from the index table in wingdi.h; no Windows SDK headers were at hand to cross-check.
 *       The 0..44 range (HORZRES=8 etc.) is a high-confidence ABI that has been stable for decades; the low-confidence ones
 *       are flagged individually in the switch, and the game in practice only queries HORZRES/VERTRES/BITSPIXEL/
 *       PLANES/LOGPIXELS*.
 *   R3  All of OpenAL is a "bookkeeping implementation": handles, IDs and state are tracked for real, but nothing sounds.
 *       Therefore alGetSourcei(AL_BUFFERS_PROCESSED) is always 0 -- deliberately:
 *       lying that playback finished would make the music stream refill buffers forever at CPU speed (busy loop).
 *       The cost is silence; the gain is no CPU burn and no crash. See the comment on ga_alGetSourcei.
 *   R4  RING depth 16 / 256 source cap are resource limits; beyond that the oldest slot is reused
 *       (that old ID is then judged invalid). The Vita version gives 80; here there is 3x headroom.
 *
 * ---------------------------------------------------------------------------
 * 5. External interface (the parent agent does the wiring)
 *
 *   int host_gl_dispatch(CPU *__restrict c, const char *name);   GL+GDI, 11 entries
 *   int host_gl_count(void);                                     -> 11
 *   int host_audio_dispatch(CPU *__restrict c, const char *name); OpenAL, 24 entries
 *   int host_audio_count(void);                                  -> 24
 *
 *   name is the full form "OPENGL32.dll!wglCreateContext" (exact strcmp, the same
 *   convention as host_android.c; no guessing of case variants).
 *
 *   Optional extension (compiles and links even when undefined):
 *     extern uint32_t host_gl_proc_resolve(const char *name);  // weak symbol
 *     Defining it = give wglGetProcAddress a real resolver (e.g. the gl_bridge token).
 *   Tunable external variables (so the integrator can change the resolution at startup):
 *     unsigned host_gl_screen_width  = 1280;
 *     unsigned host_gl_screen_height = 720;
 * ========================================================================== */

#define _GNU_SOURCE
#include <unistd.h>   /* access(), F_OK -- file-based switch probing */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "guest.h"

#ifndef MAP_FIXED_NOREPLACE
/* On old kernels/bionic that lack this flag, degrade to a "hint address": the got != want check
 * below munmaps any mapping that did not land at the target address, so the semantics stay safe (it just fails more easily). */
#define MAP_FIXED_NOREPLACE 0
#endif

/* Display size the integrator may change. Default 1280x720 (the default window size of the PC version). */
unsigned host_gl_screen_width  = 1280U;
unsigned host_gl_screen_height = 720U;

/* ==========================================================================
 * 1. Argument reading / returning (conventions in header section 2)
 * ========================================================================== */

/* i-th stdcall argument: esp points at the return address, arguments start at esp+4. */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}

/* stdcall return: pop the return address + clean n bytes of arguments. */
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* cdecl return: pop **only** the return address. OpenAL arguments are cleaned by the guest's own add esp,N,
 * so cleaning even one extra byte here would misalign the return address of the next call. */
static void cdecl_ret(CPU *__restrict c)
{
    (void)gpop(c);
}

/* ==========================================================================
 * 2. Logging (budgeted, no screen flooding)
 *
 * This layer is called hundreds or thousands of times per frame (SwapBuffers / alSourcePlay). Printing
 * everything would make stderr a bottleneck, so: a 256-line budget, then one "suppressed" line and silence.
 * Only key events (handle creation, unknown arguments, latched errors) are allowed through here.
 * ========================================================================== */

#define GA_NOTE_BUDGET 256U
static unsigned g_ga_note_left = GA_NOTE_BUDGET;

static void ga_note(const char *fmt, ...)
{
    va_list ap;

    if (g_ga_note_left == 0u)
        return;
    --g_ga_note_left;
    fputs("[glaudio] ", stderr);
    va_start(ap, fmt);
    (void)vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_ga_note_left == 0u)
        fputs("[glaudio] (further notes suppressed)\n", stderr);
    (void)fflush(stderr);
}

/* Copy an ASCII name out of a low guest address (used by wglGetProcAddress).
 * Returns 1 = fully captured and NUL-terminated; 0 = null pointer / non-ASCII / no terminator.
 * Read only when address is clearly user space (> 0xFFFF), so 0/small integers are never walked as strings. */
static int ga_copy_ascii(uint32_t address, char *out, size_t cap)
{
    size_t i;

    if (!address || (address >> 16u) == 0u || !out || cap == 0u)
        return 0;
    for (i = 0u; i + 1u < cap; ++i) {
        uint8_t ch = ld8(address + (uint32_t)i);
        if (ch == 0u) {
            out[i] = '\0';
            return 1;
        }
        if (ch > 0x7fu) {         /* GL/WGL symbol names are always ASCII */
            out[i] = '\0';
            return 0;
        }
        out[i] = (char)ch;
    }
    out[cap - 1u] = '\0';
    return 0;                     /* truncated: the caller treats it as an unknown name */
}

/* ==========================================================================
 * 3. Low-4GB bump allocator
 *
 * Starts at 0x3C000000, upper bound 0x3FF00000 (1 MiB below the guest stack at 0x40000000).
 * Never freed: over the whole process lifetime this layer allocates only single-digit numbers of device/context objects.
 * ========================================================================== */

#define GA_LOW_BASE 0x3C000000u
#define GA_LOW_END  0x3FF00000u

static uint32_t g_ga_bump = GA_LOW_BASE;

static uint32_t ga_low_alloc(uint32_t size)
{
    uint32_t sz = (size + 15u) & ~15u;
    void *want;
    void *got;

    if (sz == 0u)
        return 0u;
    if (g_ga_bump > GA_LOW_END - sz) {
        ga_note("low-4GB allocator exhausted (wanted %u bytes, start 0x%08x)\n",
                (unsigned)sz, (unsigned)g_ga_bump);
        return 0u;
    }
    want = (void *)(uintptr_t)g_ga_bump;
    got = mmap(want, (size_t)sz, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got != want) {
        if (got != MAP_FAILED)
            (void)munmap(got, (size_t)sz);
        ga_note("low-4GB mapping at 0x%08x failed (occupied); this layer switches to synthetic handles\n",
                (unsigned)g_ga_bump);
        return 0u;                /* the caller must degrade to a tag handle, never return NULL */
    }
    g_ga_bump += sz;
    return (uint32_t)(uintptr_t)want;
}

/* ==========================================================================
 * 4. GDI32 (6) -- fake handles + real data
 * ========================================================================== */

/* In Win32, HDC/HGLRC are kernel handles (small integers) and the guest never dereferences them.
 * Use tagged small integers: obviously synthetic at a glance, and never treated as a readable pointer. */
#define GA_HDC_TAG   0x00DC0000u
#define GA_HGLRC_TAG 0x00AC0000u
#define GA_HANDLE_KIND(h) ((h) & 0xFFFF0000u)

static uint32_t g_ga_hdc_next   = GA_HDC_TAG;
static uint32_t g_ga_hglrc_next = GA_HGLRC_TAG;

/* The handle is used only to classify log lines, **never** to reject a call: the HDC may also come
 * from another agent's user32 layer (GetDC), and that layer's encoding is not ours to assume. */
static int ga_is_our_hdc(uint32_t hdc)
{
    return GA_HANDLE_KIND(hdc) == GA_HDC_TAG;
}

/* GetDeviceCaps indices (wingdi.h). The index values are part of the ABI and cannot change. */
#define GA_CAP_DRIVERVERSION    0
#define GA_CAP_TECHNOLOGY       2
#define GA_CAP_HORZSIZE         4
#define GA_CAP_VERTSIZE         6
#define GA_CAP_HORZRES          8
#define GA_CAP_VERTRES          10
#define GA_CAP_BITSPIXEL        12
#define GA_CAP_PLANES           14
#define GA_CAP_NUMBRUSHES       16
#define GA_CAP_NUMPENS          18
#define GA_CAP_NUMMARKERS       20
#define GA_CAP_NUMFONTS         22
#define GA_CAP_NUMCOLORS        24
#define GA_CAP_PDEVICESIZE      26
#define GA_CAP_CURVECAPS        28
#define GA_CAP_LINECAPS         30
#define GA_CAP_POLYGONCAPS      32
#define GA_CAP_TEXTCAPS         34
#define GA_CAP_CLIPCAPS         36
#define GA_CAP_RASTERCAPS       38
#define GA_CAP_ASPECTX          40
#define GA_CAP_ASPECTY          42
#define GA_CAP_ASPECTXY         44
#define GA_CAP_LOGPIXELSX       88
#define GA_CAP_LOGPIXELSY       90
#define GA_CAP_SIZEPALETTE      104
#define GA_CAP_NUMRESERVED      106
#define GA_CAP_COLORRES         108
#define GA_CAP_PHYSICALWIDTH    110
#define GA_CAP_PHYSICALHEIGHT   111
#define GA_CAP_PHYSICALOFFSETX  112
#define GA_CAP_PHYSICALOFFSETY  113
#define GA_CAP_SCALINGFACTORX   114
#define GA_CAP_SCALINGFACTORY   115
#define GA_CAP_VREFRESH         116
#define GA_CAP_DESKTOPVERTRES   117
#define GA_CAP_DESKTOPHORZRES   118
#define GA_CAP_BLTALIGNMENT     119
#define GA_CAP_SHADEBLENDCAPS   120
#define GA_CAP_COLORMGMTCAPS    121

/* RASTERCAPS bits (wingdi.h). Written in OR form so readers need not look up the magic numbers. */
#define GA_RC_BITBLT      0x0001u
#define GA_RC_BITMAP64    0x0008u
#define GA_RC_DEVBITS     0x0020u
#define GA_RC_DI_BITMAP   0x0080u
#define GA_RC_DIBTODEV    0x0200u
#define GA_RC_STRETCHBLT  0x0800u
#define GA_RC_STRETCHDIB  0x2000u

/* Single source of truth for "this device's capabilities". An unknown index returns -1 (0 would be misread
 * as "unsupported"; -1 stands out more and is close to what real GDI does for an unknown index). */
static int32_t ga_device_caps(uint32_t index)
{
    uint32_t w = host_gl_screen_width ? host_gl_screen_width : 1280U;
    uint32_t h = host_gl_screen_height ? host_gl_screen_height : 720U;

    switch (index) {
    case GA_CAP_DRIVERVERSION:  return 0x0400;      /* synthetic display driver version */
    case GA_CAP_TECHNOLOGY:     return 1;           /* DT_RASDISPLAY */
    /* Physical size: derived from pixels at 96 DPI, inch = px/96, mm = inch*25.4 */
    case GA_CAP_HORZSIZE:       return (int32_t)((w * 254U) / 960U);
    case GA_CAP_VERTSIZE:       return (int32_t)((h * 254U) / 960U);
    case GA_CAP_HORZRES:        return (int32_t)w;
    case GA_CAP_VERTRES:        return (int32_t)h;
    case GA_CAP_BITSPIXEL:      return 32;          /* RGBA8888 back buffer */
    case GA_CAP_PLANES:         return 1;           /* always 1 on modern display devices */
    /* 16/18/20/22: 0xFFFF = no device-side limit (GDI emulates brushes/pens/fonts itself). */
    case GA_CAP_NUMBRUSHES:     return 0xFFFF;
    case GA_CAP_NUMPENS:        return 0xFFFF;
    case GA_CAP_NUMMARKERS:     return 0xFFFF;
    case GA_CAP_NUMFONTS:       return 0xFFFF;
    /* NUMCOLORS: for >8bpp devices MSDN says return -1 ("no color table").
     * Returning 0 would make the game think there is a 0-entry color table; 256 would be worse; -1 is the standard answer. */
    case GA_CAP_NUMCOLORS:      return -1;
    case GA_CAP_PDEVICESIZE:    return 0;           /* no CDC private block */
    /* GDI emulates every curve/line/polygon capability on behalf of the display driver. */
    case GA_CAP_CURVECAPS:      return 0x01FF;      /* all CC_* set */
    case GA_CAP_LINECAPS:       return 0x01FF;      /* all LC_* set */
    case GA_CAP_POLYGONCAPS:    return 0x00FF;      /* all PC_* set */
    case GA_CAP_TEXTCAPS:       return 0x0001;      /* TC_OP_CHARACTER only */
    case GA_CAP_CLIPCAPS:       return 1;           /* CP_RECTANGLE */
    case GA_CAP_RASTERCAPS:
        /* No RC_PALETTE (no palette at 32bpp); bitblt/stretch/DIB/devbits are all supported. */
        return (int32_t)(GA_RC_BITBLT | GA_RC_BITMAP64 | GA_RC_DEVBITS |
                         GA_RC_DI_BITMAP | GA_RC_DIBTODEV |
                         GA_RC_STRETCHBLT | GA_RC_STRETCHDIB);
    case GA_CAP_ASPECTX:        return 36;          /* pixel aspect ratio used for line drawing */
    case GA_CAP_ASPECTY:        return 36;
    case GA_CAP_ASPECTXY:       return 51;          /* round(36*sqrt2) */
    case GA_CAP_LOGPIXELSX:     return 96;          /* 96 DPI logical coordinates */
    case GA_CAP_LOGPIXELSY:     return 96;
    case GA_CAP_SIZEPALETTE:    return 0;           /* no palette */
    case GA_CAP_NUMRESERVED:    return 0;
    case GA_CAP_COLORRES:       return 24;          /* 24-bit color resolution */
    /* 110..121: indices follow wingdi.h; the values just give the game a self-consistent desktop geometry.
     * Low confidence (see header R2), but the game does not actually query this range in practice. */
    case GA_CAP_PHYSICALWIDTH:   return (int32_t)w;
    case GA_CAP_PHYSICALHEIGHT:  return (int32_t)h;
    case GA_CAP_PHYSICALOFFSETX: return 0;
    case GA_CAP_PHYSICALOFFSETY: return 0;
    case GA_CAP_SCALINGFACTORX:  return 0;
    case GA_CAP_SCALINGFACTORY:  return 0;
    case GA_CAP_VREFRESH:        return 60;         /* target frame rate */
    case GA_CAP_DESKTOPVERTRES:  return (int32_t)h;
    case GA_CAP_DESKTOPHORZRES:  return (int32_t)w;
    case GA_CAP_BLTALIGNMENT:    return 4;          /* 4-byte alignment is enough */
    case GA_CAP_SHADEBLENDCAPS:  return 0;          /* no device-side alpha blending */
    case GA_CAP_COLORMGMTCAPS:   return 0;          /* no ICM */
    default:                     return -1;
    }
}

/* Synthetic PIXELFORMATDESCRIPTOR: 40 bytes, 32bpp RGBA + 24-bit depth + 8-bit stencil +
 * double buffering + OpenGL support. The game only inspects it to see whether the format is agreeable; it never renders. */
#define GA_PFD_BYTES 40u

static void ga_write_pfd(uint32_t dst, uint32_t bytes)
{
    uint8_t pfd[GA_PFD_BYTES];
    uint32_t n = bytes < GA_PFD_BYTES ? bytes : GA_PFD_BYTES;

    memset(pfd, 0, sizeof pfd);
    /* nSize */     pfd[0] = (uint8_t)GA_PFD_BYTES;
    /* nVersion */  pfd[2] = 1;
    /* dwFlags = PFD_DOUBLEBUFFER(0x1)|PFD_DRAW_TO_WINDOW(0x4)|PFD_SUPPORT_OPENGL(0x20) */
    pfd[4] = 0x25;
    pfd[8]  = 0;    /* iPixelType = PFD_TYPE_RGBA */
    pfd[9]  = 32;   /* cColorBits */
    pfd[10] = 8;    /* cRedBits */
    pfd[11] = 16;   /* cRedShift */
    pfd[12] = 8;    /* cGreenBits */
    pfd[13] = 8;    /* cGreenShift */
    pfd[14] = 8;    /* cBlueBits */
    pfd[15] = 0;    /* cBlueShift */
    pfd[16] = 8;    /* cAlphaBits */
    pfd[17] = 24;   /* cAlphaShift */
    pfd[23] = 24;   /* cDepthBits */
    pfd[24] = 8;    /* cStencilBits */
    pfd[26] = 0;    /* iLayerType = PFD_MAIN_PLANE */
    memcpy((void *)(uintptr_t)dst, pfd, (size_t)n);
}

/* `HDC WINAPI CreateDCA(LPCSTR, LPCSTR, LPCSTR, const DEVMODEA*)` -- stdcall, 16B.
 * Returns a synthetic HDC: the game passes it to SetPixelFormat / wglMakeCurrent / SwapBuffers,
 * and none of those dereference it, so one tag integer is enough and is safer than forging an address. */
static void ga_CreateDCA(CPU *__restrict c)
{
    uint32_t driver = A(c, 0);
    uint32_t device = A(c, 1);
    uint32_t port = A(c, 2);
    uint32_t devmode = A(c, 3);
    uint32_t hdc;

    (void)driver; (void)device; (void)port;   /* guest strings not read: nothing to configure without a backend */
    hdc = ++g_ga_hdc_next;
    /* "Never return a NULL handle without logging": every handle creation leaves a trace. */
    ga_note("CreateDCA -> HDC 0x%08x (synthetic, devmode=%s)\n",
            (unsigned)hdc, devmode ? "present (ignored)" : "NULL");
    c->eax = hdc;
    R(c, 16u);
}

/* `int WINAPI GetDeviceCaps(HDC, int index)` -- stdcall, 8B.
 * This tier must return **correct data**: the game uses it to decide resolution/color depth/DPI, and wrong values pick a wrong init branch. */
static void ga_GetDeviceCaps(CPU *__restrict c)
{
    uint32_t hdc = A(c, 0);
    uint32_t index = A(c, 1);
    int32_t value = ga_device_caps(index);

    if (!ga_is_our_hdc(hdc)) {
        /* Log once only, do not reject: the HDC may come from the user32 layer. */
        static int noted;
        if (!noted) {
            noted = 1;
            ga_note("GetDeviceCaps received an HDC from another layer 0x%08x (answering anyway)\n",
                    (unsigned)hdc);
        }
    }
    if (value < 0 && index != GA_CAP_NUMCOLORS) {
        static int unknown_noted;
        if (!unknown_noted) {
            unknown_noted = 1;
            ga_note("GetDeviceCaps unknown index %u -> -1\n", (unsigned)index);
        }
    }
    c->eax = (uint32_t)value;
    R(c, 8u);
}

/* `BOOL WINAPI DeleteDC(HDC)` -- stdcall, 4B. TRUE: deleting a synthetic handle always succeeds. */
static void ga_DeleteDC(CPU *__restrict c)
{
    (void)A(c, 0);
    c->eax = 1u;
    R(c, 4u);
}

/* `int WINAPI SetPixelFormat(HDC, int format, const PIXELFORMATDESCRIPTOR*)`
 * -- stdcall, 12B. Returns TRUE: there is no real context on which to choose a format, but rejecting would
 * make the game fall straight into its error branch during window creation. ppfd == NULL is only logged (still TRUE),
 * because this is a bring-up layer: better one step too many than a fake failure. */
static void ga_SetPixelFormat(CPU *__restrict c)
{
    uint32_t hdc = A(c, 0);
    uint32_t format = A(c, 1);
    uint32_t ppfd = A(c, 2);

    if (!ppfd) {
        static int noted;
        if (!noted) { noted = 1; ga_note("SetPixelFormat(ppfd=NULL) still returns TRUE\n"); }
    }
    if (!ga_is_our_hdc(hdc)) {
        static int noted_hdc;
        if (!noted_hdc) {
            noted_hdc = 1;
            ga_note("SetPixelFormat received an HDC from another layer 0x%08x\n", (unsigned)hdc);
        }
    }
    ga_note("SetPixelFormat(HDC 0x%08x, format %u) -> TRUE\n",
            (unsigned)hdc, (unsigned)format);
    c->eax = 1u;
    R(c, 12u);
}

/* `int WINAPI DescribePixelFormat(HDC, int format, UINT nBytes, PFD*)`
 * -- stdcall, 16B.
 * The return value is the "total number of pixel formats"; there is exactly one synthetic format here, so it is always 1;
 * real GDI writes format number `format` into the caller buffer when ppfd!=NULL and nBytes>0. */
static void ga_DescribePixelFormat(CPU *__restrict c)
{
    uint32_t hdc = A(c, 0);
    uint32_t format = A(c, 1);
    uint32_t bytes = A(c, 2);
    uint32_t ppfd = A(c, 3);

    (void)hdc;
    if (ppfd && bytes)
        ga_write_pfd(ppfd, bytes);
    if (!ppfd || !bytes) {
        /* Pure "how many formats" query, no out-of-bounds write. */
        static int noted;
        if (!noted) { noted = 1; ga_note("DescribePixelFormat format count query\n"); }
    }
    ga_note("DescribePixelFormat(format %u, nBytes %u) -> 1 synthetic format\n",
            (unsigned)format, (unsigned)bytes);
    c->eax = 1u;
    R(c, 16u);
}

/* `BOOL WINAPI SwapBuffers(HDC)` -- stdcall, 4B. TRUE: there is no back buffer to flip,
 * but returning FALSE would make the game think presenting failed and possibly quit. Bookkeeping only, no printing (called every frame). */
static uint32_t g_ga_swaps;

unsigned host_gl_swap_count(void) { return g_ga_swaps; }

static void ga_SwapBuffers(CPU *__restrict c)
{
    (void)A(c, 0);
    ++g_ga_swaps;
    /* ★ 2026-11-06: time from boot to the first frame -- the "black screen wait" the user perceives. */
    if (g_ga_swaps == 1u) {
        extern void host_boot_mark(const char *) __attribute__((weak));
        if (host_boot_mark) host_boot_mark("first frame rendered (SwapBuffers #1)");
    }
    if (g_ga_swaps == 1u)
        ga_note("SwapBuffers called for the first time (synthetic, no present)\n");

    /* ★★ 2026-11-10: texture **final state** summary.
     *
     * Why needed: the existing texstat counts per call, and transient placeholder states pollute
     * the data -- the same id uploaded empty first and with real data later counts as one "all zero".
     * Report the numbers that are really stable on a 300-frame (~5 s) cadence:
     *   - how many texture ids have a known state
     *   - of those, how many still have all-zero final contents
     *   - whether the one bound this frame is all zero (shows up directly on screen)
     * Only this number can line up with the "missing textures" the user sees. */
    if ((g_ga_swaps % 300u) == 0u) {
        extern unsigned char g_tex_zero[];
        extern unsigned g_tex_w[];
        extern unsigned g_tex_h[];
        static const unsigned N = 2048;
        unsigned known = 0, zero = 0;
        for (unsigned i = 1; i < N; i++) {
            if (g_tex_zero[i]) { known++; if (g_tex_zero[i] == 2) zero++; }
        }
        /* ★ 2026-11-10: FBO/placeholder textures (g_tex_zero==0) are excluded from the denominator.
         * Their glTexImage2D passes NULL, so the CPU side necessarily reads zeros --
         * that is invisibility, not a broken image. The previous version counted them in the "all zero" denominator
         * and reported 19-20%, which did not match what the user saw. */
        extern int g_cur_tex_id;
        unsigned curz = 0;
        if (g_cur_tex_id > 0 && (unsigned)g_cur_tex_id < N)
            curz = g_tex_zero[g_cur_tex_id];
        /* ★ draw attribution within these 300 frames: screen vs offscreen RT */
        {
            extern unsigned long g_draws_fb0, g_draws_fb1;
            static unsigned long p0, p1;
            fprintf(stderr,
                "[drawstat] frame#%u  draws this window: screen=%lu  offscreenRT=%lu  (total %lu/%lu)\n",
                g_ga_swaps, g_draws_fb0 - p0, g_draws_fb1 - p1,
                g_draws_fb0, g_draws_fb1);
            p0 = g_draws_fb0; p1 = g_draws_fb1;
        }
        fprintf(stderr, "[texfinal] frame#%u  really uploaded %u: good %u all-zero %u (%.1f%%)"
                "  current binding id=%d %ux%u %s\n",
                g_ga_swaps, known, known - zero, zero,
                known ? 100.0 * (double)zero / (double)known : 0.0,
                g_cur_tex_id,
                (g_cur_tex_id > 0 && (unsigned)g_cur_tex_id < N) ? g_tex_w[g_cur_tex_id] : 0u,
                (g_cur_tex_id > 0 && (unsigned)g_cur_tex_id < N) ? g_tex_h[g_cur_tex_id] : 0u,
                curz == 2 ? "[all zero]" : (curz == 1 ? "[has data]" : "[FBO/placeholder]"));

        /* ★★ 2026-11-10 (plan B): effect of the re-upload repair.
         *
         * The "all zero" number now drops because of the re-upload at draw time --
         * but the drop may be a real fix, or it may be the "content someone else wrote later"
         * being replayed (a wrong image). So both numbers must be read together:
         *   re-uploaded N       → N textures were uploaded once more at draw time
         *   still not ready M   → their buffers are still zero to this day, beyond repair
         * only together with the same frame's [repair] line (first 4 bytes and RGB) can right or wrong be judged. */
        {
            extern unsigned g_tex_repairs, g_tex_repair_wait;
            extern int host_tex_repair_enabled(void);
            /* Do not print this line when disabled -- otherwise one "0 textures" line every 300 frames.
             * The switch state itself was already logged once at the first probe. */
            if (host_tex_repair_enabled())
                fprintf(stderr,
                    "[texrepair] frame#%u  re-uploads so far %u, %u probes found data still not ready\n",
                    g_ga_swaps, g_tex_repairs, g_tex_repair_wait);
        }

        /* ★ 2026-11-10: list the all-zero ids in the first 3 summaries.
         * Knowing only "23% all zero" is useless -- we need to know which images at which sizes,
         * to tell whether they are "blank by design" (empty item slots, unused atlas pages)
         * or "decode failures". */
        {
            static unsigned dumps;
            if (dumps < 3u) {
                dumps++;
                fprintf(stderr, "[zero-list] frame#%u all-zero id/size:", g_ga_swaps);
                unsigned shown = 0;
                for (unsigned i = 1; i < N && shown < 40; i++)
                    if (g_tex_zero[i] == 2) {
                        fprintf(stderr, " %u(%ux%u)", i, g_tex_w[i], g_tex_h[i]);
                        shown++;
                    }
                fprintf(stderr, "%s\n", zero > shown ? " ..." : "");
            }
        }

        /* ★★ 2026-11-10: channel distribution summary -- decides "is the data layer really broken".
         *
         * On device the whole picture is washed out and grey, while the 6 hand-checked 32x32 textures
         * (HUD icons) all read R=35 G=0 B=0 (G/B exactly zero).
         * Real images are never like that, so either the data has only one channel
         * or the declared format does not match reality.
         *
         * This line gives the full distribution and directly decides the next direction:
         *   - a high "R only" share → format/channel problem, change the upload parameters
         *   - a high "color" share → the data is fine, the problem is in sampling/blending
         */
        {
            extern unsigned char g_tex_class[];
            unsigned c_color = 0, c_ronly = 0, c_aonly = 0, c_gray = 0;
            for (unsigned i = 1; i < N; i++) {
                switch (g_tex_class[i]) {
                    case 1: c_color++; break;
                    case 2: c_ronly++; break;
                    case 3: c_aonly++; break;
                    case 4: c_gray++;  break;
                    default: break;
                }
            }
            fprintf(stderr,
                "[texchan] frame#%u  color=%u  only-R=%u  only-A=%u  pure-gray=%u"
                "  (total %u)\n",
                g_ga_swaps, c_color, c_ronly, c_aonly, c_gray,
                c_color + c_ronly + c_aonly + c_gray);

            /* ★★ 2026-11-10: list the concrete "R only" ids.
             *
             * These are the suspects: even a mostly-red real image has
             * non-zero G/B from antialiasing/shadows. Exactly 0 means those two bytes
             * were never written at all.
             *
             * Only with the concrete id and size can the [draw] lines tell us
             * whether these are the ones used for the HUD/sprites. */
            {
                static unsigned dumped;
                if (dumped < 2u) {
                    dumped++;
                    fprintf(stderr, "[ronly-list] frame#%u:", g_ga_swaps);
                    unsigned sh = 0;
                    for (unsigned i = 1; i < N && sh < 30; i++)
                        if (g_tex_class[i] == 2) {
                            fprintf(stderr, " %u(%ux%u)", i,
                                    g_tex_w[i], g_tex_h[i]);
                            sh++;
                        }
                    fprintf(stderr, "%s\n", c_ronly > sh ? " ..." : "");

                    /* Also list the "pure grey" size distribution -- if there are many
                     * 32x32 ones, the HUD icons are probably being treated as greyscale. */
                    unsigned g32 = 0, g_other = 0;
                    for (unsigned i = 1; i < N; i++)
                        if (g_tex_class[i] == 4) {
                            if (g_tex_w[i] == 32 && g_tex_h[i] == 32) g32++;
                            else g_other++;
                        }
                    fprintf(stderr, "[gray-list] of the pure-gray ones 32x32=%u, other sizes=%u\n",
                            g32, g_other);

                    /* ★★ 2026-11-10: hit counts of the untranslated stubs.
                     * This is the only reliable way to decide "are the all-zero textures caused by unimplemented SIMD decode functions"
                     * -- it does not depend on the log buffer; the counters always arrive. */
                    {
                        extern int g_stub_hits[];
                        unsigned total = 0, distinct = 0;
                        for (int i = 0; i < 65536; i++)
                            if (g_stub_hits[i]) { total += g_stub_hits[i]; distinct++; }
                        fprintf(stderr, "[stubhits] frame#%u hits total=%u across %u distinct stubs\n",
                                g_ga_swaps, total, distinct);
                        if (total) {
                            /* list the 10 most-hit stubs */
                            for (int k = 0; k < 10; k++) {
                                int best = -1; int bestv = 0;
                                for (int i = 0; i < 65536; i++)
                                    if (g_stub_hits[i] > bestv) { bestv = g_stub_hits[i]; best = i; }
                                if (best < 0) break;
                                fprintf(stderr, "[stubhits]   0x%04x%04x -> %d hits\n",
                                        0x005d0000 + (best >> 8), best & 0xff, bestv);
                                g_stub_hits[best] = 0;   /* report the Top10 only once */
                            }
                        }
                    }
                }
            }
        }
        fflush(NULL);
    }
    /* Frame-count heartbeat: used to decide "did we actually get into the main loop".
     * One line every 600 swaps (~10 s at 60fps), output only when FRAME_TRACE=1. */
    /* Frame interval statistics: average/max frame time every 600 frames.
     * Used to tell whether "50fps" is a stable cap or dropped frames. */
    {
        static uint64_t t_prev;
        static uint64_t sum_dt;
        static uint64_t max_dt;
        static unsigned n;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        if (t_prev) {
            uint64_t dt = now - t_prev;
            sum_dt += dt;
            if (dt > max_dt) max_dt = dt;
            n++;

            extern unsigned g_host_import_calls;
            static unsigned prev_calls;
            /* ★ Updated every frame: count only the imports within **this frame**,
             * otherwise the delta would accumulate from startup and lose its meaning. */
            unsigned delta = g_host_import_calls - prev_calls;
            prev_calls = g_host_import_calls;

            /* ★★ 2026-11-10: slow-frame scene.
             *
             * perf keeps showing "max 1005-1026 ms" spikes while good frames take only
             * 17 ms -- that is not uniform slowdown, it is periodic stalling.
             *
             * Two fields read together settle it:
             *   - previous import = the last non-SwapBuffers import before the stall
             *   - imports during  = how many imports the guest ran inside this frame
             *
             * Reading:
             *   imports during ~0 and previous import is a sleep/lock stub → that is the blocker
             *   imports during is large → the guest spins in translated code (needs CPU sampling)
             *   previous import is gl* → the GL side (eglSwapBuffers blocking) */
            if (dt >= 200000000ull) {
                extern const char *host_prev_import(void);
                fprintf(stderr, "[stall] single frame %.1f ms  previous import=%s  "
                        "imports during it=%u  frame#%u\n",
                        (double)dt / 1e6,
                        host_prev_import(),
                        delta, g_ga_swaps);
                fflush(NULL);
            }
        }
        t_prev = now;
        /* ★ 2026-11-07: shorter period. It used to print once per 600 frames, but the known frame rate
         * may be single digits -- 600 frames take minutes, [perf] would never appear in the log,
         * i.e. no measurement at all. Changed to 60 frames (once per second at 60fps, once per 30 s at 2fps). */
        if (n >= 60u) {
            fprintf(stderr, "[perf] %u frames: avg %.2f ms (%.1f fps), max %.2f ms\n",
                    n, (double)sum_dt / (double)n / 1e6,
                    1e9 * (double)n / (double)sum_dt,
                    (double)max_dt / 1e6);
            fflush(NULL);
            sum_dt = max_dt = 0; n = 0;
        }
    }
    if ((g_ga_swaps % 600u) == 0u) {
        static int on = -1;
        if (on < 0) on = getenv("FRAME_TRACE") != NULL;
        if (on) {            extern unsigned g_frame_draws, g_frame_clears, g_frame_texbinds;
            fprintf(stderr,
                    "[frame] SwapBuffers total %u | since last: draw=%u clear=%u "
                    "bindTex=%u\n",
                    g_ga_swaps, g_frame_draws, g_frame_clears, g_frame_texbinds);
            { extern void host_gl_tex_hist_report(void);
              host_gl_tex_hist_report(); }
            fflush(NULL);
            g_frame_draws = g_frame_clears = g_frame_texbinds = 0;
        }
        /* ★ 2026-11-06: XInput polling probe.
         *
         * On-device logs show GetStateEx is called only 2 times and then stops forever, while GetCapabilities
         * is called 4+ times. The head of sub_0059d720 has three switches that decide whether it really samples:
         *   0x3c803040  gamepad system initialized (byte)
         *   0x3c8090d8  reentrancy guard (byte, must be 0)
         *   0x3c803044  registered device count (dword, <=0 skips the whole thing)
         * The first byte inside the device object is also a switch (0 = skip GetState for that device).
         * Printing these items locates the broken layer in one shot. */
        {
            extern void host_xinput_probe(void) __attribute__((weak));
            if (host_xinput_probe && !getenv("XINPUT_PROBE_OFF"))
                host_xinput_probe();
            /* Resource loading takes about 100 s; after 2400 frames (~60-80 s) scan the heap once for decompressed output */
            if (g_ga_swaps >= 2400u) {
                extern void png_scan_heap_once_pub(void) __attribute__((weak));
                if (png_scan_heap_once_pub) png_scan_heap_once_pub();
            }
        }
    }
    /* ★★ 2026-11-11: frame capture must come **before** present_window.
     *
     * The trap we hit: capture used to run after present, and in in-process window mode
     * (host_gl4es_present_window returns 1) the code `return`s right here,
     * so **not a single frame was ever captured on device**; only local pbuffer mode could capture.
     * The logs prove it: on device, `[frame]` appears 0 times in native.log.
     *
     * It also makes more semantic sense: after present the back buffer contents are no longer guaranteed valid,
     * so read before the swap.
     */
    /* Frame capture: the pbuffer is offscreen, so reading it back shows what the game really rendered.
     * FRAME_DUMP_EVERY=N saves one frame at frames N, 2N... up to FRAME_DUMP_MAX (default 5);
     * FRAME_DUMP_AT=N    saves only frame N;
     * FRAME_DUMP_FROM=N  captures only frames with SwapBuffers >= N (with EVERY, to skip past early init).
     *
     * 2026-11-04: the cap went from a hard-coded 5 to configurable -- the main menu alone runs over ten thousand frames,
     * and a fixed 5 only covers up to the cutscene, so the screens really wanted were never visible. */
    {
        static int every = -1, at = -1, from = 0, max = 5, dumped;
        static const char *outdir = "frames";
        if (every < 0) {
            const char *e = getenv("FRAME_DUMP_EVERY");
            const char *a = getenv("FRAME_DUMP_AT");
            const char *f = getenv("FRAME_DUMP_FROM");
            const char *m = getenv("FRAME_DUMP_MAX");
            every = e ? atoi(e) : 0;
            at    = a ? atoi(a) : 0;
            from  = f ? atoi(f) : 0;
            max   = m ? atoi(m) : 5;
            if (max <= 0) max = 5;

            /* ★★ 2026-11-10: file-based switch.
             *
             * The app is launched by Java and cannot see environment variables, so files are the only option.
             * Same convention as <game>/exec, <game>/fast, <game>/repair.
             *
             * Uses only the **existing** host_gl_dump_frame (glFinish + glReadPixels).
             * ★ Never bind a framebuffer here, never call glGetIntegerv --
             * last time the capture path added glBindFramebuffer, which corrupted
             * state and crashed on device in glDeleteProgram → std::terminate.
             * Frame capture must be pure reads. */
            if (!every && !at) {
                static const char *const probe[] = {
                    "/storage/emulated/0/Android/media/com.isaacport.launcher"
                    "/files/game/frame_dump",
                    "/data/user/0/com.isaacport.launcher/files/game/frame_dump",
                    "frame_dump",
                    NULL
                };
                static const char *const dirs[] = {
                    "/storage/emulated/0/Android/media/com.isaacport.launcher"
                    "/files/game/frames",
                    "/data/user/0/com.isaacport.launcher/files/game/frames",
                    "frames",
                    NULL
                };
                for (int i = 0; probe[i]; i++) {
                    /* ★ As above: use fopen rather than access(). */
                    {
                        FILE *fp = fopen(probe[i], "rb");
                        if (!fp) continue;
                        fclose(fp);
                    }
                    every = 120; from = 60; max = 24;
                    outdir = dirs[i];
                    mkdir(outdir, 0777);
                    fprintf(stderr, "[frame] switch %s -> one capture every %d frames, starting at frame %d, at most %d -> %s\n",
                            probe[i], every, from, max, outdir);
                    fflush(NULL);
                    break;
                }
            }
            if (every > 0 || at > 0) mkdir("frames", 0777);
        }
        int want = 0;
        unsigned n = g_ga_swaps;
        if (at > 0 && (int)n == at) want = 1;
        if (every > 0 && (int)n % every == 0 && (int)n >= from && dumped < max) {
            want = 1; dumped++;
        }
        if (want) {
            extern int host_gl_dump_frame(const char *);
            char path[192];
            snprintf(path, sizeof path, "%s/frame_%u.ppm", outdir, n);
            host_gl_dump_frame(path);
        }
    }

    /* ★ 2026-11-06: in-process window mode -- hand this frame straight to SurfaceFlinger.
     *
     * This is the core win of .so-ification: zero copy. Each frame used to need
     *   glReadPixels(GPU→CPU) + RGB565 conversion + TCP + Java copy + drawBitmap
     * and now it needs a single eglSwapBuffers.
     *
     * Return 1 = already presented; every pixel read/stream/capture below is skipped.
     * Return 0 = still in pbuffer mode (running in Termux, or Java used the subprocess fallback),
     * so take the old streaming path with completely unchanged behavior. */
    {
        extern int host_gl4es_present_window(void) __attribute__((weak));
        if (host_gl4es_present_window && host_gl4es_present_window()) {
            c->eax = 1u;
            R(c, 4u);
            return;
        }
    }

    /* Stream frames to Java (for SurfaceView display). With FRAME_STREAM=N, push one frame every N frames. */
    {
        extern void host_gl_stream_init(void);
        extern void host_gl_stream_frame(void);
        host_gl_stream_init();
        host_gl_stream_frame();
    }
    c->eax = 1u;
    R(c, 4u);
}

/* ==========================================================================
 * 5. OPENGL32 (5) -- all WGL context management + the dynamic resolver
 *
 * Note R1: the "context" this tier returns is just a tag. Real GL calls do not go through here,
 * the guest gets the address from wglGetProcAddress and calls it directly.
 * ========================================================================== */

/* `HGLRC WINAPI wglCreateContext(HDC)` -- stdcall, 4B.
 * Returns a non-NULL synthetic context: returning NULL would make the game think "the driver does not support OpenGL",
 * pop an error box and quit, and then nothing further could be measured. */
static void ga_wglCreateContext(CPU *__restrict c)
{
    uint32_t hdc = A(c, 0);
    uint32_t hglrc = ++g_ga_hglrc_next;

    (void)hdc;
    ga_note("wglCreateContext(HDC 0x%08x) -> HGLRC 0x%08x (synthetic)\n",
            (unsigned)hdc, (unsigned)hglrc);
    c->eax = hglrc;
    R(c, 4u);
}

/* `BOOL WINAPI wglDeleteContext(HGLRC)` -- stdcall, 4B. TRUE. */
static void ga_wglDeleteContext(CPU *__restrict c)
{
    uint32_t hglrc = A(c, 0);

    ga_note("wglDeleteContext(HGLRC 0x%08x) -> TRUE\n", (unsigned)hglrc);
    c->eax = 1u;
    R(c, 4u);
}

/* `BOOL WINAPI wglMakeCurrent(HDC, HGLRC)` -- stdcall, 8B.
 * TRUE: the game treats a "current context" as the precondition for later GL calls, and FALSE makes it retry
 * or even abandon the render path. When a real rejection is needed (no backend), the R1 resolver returning 0 expresses it. */
static void ga_wglMakeCurrent(CPU *__restrict c)
{
    uint32_t hdc = A(c, 0);
    uint32_t hglrc = A(c, 1);

    ga_note("wglMakeCurrent(HDC 0x%08x, HGLRC 0x%08x) -> TRUE\n",
            (unsigned)hdc, (unsigned)hglrc);
    c->eax = 1u;
    R(c, 8u);
}

/* `BOOL WINAPI wglShareLists(HGLRC, HGLRC)` -- stdcall, 8B. TRUE. */
static void ga_wglShareLists(CPU *__restrict c)
{
    uint32_t a = A(c, 0);
    uint32_t b = A(c, 1);

    ga_note("wglShareLists(0x%08x, 0x%08x) -> TRUE\n",
            (unsigned)a, (unsigned)b);
    c->eax = 1u;
    R(c, 8u);
}

/* Optional seam: if the integrator defines this weak symbol, wglGetProcAddress takes its token
 * (for example a tagged gl_bridge token 0x7e..., dispatched by guest_host_dynamic).
 * When it is not defined the weak symbol is NULL and we honestly answer 0. */
extern uint32_t host_gl_proc_resolve(const char *name) __attribute__((weak));

/* Deduplicating log of the names requested from wglGetProcAddress: each distinct name printed once, at most 24. */
#define GA_PROC_LOG 24u
#define GA_PROC_NAME_MAX 32u
static char     g_ga_proc_names[GA_PROC_LOG][GA_PROC_NAME_MAX];
static unsigned g_ga_proc_names_n;

static void ga_proc_note(const char *name, uint32_t token)
{
    unsigned i;

    for (i = 0u; i < g_ga_proc_names_n; ++i) {
        if (strcmp(g_ga_proc_names[i], name) == 0)
            return;                       /* do not reprint the same name */
    }
    if (g_ga_proc_names_n < GA_PROC_LOG) {
        size_t n = strlen(name);
        if (n >= GA_PROC_NAME_MAX)
            n = GA_PROC_NAME_MAX - 1u;
        memcpy(g_ga_proc_names[g_ga_proc_names_n], name, n);
        g_ga_proc_names[g_ga_proc_names_n][n] = '\0';
        ++g_ga_proc_names_n;
    }
    ga_note("wglGetProcAddress(\"%s\") -> 0x%08x\n",
            name, (unsigned)token);
}

/* `PROC WINAPI wglGetProcAddress(LPCSTR)` -- stdcall, 4B.
 *
 * This is the **only entry point** of the whole GL surface: the game (a libepoxy-style loader) resolves
 * every glClear/glTexImage2D/... entry here. There is no GL backend yet, so:
 *   - with the host_gl_proc_resolve weak symbol → use its token (a tagged token in the low 4GB space,
 *     dispatched by guest_host_dynamic; returning a raw host function pointer would make the guest call it as
 *     an x86 address and it would crash for sure)
 *   - otherwise return 0, and **name-record** which functions were requested
 * Returning 0 is honest: the loader stores 0 and the first real draw raises an attributable
 * "indirect call to untranslated address" on the guest side. Any non-zero forged value just pushes the crash elsewhere.
 * Guessing case variants is pointless: the guest always passes a compile-time string. */
static void ga_wglGetProcAddress(CPU *__restrict c)
{
    uint32_t name_ptr = A(c, 0);
    char name[64];
    uint32_t token = 0u;

    if (ga_copy_ascii(name_ptr, name, sizeof name) && name[0]) {
        if (host_gl_proc_resolve)
            token = host_gl_proc_resolve(name);
        ga_proc_note(name, token);
    } else {
        static int noted;
        if (!noted) {
            noted = 1;
            ga_note("wglGetProcAddress(NULL/non-ASCII name 0x%08x) -> 0\n",
                    (unsigned)name_ptr);
        }
    }
    c->eax = token;
    R(c, 4u);
}

/* ==========================================================================
 * 6. OpenAL32 (24) -- bookkeeping implementation
 *
 * Convention: all cdecl (gpop only). See the evidence in header section 2.
 * Goal: handles never NULL, IDs strictly increasing, error-latch semantics matching real OpenAL,
 * but **no audio**. No al* call errors out unless the call itself is obviously illegal (NULL out param etc.).
 * ========================================================================== */

/* AL constants (al.h) */
#define GA_AL_NO_ERROR          0x0000u
#define GA_AL_INVALID_NAME      0xA001u
#define GA_AL_INVALID_VALUE     0xA003u

#define GA_AL_SOURCE_STATE      0x1010u
#define GA_AL_INITIAL           0x1011u
#define GA_AL_PLAYING           0x1012u
#define GA_AL_PAUSED            0x1013u
#define GA_AL_STOPPED           0x1014u
#define GA_AL_BUFFERS_PROCESSED 0x1015u
#define GA_AL_BUFFERS_QUEUED    0x1016u
#define GA_AL_SOURCE_TYPE       0x1027u
#define GA_AL_UNDETERMINED      0x1030u

#define GA_AL_TRUE              1u

/* ALC object block: low 4GB + magic check. 64 bytes is enough, and leaves room to store a real backend pointer later. */
#define GA_ALC_DEVICE_MAGIC  0x414C4344u
#define GA_ALC_CONTEXT_MAGIC 0x414C4343u
#define GA_ALC_BLOCK_BYTES   64u

/* ALC object header: magic at 0, kind at 4, sequence number at 8. */
static uint32_t g_ga_alc_dev_block;
static uint32_t g_ga_alc_ctx_block;
static uint32_t g_ga_alc_current;
static unsigned g_ga_alc_dev_seq;
static unsigned g_ga_alc_ctx_seq;

/* Degraded handle used when allocation fails: a tag integer, non-NULL, never confused with a real pointer. */
#define GA_ALC_DEVICE_FALLBACK  0x00A1D001u
#define GA_ALC_CONTEXT_FALLBACK 0x00A1C001u

/* Identity first, then validate: **first** compare against the very block we handed out, then dereference.
 * A range check alone is not enough -- the guest may pass a random 0x3Cxxxxxx value, and in that range
 * only pages the bump allocator really mapped are readable; a bare ld32 would SEGV.
 * The degraded tag handle (used when allocation failed) lies outside the low-4GB range with no memory to inspect,
 * but it is still "ours", so only the identity comparison is done and nothing is dereferenced. */
static int ga_alc_valid(uint32_t block, uint32_t magic)
{
    uint32_t known = (magic == GA_ALC_DEVICE_MAGIC)
                     ? g_ga_alc_dev_block : g_ga_alc_ctx_block;

    if (!block || block != known)
        return 0;                  /* not ours: never dereference */
    if (block < GA_LOW_BASE || block >= GA_LOW_END)
        return 1;                  /* degraded tag handle: considered live, no memory to inspect */
    if ((block & 7u) != 0u)
        return 0;                  /* should not happen: the bump allocator is 16-byte aligned */
    return ld32(block) == magic;    /* magic is 0 once destroyed */
}

/* Destroy: only blocks that really lie in the low-4GB range have memory whose magic can be cleared. */
static void ga_alc_invalidate(uint32_t block)
{
    if (block >= GA_LOW_BASE && block < GA_LOW_END && (block & 7u) == 0u)
        st32(block, 0u);
}

/* ---------------- source / buffer bookkeeping ---------------- */

#define GA_AL_MAX_SOURCES 256u
#define GA_AL_RING        16u

typedef struct {
    uint32_t id;          /* 0 = empty slot */
    uint16_t state;       /* AL_INITIAL / AL_PLAYING / AL_PAUSED / AL_STOPPED */
    uint16_t queued;      /* number of queued buffers */
    uint16_t ring_head;
    uint16_t ring_count;  /* how many ids are actually stored in the ring (<= GA_AL_RING) */
    uint32_t ring[GA_AL_RING];
} ga_al_source;

static ga_al_source g_ga_sources[GA_AL_MAX_SOURCES];

/* Source IDs increase monotonically from 1; buffer IDs start at 0x00010001 (two separate namespaces,
 * easy to tell apart in the logs, and neither is ever AL_NONE=0). */
static uint32_t g_ga_source_next;
static uint32_t g_ga_buffer_next  = 0x00010001u;
static uint32_t g_ga_buffers_made;
static uint64_t g_ga_audio_bytes;

/* AL errors are a "read once" latch: multiple errors keep only the first, and alGetError clears it. */
static uint32_t g_ga_al_error;

static void ga_al_set_error(uint32_t error)
{
    if (g_ga_al_error == GA_AL_NO_ERROR)
        g_ga_al_error = error;
}

static ga_al_source *ga_al_slot(uint32_t id)
{
    ga_al_source *s;
    unsigned slot;

    if (id == 0u)
        return NULL;
    slot = (unsigned)((id - 1u) % GA_AL_MAX_SOURCES);
    s = &g_ga_sources[slot];
    if (s->id != id)
        return NULL;    /* never issued, or the slot was reused by a younger source */
    return s;
}

/* ---------------- device / context (6 entries) ---------------- */

/* `ALCdevice *alcOpenDevice(const ALCchar*)` -- cdecl.
 * Non-NULL: the game treats NULL as "no audio device" and skips the whole Sound::Manager
 * initialization. Low-4GB block + magic, verifiable when it comes back. */
static void ga_alcOpenDevice(CPU *__restrict c)
{
    uint32_t name = A(c, 0);
    uint32_t device;

    (void)name;                    /* device name string not read: nothing to pick without a backend */
    if (!g_ga_alc_dev_block) {
        g_ga_alc_dev_block = ga_low_alloc(GA_ALC_BLOCK_BYTES);
        if (g_ga_alc_dev_block) {
            st32(g_ga_alc_dev_block, GA_ALC_DEVICE_MAGIC);
            st32(g_ga_alc_dev_block + 4u, 1u);        /* kind = device */
            st32(g_ga_alc_dev_block + 8u, ++g_ga_alc_dev_seq);
        } else {
            /* Even if the allocator fails, do not return NULL -- degrade to a tag handle and leave a trace. */
            g_ga_alc_dev_block = GA_ALC_DEVICE_FALLBACK;
        }
        ga_note("alcOpenDevice -> ALCdevice 0x%08x (%s)\n",
                (unsigned)g_ga_alc_dev_block,
                g_ga_alc_dev_block == GA_ALC_DEVICE_FALLBACK
                    ? "degrading to a tag handle" : "low-4GB object");
    }
    device = g_ga_alc_dev_block;
    c->eax = device;
    cdecl_ret(c);
}

/* `ALCcontext *alcCreateContext(ALCdevice*, const ALCint*)` -- cdecl.
 * Non-NULL for the same reason: NULL would make the game think "context creation failed" and give up on audio. */
static void ga_alcCreateContext(CPU *__restrict c)
{
    uint32_t device = A(c, 0);
    uint32_t attrlist = A(c, 1);
    uint32_t context;

    (void)attrlist;                /* sample rate/channel count attributes: no backend, ignored */
    if (!g_ga_alc_ctx_block) {
        g_ga_alc_ctx_block = ga_low_alloc(GA_ALC_BLOCK_BYTES);
        if (g_ga_alc_ctx_block) {
            st32(g_ga_alc_ctx_block, GA_ALC_CONTEXT_MAGIC);
            st32(g_ga_alc_ctx_block + 4u, 2u);        /* kind = context */
            st32(g_ga_alc_ctx_block + 8u, ++g_ga_alc_ctx_seq);
        } else {
            g_ga_alc_ctx_block = GA_ALC_CONTEXT_FALLBACK;
        }
        ga_note("alcCreateContext(device 0x%08x) -> ALCcontext 0x%08x\n",
                (unsigned)device, (unsigned)g_ga_alc_ctx_block);
    }
    context = g_ga_alc_ctx_block;
    c->eax = context;
    cdecl_ret(c);
}

/* `ALCboolean alcMakeContextCurrent(ALCcontext*)` -- cdecl.
 * Returns ALC_TRUE(1). Per the spec, passing NULL means "release the current context" and also returns TRUE,
 * so this is always TRUE and only changes the current value we keep ourselves. */
static void ga_alcMakeContextCurrent(CPU *__restrict c)
{
    uint32_t context = A(c, 0);

    if (context && !ga_alc_valid(context, GA_ALC_CONTEXT_MAGIC)) {
        static int noted;
        if (!noted) {
            noted = 1;
            ga_note("alcMakeContextCurrent received a context from another layer 0x%08x (accepting anyway)\n",
                    (unsigned)context);
        }
    }
    g_ga_alc_current = context;
    c->eax = GA_AL_TRUE;
    cdecl_ret(c);
}

/* `void alcDestroyContext(ALCcontext*)` -- cdecl. Only invalidate the magic,
 * no munmap: this layer is not worth returning low-4GB range for single-digit objects. */
static void ga_alcDestroyContext(CPU *__restrict c)
{
    uint32_t context = A(c, 0);

    if (ga_alc_valid(context, GA_ALC_CONTEXT_MAGIC)) {
        ga_alc_invalidate(context); /* magic cleared = destroyed */
        g_ga_alc_ctx_block = 0u;    /* release the "a context was issued" slot */
    }
    if (g_ga_alc_current == context)
        g_ga_alc_current = 0u;
    ga_note("alcDestroyContext(0x%08x)\n", (unsigned)context);
    cdecl_ret(c);
}

/* `ALCboolean alcCloseDevice(ALCdevice*)` -- cdecl. Returns TRUE:
 * closing one of our own devices always succeeds; failing would stall the game on its shutdown path. */
static void ga_alcCloseDevice(CPU *__restrict c)
{
    uint32_t device = A(c, 0);

    if (ga_alc_valid(device, GA_ALC_DEVICE_MAGIC)) {
        ga_alc_invalidate(device);
        g_ga_alc_dev_block = 0u;    /* release the slot; the next alcOpenDevice allocates again */
    }
    ga_note("alcCloseDevice(0x%08x) -> TRUE\n", (unsigned)device);
    c->eax = GA_AL_TRUE;
    cdecl_ret(c);
}

/* `void alcProcessContext(ALCcontext*)` -- cdecl. A no-op is correct:
 * our "context" needs no explicit commit and has no notion of being suspended anyway. */
static void ga_alcProcessContext(CPU *__restrict c)
{
    (void)A(c, 0);
    cdecl_ret(c);
}

/* ---------------- source / buffer generation and deletion (5 entries) ---------------- */

/* `void alGenSources(ALsizei n, ALuint *sources)` -- cdecl.
 * Writes n **monotonically increasing, non-zero** IDs: 0 is AL_NONE, and the game would treat it as "no source"
 * and skip the following alSourcei/alSourcePlay, which silently turns audio off. */
static void ga_alGenSources(CPU *__restrict c)
{
    int32_t n = (int32_t)A(c, 0);
    uint32_t out = A(c, 1);
    int32_t i;

    if (n < 0 || (n > 0 && !out)) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        ga_note("alGenSources(n=%d, out=0x%08x) -> AL_INVALID_VALUE\n",
                (int)n, (unsigned)out);
        cdecl_ret(c);
        return;
    }
    for (i = 0; i < n; ++i) {
        uint32_t id = ++g_ga_source_next;
        ga_al_source *s =
            &g_ga_sources[(unsigned)((id - 1u) % GA_AL_MAX_SOURCES)];

        if (s->id != 0u && s->id != id) {
            static int noted;
            if (!noted) {
                noted = 1;
                ga_note("alGenSources exceeded %u active sources; reusing the oldest slot\n",
                        (unsigned)GA_AL_MAX_SOURCES);
            }
        }
        s->id = id;
        s->state = (uint16_t)GA_AL_INITIAL;
        s->queued = 0u;
        s->ring_head = 0u;
        s->ring_count = 0u;
        st32(out + (uint32_t)i * 4u, id);
    }
    if (n > 0)
        ga_note("alGenSources(%d) -> newest ID %u\n", (int)n,
                (unsigned)g_ga_source_next);
    cdecl_ret(c);
}

/* `void alGenBuffers(ALsizei n, ALuint *buffers)` -- cdecl.
 * Same as alGenSources: non-zero, monotonically increasing IDs, in a separate 0x00010001 range to keep them apart. */
static void ga_alGenBuffers(CPU *__restrict c)
{
    int32_t n = (int32_t)A(c, 0);
    uint32_t out = A(c, 1);
    int32_t i;

    if (n < 0 || (n > 0 && !out)) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        ga_note("alGenBuffers(n=%d, out=0x%08x) -> AL_INVALID_VALUE\n",
                (int)n, (unsigned)out);
        cdecl_ret(c);
        return;
    }
    for (i = 0; i < n; ++i) {
        uint32_t id = g_ga_buffer_next++;
        st32(out + (uint32_t)i * 4u, id);
        ++g_ga_buffers_made;
    }
    if (n > 0)
        ga_note("alGenBuffers(%d) -> next ID 0x%08x (%u issued total)\n",
                (int)n, (unsigned)g_ga_buffer_next,
                (unsigned)g_ga_buffers_made);
    cdecl_ret(c);
}

/* `void alDeleteSources(ALsizei n, const ALuint *sources)` -- cdecl.
 * Accepts only IDs issued by this layer; on an unfamiliar ID it sets AL_INVALID_NAME per real OpenAL semantics and
 * stops immediately. This is one of the few rejecting paths: it exists to expose wiring mistakes, not to hide them. */
static void ga_alDeleteSources(CPU *__restrict c)
{
    int32_t n = (int32_t)A(c, 0);
    uint32_t list = A(c, 1);
    int32_t i;

    if (n < 0 || (n > 0 && !list)) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        cdecl_ret(c);
        return;
    }
    for (i = 0; i < n; ++i) {
        uint32_t id = ld32(list + (uint32_t)i * 4u);
        ga_al_source *s = ga_al_slot(id);

        if (!s) {
            ga_al_set_error(GA_AL_INVALID_NAME);
            ga_note("alDeleteSources: unknown source ID %u -> AL_INVALID_NAME\n",
                    (unsigned)id);
            break;
        }
        s->id = 0u;                /* slot released; queue state cleared with it */
        s->queued = 0u;
        s->ring_count = 0u;
        s->ring_head = 0u;
    }
    cdecl_ret(c);
}

/* `void alDeleteBuffers(ALsizei n, const ALuint *buffers)` -- cdecl.
 * Buffers carry no per-item state (streaming music creates thousands), so only the "issued ID range" is checked:
 * an ID never issued counts as AL_INVALID_NAME. Deleting twice does not error (harmless, see R3). */
static void ga_alDeleteBuffers(CPU *__restrict c)
{
    int32_t n = (int32_t)A(c, 0);
    uint32_t list = A(c, 1);
    int32_t i;

    if (n < 0 || (n > 0 && !list)) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        cdecl_ret(c);
        return;
    }
    for (i = 0; i < n; ++i) {
        uint32_t id = ld32(list + (uint32_t)i * 4u);

        if (id < 0x00010001u || id >= g_ga_buffer_next) {
            ga_al_set_error(GA_AL_INVALID_NAME);
            ga_note("alDeleteBuffers: unknown buffer ID 0x%08x -> AL_INVALID_NAME\n",
                    (unsigned)id);
            break;
        }
    }
    cdecl_ret(c);
}

/* ---------------- playback control (4 entries) ---------------- */

/* `void alSourcePlay(ALuint)` -- cdecl. Sets the state to AL_PLAYING:
 * the game polls AL_SOURCE_STATE, and a permanent AL_INITIAL would make it retrigger playback over and over. */
static void ga_alSourcePlay(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    ga_al_source *s = ga_al_slot(id);

    if (s)
        s->state = (uint16_t)GA_AL_PLAYING;
    else
        ga_al_set_error(GA_AL_INVALID_NAME);
    cdecl_ret(c);
}

/* `void alSourceStop(ALuint)` -- cdecl. → AL_STOPPED (can be played again). */
static void ga_alSourceStop(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    ga_al_source *s = ga_al_slot(id);

    if (s)
        s->state = (uint16_t)GA_AL_STOPPED;
    else
        ga_al_set_error(GA_AL_INVALID_NAME);
    cdecl_ret(c);
}

/* `void alSourcePause(ALuint)` -- cdecl. Only PLAYING turns into PAUSED;
 * other states are left as they are (matching real OpenAL's no-op pause on a non-playing source). */
static void ga_alSourcePause(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    ga_al_source *s = ga_al_slot(id);

    if (s) {
        if (s->state == (uint16_t)GA_AL_PLAYING)
            s->state = (uint16_t)GA_AL_PAUSED;
    } else {
        ga_al_set_error(GA_AL_INVALID_NAME);
    }
    cdecl_ret(c);
}

/* `void alSourceQueueBuffers(ALuint, ALsizei, const ALuint*)` -- cdecl.
 * Real bookkeeping: each source's queued IDs go into a ring buffer (depth 16); alGetSourcei(QUEUED) and
 * alSourceUnqueueBuffers both read this record. Beyond the ring depth only the counter grows and no ID is stored. */
static void ga_alSourceQueueBuffers(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    int32_t n = (int32_t)A(c, 1);
    uint32_t list = A(c, 2);
    ga_al_source *s = ga_al_slot(id);
    int32_t i;

    if (!s) {
        ga_al_set_error(GA_AL_INVALID_NAME);
        cdecl_ret(c);
        return;
    }
    if (n < 0 || (n > 0 && !list)) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        cdecl_ret(c);
        return;
    }
    for (i = 0; i < n; ++i) {
        uint32_t buf = ld32(list + (uint32_t)i * 4u);
        unsigned tail =
            (unsigned)((s->ring_head + s->ring_count) % GA_AL_RING);

        if (s->ring_count < (uint16_t)GA_AL_RING) {
            s->ring[tail] = buf;
            ++s->ring_count;
        }
        if (s->queued < 0xFFFFu)
            ++s->queued;
    }
    cdecl_ret(c);
}

/* `void alSourceUnqueueBuffers(ALuint, ALsizei, ALuint*)` -- cdecl.
 *
 * This is our only semantic divergence from real OpenAL, see R3: since nothing really plays,
 * AL_BUFFERS_PROCESSED is always 0, so the game **never** gets here. If it calls anyway,
 * we return IDs from the ring head in FIFO order (padding with 0), guaranteeing the out params always have defined values
 * and the guest never reads uninitialized memory -- better to "return one buffer too many" than let the guest read garbage. */
static void ga_alSourceUnqueueBuffers(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    int32_t n = (int32_t)A(c, 1);
    uint32_t out = A(c, 2);
    ga_al_source *s = ga_al_slot(id);
    int32_t i;

    if (!s) {
        ga_al_set_error(GA_AL_INVALID_NAME);
        cdecl_ret(c);
        return;
    }
    if (n < 0 || (n > 0 && !out)) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        cdecl_ret(c);
        return;
    }
    for (i = 0; i < n; ++i) {
        uint32_t buf = 0u;

        if (s->ring_count > 0u) {
            buf = s->ring[s->ring_head];
            s->ring_head = (uint16_t)((s->ring_head + 1u) % GA_AL_RING);
            --s->ring_count;
        }
        if (s->queued > 0u)
            --s->queued;
        st32(out + (uint32_t)i * 4u, buf);
    }
    cdecl_ret(c);
}

/* ---------------- queries (4 entries) ---------------- */

/* `ALenum alGetError(void)` -- cdecl. Returns the latched error (default AL_NO_ERROR=0)
 * and clears it. Real OpenAL has one latch per thread; this runtime executes synchronously on a single guest thread,
 * so one static variable is enough. */
static void ga_alGetError(CPU *__restrict c)
{
    uint32_t error = g_ga_al_error;

    g_ga_al_error = GA_AL_NO_ERROR;
    c->eax = error;
    cdecl_ret(c);
}

/* `void alGetSourcei(ALuint, ALenum, ALint*)` -- cdecl.
 * Three params have real values: SOURCE_STATE / BUFFERS_QUEUED / SOURCE_TYPE;
 * everything else is 0 -- 0 is a legal value for these enums, so the game never reads garbage.
 * AL_BUFFERS_PROCESSED is deliberately always 0: lying that playback finished would make the music stream
 * refill buffers forever at full CPU speed (busy loop). See header R3. */
static void ga_alGetSourcei(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    uint32_t param = A(c, 1);
    uint32_t out = A(c, 2);
    ga_al_source *s;

    if (!out) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        cdecl_ret(c);
        return;
    }
    s = ga_al_slot(id);
    if (!s) {
        ga_al_set_error(GA_AL_INVALID_NAME);
        st32(out, 0u);             /* still write a defined value so the guest does not read garbage */
        cdecl_ret(c);
        return;
    }
    switch (param) {
    case GA_AL_SOURCE_STATE:      st32(out, (uint32_t)s->state); break;
    case GA_AL_BUFFERS_QUEUED:    st32(out, (uint32_t)s->queued); break;
    case GA_AL_BUFFERS_PROCESSED: st32(out, 0u); break;
    /* Queued buffers mean a streaming source; none means UNDETERMINED. Neither affects playback
     * semantics; it just gives the game a self-consistent answer. */
    case GA_AL_SOURCE_TYPE:       st32(out, GA_AL_UNDETERMINED); break;
    default:                      st32(out, 0u); break;
    }
    cdecl_ret(c);
}

/* `void alGetSourcef(ALuint, ALenum, ALfloat*)` -- cdecl.
 * Writes 0.0f everywhere: with no mixer there is no gain/offset/seconds to report.
 * Note that 0.0f means "silence/zero pitch" for AL_GAIN/AL_PITCH, but this layer makes no sound anyway;
 * if a real backend is wired later this function must read real values, or volume control will always read 0. */
static void ga_alGetSourcef(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    uint32_t out;

    (void)A(c, 1);                 /* param: nothing to query, always 0.0f */
    out = A(c, 2);
    if (!out) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        cdecl_ret(c);
        return;
    }
    if (!ga_al_slot(id))
        ga_al_set_error(GA_AL_INVALID_NAME);
    stf(out, 0.0f);
    cdecl_ret(c);
}

/* ---------------- parameter setting (5 entries) ---------------- */

/* `void alSourcei(ALuint, ALenum, ALint)` -- cdecl. Accept but do not record:
 * with no mixer these parameters have nowhere to go, but they are not errors, so an error code is set only for an invalid source. */
static void ga_alSourcei(CPU *__restrict c)
{
    uint32_t id = A(c, 0);

    (void)A(c, 1);
    (void)A(c, 2);
    if (!ga_al_slot(id))
        ga_al_set_error(GA_AL_INVALID_NAME);
    cdecl_ret(c);
}

/* `void alSourcef(ALuint, ALenum, ALfloat)` -- cdecl.
 * A float argument occupies 4 bytes on the x86 stack; we do not read it here (no mixer), but we **must not misalign**:
 * under cdecl the argument count is cleaned by the guest's own add esp,N. */
static void ga_alSourcef(CPU *__restrict c)
{
    uint32_t id = A(c, 0);

    (void)A(c, 1);
    (void)A(c, 2);
    if (!ga_al_slot(id))
        ga_al_set_error(GA_AL_INVALID_NAME);
    cdecl_ret(c);
}

/* `void alSource3f(ALuint, ALenum, ALfloat, ALfloat, ALfloat)` -- cdecl. */
static void ga_alSource3f(CPU *__restrict c)
{
    uint32_t id = A(c, 0);

    (void)A(c, 1);
    (void)A(c, 2);
    (void)A(c, 3);
    (void)A(c, 4);
    if (!ga_al_slot(id))
        ga_al_set_error(GA_AL_INVALID_NAME);
    cdecl_ret(c);
}

/* `void alListener3f(ALenum, ALfloat, ALfloat, ALfloat)` -- cdecl.
 * Listener position/velocity: no backend, so just accept it (no error code, to keep the game out of its error branch). */
static void ga_alListener3f(CPU *__restrict c)
{
    (void)A(c, 0);
    (void)A(c, 1);
    (void)A(c, 2);
    (void)A(c, 3);
    cdecl_ret(c);
}

/* `void alListenerfv(ALenum, const ALfloat*)` -- cdecl.
 * The only check is that the pointer is not NULL (real OpenAL sets AL_INVALID_VALUE).
 * We never dereference it (nothing sounds anyway), so we do not even read it; that is safe. */
static void ga_alListenerfv(CPU *__restrict c)
{
    uint32_t values;

    (void)A(c, 0);
    values = A(c, 1);
    if (!values)
        ga_al_set_error(GA_AL_INVALID_VALUE);
    cdecl_ret(c);
}

/* `void alBufferData(ALuint, ALenum, const ALvoid*, ALsizei, ALsizei)` -- cdecl.
 * Real bookkeeping: accumulate uploaded bytes, to be checked when a real backend is wired (the Vita version uploads this way).
 * Audio data is **not copied** here: with no mixer reading it, copying only wastes memory bandwidth.
 * Obviously illegal calls (size>0 but data=NULL, or a never-issued buffer ID) only set an error code. */
static void ga_alBufferData(CPU *__restrict c)
{
    uint32_t buffer = A(c, 0);
    uint32_t format = A(c, 1);
    uint32_t data = A(c, 2);
    int32_t size = (int32_t)A(c, 3);
    int32_t freq = (int32_t)A(c, 4);

    if (buffer < 0x00010001u || buffer >= g_ga_buffer_next) {
        ga_al_set_error(GA_AL_INVALID_NAME);
        ga_note("alBufferData: unknown buffer 0x%08x -> AL_INVALID_NAME\n",
                (unsigned)buffer);
        cdecl_ret(c);
        return;
    }
    if (size < 0 || (size > 0 && !data)) {
        ga_al_set_error(GA_AL_INVALID_VALUE);
        ga_note("alBufferData(buffer 0x%08x, size %d, data 0x%08x)"
                " -> AL_INVALID_VALUE\n",
                (unsigned)buffer, (int)size, (unsigned)data);
        cdecl_ret(c);
        return;
    }
    g_ga_audio_bytes += (uint64_t)(uint32_t)size;
    {
        /* Log the shape of only the first 16 uploads: streaming music calls this thousands of times. */
        static unsigned noted;
        if (noted < 16u) {
            ++noted;
            ga_note("alBufferData(buffer 0x%08x, format 0x%x, %d bytes, %d Hz)"
                    " %u bytes total\n",
                    (unsigned)buffer, (unsigned)format, (int)size, (int)freq,
                    (unsigned)g_ga_audio_bytes);
        }
    }
    cdecl_ret(c);
}

/* ==========================================================================
 * 7. Dispatch tables + external interface
 * ========================================================================== */

typedef struct {
    const char *name;
    void (*fn)(CPU *__restrict);
} ga_entry;

static const ga_entry GA_GL_TABLE[] = {
    /* GDI32 (6) */
    { "GDI32.dll!SetPixelFormat",      ga_SetPixelFormat },
    { "GDI32.dll!GetDeviceCaps",       ga_GetDeviceCaps },
    { "GDI32.dll!DeleteDC",            ga_DeleteDC },
    { "GDI32.dll!DescribePixelFormat", ga_DescribePixelFormat },
    { "GDI32.dll!SwapBuffers",         ga_SwapBuffers },
    { "GDI32.dll!CreateDCA",           ga_CreateDCA },
    /* OPENGL32 (5) -- all WGL, no gl* draw imports (see header section 1) */
    { "OPENGL32.dll!wglDeleteContext",  ga_wglDeleteContext },
    { "OPENGL32.dll!wglCreateContext",  ga_wglCreateContext },
    { "OPENGL32.dll!wglShareLists",     ga_wglShareLists },
    { "OPENGL32.dll!wglGetProcAddress", ga_wglGetProcAddress },
    { "OPENGL32.dll!wglMakeCurrent",    ga_wglMakeCurrent },
};

static const ga_entry GA_AUDIO_TABLE[] = {
    /* OpenAL32 (24) -- ordered per the IAT order in guest_table.c */
    { "OpenAL32.dll!alSourcePlay",           ga_alSourcePlay },
    { "OpenAL32.dll!alGetSourcei",           ga_alGetSourcei },
    { "OpenAL32.dll!alSourceStop",           ga_alSourceStop },
    { "OpenAL32.dll!alGenBuffers",           ga_alGenBuffers },
    { "OpenAL32.dll!alSourceQueueBuffers",   ga_alSourceQueueBuffers },
    { "OpenAL32.dll!alSourcePause",          ga_alSourcePause },
    { "OpenAL32.dll!alSourceUnqueueBuffers", ga_alSourceUnqueueBuffers },
    { "OpenAL32.dll!alcMakeContextCurrent",  ga_alcMakeContextCurrent },
    { "OpenAL32.dll!alcDestroyContext",      ga_alcDestroyContext },
    { "OpenAL32.dll!alcOpenDevice",          ga_alcOpenDevice },
    { "OpenAL32.dll!alcCreateContext",       ga_alcCreateContext },
    { "OpenAL32.dll!alcCloseDevice",         ga_alcCloseDevice },
    { "OpenAL32.dll!alListener3f",           ga_alListener3f },
    { "OpenAL32.dll!alSourcei",              ga_alSourcei },
    { "OpenAL32.dll!alcProcessContext",      ga_alcProcessContext },
    { "OpenAL32.dll!alGetSourcef",           ga_alGetSourcef },
    { "OpenAL32.dll!alGenSources",           ga_alGenSources },
    { "OpenAL32.dll!alGetError",             ga_alGetError },
    { "OpenAL32.dll!alDeleteBuffers",        ga_alDeleteBuffers },
    { "OpenAL32.dll!alListenerfv",           ga_alListenerfv },
    { "OpenAL32.dll!alDeleteSources",        ga_alDeleteSources },
    { "OpenAL32.dll!alSource3f",             ga_alSource3f },
    { "OpenAL32.dll!alSourcef",              ga_alSourcef },
    { "OpenAL32.dll!alBufferData",           ga_alBufferData },
};

_Static_assert(sizeof GA_GL_TABLE / sizeof GA_GL_TABLE[0] == 11u,
               "GDI32(6)+OPENGL32(5) import count should be 11");
_Static_assert(sizeof GA_AUDIO_TABLE / sizeof GA_AUDIO_TABLE[0] == 24u,
               "OpenAL32 import count should be 24");

/* Exact strcmp, the same convention as host_android.c: names come from the compile-time import table,
 * so no case-variant guessing is needed (a wrong guess silently reaches the wrong implementation). */
int host_gl_dispatch(CPU *__restrict c, const char *name)
{
    size_t i;

    if (!c || !name)
        return 0;
    for (i = 0u; i < sizeof GA_GL_TABLE / sizeof GA_GL_TABLE[0]; ++i) {
        if (strcmp(GA_GL_TABLE[i].name, name) == 0) {
            GA_GL_TABLE[i].fn(c);
            return 1;
        }
    }
    return 0;
}

int host_gl_count(void)
{
    return (int)(sizeof GA_GL_TABLE / sizeof GA_GL_TABLE[0]);
}

int host_audio_dispatch(CPU *__restrict c, const char *name)
{
    size_t i;

    if (!c || !name)
        return 0;

    /* ★ 2026-11-07: first forward OpenAL32.dll imports to the real OpenAL Soft
     * (host_openal.c). The old stubs only ever did bookkeeping and never made a sound.
     *
     * Returning 0 means "real OpenAL is unavailable or does not know this name"; then fall through to the
     * old stubs below -- on failure the behavior is exactly as before and no new crash point is introduced. */
    {
        extern int host_openal_try(CPU *__restrict, const char *) __attribute__((weak));
        if (host_openal_try && host_openal_try(c, name))
            return 1;
    }
    for (i = 0u; i < sizeof GA_AUDIO_TABLE / sizeof GA_AUDIO_TABLE[0]; ++i) {
        if (strcmp(GA_AUDIO_TABLE[i].name, name) == 0) {
            GA_AUDIO_TABLE[i].fn(c);
            return 1;
        }
    }
    return 0;
}

int host_audio_count(void)
{
    return (int)(sizeof GA_AUDIO_TABLE / sizeof GA_AUDIO_TABLE[0]);
}
