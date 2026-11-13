/* host_user32.c -- USER32.dll implementation for the Android host layer (all 59 static imports)
 *
 * Background
 * ----
 * The game is a PC x86 PE32 image, statically translated to C by repentogxm and then built for arm64. guest pointers are
 * uint32_t, so every address handed to the guest must fall in the low 4GB; this file uses only three kinds of address:
 *   1) buffers the guest passes in itself (we only write them; the address stays 32-bit as it is);
 *   2) low-4GB blocks we mmap ourselves near 0x3B000000 (see u_low_alloc);
 *   3) small integer "handle tokens" (HWND/HDC/HICON/...), which the guest only passes back as opaque values.
 *
 * Calling convention
 * --------
 * All 59 symbols are WINAPI = stdcall: the caller pushes the arguments and the callee cleans the stack with ret N.
 *   uA(c, i)  = the i-th argument (c->esp points at the return address)
 *   uR(c, n)  = pop the return address and clean n bytes of arguments
 * The uR byte count at the end of each function is pinned to the real Win32 signature, and the signature is noted in the function header comment.
 * One wrong cleanup byte count unbalances the stack and crashes far away, so every number must check out against the signature.
 *
 * What this layer deliberately does not do
 * ------------------
 * It does not create an Android window / EGL surface. The goal is to keep the game running so we can see what it asks for next.
 * Therefore:
 *   · the window is a "virtual window": CreateWindowExA records a ledger entry (frame rect, style, wndproc),
 *     and GetClientRect / SetWindowPos / ShowWindow / GetWindowLongA all operate on that ledger,
 *     returning self-consistent real data instead of constant garbage.
 *   · no messages are ever produced: PeekMessageA returns 0 (no messages), and GetMessageA, as the task requires,
 *     returns 1 plus a filled WM_NULL (never 0, because 0 == WM_QUIT would end the message loop and
 *     the game would close its window), then sleeps RGXM_USER32_IDLE_US microseconds to avoid a 100% spin.
 *   · the guest WndProc is almost never called: we never synthesize messages that a window procedure must handle
 *     (the default handling of WM_NULL is "do nothing"), so DispatchMessageA simply returns 0.
 *
 * Three kinds of handling (each function header comment says which one applies)
 *   [real] really computes / really stores / really fills in structures
 *   [stub] returns a legal value that keeps the flow going, and explains why it cannot mislead the guest
 *   [fail] honestly returns failure (0 / -1), letting the caller take its error branch
 *
 * Naming: every static symbol in this file carries a u prefix. That way, if someone ever #includes this file into
 * host_android.c (which already has static A/R/low_alloc), no symbols collide. The semantics still match
 * the existing A/R exactly.
 *
 * Argument byte count table (for auditing):
 *   BringWindowToTop 1->4   SetCapture 1->4       SetCursor 1->4
 *   SetFocus 1->4           ClipCursor 1->4       TranslateMessage 1->4
 *   GetClientRect 2->8      PeekMessageA 5->20    ReleaseCapture 0->0
 *   SetForegroundWindow 1->4 ShowCursor 1->4      SetCursorPos 2->8
 *   GetCursorPos 1->4       DefWindowProcA 4->16  LoadIconA 2->8
 *   GetActiveWindow 0->0    MessageBoxA 4->16     GetWindowLongA 2->8
 *   LoadImageA 6->24        SendMessageA 4->16    SetWindowPos 7->28
 *   GetSystemMetrics 1->4   SetWindowLongA 3->12  OpenClipboard 1->4
 *   CloseClipboard 0->0     EmptyClipboard 0->0   GetClipboardData 1->4
 *   SetClipboardData 2->8   SendInput 3->12       GetMessageTime 0->0
 *   RegisterClassA 1->4     MapVirtualKeyA 2->8   TrackMouseEvent 1->4
 *   ClientToScreen 2->8     SystemParametersInfoA 4->16 GetDC 1->4
 *   ReleaseDC 2->8          EnumDisplayDevicesA 4->16
 *   EnumDisplaySettingsExA 4->16   ChangeDisplaySettingsExA 5->20
 *   EnumDisplaySettingsA 3->12     GetMessageA 4->16
 *   RegisterDeviceNotificationA 3->12   GetRawInputDeviceInfoA 4->16
 *   UnregisterClassA 2->8   UnregisterDeviceNotification 1->4
 *   CreateWindowExA 12->48  RegisterClassExA 1->4 GetRawInputDeviceList 3->12
 *   AdjustWindowRectEx 4->16 GetKeyState 1->4     DispatchMessageA 1->4
 *   GetMenu 1->4            LoadCursorA 2->8      DestroyWindow 1->4
 *   WindowFromPoint 8 bytes by value ->8          ShowWindow 2->8
 *   SetClassLongA 3->12     GetAsyncKeyState 1->4
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

#include "guest.h"

/* MAP_FIXED_NOREPLACE only appeared in Linux 4.17; older kernel headers lack this macro.
 * If it is missing, define it ourselves with the same value (0x100000), so this file still compiles on old platforms.
 * Note: if the kernel really does not support this flag, mmap returns EINVAL and u_low_alloc will
 * move forward and finally return 0 as it should -- it never silently clobbers a block someone else mapped. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* Yield time for GetMessageA when there are no messages (microseconds). 1ms => at most ~1000 loops/second.
 * Build with -DRGXM_USER32_IDLE_US=0 to turn it off and let the loop spin at full speed. */
#ifndef RGXM_USER32_IDLE_US
#define RGXM_USER32_IDLE_US 1000
#endif

/* ---------------------------------------------------------------- calling convention -- */

/* The i-th argument. stdcall: c->esp points at the return address, arguments start at esp+4. */
static uint32_t uA(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}

/* stdcall return: pop the return address and clean n bytes of arguments. */
static void uR(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ------------------------------------------------------------ pointer validity -- */

/* guest pointers are only guaranteed to be 32-bit; truncation, 0 and out-of-range must all be blocked, or a 4-byte write crashes.
 * We do not verify "is it mapped" here (that would need /proc/self/maps), but truncation and null pointers are the two
 * classes that have actually bitten us, so they must be blocked first. */
static int u_ptr_ok(uint32_t p, uint32_t n)
{
    if (p < 0x10000u) return 0;              /* null pointer and DOS page */
    if (n > 0xFFFFFFFFu - p) return 0;       /* 32-bit wraparound */
    if (p > 0xFFFF0000u) return 0;
    return 1;
}

static int u_w32(uint32_t p, uint32_t v)
{
    if (!u_ptr_ok(p, 4u)) return 0;
    st32(p, v);
    return 1;
}

/* Read a guest C string into a host buffer. Returns 1 when a complete NUL-terminated string was read.
 * Range is checked byte by byte: the pointer the caller gives may sit exactly at the end of a mapping. */
static int u_copy_str(uint32_t src, char *dst, size_t cap)
{
    size_t i;
    if (!dst || cap == 0u) return 0;
    dst[0] = '\0';
    if (!src) return 0;
    for (i = 0; i + 1u < cap; i++) {
        if (!u_ptr_ok(src + (uint32_t)i, 1u)) return 0;
        dst[i] = (char)ld8(src + (uint32_t)i);
        if (dst[i] == '\0') return 1;
    }
    dst[cap - 1u] = '\0';
    return 0;                                 /* no terminating NUL, or too long */
}

static uint32_t u_hash_name(const char *s)
{
    uint32_t h = 2166136261u;                 /* FNV-1a; all it needs is to be stable */
    while (*s) {
        h ^= (uint32_t)(unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static uint32_t u_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u +
                      (uint64_t)ts.tv_nsec / 1000000u);
}

/* ------------------------------------------------------- low-4GB memory allocator -- */

/* Address plan (see CONTEXT_FOR_AGENTS.md): image 0x3c000000, TIB/TLS 0x32000000,
 * stack 0x40000000, CRT heap 0x48000000, Lua heap 0x50000000.
 * We take 0x3B000000 -- between the top of the TIB area and the stack, and distinct from
 * host_android.c's 0x38000000, so the bump pointers of the two layers never step on each other.
 * MAP_FIXED_NOREPLACE: if this block is already mapped by someone else, mmap fails instead of overwriting,
 * so we move forward 64KB and retry; if everything fails we return 0 (the caller must handle 0; we never return a
 * high address and let the guest truncate it into garbage). */
static uint32_t g_low_bump = 0x3B000000u;

static uint32_t u_low_alloc(uint32_t n)
{
    uint32_t base;
    int i;
    if (n == 0u) return 0;
    n = (n + 15u) & ~15u;
    base = g_low_bump;
    for (i = 0; i < 64 && base < 0x3F000000u; i++) {
        void *p = mmap((void *)(uintptr_t)base, (size_t)n,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                       -1, 0);
        if (p == (void *)(uintptr_t)base) {
            g_low_bump = base + n;
            return base;
        }
        base += 0x10000u;
    }
    fprintf(stderr, "[user32] low_alloc(%u) failed: low-4GB region exhausted\n", n);
    return 0;
}

/* ------------------------------------------------------------ Win32 constants -- */

#define U_WM_NULL              0x0000u
#define U_WM_SETICON           0x0080u

#define U_WS_VISIBLE           0x10000000u
#define U_WS_CAPTION           0x00C00000u
#define U_WS_BORDER            0x00800000u
#define U_WS_THICKFRAME        0x00040000u
#define U_WS_EX_CLIENTEDGE     0x00000200u

#define U_SWP_NOSIZE           0x0001u
#define U_SWP_NOMOVE           0x0002u
#define U_SWP_SHOWWINDOW       0x0040u

#define U_SW_HIDE              0

#define U_GWL_WNDPROC         (-4)
#define U_GWL_HINSTANCE       (-6)
#define U_GWL_HWNDPARENT      (-8)
#define U_GWL_ID             (-12)
#define U_GWL_STYLE          (-16)
#define U_GWL_EXSTYLE        (-20)
#define U_GWL_USERDATA       (-21)    /* GWLP_USERDATA: GLFW stores the window pointer here */

#define U_GCL_CBCLSEXTRA     (-20)
#define U_GCL_CBWNDEXTRA     (-18)
#define U_GCL_HICONSM        (-34)
#define U_GCL_HICON          (-14)
#define U_GCL_HCURSOR        (-12)
#define U_GCL_HBRBACKGROUND  (-10)
#define U_GCL_MENUNAME        (-8)
#define U_GCL_WNDPROC        (-24)
#define U_GCL_STYLE          (-26)

#define U_SPI_GETSCREENSAVETIMEOUT    0x000Eu
#define U_SPI_SETSCREENSAVETIMEOUT    0x000Fu
#define U_SPI_GETSCREENSAVEACTIVE     0x0010u
#define U_SPI_SETSCREENSAVEACTIVE     0x0011u
#define U_SPI_GETWORKAREA             0x0030u
#define U_SPI_SETFOREGROUNDLOCKTIMEOUT 0x2001u
#define U_SPI_GETFOREGROUNDLOCKTIMEOUT 0x2000u

#define U_IDOK                 1
#define U_IDYES                6

#define U_CF_TEXT              1u
#define U_CF_BITMAP            2u
#define U_CF_UNICODETEXT      13u

#define U_IMAGE_BITMAP         0u
#define U_IMAGE_ICON           1u
#define U_IMAGE_CURSOR         2u

/* Virtual handle tokens: small integers, non-zero; the guest only passes them back to us unchanged.
 * They must never be host pointers -- those live at 0x5xxxxxxxxx and truncate to garbage as uint32_t. */
#define U_HWND_BASE        0x00010001u
#define U_HWND_STEP        0x00000004u
#define U_ATOM_BASE        0x0000C001u
#define U_HDC_TOKEN        0x0000A001u
#define U_ICON_TOKEN       0x0000E000u
#define U_ICON_STR_BITS    0x00000800u
#define U_IMAG_TOKEN       0x0000E400u
#define U_IMAG_STR_BITS    0x00000800u
#define U_CURSOR_TOKEN     0x0000C000u
#define U_CURSOR_STR_BITS  0x00000800u
#define U_NOTIFY_TOKEN     0x0000D001u

/* ------------------------------------------------------- virtual windows / class table -- */

#define U_WIN_MAX 16
typedef struct u_vwin {
    int      used;
    uint32_t hwnd;
    uint32_t wndproc;
    uint32_t hinst;
    uint32_t style;
    uint32_t exstyle;
    uint32_t userdata;
    uint32_t menu;
    uint32_t cls_atom;
    int32_t  win_x, win_y;        /* frame top-left corner (screen coordinates) */
    int32_t  win_cx, win_cy;      /* frame size */
    int      visible;
} u_vwin;

static u_vwin g_wins[U_WIN_MAX];

#define U_CLS_MAX 16
typedef struct u_vcls {
    int      used;
    uint32_t atom;
    uint32_t style;
    uint32_t wndproc;
    uint32_t hinst;
    uint32_t hicon;
    uint32_t hicon_sm;
    uint32_t hcursor;
    uint32_t hbr;
    uint32_t cls_extra;
    uint32_t wnd_extra;
    char     name[128];
} u_vcls;

static u_vcls g_cls[U_CLS_MAX];

static u_vwin *u_win_find(uint32_t hwnd)
{
    int i;
    if (!hwnd) return NULL;
    for (i = 0; i < U_WIN_MAX; i++)
        if (g_wins[i].used && g_wins[i].hwnd == hwnd) return &g_wins[i];
    return NULL;
}

static u_vcls *u_cls_find_name(const char *name)
{
    int i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < U_CLS_MAX; i++)
        if (g_cls[i].used && strcmp(g_cls[i].name, name) == 0) return &g_cls[i];
    return NULL;
}

static u_vcls *u_cls_find_atom(uint32_t atom)
{
    int i;
    for (i = 0; i < U_CLS_MAX; i++)
        if (g_cls[i].used && g_cls[i].atom == atom) return &g_cls[i];
    return NULL;
}

/* Frame metrics shared with AdjustWindowRectEx. The window is virtual, but the size ledger must be self-consistent:
 * GLFW uses AdjustWindowRectEx to compute the frame from the client area, then passes the frame to CreateWindowExA,
 * and we do the inverse of that same arithmetic here, so GetClientRect gives back the 1280x720 the game wants. */
static void u_frame_metrics(uint32_t style, uint32_t exstyle, int has_menu,
                            int *bx, int *by_top, int *by_bot)
{
    int border  = (style & U_WS_THICKFRAME) ? 8 : ((style & U_WS_BORDER) ? 1 : 0);
    int caption = ((style & U_WS_CAPTION) == U_WS_CAPTION) ? 23 : 0;
    int menu    = has_menu ? 19 : 0;
    int edge    = (exstyle & U_WS_EX_CLIENTEDGE) ? 2 : 0;

    *bx     = border + edge;
    *by_top = border + caption + menu + edge;
    *by_bot = border + edge;
}

/* Client size: the inverse of AdjustWindowRectEx applied to the frame. */
static void u_client_size(const u_vwin *w, int32_t *cx, int32_t *cy)
{
    int bx, by_top, by_bot;
    int32_t c;
    u_frame_metrics(w->style, w->exstyle, w->menu != 0u, &bx, &by_top, &by_bot);
    c = w->win_cx - 2 * (int32_t)bx;
    if (c < 1) c = 1;
    *cx = c;
    c = w->win_cy - (int32_t)by_top - (int32_t)by_bot;
    if (c < 1) c = 1;
    *cy = c;
}

/* Remaining global state */
static uint32_t g_capture_hwnd;
static uint32_t g_focus_hwnd;
static uint32_t g_active_hwnd;
static uint32_t g_cursor_handle;
static int32_t  g_cursor_x = 640, g_cursor_y = 360;   /* screen center */
static int      g_show_cursor_count;                  /* Win32 show counter */
static int      g_clip_open;
static uint32_t g_foreground_lock_timeout = 200000u;  /* Windows default */
static int      g_warned_spi;
static int      g_warned_unknown_class;

#define U_CLIP_MAX 8
static struct { int used; uint32_t fmt; uint32_t handle; } g_clip[U_CLIP_MAX];

/* Screen parameters: the whole host layer is designed around 1280x720 (the game's target resolution). */
#define U_SCREEN_W 1280
#define U_SCREEN_H 720

static const char *const u_display_name = "\\\\.\\DISPLAY1";
static const char *const u_display_desc = "Android virtual display";

/* ============================ ① messages ==================================== */

/* BOOL PeekMessageA(LPMSG, HWND, UINT, UINT, UINT) -- stdcall, cleans 20 bytes.
 * [real] We have no message source at all (no window, no key/mouse injection), so "the queue is empty" is
 * the truth rather than a lie: return FALSE, and per Win32 semantics do not touch lpMsg (the caller only reads
 * it when TRUE). GLFW's `while (PeekMessageA(...))` therefore exits immediately and does nothing. */
static void u_PeekMessageA(CPU *__restrict c)
{
    (void)uA(c, 0);
    (void)uA(c, 1);
    (void)uA(c, 2);
    (void)uA(c, 3);
    (void)uA(c, 4);
    c->eax = 0;
    uR(c, 20u);
}

/* BOOL GetMessageA(LPMSG, HWND, UINT, UINT) -- stdcall, cleans 16 bytes.
 * [trade-off] Two options:
 *   (a) return 0 -- Win32 semantics say that is WM_QUIT, glfwWindowShouldClose turns true at once,
 *       and the game exits. Absolutely unusable.
 *   (b) return 1 plus one WM_NULL -- the message loop continues, and DispatchMessageA(WM_NULL) is
 *       a no-op by definition, so the game never sees a fake event.
 * We choose (b). The cost is that with no messages the loop spins again immediately, so we sleep
 * RGXM_USER32_IDLE_US microseconds here (1ms by default), keeping the spin at ~1000 loops/second:
 * the CPU never saturates and glfwWaitEvents never starves rendering.
 * MSG structure (32-bit, 28 bytes): hwnd@0 message@4 wParam@8 lParam@12 time@16
 * pt.x@20 pt.y@24. time is monotonic milliseconds on the same time base as GetMessageTime. */
static void u_GetMessageA(CPU *__restrict c)
{
    uint32_t msg = uA(c, 0);
    struct timespec ts;

    (void)uA(c, 1);                            /* hWnd filter: there are no messages */
    (void)uA(c, 2);                            /* wMsgFilterMin */
    (void)uA(c, 3);                            /* wMsgFilterMax */

    if (u_ptr_ok(msg, 28u)) {
        memset((void *)(uintptr_t)msg, 0, 28u);
        st32(msg + 0u, g_active_hwnd);          /* owning window */
        st32(msg + 4u, U_WM_NULL);              /* WM_NULL: no-op message */
        st32(msg + 8u, 0u);
        st32(msg + 12u, 0u);
        st32(msg + 16u, u_now_ms());
        st32(msg + 20u, (uint32_t)g_cursor_x);
        st32(msg + 24u, (uint32_t)g_cursor_y);
    }

    ts.tv_sec = 0;
    ts.tv_nsec = (long)RGXM_USER32_IDLE_US * 1000L;
    (void)nanosleep(&ts, NULL);

    c->eax = 1;                                /* message available: keep the loop alive */
    uR(c, 16u);
}

/* BOOL TranslateMessage(const MSG*) -- stdcall, cleans 4 bytes.
 * [real] No message was ever translated, so FALSE is the honest answer. We never produce WM_KEYDOWN,
 * so there is no WM_CHAR to post either. */
static void u_TranslateMessage(CPU *__restrict c)
{
    (void)uA(c, 0);
    c->eax = 0;
    uR(c, 4u);
}

/* LRESULT DispatchMessageA(const MSG*) -- stdcall, cleans 4 bytes.
 * [stub] Returns 0. Reason: the only thing that can reach here is the WM_NULL synthesized by GetMessageA,
 * and the window procedure handling of WM_NULL is "return 0, do nothing". We therefore never need to call back
 * the guest WndProc -- in a world with no real input, calling it would only feed the game fake events.
 * When messages really must be posted later, change this one place (wndproc is already recorded in u_vcls / u_vwin). */
static void u_DispatchMessageA(CPU *__restrict c)
{
    (void)uA(c, 0);
    c->eax = 0;
    uR(c, 4u);
}

/* LONG GetMessageTime(void) -- stdcall, cleans 0 bytes.
 * [real] There is no message queue, but if "the time of the last message" returned 0, callers computing deltas would
 * get huge negative values/wraparound. Return monotonic clock milliseconds, guaranteed never to decrease. */
static void u_GetMessageTime(CPU *__restrict c)
{
    c->eax = u_now_ms();
    uR(c, 0u);
}

/* LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) -- stdcall, cleans 16 bytes.
 * [stub] Returns 0. We never forward messages to the guest WndProc, so this default procedure is in practice
 * never called; if it ever is, 0 is the default result for WM_NULL/WM_SETICON/WM_SIZE and friends.
 * A reminder for later implementers: once WndProc is really called, WM_NCCREATE must return TRUE,
 * otherwise window creation will be judged a failure. */
static void u_DefWindowProcA(CPU *__restrict c)
{
    (void)uA(c, 0);
    (void)uA(c, 1);
    (void)uA(c, 2);
    (void)uA(c, 3);
    c->eax = 0;
    uR(c, 16u);
}

/* LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM) -- stdcall, cleans 16 bytes.
 * [stub] Returns 0 = not handled. We do not call the guest WndProc synchronously (that would need guest_call and
 * a correct call frame), so any synchronous query gets the "default value"; when the game uses it to set the icon
 * (WM_SETICON), 0 is exactly the right return value. Other messages go to stderr so we can see what the game wants. */
static void u_SendMessageA(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    uint32_t msg  = uA(c, 1);
    (void)uA(c, 2);
    (void)uA(c, 3);
    if (msg != U_WM_SETICON)
        fprintf(stderr, "[user32] SendMessageA(hwnd=%08x, msg=%04x) -> 0\n",
                hwnd, msg);
    c->eax = 0;
    uR(c, 16u);
}

/* int MessageBoxA(HWND, LPCSTR, LPCSTR, UINT) -- stdcall, cleans 16 bytes.
 * [stub] Really popping an Android dialog would hang a headless run; but silently returning IDOK would throw away the only
 * diagnostic information. Compromise: print the title and text to stderr, and return "the user clicked the first
 * affirmative button" according to the button type (MB_OK->IDOK, MB_YESNO->IDYES), so the flow continues.
 * The button bits are type & 0x0F: 0=OK 1=OKCANCEL 3=YESNOCANCEL 4=YESNO. */
static void u_MessageBoxA(CPU *__restrict c)
{
    uint32_t hwnd  = uA(c, 0);
    uint32_t text  = uA(c, 1);
    uint32_t title = uA(c, 2);
    uint32_t type  = uA(c, 3);
    char tbuf[512];
    char cbuf[256];
    uint32_t result;

    (void)u_copy_str(text, tbuf, sizeof tbuf);
    (void)u_copy_str(title, cbuf, sizeof cbuf);
    fprintf(stderr, "[user32] MessageBoxA(hwnd=%08x, type=0x%x)\n"
                    "          title: %s\n"
                    "          text : %s\n",
            hwnd, type, cbuf[0] ? cbuf : "(null)", tbuf[0] ? tbuf : "(null)");

    switch (type & 0x0Fu) {
    case 3u:                                   /* MB_YESNOCANCEL */
    case 4u:                                   /* MB_YESNO */
        result = U_IDYES;
        break;
    default:                                   /* MB_OK / MB_OKCANCEL / ... */
        result = U_IDOK;
        break;
    }

    c->eax = result;
    uR(c, 16u);
}

/* ============================ ② window creation / classes ============================= */

/* HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int,int,int,int,
 *                      HWND, HMENU, HINSTANCE, LPVOID) -- stdcall, cleans 48 bytes.
 * [stub+real] Create a virtual window: the handle is a small non-zero integer token (starting at U_HWND_BASE),
 * while also recording the frame rect, style/exstyle, class atom, menu and wndproc; later
 * GetClientRect / SetWindowPos / ShowWindow / GetWindowLongA / GetMenu all read from
 * that ledger, so the sizes and styles the game gets are self-consistent data, not constant garbage.
 * The client size is recovered with the inverse of AdjustWindowRectEx: GLFW passes in the frame size
 * (it has already applied AdjustWindowRectEx), and after subtracting border/caption/menu it is exactly 1280x720.
 * If the class name was never registered we still create it (wndproc=0) and warn once -- in our world WndProc is never
 * called, so refusing creation over that would only stall startup right here.
 * But when lpClassName itself is NULL we return 0 honestly: Win32 fails too, and conjuring a
 * class-less window would only make later GetClientRect/WindowFromPoint point at something the game does not know. */
static void u_CreateWindowExA(CPU *__restrict c)
{
    uint32_t exstyle = uA(c, 0);
    uint32_t cls     = uA(c, 1);
    uint32_t title   = uA(c, 2);
    uint32_t style   = uA(c, 3);
    int32_t  x       = (int32_t)uA(c, 4);
    int32_t  y       = (int32_t)uA(c, 5);
    int32_t  cx      = (int32_t)uA(c, 6);
    int32_t  cy      = (int32_t)uA(c, 7);
    uint32_t parent  = uA(c, 8);
    uint32_t menu    = uA(c, 9);
    uint32_t hinst   = uA(c, 10);
    uint32_t param   = uA(c, 11);
    u_vcls  *vc = NULL;
    u_vwin  *w = NULL;
    char     name[128];
    int      i;

    (void)title;
    (void)param;                               /* no WM_CREATE/WM_NCCREATE */

    /* Class: may be a string pointer or the MAKEINTATOM returned by RegisterClassA */
    if (!cls) {
        fprintf(stderr, "[user32] CreateWindowExA: lpClassName == NULL, returning 0\n");
        c->eax = 0;
        uR(c, 48u);
        return;
    }
    if (((cls >> 16) & 0xFFFFu) == 0u) {
        vc = u_cls_find_atom(cls);
    } else if (u_copy_str(cls, name, sizeof name)) {
        vc = u_cls_find_name(name);
    }
    if (!vc && !g_warned_unknown_class) {
        g_warned_unknown_class = 1;
        fprintf(stderr, "[user32] CreateWindowExA: class was not registered with us; creating anyway\n");
    }

    for (i = 0; i < U_WIN_MAX; i++)
        if (!g_wins[i].used) { w = &g_wins[i]; break; }
    if (!w) {
        fprintf(stderr, "[user32] CreateWindowExA: virtual window table full\n");
        c->eax = 0;
        uR(c, 48u);
        return;
    }

    if (cx <= 0 || cy <= 0) { cx = U_SCREEN_W; cy = U_SCREEN_H; }
    if (cx > 16384) cx = 16384;
    if (cy > 16384) cy = 16384;
    if (x == (int32_t)0x80000000) x = 0;        /* CW_USEDEFAULT */
    if (y == (int32_t)0x80000000) y = 0;

    memset(w, 0, sizeof *w);
    w->used     = 1;
    w->hwnd     = U_HWND_BASE + (uint32_t)i * U_HWND_STEP;
    w->wndproc  = vc ? vc->wndproc : 0u;
    w->hinst    = hinst ? hinst : (vc ? vc->hinst : 0u);
    w->style    = style;
    w->exstyle  = exstyle;
    w->menu     = menu;
    w->cls_atom = vc ? vc->atom : 0u;
    w->win_x    = x;
    w->win_y    = y;
    w->win_cx   = cx;
    w->win_cy   = cy;
    w->visible  = (style & U_WS_VISIBLE) ? 1 : 0;

    (void)parent;                              /* no parent/child z-order to maintain */

    /* The new window becomes the active/focus window: GLFW assumes it gets focus right after creating it */
    g_active_hwnd = w->hwnd;
    g_focus_hwnd  = w->hwnd;

    c->eax = w->hwnd;
    uR(c, 48u);
}

/* Shared implementation of RegisterClassA / RegisterClassExA.
 * WNDCLASSA (32-bit, 40 bytes): style@0, lpfnWndProc@4, cbClsExtra@8,
 * cbWndExtra@12, hInstance@16, hIcon@20, hCursor@24, hbrBackground@28,
 * lpszMenuName@32, lpszClassName@36.
 * WNDCLASSEXA (48 bytes): cbSize@0, style@4, lpfnWndProc@8, cbClsExtra@12,
 * cbWndExtra@16, hInstance@20, hIcon@24, hCursor@28, hbrBackground@32,
 * lpszMenuName@36, lpszClassName@40, hIconSm@44. */
static void u_register_class_common(CPU *__restrict c, uint32_t ptr, int extended)
{
    uint32_t style, wndproc, cls_extra, wnd_extra, hinst, hicon, hcursor, hbr;
    uint32_t name_ptr;
    char name[128];
    u_vcls *vc = NULL;
    int i;

    if (!u_ptr_ok(ptr, extended ? 48u : 40u)) {
        fprintf(stderr, "[user32] RegisterClass%sA: invalid struct pointer %08x\n",
                extended ? "Ex" : "", ptr);
        c->eax = 0;
        uR(c, 4u);
        return;
    }
    if (extended) {
        uint32_t cb = ld32(ptr + 0u);
        if (cb != 48u) {                       /* Win32 requires cbSize to match exactly */
            fprintf(stderr, "[user32] RegisterClassExA: cbSize=%u != 48\n", cb);
            c->eax = 0;
            uR(c, 4u);
            return;
        }
        style     = ld32(ptr + 4u);
        wndproc   = ld32(ptr + 8u);
        cls_extra = ld32(ptr + 12u);
        wnd_extra = ld32(ptr + 16u);
        hinst     = ld32(ptr + 20u);
        hicon     = ld32(ptr + 24u);
        hcursor   = ld32(ptr + 28u);
        hbr       = ld32(ptr + 32u);
        name_ptr  = ld32(ptr + 40u);
    } else {
        style     = ld32(ptr + 0u);
        wndproc   = ld32(ptr + 4u);
        cls_extra = ld32(ptr + 8u);
        wnd_extra = ld32(ptr + 12u);
        hinst     = ld32(ptr + 16u);
        hicon     = ld32(ptr + 20u);
        hcursor   = ld32(ptr + 24u);
        hbr       = ld32(ptr + 28u);
        name_ptr  = ld32(ptr + 36u);
    }
    if (!u_copy_str(name_ptr, name, sizeof name) || name[0] == '\0') {
        fprintf(stderr, "[user32] RegisterClass%sA: invalid class name pointer %08x\n",
                extended ? "Ex" : "", name_ptr);
        c->eax = 0;
        uR(c, 4u);
        return;
    }
    vc = u_cls_find_name(name);
    if (!vc) {
        for (i = 0; i < U_CLS_MAX; i++)
            if (!g_cls[i].used) { vc = &g_cls[i]; break; }
        if (vc) {
            memset(vc, 0, sizeof *vc);
            vc->atom = U_ATOM_BASE + (uint32_t)(vc - g_cls);
        }
    }
    if (!vc) {
        fprintf(stderr, "[user32] RegisterClass%sA: class table full\n",
                extended ? "Ex" : "");
        c->eax = 0;
        uR(c, 4u);
        return;
    }
    /* Duplicate registration of the same name: reuse the existing atom (Win32 returns 0, but that would only stall startup here) */
    vc->used      = 1;
    vc->style     = style;
    vc->wndproc   = wndproc;
    vc->hinst     = hinst;
    vc->hicon     = hicon;
    vc->hcursor   = hcursor;
    vc->hbr       = hbr;
    vc->cls_extra = cls_extra;
    vc->wnd_extra = wnd_extra;
    memcpy(vc->name, name, sizeof vc->name);
    if (extended) vc->hicon_sm = ld32(ptr + 44u);

    c->eax = vc->atom;
    uR(c, 4u);
}

/* ATOM RegisterClassA(const WNDCLASSA*) -- stdcall, cleans 4 bytes.
 * [real] Parse the structure, store it in the class table, return a non-zero atom. */
static void u_RegisterClassA(CPU *__restrict c)
{
    u_register_class_common(c, uA(c, 0), 0);
}

/* ATOM RegisterClassExA(const WNDCLASSEXA*) -- stdcall, cleans 4 bytes.
 * [real] Same as above; cbSize must be 48, otherwise return 0 honestly. */
static void u_RegisterClassExA(CPU *__restrict c)
{
    u_register_class_common(c, uA(c, 0), 1);
}

/* BOOL UnregisterClassA(LPCSTR, HINSTANCE) -- stdcall, cleans 8 bytes.
 * [real] If found, remove it from the class table and return TRUE; if not found, return FALSE honestly (Win32 behavior). */
static void u_UnregisterClassA(CPU *__restrict c)
{
    uint32_t name_ptr = uA(c, 0);
    u_vcls *vc;
    char name[128];

    (void)uA(c, 1);                            /* hInstance: looking up by name is enough */
    if (!u_copy_str(name_ptr, name, sizeof name)) {
        c->eax = 0;
        uR(c, 8u);
        return;
    }
    vc = u_cls_find_name(name);
    if (!vc) { c->eax = 0; uR(c, 8u); return; }
    memset(vc, 0, sizeof *vc);
    c->eax = 1;
    uR(c, 8u);
}

/* BOOL DestroyWindow(HWND) -- stdcall, cleans 4 bytes.
 * [real] Only destroy handles we issued; also clear the focus/capture/active window if it points at this one,
 * so GetActiveWindow does not later return a dead handle. */
static void u_DestroyWindow(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    u_vwin *w = u_win_find(hwnd);

    if (!w) { c->eax = 0; uR(c, 4u); return; }
    if (g_focus_hwnd == hwnd)  g_focus_hwnd = 0;
    if (g_capture_hwnd == hwnd) g_capture_hwnd = 0;
    if (g_active_hwnd == hwnd) g_active_hwnd = 0;
    memset(w, 0, sizeof *w);
    c->eax = 1;
    uR(c, 4u);
}

/* BOOL ShowWindow(HWND, int) -- stdcall, cleans 8 bytes.
 * [real] Update the visible flag and the WS_VISIBLE bit of style, and return "was it visible before"
 * (that is exactly what Win32 returns), so the caller can tell accurately whether the state changed. */
static void u_ShowWindow(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    uint32_t cmd  = uA(c, 1);
    u_vwin *w = u_win_find(hwnd);
    uint32_t was;

    if (!w) { c->eax = 0; uR(c, 8u); return; }
    was = w->visible ? 1u : 0u;
    if ((int32_t)cmd == U_SW_HIDE) {
        w->visible = 0;
        w->style &= ~U_WS_VISIBLE;
    } else {
        w->visible = 1;
        w->style |= U_WS_VISIBLE;
    }
    c->eax = was;
    uR(c, 8u);
}

/* BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) -- stdcall, cleans 28 bytes.
 * [real] Update the virtual frame rect according to SWP_NOMOVE / SWP_NOSIZE. There is only one z-order layer, so
 * hWndInsertAfter is ignored; SWP_SHOWWINDOW also marks the window visible (Win32 does the same). */
static void u_SetWindowPos(CPU *__restrict c)
{
    uint32_t hwnd  = uA(c, 0);
    uint32_t after = uA(c, 1);
    int32_t  x     = (int32_t)uA(c, 2);
    int32_t  y     = (int32_t)uA(c, 3);
    int32_t  cx    = (int32_t)uA(c, 4);
    int32_t  cy    = (int32_t)uA(c, 5);
    uint32_t flags = uA(c, 6);
    u_vwin *w = u_win_find(hwnd);

    (void)after;
    if (!w) { c->eax = 0; uR(c, 28u); return; }
    if (!(flags & U_SWP_NOMOVE)) {
        if (x != (int32_t)0x80000000) w->win_x = x;
        if (y != (int32_t)0x80000000) w->win_y = y;
    }
    if (!(flags & U_SWP_NOSIZE)) {
        if (cx > 0 && cx <= 16384) w->win_cx = cx;
        if (cy > 0 && cy <= 16384) w->win_cy = cy;
    }
    if (flags & U_SWP_SHOWWINDOW) {
        w->visible = 1;
        w->style |= U_WS_VISIBLE;
    }
    c->eax = 1;
    uR(c, 28u);
}

/* BOOL GetClientRect(HWND, LPRECT) -- stdcall, cleans 8 bytes.
 * [real] RECT = 16 bytes (left,top,right,bottom). The client origin is always (0,0),
 * and the size is the frame minus border/caption/menu -- strictly inverse to AdjustWindowRectEx,
 * so a 1280x720 client-area request comes back as exactly 1280x720.
 * For an unknown window return FALSE as Win32 does, without writing the buffer. */
static void u_GetClientRect(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    uint32_t out  = uA(c, 1);
    u_vwin *w = u_win_find(hwnd);
    int32_t cx, cy;

    if (!w || !u_ptr_ok(out, 16u)) { c->eax = 0; uR(c, 8u); return; }
    u_client_size(w, &cx, &cy);
    st32(out + 0u, 0u);
    st32(out + 4u, 0u);
    st32(out + 8u, (uint32_t)cx);
    st32(out + 12u, (uint32_t)cy);
    c->eax = 1;
    uR(c, 8u);
}

/* BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) -- stdcall, cleans 16 bytes.
 * [real] Expand the client rect into a frame using our own frame metrics: add border on the left and right,
 * add border+caption+menu on top, border on the bottom (plus 2 more each when WS_EX_CLIENTEDGE is set).
 * The two functions must stay consistent as a pair, or the window size GLFW computes fights with GetClientRect. */
static void u_AdjustWindowRectEx(CPU *__restrict c)
{
    uint32_t rect  = uA(c, 0);
    uint32_t style = uA(c, 1);
    uint32_t menu  = uA(c, 2);
    uint32_t ex    = uA(c, 3);
    int bx, by_top, by_bot;
    int32_t l, t, r, b;

    if (!u_ptr_ok(rect, 16u)) { c->eax = 0; uR(c, 16u); return; }
    u_frame_metrics(style, ex, menu != 0u, &bx, &by_top, &by_bot);
    l = (int32_t)ld32(rect + 0u);
    t = (int32_t)ld32(rect + 4u);
    r = (int32_t)ld32(rect + 8u);
    b = (int32_t)ld32(rect + 12u);
    st32(rect + 0u,  (uint32_t)(l - bx));
    st32(rect + 4u,  (uint32_t)(t - by_top));
    st32(rect + 8u,  (uint32_t)(r + bx));
    st32(rect + 12u, (uint32_t)(b + by_bot));
    c->eax = 1;
    uR(c, 16u);
}

/* ============================ ③ window state =============================== */

/* BOOL BringWindowToTop(HWND) -- stdcall, cleans 4 bytes.
 * [stub] There is only one z-order layer, so raising has no observable effect; return TRUE if the handle is valid. */
static void u_BringWindowToTop(CPU *__restrict c)
{
    c->eax = u_win_find(uA(c, 0)) ? 1u : 0u;
    uR(c, 4u);
}

/* HWND SetCapture(HWND) -- stdcall, cleans 4 bytes.
 * [real] Return the previous capture window (Win32 semantics) and remember the new one; an invalid handle means release. */
static void u_SetCapture(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    uint32_t prev = g_capture_hwnd;
    g_capture_hwnd = u_win_find(hwnd) ? hwnd : 0u;
    c->eax = prev;
    uR(c, 4u);
}

/* BOOL ReleaseCapture(void) -- stdcall, cleans 0 bytes.
 * [real] The postcondition (no capture window) always holds, so return TRUE and do not fake a "release failed". */
static void u_ReleaseCapture(CPU *__restrict c)
{
    g_capture_hwnd = 0;
    c->eax = 1;
    uR(c, 0u);
}

/* HCURSOR SetCursor(HCURSOR) -- stdcall, cleans 4 bytes.
 * [real] Record the cursor and return the old handle (initially 0 = unset, as in Win32). */
static void u_SetCursor(CPU *__restrict c)
{
    uint32_t cur = uA(c, 0);
    uint32_t prev = g_cursor_handle;
    g_cursor_handle = cur;
    c->eax = prev;
    uR(c, 4u);
}

/* HWND SetFocus(HWND) -- stdcall, cleans 4 bytes.
 * [real] Return the previous focus window; a valid handle also becomes the active window. */
static void u_SetFocus(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    uint32_t prev = g_focus_hwnd;
    if (hwnd && !u_win_find(hwnd)) { c->eax = 0; uR(c, 4u); return; }
    g_focus_hwnd = hwnd;
    if (hwnd) g_active_hwnd = hwnd;
    c->eax = prev;
    uR(c, 4u);
}

/* HWND GetActiveWindow(void) -- stdcall, cleans 0 bytes.
 * [real] Return this thread's active window (set when the window was created). Return 0 honestly when there is none. */
static void u_GetActiveWindow(CPU *__restrict c)
{
    c->eax = g_active_hwnd;
    uR(c, 0u);
}

/* BOOL SetForegroundWindow(HWND) -- stdcall, cleans 4 bytes.
 * [stub] Android has no "foreground window arbitration"; record the foreground state on the active window and return TRUE. */
static void u_SetForegroundWindow(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    if (hwnd && u_win_find(hwnd)) g_active_hwnd = hwnd;
    c->eax = 1;
    uR(c, 4u);
}

/* BOOL ClipCursor(const RECT*) -- stdcall, cleans 4 bytes.
 * [stub] A trap rectangle only means something for a real cursor, and we have none. A null pointer = release the trap;
 * a non-null but invalid one returns FALSE honestly. */
static void u_ClipCursor(CPU *__restrict c)
{
    uint32_t rect = uA(c, 0);
    c->eax = (!rect || u_ptr_ok(rect, 16u)) ? 1u : 0u;
    uR(c, 4u);
}

/* int ShowCursor(BOOL) -- stdcall, cleans 4 bytes.
 * [real] In Win32 this is a counter: TRUE adds one, FALSE subtracts one, and the new count is returned;
 * the cursor is visible while the count >= 0. Just implement that; there is no need to actually draw an arrow. */
static void u_ShowCursor(CPU *__restrict c)
{
    int32_t show = (int32_t)uA(c, 0);
    if (show) g_show_cursor_count++;
    else      g_show_cursor_count--;
    c->eax = (uint32_t)g_show_cursor_count;
    uR(c, 4u);
}

/* BOOL SetCursorPos(int, int) -- stdcall, cleans 8 bytes.
 * [real] Record into the virtual cursor position; GetCursorPos and the pt in GetMessageA both read it. */
static void u_SetCursorPos(CPU *__restrict c)
{
    g_cursor_x = (int32_t)uA(c, 0);
    g_cursor_y = (int32_t)uA(c, 1);
    c->eax = 1;
    uR(c, 8u);
}

/* BOOL GetCursorPos(LPPOINT) -- stdcall, cleans 4 bytes.
 * [real] Write POINT{x,y} (8 bytes); initially at the screen center, 640,360. */
static void u_GetCursorPos(CPU *__restrict c)
{
    uint32_t out = uA(c, 0);
    if (!u_ptr_ok(out, 8u)) { c->eax = 0; uR(c, 4u); return; }
    st32(out + 0u, (uint32_t)g_cursor_x);
    st32(out + 4u, (uint32_t)g_cursor_y);
    c->eax = 1;
    uR(c, 4u);
}

/* BOOL ClientToScreen(HWND, LPPOINT) -- stdcall, cleans 8 bytes.
 * [real] The client origin equals the frame top-left corner (the virtual window carries no decoration offset; the border is only accounted for in the size),
 * so the addition is simply win_x/win_y. An unknown window returns FALSE. */
static void u_ClientToScreen(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    uint32_t pt   = uA(c, 1);
    u_vwin *w = u_win_find(hwnd);
    int32_t x, y;

    if (!w || !u_ptr_ok(pt, 8u)) { c->eax = 0; uR(c, 8u); return; }
    x = (int32_t)ld32(pt + 0u) + w->win_x;
    y = (int32_t)ld32(pt + 4u) + w->win_y;
    st32(pt + 0u, (uint32_t)x);
    st32(pt + 4u, (uint32_t)y);
    c->eax = 1;
    uR(c, 8u);
}

/* HWND WindowFromPoint(POINT) -- stdcall, POINT passed by value in 8 bytes, cleans 8 bytes.
 * [real] There is only one window layer: if the point falls inside the frame, return that window; otherwise 0 (the desktop). */
static void u_WindowFromPoint(CPU *__restrict c)
{
    int32_t px = (int32_t)uA(c, 0);
    int32_t py = (int32_t)uA(c, 1);
    int i;

    for (i = 0; i < U_WIN_MAX; i++) {
        u_vwin *w = &g_wins[i];
        if (!w->used) continue;
        if (px >= w->win_x && px < w->win_x + w->win_cx &&
            py >= w->win_y && py < w->win_y + w->win_cy) {
            c->eax = w->hwnd;
            uR(c, 8u);
            return;
        }
    }
    c->eax = 0;
    uR(c, 8u);
}

/* HMENU GetMenu(HWND) -- stdcall, cleans 4 bytes.
 * [real] Return the menu handle recorded at window creation; GLFW passes NULL, so the normal path yields 0,
 * consistent with the fact that the window has no menu. */
static void u_GetMenu(CPU *__restrict c)
{
    u_vwin *w = u_win_find(uA(c, 0));
    c->eax = w ? w->menu : 0u;
    uR(c, 4u);
}

/* LONG GetWindowLongA(HWND, int) -- stdcall, cleans 8 bytes.
 * [real] Read from the virtual window: GWL_STYLE(-16)/GWL_EXSTYLE(-20)/GWL_WNDPROC(-4)/
 * GWL_HINSTANCE(-6)/GWL_USERDATA(-21, where GLFW stores the window pointer). An unknown index returns
 * 0 as in Win32; an unknown window also returns 0 (Win32 sets ERROR_INVALID_WINDOW_HANDLE).
 * Note: -21 must be supported, otherwise GLFW's window<->object mapping gets no pointer and crashes outright. */
static void u_GetWindowLongA(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    int32_t index = (int32_t)uA(c, 1);
    u_vwin *w = u_win_find(hwnd);

    if (!w) { c->eax = 0; uR(c, 8u); return; }
    switch (index) {
    case U_GWL_STYLE:      c->eax = w->style;    break;
    case U_GWL_EXSTYLE:    c->eax = w->exstyle;  break;
    case U_GWL_WNDPROC:    c->eax = w->wndproc;  break;
    case U_GWL_HINSTANCE:  c->eax = w->hinst;    break;
    case U_GWL_USERDATA:   c->eax = w->userdata; break;
    case U_GWL_HWNDPARENT: c->eax = 0;           break;   /* no parent window */
    case U_GWL_ID:         c->eax = 0;           break;
    default:               c->eax = 0;           break;
    }
    uR(c, 8u);
}

/* LONG SetWindowLongA(HWND, int, LONG) -- stdcall, cleans 12 bytes.
 * [real] Write into the virtual window and return the old value (Win32 semantics). After a style change the client size
 * follows -- GetClientRect recomputes from the current style every time, so operations like a fullscreen switch are self-consistent. */
static void u_SetWindowLongA(CPU *__restrict c)
{
    uint32_t hwnd  = uA(c, 0);
    int32_t  index = (int32_t)uA(c, 1);
    uint32_t value = uA(c, 2);
    u_vwin *w = u_win_find(hwnd);
    uint32_t old = 0;

    if (!w) { c->eax = 0; uR(c, 12u); return; }
    switch (index) {
    case U_GWL_STYLE:     old = w->style;    w->style = value;    break;
    case U_GWL_EXSTYLE:   old = w->exstyle;  w->exstyle = value;  break;
    case U_GWL_WNDPROC:   old = w->wndproc;  w->wndproc = value;  break;
    case U_GWL_HINSTANCE: old = w->hinst;    w->hinst = value;    break;
    case U_GWL_USERDATA:  old = w->userdata; w->userdata = value; break;
    default:              old = 0;                                break;
    }
    c->eax = old;
    uR(c, 12u);
}

/* DWORD SetClassLongA(HWND, int, LONG) -- stdcall, cleans 12 bytes.
 * [real] Write the corresponding field in the class table and return the old value. The game uses it to swap the window icon (GCL_HICON -14),
 * and an old value of 0 means there was no icon before -- which is the truth. An unknown window returns 0. */
static void u_SetClassLongA(CPU *__restrict c)
{
    uint32_t hwnd  = uA(c, 0);
    int32_t  index = (int32_t)uA(c, 1);
    uint32_t value = uA(c, 2);
    u_vwin *w = u_win_find(hwnd);
    u_vcls *vc;
    uint32_t old = 0;

    if (!w) { c->eax = 0; uR(c, 12u); return; }
    vc = u_cls_find_atom(w->cls_atom);
    if (!vc) { c->eax = 0; uR(c, 12u); return; }
    switch (index) {
    case U_GCL_STYLE:         old = vc->style;     vc->style = value;     break;
    case U_GCL_WNDPROC:       old = vc->wndproc;   vc->wndproc = value;   break;
    case U_GCL_CBCLSEXTRA:    old = vc->cls_extra; vc->cls_extra = value; break;
    case U_GCL_CBWNDEXTRA:    old = vc->wnd_extra; vc->wnd_extra = value; break;
    case U_GCL_HICON:         old = vc->hicon;     vc->hicon = value;     break;
    case U_GCL_HICONSM:       old = vc->hicon_sm;  vc->hicon_sm = value;  break;
    case U_GCL_HCURSOR:       old = vc->hcursor;   vc->hcursor = value;   break;
    case U_GCL_HBRBACKGROUND: old = vc->hbr;       vc->hbr = value;       break;
    case U_GCL_MENUNAME:      old = 0;             break;   /* no menu resource */
    default:                  old = 0;             break;
    }
    c->eax = old;
    uR(c, 12u);
}

/* ============================ ④ icon / cursor resources ========================= */

/* HICON LoadIconA(HINSTANCE, LPCSTR) -- stdcall, cleans 8 bytes.
 * [stub] We do not parse PE resources and do not load icon bitmaps; but returning 0 would make GLFW treat it as
 * "icon load failed" and then as a fatal error. So we derive a stable, non-zero token from the request that can never be
 * confused with a host pointer: integer resource id (HIWORD==0) -> 0xE000|id, string name -> 0xE800|hash.
 * The same request always yields the same token, so guest equality comparisons hold too. */
static void u_LoadIconA(CPU *__restrict c)
{
    uint32_t name = uA(c, 1);
    (void)uA(c, 0);                            /* hInstance: the resource lives in the built-in image */

    if (!name) { c->eax = 0; uR(c, 8u); return; }
    if (((name >> 16) & 0xFFFFu) == 0u) {
        uint32_t id = name & 0xFFFFu;
        if (id == 0u) { c->eax = 0; uR(c, 8u); return; }
        c->eax = U_ICON_TOKEN | (id & 0x7FFu);
    } else {
        char buf[128];
        (void)u_copy_str(name, buf, sizeof buf);
        c->eax = U_ICON_TOKEN | U_ICON_STR_BITS | (u_hash_name(buf) & 0x7FFu);
    }
    uR(c, 8u);
}

/* HCURSOR LoadCursorA(HINSTANCE, LPCSTR) -- stdcall, cleans 8 bytes.
 * [stub] Same reasoning as LoadIconA: GLFW fills the class hCursor with LoadCursorA(NULL, IDC_ARROW=32512),
 * and returning 0 would make window creation fail. The token is 0xC000|(id&0x7FF), so IDC_ARROW
 * yields 0x0000CF00. */
static void u_LoadCursorA(CPU *__restrict c)
{
    uint32_t name = uA(c, 1);
    (void)uA(c, 0);

    if (!name) { c->eax = 0; uR(c, 8u); return; }
    if (((name >> 16) & 0xFFFFu) == 0u) {
        uint32_t id = name & 0xFFFFu;
        if (id == 0u) { c->eax = 0; uR(c, 8u); return; }
        c->eax = U_CURSOR_TOKEN | (id & 0x7FFu);
    } else {
        char buf[128];
        (void)u_copy_str(name, buf, sizeof buf);
        c->eax = U_CURSOR_TOKEN | U_CURSOR_STR_BITS | (u_hash_name(buf) & 0x7FFu);
    }
    uR(c, 8u);
}

/* HANDLE LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) -- stdcall, cleans 24.
 * [stub] No bitmap decoding; derive a stable non-zero token from (type, resource id). Only
 * IMAGE_BITMAP(0)/IMAGE_ICON(1)/IMAGE_CURSOR(2) are accepted; anything else returns 0 honestly.
 * cx/cy/fuLoad only affect the decoded size and are ignored here (read them once real decoding is wired up). */
static void u_LoadImageA(CPU *__restrict c)
{
    uint32_t name = uA(c, 1);
    uint32_t type = uA(c, 2);
    int32_t  cx   = (int32_t)uA(c, 3);
    int32_t  cy   = (int32_t)uA(c, 4);
    uint32_t flags = uA(c, 5);
    uint32_t base;

    (void)uA(c, 0);
    (void)cx;
    (void)cy;
    (void)flags;

    if (type > U_IMAGE_CURSOR) { c->eax = 0; uR(c, 24u); return; }
    if (!name) { c->eax = 0; uR(c, 24u); return; }
    base = U_IMAG_TOKEN | (type << 10);
    if (((name >> 16) & 0xFFFFu) == 0u) {
        uint32_t id = name & 0xFFFFu;
        if (id == 0u) { c->eax = 0; uR(c, 24u); return; }
        c->eax = base | (id & 0x3FFu);
    } else {
        char buf[128];
        (void)u_copy_str(name, buf, sizeof buf);
        c->eax = base | U_IMAG_STR_BITS | (u_hash_name(buf) & 0x3FFu);
    }
    uR(c, 24u);
}

/* ============================ ⑤ system metrics / display ======================== */

/* int GetSystemMetrics(int) -- stdcall, cleans 4 bytes.
 * [real] For a single 1280x720 screen, report metrics consistent with AdjustWindowRectEx (SM_CXFRAME=8,
 * SM_CYCAPTION=23, SM_CYMENU=19 ...). An unknown selector returns 0 = "no such feature",
 * which is a legal Win32 answer for unsupported selectors and never hands the caller garbage sizes. */
static void u_GetSystemMetrics(CPU *__restrict c)
{
    int32_t index = (int32_t)uA(c, 0);
    int32_t v;

    switch (index) {
    case 0:  v = U_SCREEN_W; break;            /* SM_CXSCREEN */
    case 1:  v = U_SCREEN_H; break;            /* SM_CYSCREEN */
    case 2:  v = 17; break;                    /* SM_CXVSCROLL */
    case 3:  v = 17; break;                    /* SM_CYHSCROLL */
    case 4:  v = 23; break;                    /* SM_CYCAPTION */
    case 5:  v = 1;  break;                    /* SM_CXBORDER */
    case 6:  v = 1;  break;                    /* SM_CYBORDER */
    case 7:  v = 3;  break;                    /* SM_CXFIXEDFRAME */
    case 8:  v = 3;  break;                    /* SM_CYFIXEDFRAME */
    case 9:  v = 17; break;                    /* SM_CYVTHUMB */
    case 10: v = 17; break;                    /* SM_CXHTHUMB */
    case 11: v = 32; break;                    /* SM_CXICON */
    case 12: v = 32; break;                    /* SM_CYICON */
    case 13: v = 32; break;                    /* SM_CXCURSOR */
    case 14: v = 32; break;                    /* SM_CYCURSOR */
    case 15: v = 19; break;                    /* SM_CYMENU */
    case 16: v = U_SCREEN_W; break;            /* SM_CXFULLSCREEN */
    case 17: v = U_SCREEN_H; break;            /* SM_CYFULLSCREEN */
    case 19: v = 1;  break;                    /* SM_MOUSEPRESENT */
    case 20: v = 17; break;                    /* SM_CYVSCROLL */
    case 21: v = 17; break;                    /* SM_CXHSCROLL */
    case 23: v = 0;  break;                    /* SM_SWAPBUTTON */
    case 28: v = 112; break;                   /* SM_CXMIN */
    case 29: v = 27;  break;                   /* SM_CYMIN */
    case 30: v = 23;  break;                   /* SM_CXSIZE */
    case 31: v = 23;  break;                   /* SM_CYSIZE */
    case 32: v = 8;   break;                   /* SM_CXFRAME = thick frame */
    case 33: v = 8;   break;                   /* SM_CYFRAME */
    case 34: v = 112; break;                   /* SM_CXMINTRACK */
    case 35: v = 27;  break;                   /* SM_CYMINTRACK */
    case 36: v = 4;   break;                   /* SM_CXDOUBLECLK */
    case 37: v = 4;   break;                   /* SM_CYDOUBLECLK */
    case 38: v = 64;  break;                   /* SM_CXICONSPACING */
    case 39: v = 64;  break;                   /* SM_CYICONSPACING */
    case 43: v = 3;   break;                   /* SM_CMOUSEBUTTONS */
    case 45: v = 2;   break;                   /* SM_CXEDGE */
    case 46: v = 2;   break;                   /* SM_CYEDGE */
    case 47: v = 112; break;                   /* SM_CXMINSPACING */
    case 48: v = 27;  break;                   /* SM_CYMINSPACING */
    case 49: v = 16;  break;                   /* SM_CXSMICON */
    case 50: v = 16;  break;                   /* SM_CYSMICON */
    case 51: v = 19;  break;                   /* SM_CYSMCAPTION */
    case 52: v = 17;  break;                   /* SM_CXSMSIZE */
    case 53: v = 17;  break;                   /* SM_CYSMSIZE */
    case 54: v = 19;  break;                   /* SM_CXMENUSIZE */
    case 55: v = 19;  break;                   /* SM_CYMENUSIZE */
    case 57: v = 160; break;                   /* SM_CXMINIMIZED */
    case 58: v = 28;  break;                   /* SM_CYMINIMIZED */
    case 59: v = U_SCREEN_W + 16; break;       /* SM_CXMAXTRACK */
    case 60: v = U_SCREEN_H + 26; break;       /* SM_CYMAXTRACK */
    case 61: v = U_SCREEN_W; break;            /* SM_CXMAXIMIZED */
    case 62: v = U_SCREEN_H; break;            /* SM_CYMAXIMIZED */
    case 63: v = 1;   break;                   /* SM_NETWORK */
    case 67: v = 0;   break;                   /* SM_CLEANBOOT */
    case 68: v = 4;   break;                   /* SM_CXDRAG */
    case 69: v = 4;   break;                   /* SM_CYDRAG */
    case 71: v = 13;  break;                   /* SM_CXMENUCHECK */
    case 72: v = 13;  break;                   /* SM_CYMENUCHECK */
    case 75: v = 1;   break;                   /* SM_MOUSEWHEELPRESENT */
    case 76: v = 0;   break;                   /* SM_XVIRTUALSCREEN */
    case 77: v = 0;   break;                   /* SM_YVIRTUALSCREEN */
    case 78: v = U_SCREEN_W; break;            /* SM_CXVIRTUALSCREEN */
    case 79: v = U_SCREEN_H; break;            /* SM_CYVIRTUALSCREEN */
    case 80: v = 1;   break;                   /* SM_CMONITORS */
    case 81: v = 1;   break;                   /* SM_SAMEDISPLAYFORMAT */
    case 83: v = 1;   break;                   /* SM_CXFOCUSBORDER */
    case 84: v = 1;   break;                   /* SM_CYFOCUSBORDER */
    case 91: v = 0;   break;                   /* SM_MOUSEHORIZONTALWHEELPRESENT */
    case 92: v = 0;   break;                   /* SM_CXPADDEDBORDER */
    case 0x1000: v = 0; break;                 /* SM_REMOTESESSION */
    case 0x2000: v = 0; break;                 /* SM_SHUTTINGDOWN */
    default:
        /* Unknown selector = no such feature. 0 is the legal Win32 answer, so the caller takes the
         * "feature not present" branch instead of receiving an invented size. */
        v = 0;
        break;
    }
    c->eax = (uint32_t)v;
    uR(c, 4u);
}

/* BOOL EnumDisplayDevicesA(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD) -- stdcall, cleans 16 bytes.
 * [real] We have exactly one display adapter: iDevNum==0 returns "\\\\.\\DISPLAY1",
 * StateFlags=ATTACHED_TO_DESKTOP|PRIMARY_DEVICE(0x5); iDevNum>0 returns FALSE honestly.
 * DISPLAY_DEVICEA layout (32-bit, 424 bytes): cb@0, DeviceName[32]@4,
 * DeviceString[128]@36, StateFlags@164, DeviceID[128]@168, DeviceKey[128]@296.
 * When the caller's cb is smaller than 424, return FALSE with Win32's ERROR_INSUFFICIENT_BUFFER semantics. */
static void u_EnumDisplayDevicesA(CPU *__restrict c)
{
    uint32_t dev_num = uA(c, 1);
    uint32_t out     = uA(c, 2);
    uint32_t cb;

    (void)uA(c, 0);                            /* lpDevice: NULL = all adapters */
    (void)uA(c, 3);                            /* dwFlags: the interface-name flag is meaningless here */

    if (!u_ptr_ok(out, 4u) || dev_num != 0u) { c->eax = 0; uR(c, 16u); return; }
    cb = ld32(out + 0u);
    if (cb != 0u && cb < 424u) { c->eax = 0; uR(c, 16u); return; }
    if (!u_ptr_ok(out, 424u)) { c->eax = 0; uR(c, 16u); return; }

    memset((void *)(uintptr_t)out, 0, 424u);
    memcpy((void *)(uintptr_t)(out + 4u), u_display_name,
           strlen(u_display_name) + 1u);
    memcpy((void *)(uintptr_t)(out + 36u), u_display_desc,
           strlen(u_display_desc) + 1u);
    st32(out + 164u, 0x00000005u);             /* ATTACHED_TO_DESKTOP | PRIMARY */
    st32(out + 0u, 424u);                      /* cb = sizeof(DISPLAY_DEVICEA) */
    c->eax = 1;
    uR(c, 16u);
}

/* Fill in a DEVMODEA describing the "current display mode" (32-bit, 156 bytes).
 * Offsets: dmDeviceName[32]@0, dmSpecVersion@32, dmDriverVersion@34, dmSize@36,
 * dmDriverExtra@38, dmFields@40, dmBitsPerPel@104, dmPelsWidth@108,
 * dmPelsHeight@112, dmDisplayFlags@116, dmDisplayFrequency@120. */
static int u_fill_devmode(uint32_t out)
{
    if (!u_ptr_ok(out, 156u)) return 0;
    memset((void *)(uintptr_t)out, 0, 156u);
    memcpy((void *)(uintptr_t)out, u_display_name, strlen(u_display_name) + 1u);
    st16(out + 32u, 0x0400u);                  /* dmSpecVersion */
    st16(out + 34u, 0x0400u);                  /* dmDriverVersion */
    st16(out + 36u, 156u);                     /* dmSize */
    st16(out + 38u, 0u);                       /* dmDriverExtra */
    st32(out + 40u, 0x001C0000u);              /* DM_BITSPERPEL|PELSWIDTH|
                                                  PELSHEIGHT|DISPLAYFREQUENCY */
    st32(out + 104u, 32u);                     /* dmBitsPerPel */
    st32(out + 108u, U_SCREEN_W);              /* dmPelsWidth */
    st32(out + 112u, U_SCREEN_H);              /* dmPelsHeight */
    st32(out + 116u, 0u);                      /* dmDisplayFlags */
    st32(out + 120u, 60u);                     /* dmDisplayFrequency */
    return 1;
}

/* BOOL EnumDisplaySettingsA(LPCSTR, DWORD, DEVMODEA*) -- stdcall, cleans 12 bytes.
 * [real] There is only one mode: ENUM_CURRENT_SETTINGS(0xFFFFFFFF) or 0 returns
 * 1280x720@60 32bpp; any other index returns FALSE honestly (there are no more modes). */
static void u_EnumDisplaySettingsA(CPU *__restrict c)
{
    uint32_t mode = uA(c, 1);
    uint32_t out  = uA(c, 2);

    (void)uA(c, 0);
    if (mode != 0xFFFFFFFFu && mode != 0u) { c->eax = 0; uR(c, 12u); return; }
    if (!u_fill_devmode(out)) { c->eax = 0; uR(c, 12u); return; }
    c->eax = 1;
    uR(c, 12u);
}

/* BOOL EnumDisplaySettingsExA(LPCSTR, DWORD, DEVMODEA*, DWORD) -- stdcall, cleans 16 bytes.
 * [real] Same as above; EDS_RAWMODE offers no extra modes (we only have the desktop mode), so the behavior matches. */
static void u_EnumDisplaySettingsExA(CPU *__restrict c)
{
    uint32_t mode = uA(c, 1);
    uint32_t out  = uA(c, 2);

    (void)uA(c, 0);
    (void)uA(c, 3);
    if (mode != 0xFFFFFFFFu && mode != 0u) { c->eax = 0; uR(c, 16u); return; }
    if (!u_fill_devmode(out)) { c->eax = 0; uR(c, 16u); return; }
    c->eax = 1;
    uR(c, 16u);
}

/* LONG ChangeDisplaySettingsExA(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID) -- cleans 20 bytes.
 * [real] On Android the surface size is decided by the host, and there is no switchable display mode; but
 * "1280x720" is exactly the state we are already in, so return DISP_CHANGE_SUCCESSFUL(0).
 * Requesting another mode returns 0 as well, noting that this is only self-consistent while the graphics backend outputs 1280x720.
 * An invalid lpDevMode pointer returns DISP_CHANGE_BADMODE(-2). */
static void u_ChangeDisplaySettingsExA(CPU *__restrict c)
{
    uint32_t mode = uA(c, 1);

    (void)uA(c, 0);
    (void)uA(c, 2);
    (void)uA(c, 3);
    (void)uA(c, 4);
    if (mode && !u_ptr_ok(mode, 156u)) { c->eax = 0xFFFFFFFEu; uR(c, 20u); return; }
    c->eax = 0;                                /* DISP_CHANGE_SUCCESSFUL */
    uR(c, 20u);
}

/* BOOL SystemParametersInfoA(UINT, UINT, PVOID, UINT) -- stdcall, cleans 16 bytes.
 * [real] Only the actions the game actually uses are implemented; the rest return FALSE honestly (with a one-time warning, so a
 * "silent success" cannot leave later behavior unexplained):
 *   SPI_GETFOREGROUNDLOCKTIMEOUT(0x2000) -> write 200000 (the Windows default)
 *   SPI_SETFOREGROUNDLOCKTIMEOUT(0x2001) -> remember the new value (pvParam is a value, not a pointer)
 *   SPI_GETWORKAREA(0x0030)              -> a 1280x720 RECT (no taskbar)
 *   SPI_GET/SETSCREENSAVEACTIVE/TIMEOUT  -> no screensaver: write 0 / accept
 * Repentance reads and rewrites this foreground lock timeout when a window is activated; with no desktop there is nothing to arbitrate, but GET/SET
 * round-trips must be self-consistent, or the game will think its change failed. */
static void u_SystemParametersInfoA(CPU *__restrict c)
{
    uint32_t action  = uA(c, 0);
    uint32_t uiparam = uA(c, 1);
    uint32_t param   = uA(c, 2);
    uint32_t flags   = uA(c, 3);

    (void)uiparam;                             /* these actions require uParam = 0 */
    (void)flags;

    switch (action) {
    case U_SPI_GETFOREGROUNDLOCKTIMEOUT:
        c->eax = u_w32(param, g_foreground_lock_timeout) ? 1u : 0u;
        break;
    case U_SPI_SETFOREGROUNDLOCKTIMEOUT:
        g_foreground_lock_timeout = param;     /* per the generic API docs: this is a value */
        c->eax = 1;
        break;
    case U_SPI_GETWORKAREA:
        if (u_ptr_ok(param, 16u)) {
            st32(param + 0u, 0u);
            st32(param + 4u, 0u);
            st32(param + 8u, U_SCREEN_W);
            st32(param + 12u, U_SCREEN_H);
            c->eax = 1;
        } else {
            c->eax = 0;
        }
        break;
    case U_SPI_GETSCREENSAVEACTIVE:
        c->eax = u_w32(param, 0u) ? 1u : 0u;    /* no screensaver */
        break;
    case U_SPI_SETSCREENSAVEACTIVE:
        c->eax = 1;                             /* accept it, but it never fires */
        break;
    case U_SPI_GETSCREENSAVETIMEOUT:
        c->eax = u_w32(param, 0u) ? 1u : 0u;
        break;
    case U_SPI_SETSCREENSAVETIMEOUT:
        c->eax = 1;
        break;
    default:
        if (!g_warned_spi) {
            g_warned_spi = 1;
            fprintf(stderr, "[user32] SystemParametersInfoA: unimplemented action 0x%x;"
                            "returning FALSE\n", action);
        }
        c->eax = 0;                             /* fail honestly, do not pretend success */
        break;
    }
    uR(c, 16u);
}

/* HDC GetDC(HWND) -- stdcall, cleans 4 bytes.
 * [stub] There is no real GDI surface to hand out; return the opaque token 0x0000A001 (non-zero, and the guest only
 * passes it back to ReleaseDC). Real drawing goes through the GDI32/OPENGL32 layer, not through here. */
static void u_GetDC(CPU *__restrict c)
{
    (void)uA(c, 0);
    c->eax = U_HDC_TOKEN;
    uR(c, 4u);
}

/* int ReleaseDC(HWND, HDC) -- stdcall, cleans 8 bytes.
 * [real] Our DC is a token, so there is no resource to free; return 1 = "released" (the postcondition holds). */
static void u_ReleaseDC(CPU *__restrict c)
{
    (void)uA(c, 0);
    (void)uA(c, 1);
    c->eax = 1;
    uR(c, 8u);
}

/* ============================ ⑥ input ================================== */

/* Forward declaration of the external input bridge (the implementation is farther down, before GetAsyncKeyState). */
static int u_key_down(uint32_t vk);

/* SHORT GetKeyState(int) -- stdcall, cleans 4 bytes.
 * [real] There is no keyboard/mouse input source, so the key state is always "up, not toggled" = 0.
 * 0 is the true state rather than a faked high bit (0x8000 would be read as "pressed"). */
static void u_GetKeyState(CPU *__restrict c)
{
    uint32_t vk = uA(c, 0);
    c->eax = u_key_down(vk) ? 0x8000u : 0u;
    uR(c, 4u);
}

/* ==========================================================================
 * External input bridge (2026-11-04)
 *
 * Why it is needed
 * ----------
 * DirectInput's CreateDevice always returns DIERR_DEVICENOTREG (see host_dinput.c),
 * so the game cannot get a DInput keyboard/gamepad device and can only use the Win32 GetAsyncKeyState path.
 * And this stub used to return 0 unconditionally -- the game **could not see any input at all**:
 *   · press any key to skip the intro movie → no reaction
 *   · main menu PRESS START → cannot get in
 *   · gamepad/keyboard entirely dead
 *
 * On Android /dev/input requires root (measured as Permission denied under Termux),
 * and Java-level KeyEvent/MotionEvent can only be captured in-process; a child process cannot read them.
 * So we use the plainest possible bridge: the Java side writes key states line by line into TCP:127.0.0.1:7137,
 * and a thread on the host side receives them and updates the state table, which GetAsyncKeyState then queries.
 *
 * Protocol (plain text, one record per line, fault-tolerant parsing)
 * ----------------------------------
 *      <VK in decimal> <1|0>\n     press/release, e.g. "32 1" = space pressed
 *      R\n                    clear all keys (used when the gamepad disconnects / the Activity pauses)
 *
 * Why "state" and not "events": GetAsyncKeyState is level semantics (is it held right now), and
 * the game polls every frame. Sending events would make the host maintain a queue and edges, which is more error-prone.
 *
 * Thread safety: the state table is a volatile uint8_t[256]; a single-byte write is naturally atomic,
 * and the reading side only reads, so no lock is needed. This is the least error-prone part of the whole design -- deliberately so.
 * ========================================================================== */
#define IN_BRIDGE_PORT 7137
static volatile unsigned char u_key_state[256];
static int u_bridge_listen = -1;

/* ---- gamepad state (used by XInput) ----
 *
 * 2026-11-06: the game's main input path is XInput, not the DInput keyboard.
 * The Java side sends the **complete state** of the virtual gamepad as this line:
 *
 *     G <buttons in hex> <lx> <ly> <rx> <ry> <lt> <rt>\n
 *
 * Axes are -32768..32767 (Y positive upward, matching XInput), triggers 0..255.
 * host_xinput.c reads it through u_pad_get(). A mutex is used rather than a volatile array,
 * because a single read takes 7 fields and must be a consistent snapshot (sticks and buttons cannot come from two frames). */
static pthread_mutex_t u_pad_mu = PTHREAD_MUTEX_INITIALIZER;

/* Active connection count. Several Activity instances may connect at once (when the app is reopened the old connection
 * has not timed out yet), so on disconnect we must know whether this was the last one. */
static pthread_mutex_t u_bridge_mu = PTHREAD_MUTEX_INITIALIZER;
static int u_bridge_conns;

/* ★★ 2026-11-07: delayed clearing.
 *
 * Device symptom: the gamepad "cannot input correctly no matter how many times you press", with 728 reconnects in app.log.
 *
 * The cause is `u_pad_clear()` on disconnect: the **gap** in Java's reconnect (close the old
 * socket → open a new one → write the first line) is only a few tens of milliseconds, but the moment the old connection
 * drops, conns hits zero, so the freshly set sticks/buttons are wiped wholesale, and on the next frame the game
 * reads "everything released". The user wants to hold left, but the actual effect is dozens of releases per second.
 *
 * Fix: on disconnect **only record the time, do not clear the table**; the real clearing is left to a grace period:
 * only after 1500 ms with no connection at all is everything treated as released. */
static uint32_t u_bridge_zero_at;      /* time the last connection dropped (ms) */

static void u_bridge_tick(void);       /* defined after u_pad_clear */
static struct {
    unsigned buttons;
    int lx, ly, rx, ry;   /* -32768..32767 */
    int lt, rt;           /* 0..255 */
} u_pad;

int u_pad_get(unsigned *buttons, int *lx, int *ly, int *rx, int *ry,
              int *lt, int *rt)
{
    u_bridge_tick();   /* clear only after the grace period expires, not on disconnect */
    pthread_mutex_lock(&u_pad_mu);
    if (buttons) *buttons = u_pad.buttons;
    if (lx) *lx = u_pad.lx;
    if (ly) *ly = u_pad.ly;
    if (rx) *rx = u_pad.rx;
    if (ry) *ry = u_pad.ry;
    if (lt) *lt = u_pad.lt;
    if (rt) *rt = u_pad.rt;
    pthread_mutex_unlock(&u_pad_mu);
    return 1;   /* the virtual gamepad always exists; with no data ever received the state is all zeros */
}

static void u_pad_clear(void)
{
    pthread_mutex_lock(&u_pad_mu);
    memset(&u_pad, 0, sizeof u_pad);
    pthread_mutex_unlock(&u_pad_mu);
}

/* Really clear only once the grace period has ended. Both u_pad_get and host_key_table call this.
 *
 * Why it is deferred: see the comment on u_bridge_zero_at. Nothing is cleared within 1500ms of a disconnect,
 * so Java's reconnect gap fits inside the grace period. */
static void u_bridge_tick(void)
{
    int conns;
    uint32_t at, now;

    pthread_mutex_lock(&u_bridge_mu);
    conns = u_bridge_conns;
    at = u_bridge_zero_at;
    pthread_mutex_unlock(&u_bridge_mu);

    if (conns > 0 || at == 0) return;
    now = u_now_ms();
    if ((uint32_t)(now - at) < 1500u) return;   /* inside the grace period: keep the last state */

    pthread_mutex_lock(&u_bridge_mu);
    if (u_bridge_conns == 0 && u_bridge_zero_at == at) {
        u_bridge_zero_at = 0;
        pthread_mutex_unlock(&u_bridge_mu);
        memset((void *)u_key_state, 0, sizeof u_key_state);
        u_pad_clear();
        fprintf(stderr, "[input] no connection for over 1500ms; pad and keyboard state cleared\n");
        fflush(NULL);
        return;
    }
    pthread_mutex_unlock(&u_bridge_mu);
}

/* The active connection count and the grace-period state are defined next to u_pad_mu (see the comment on u_bridge_zero_at). */

/* High-bit convention for GET_ASYNC_KEY_STATE:
 *   0x8000 = currently pressed, 0x0001 = pressed since the last query ("just pressed").
 * Both are set here, because the game may poll the level or the edge. */
/* ==========================================================================
 * Per-connection service body (added 2026-11-06)
 *
 * ★ Why it had to be split out: it used to be "after accept, block in read on this same thread until disconnect",
 * i.e. **single-threaded serial accept**.
 *
 * Consequences seen on device (run.log had 7 "connection established" lines, app.log said connected,
 * yet the native side's "pad state" received 0 records):
 *
 *   · Java opens a new socket on every reconnect and does **not close** the old one; inOut points at the newest
 *   · native is stuck in read on the first connection and can never get back to accept
 *   · so data is written to the N-th socket while native reads the 1st -- it can never arrive
 *
 * Now: as soon as a fd is accepted it is handed to a detached thread, and the main loop goes straight back to accept.
 * Whichever connection sends data, we receive it, regardless of connection order.
 * ========================================================================== */
static void *u_bridge_serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    pthread_mutex_lock(&u_bridge_mu);
    u_bridge_conns++;
    int nconn = u_bridge_conns;
    pthread_mutex_unlock(&u_bridge_mu);
    /* ★ 2026-11-07: rate limiting. The device once saw 728 connect/disconnect cycles, and this log is
     * written to /sdcard (FUSE) with an fflush every time -- synchronous, millisecond-scale writes.
     * The logging alone can slow the main loop down. The first 20 are enough for diagnosis, then stay silent. */
    {
        static unsigned shown_conn;
        if (shown_conn++ < 20u) {
            fprintf(stderr, "[input] connection established (%d now, %u total)\n",
                    nconn, shown_conn);
            fflush(NULL);
        }
    }

    char line[64];
    size_t len = 0;
    for (;;) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        /* ★★ 2026-11-07: EINTR must be retried, not treated as a disconnect.
         *
         * Device symptom: `input bridge connected` appeared 131 times in app.log,
         * reconnecting once per touch (each reconnect round costs a 500 ms timeout).
         *
         * The root cause is right here: signals can arrive in this process at any time (SIGPROF sampling,
         * ART's RCU/GC signals, our crash handler machinery...),
         * so read() gets interrupted and returns -1 with errno == EINTR.
         * The old `if (n <= 0) break;` treated EINTR as the peer closing -- it broke out,
         * closed the fd. Java's next write then got EPIPE, hence the reconnect.
         *
         * This also explains that long-standing puzzle in the handover document:
         * "124 connections, native received 0 records" -- it was never the JVM's fault.
         *
         * Correct semantics:
         *   n > 0   data available
         *   n == 0  peer closed normally → break
         *   n < 0   retry on EINTR; break only on other errors */
        if (n < 0) {
            if (errno == EINTR) continue;
            /* ★ 2026-11-07: diagnostics. With 728 reconnects we must determine whether it was "peer closed" or a "read error". */
            {
                static unsigned shown_err;
                if (shown_err++ < 30u)
                    fprintf(stderr, "[input] ✗ read error fd=%d n=%zd errno=%d(%s)"
                            " (%u total)\n",
                            fd, n, errno, strerror(errno), shown_err);
            }
            break;
        }
        if (n == 0) {
            /* Peer closed normally. If "remaining" drops to zero quickly the whole key table gets cleared,
             * and that is the direct cause of "pressed but nothing happens". */
            static unsigned shown_eof;
            if (shown_eof++ < 30u)
                fprintf(stderr, "[input] peer closed fd=%d (%u total)\n",
                        fd, shown_eof);
            break;
        }
        if (ch == '\n') {
            line[len] = 0;
            if (len && (line[0] == 'R' || line[0] == 'r')) {
                memset((void *)u_key_state, 0, sizeof u_key_state);
                u_pad_clear();
                fprintf(stderr, "[input] reset received\n");
                fflush(NULL);
            } else if (len && (line[0] == 'G' || line[0] == 'g')) {
                /* pad state line: G <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt> */
                unsigned b = 0;
                int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
                int got = sscanf(line + 1, "%x %d %d %d %d %d %d",
                                 &b, &lx, &ly, &rx, &ry, &lt, &rt);
                if (got >= 1) {
                    pthread_mutex_lock(&u_pad_mu);
                    u_pad.buttons = b & 0xFFFFu;
                    u_pad.lx = lx; u_pad.ly = ly;
                    u_pad.rx = rx; u_pad.ry = ry;
                    u_pad.lt = lt; u_pad.rt = rt;
                    pthread_mutex_unlock(&u_pad_mu);
                    static unsigned padshown;
                    if (padshown++ < 20u) {
                        fprintf(stderr, "[input] pad state btn=%04x "
                                "L=%d,%d R=%d,%d LT=%d RT=%d (%u total)\n",
                                (unsigned)(b & 0xFFFFu), lx, ly, rx, ry,
                                lt, rt, padshown);
                        fflush(NULL);
                    }
                }
            } else {
                char *end;
                long vk = strtol(line, &end, 10);
                while (*end == ' ') end++;
                int on = (*end == '1');
                if (vk >= 0 && vk < 256) {
                    u_key_state[vk] = (unsigned char)on;
                    static unsigned shown;
                    if (shown++ < 40u) {
                        fprintf(stderr, "[input] vk=%ld %s (%u total)\n",
                                vk, on ? "pressed" : "released", shown);
                        fflush(NULL);
                    }
                }
            }
            len = 0;
        } else if (len < sizeof line - 1) {
            line[len++] = ch;
        } else {
            len = 0;   /* discard over-long lines outright, so a desynchronized stream cannot stall forever */
        }
    }
    close(fd);

    pthread_mutex_lock(&u_bridge_mu);
    if (u_bridge_conns > 0) u_bridge_conns--;
    int still = u_bridge_conns;
    if (still == 0) u_bridge_zero_at = u_now_ms();   /* start the grace period */
    pthread_mutex_unlock(&u_bridge_mu);
    {
        static unsigned shown_disc;
        if (shown_disc++ < 20u) {
            fprintf(stderr, "[input] connection closed (%d remaining, %u total)\n",
                    still, shown_disc);
            fflush(NULL);
        }
    }
    /* ★★ 2026-11-07: this used to call u_pad_clear() and clear the key table right here.
     *
     * Consequence: during the Java reconnect gap (close old → open new → write the first line, tens of milliseconds)
     * conns briefly hits zero, so the freshly set sticks/buttons get erased.
     * The user holds the direction, yet the game sees dozens of "releases" per second -- the gamepad is completely dead.
     *
     * Now we only record the time and leave the real clearing to u_bridge_tick(): only with no
     * connection at all is anything cleared. The reconnect gap fits entirely inside the grace period. */
    return NULL;
}

static void u_bridge_thread(void *arg)
{
    (void)arg;
    struct sockaddr_in addr;
    int one = 1;
    u_bridge_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (u_bridge_listen < 0) {
        fprintf(stderr, "[input] ✗ socket() failed errno=%d (%s)\n",
                errno, strerror(errno));
        fflush(NULL);
        return;
    }
    setsockopt(u_bridge_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(IN_BRIDGE_PORT);
    if (bind(u_bridge_listen, (struct sockaddr *)&addr, sizeof addr) != 0) {
        /* This necessarily fails when Android lacks the INTERNET permission -- the gamepad becomes completely dead */
        fprintf(stderr, "[input] ✗ bind(%d) failed errno=%d (%s)\n",
                IN_BRIDGE_PORT, errno, strerror(errno));
        fflush(NULL);
        close(u_bridge_listen); u_bridge_listen = -1; return;
    }
    /* ★ backlog raised from 1 to 8: the Java side may create several connections in a burst
     * (one at startup plus the reconnect thread triggered by a write failure), and backlog=1 would reject them outright. */
    if (listen(u_bridge_listen, 8) != 0) {
        fprintf(stderr, "[input] ✗ listen failed errno=%d (%s)\n",
                errno, strerror(errno));
        fflush(NULL);
        close(u_bridge_listen); u_bridge_listen = -1; return;
    }
    fprintf(stderr, "[input] ✓ listening on 127.0.0.1:%d\n", IN_BRIDGE_PORT);
    fflush(NULL);

    /* ★ One thread per connection; the main loop goes straight back to accept.
     * See the long comment above u_bridge_serve: serial accept leads to
     * "Java says connected, native receives 0 records". */
    for (;;) {
        int fd = accept(u_bridge_listen, NULL, NULL);
        if (fd < 0) { usleep(200000); continue; }
        {
            pthread_t t;
            if (pthread_create(&t, NULL, u_bridge_serve,
                               (void *)(intptr_t)fd) == 0) {
                pthread_detach(t);
            } else {
                close(fd);          /* if the thread cannot be started, close the fd rather than leaking it */
            }
        }
    }
}

__attribute__((constructor))
static void u_bridge_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, (void *(*)(void *))u_bridge_thread, NULL) == 0)
        pthread_detach(t);
}

static int u_key_down(uint32_t vk)
{
    if (vk >= 256u) return 0;
    return u_key_state[vk] != 0;
}

/* For the DirectInput fake device (host_dinput.c).
 *
 * ★ 2026-11-05: the game **never calls** GetAsyncKeyState (0 times measured),
 * it reads input through DirectInput's GetDeviceData. So the keyboard state table
 * must be reachable from that side -- export the whole table and let the other side diff it into events.
 *
 * ★ At the same time, merge the KEY_HELD debug injection into the table.
 *   Previously KEY_HELD only took effect in the GetAsyncKeyState branch, and the game never called
 *   that function, so "injected keys were never visible". Now it is merged at the source,
 *   and both consumers (GetAsyncKeyState and DInput) can see it.
 *   Real input (u_key_state, written by the TCP input bridge) has higher priority and is never overwritten with 0. */
/* Forward declarations: both functions are defined farther down in this file, and both are static. */
static int key_held_match(uint32_t vk);
static int key_hold_active(void);

const unsigned char *host_key_table(void)
{
    u_bridge_tick();   /* clear only after the grace period (see the comment on u_bridge_tick) */
    for (uint32_t vk = 0; vk < 256u; vk++) {
        if (key_held_match(vk) && key_hold_active())
            u_key_state[vk] = 1;
    }
    return (const unsigned char *)u_key_state;
}

/* SHORT GetAsyncKeyState(int) -- stdcall, cleans 4 bytes.
 *
 * ★ 2026-11-04: added KEY_HELD=<vk>[,<vk>...] to inject key presses.
 *
 * Why it is needed: DirectInput's CreateDevice returns DIERR_DEVICENOTREG
 * (see host_dinput.c), so the game cannot get a DInput keyboard device and can only use
 * the Win32 GetAsyncKeyState path. And this stub used to return 0 unconditionally,
 * so **the game saw no key presses at all** -- witnessed by "press any key to skip the intro"
 * producing no reaction at all, forcing every verification run to sit idle for 144 seconds.
 *
 * Return 0x8000 (high bit = currently pressed). The low byte's "pressed last time" bit is not set,
 * so this simulates "held down the whole time" rather than "just pressed"; both work for the edge-detection logic
 * that triggers the skip.
 *
 * Usage: KEY_HELD=0x20 (space) / KEY_HELD=0x1B (ESC)
 *       KEY_HELD=0x20,0x0D for several keys at once. */
static int key_held_match(uint32_t vk)
{
    static int inited;
    static uint32_t list[16];
    static int n;
    if (!inited) {
        inited = 1;
        const char *e = getenv("KEY_HELD");
        if (e) {
            const char *p = e;
            while (*p && n < 16) {
                char *end;
                unsigned long v = strtoul(p, &end, 0);
                if (end == p) break;
                list[n++] = (uint32_t)v;
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }
    for (int i = 0; i < n; i++)
        if (list[i] == vk) return 1;
    return 0;
}

/* KEY_HOLD_MS=N: a key counts as pressed only for N milliseconds after the first query, then releases automatically.
 *
 * Why it is needed: holding the key forever makes the game, after skipping the cutscene, sit in a state that waits for a "key release"
 * (measured: holding space → Cutscene advances 0 frames, and the log sticks at HQX4 size).
 * A real player "presses once and lets go", so a click has to be simulated on the host side. */
static int key_hold_active(void)
{
    static int inited, hold_ms;
    static uint64_t t_first;
    if (!inited) {
        inited = 1;
        const char *e = getenv("KEY_HOLD_MS");
        hold_ms = e ? atoi(e) : -1;      /* -1 = hold forever (old behavior) */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        t_first = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
    }
    if (hold_ms < 0) return 1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
    return (now - t_first) < (uint64_t)hold_ms;
}

static void u_GetAsyncKeyState(CPU *__restrict c)
{
    uint32_t vk = uA(c, 0);
    if (u_key_down(vk))
        c->eax = 0x8001u;      /* held + just pressed */
    else if (key_held_match(vk) && key_hold_active())
        c->eax = 0x8000u;      /* KEY_HELD debug injection */
    else
        c->eax = 0u;
    uR(c, 4u);
}

/* UINT MapVirtualKeyA(UINT uCode, UINT uMapType) -- stdcall, cleans 8 bytes.
 * [real (partial)] MAPVK_VK_TO_CHAR(2) gives the ASCII of printable characters (A-Z/0-9/space etc.),
 * and anything else returns 0 = "no translation", a legal Win32 result (not a garbage value).
 * No scan-code table is needed: with no real keyboard device, MAPVK_VK_TO_VSC(0) returns 0. */
static void u_MapVirtualKeyA(CPU *__restrict c)
{
    uint32_t vk   = uA(c, 0);
    uint32_t type = uA(c, 1);
    uint32_t ch = 0;

    if (type == 2u) {                          /* MAPVK_VK_TO_CHAR */
        if (vk >= 0x41u && vk <= 0x5Au)       ch = vk;                 /* A-Z */
        else if (vk >= 0x30u && vk <= 0x39u)  ch = vk;                 /* 0-9 */
        else if (vk >= 0x60u && vk <= 0x69u)  ch = vk - 0x60u + 0x30u; /* numeric keypad */
        else {
            switch (vk) {
            case 0x20u: ch = 0x20u; break;    /* VK_SPACE */
            case 0x0Du: ch = 0x0Du; break;    /* VK_RETURN */
            case 0x09u: ch = 0x09u; break;    /* VK_TAB */
            case 0x08u: ch = 0x08u; break;    /* VK_BACK */
            case 0x1Bu: ch = 0x1Bu; break;    /* VK_ESCAPE */
            default:    ch = 0u;   break;
            }
        }
    }
    c->eax = ch;
    uR(c, 8u);
}

/* UINT SendInput(UINT, LPINPUT, int) -- stdcall, cleans 12 bytes.
 * [stub] We have no channel for synthesized input (no real window, no Android InputQueue events).
 * But returning 0 would turn "injection failed" into a hard error, while the real semantics here are "the events were consumed but there is no
 * observer". Compromise: validate the array first (cbSize must be 28 = sizeof(INPUT), the pointer must be readable),
 * and if it is legal return cInputs to mean everything was consumed. Invalid arguments return 0 honestly (Win32 returns 0 too). */
static void u_SendInput(CPU *__restrict c)
{
    uint32_t count  = uA(c, 0);
    uint32_t inputs = uA(c, 1);
    uint32_t cbsize = uA(c, 2);

    if (cbsize != 28u) { c->eax = 0; uR(c, 12u); return; }
    if (count != 0u && (!inputs || !u_ptr_ok(inputs, count * 28u))) {
        c->eax = 0;
        uR(c, 12u);
        return;
    }
    c->eax = count;
    uR(c, 12u);
}

/* BOOL TrackMouseEvent(LPTRACKMOUSEEVENT) -- stdcall, cleans 4 bytes.
 * [stub] Accept the registration (cbSize must be == 16, hwndTrack must be one of our windows) and return TRUE;
 * but since we never produce mouse messages, WM_MOUSELEAVE is never posted either. For an environment
 * with no input source this is self-consistent: the event never starts, so there is no end to speak of. */
static void u_TrackMouseEvent(CPU *__restrict c)
{
    uint32_t ptr = uA(c, 0);
    uint32_t cb, hwnd;

    if (!u_ptr_ok(ptr, 16u)) { c->eax = 0; uR(c, 4u); return; }
    cb   = ld32(ptr + 0u);
    hwnd = ld32(ptr + 8u);
    if (cb != 16u) { c->eax = 0; uR(c, 4u); return; }
    if (hwnd && !u_win_find(hwnd)) { c->eax = 0; uR(c, 4u); return; }
    c->eax = 1;
    uR(c, 4u);
}

/* HDEVNOTIFY RegisterDeviceNotificationA(HANDLE, LPVOID, DWORD) -- cleans 12 bytes.
 * [stub] Return a non-zero token so the device-notification registration step passes; we never post WM_DEVICECHANGE.
 * An invalid pointer returns 0 honestly. */
static void u_RegisterDeviceNotificationA(CPU *__restrict c)
{
    uint32_t filter = uA(c, 1);
    (void)uA(c, 0);
    (void)uA(c, 2);
    c->eax = (filter && u_ptr_ok(filter, 1u)) ? U_NOTIFY_TOKEN : 0u;
    uR(c, 12u);
}

/* BOOL UnregisterDeviceNotification(HDEVNOTIFY) -- stdcall, cleans 4 bytes.
 * [real] Only tokens we issued are recognized. */
static void u_UnregisterDeviceNotification(CPU *__restrict c)
{
    c->eax = (uA(c, 0) == U_NOTIFY_TOKEN) ? 1u : 0u;
    uR(c, 4u);
}

/* UINT GetRawInputDeviceList(PRAWINPUTDEVICELIST, PUINT, UINT) -- cleans 12 bytes.
 * [real] This machine has no raw input devices at all (no USB HID passthrough), so return 0
 * devices and write the count into *puiNumDevices -- this is "really none", not "we do not know".
 * GLFW's init skips gamepad initialization when it sees devCount==0, and never dereferences the empty list.
 * cbSize must equal sizeof(RAWINPUTDEVICELIST)=8, otherwise return (UINT)-1 as Win32 does. */
static void u_GetRawInputDeviceList(CPU *__restrict c)
{
    uint32_t list  = uA(c, 0);
    uint32_t count = uA(c, 1);
    uint32_t cbsize = uA(c, 2);

    (void)list;                                /* 0 devices, nothing to write */
    if (cbsize != 8u || !u_ptr_ok(count, 4u)) {
        c->eax = 0xFFFFFFFFu;                  /* (UINT)-1 */
        uR(c, 12u);
        return;
    }
    st32(count, 0u);
    c->eax = 0u;
    uR(c, 12u);
}

/* UINT GetRawInputDeviceInfoA(HANDLE, UINT, LPVOID, PUINT) -- cleans 12 bytes.
 * [fail] No device handle can ever be valid, so every query returns (UINT)-1 honestly
 * (Win32's behavior for an invalid hDevice), and the caller takes its error branch instead of reading a garbage name. */
static void u_GetRawInputDeviceInfoA(CPU *__restrict c)
{
    (void)uA(c, 0);
    (void)uA(c, 1);
    (void)uA(c, 2);
    (void)uA(c, 3);
    c->eax = 0xFFFFFFFFu;
    uR(c, 16u);
}

/* ============================ ⑦ clipboard ================================= */

/* Copy a span of guest text into a low-4GB block of our own and return the low-address handle.
 * Why copy: SetClipboardData takes an HGLOBAL, and the KERNEL32-layer GlobalLock is
 * "return the handle unchanged" (identity), so returning the caller's handle would work too; but we would rather own
 * the bytes in the clipboard (so the caller freeing its own block afterwards cannot leave GetClipboardData dangling).
 * Wide characters are counted in UTF-16 units, at most 2048 units / at most 4096 bytes. */
static uint32_t u_clip_stage(uint32_t src, int wide)
{
    uint32_t cap = wide ? 2048u : 4096u;
    uint32_t n = 0;
    uint32_t bytes;
    uint32_t dst;

    if (!src) return 0;
    while (n < cap) {
        if (!u_ptr_ok(src + (wide ? n * 2u : n), wide ? 2u : 1u)) break;
        if (wide ? (ld16(src + n * 2u) == 0u) : (ld8(src + n) == 0u)) break;
        n++;
    }
    bytes = wide ? (n + 1u) * 2u : n + 1u;
    dst = u_low_alloc(bytes);
    if (!dst) return 0;
    memset((void *)(uintptr_t)dst, 0, bytes);
    if (n) memcpy((void *)(uintptr_t)dst, (void *)(uintptr_t)src,
                  wide ? n * 2u : n);
    return dst;
}

/* BOOL OpenClipboard(HWND) -- stdcall, cleans 4 bytes.
 * [real] Single-owner model within the process: return FALSE when it is already open (same as Win32). */
static void u_OpenClipboard(CPU *__restrict c)
{
    (void)uA(c, 0);
    if (g_clip_open) { c->eax = 0; uR(c, 4u); return; }
    g_clip_open = 1;
    c->eax = 1;
    uR(c, 4u);
}

/* BOOL CloseClipboard(void) -- stdcall, cleans 0 bytes. */
static void u_CloseClipboard(CPU *__restrict c)
{
    g_clip_open = 0;
    c->eax = 1;
    uR(c, 0u);
}

/* BOOL EmptyClipboard(void) -- stdcall, cleans 0 bytes.
 * [real] Clear every format; return FALSE honestly when the clipboard is not open. */
static void u_EmptyClipboard(CPU *__restrict c)
{
    int i;
    if (!g_clip_open) { c->eax = 0; uR(c, 0u); return; }
    for (i = 0; i < U_CLIP_MAX; i++) {
        g_clip[i].used = 0;
        g_clip[i].fmt = 0;
        g_clip[i].handle = 0;
    }
    c->eax = 1;
    uR(c, 0u);
}

/* HANDLE GetClipboardData(UINT) -- stdcall, cleans 4 bytes.
 * [real] Return the handle registered for the format in this process; 0 when the format is absent / the clipboard is not open
 * (Windows returns NULL as well). Never return a host address. */
static void u_GetClipboardData(CPU *__restrict c)
{
    uint32_t fmt = uA(c, 0);
    int i;

    if (!g_clip_open) { c->eax = 0; uR(c, 4u); return; }
    for (i = 0; i < U_CLIP_MAX; i++) {
        if (g_clip[i].used && g_clip[i].fmt == fmt) {
            c->eax = g_clip[i].handle;
            uR(c, 4u);
            return;
        }
    }
    c->eax = 0;
    uR(c, 4u);
}

/* HANDLE SetClipboardData(UINT, HANDLE) -- stdcall, cleans 8 bytes.
 * [real] For the text formats (CF_TEXT/CF_UNICODETEXT) keep our own copy in a low-4GB block;
 * other formats just record the handle. On success return the registered handle (Win32 semantics); when the clipboard
 * is not open or the handle is 0, return 0. */
static void u_SetClipboardData(CPU *__restrict c)
{
    uint32_t fmt = uA(c, 0);
    uint32_t handle = uA(c, 1);
    uint32_t stored;
    int i, slot = -1;

    if (!g_clip_open || !handle) { c->eax = 0; uR(c, 8u); return; }
    if (fmt == U_CF_TEXT) {
        stored = u_clip_stage(handle, 0);
    } else if (fmt == U_CF_UNICODETEXT) {
        stored = u_clip_stage(handle, 1);
    } else {
        stored = handle;                        /* bitmap/custom formats: just record the handle */
    }
    if (!stored) { c->eax = 0; uR(c, 8u); return; }

    for (i = 0; i < U_CLIP_MAX; i++) {
        if (g_clip[i].used && g_clip[i].fmt == fmt) { slot = i; break; }
    }
    if (slot < 0) {
        for (i = 0; i < U_CLIP_MAX; i++)
            if (!g_clip[i].used) { slot = i; break; }
    }
    if (slot < 0) { c->eax = 0; uR(c, 8u); return; }
    g_clip[slot].used = 1;
    g_clip[slot].fmt = fmt;
    g_clip[slot].handle = stored;

    c->eax = stored;
    uR(c, 8u);
}

/* ============================== dispatch table ================================== */

typedef struct {
    const char *name;
    void (*fn)(CPU *__restrict);
} u_entry;

static const u_entry u_table[] = {
    { "USER32.dll!BringWindowToTop",              u_BringWindowToTop },
    { "USER32.dll!SetCapture",                    u_SetCapture },
    { "USER32.dll!SetCursor",                     u_SetCursor },
    { "USER32.dll!SetFocus",                      u_SetFocus },
    { "USER32.dll!ClipCursor",                    u_ClipCursor },
    { "USER32.dll!TranslateMessage",              u_TranslateMessage },
    { "USER32.dll!GetClientRect",                 u_GetClientRect },
    { "USER32.dll!PeekMessageA",                  u_PeekMessageA },
    { "USER32.dll!ReleaseCapture",                u_ReleaseCapture },
    { "USER32.dll!SetForegroundWindow",           u_SetForegroundWindow },
    { "USER32.dll!ShowCursor",                    u_ShowCursor },
    { "USER32.dll!SetCursorPos",                  u_SetCursorPos },
    { "USER32.dll!GetCursorPos",                  u_GetCursorPos },
    { "USER32.dll!DefWindowProcA",                u_DefWindowProcA },
    { "USER32.dll!LoadIconA",                     u_LoadIconA },
    { "USER32.dll!GetActiveWindow",               u_GetActiveWindow },
    { "USER32.dll!MessageBoxA",                   u_MessageBoxA },
    { "USER32.dll!GetWindowLongA",                u_GetWindowLongA },
    { "USER32.dll!LoadImageA",                    u_LoadImageA },
    { "USER32.dll!SendMessageA",                  u_SendMessageA },
    { "USER32.dll!SetWindowPos",                  u_SetWindowPos },
    { "USER32.dll!GetSystemMetrics",              u_GetSystemMetrics },
    { "USER32.dll!SetWindowLongA",                u_SetWindowLongA },
    { "USER32.dll!OpenClipboard",                 u_OpenClipboard },
    { "USER32.dll!CloseClipboard",                u_CloseClipboard },
    { "USER32.dll!EmptyClipboard",                u_EmptyClipboard },
    { "USER32.dll!GetClipboardData",              u_GetClipboardData },
    { "USER32.dll!SetClipboardData",              u_SetClipboardData },
    { "USER32.dll!SendInput",                     u_SendInput },
    { "USER32.dll!GetMessageTime",                u_GetMessageTime },
    { "USER32.dll!RegisterClassA",                u_RegisterClassA },
    { "USER32.dll!MapVirtualKeyA",                u_MapVirtualKeyA },
    { "USER32.dll!TrackMouseEvent",               u_TrackMouseEvent },
    { "USER32.dll!ClientToScreen",                u_ClientToScreen },
    { "USER32.dll!SystemParametersInfoA",         u_SystemParametersInfoA },
    { "USER32.dll!GetDC",                         u_GetDC },
    { "USER32.dll!ReleaseDC",                     u_ReleaseDC },
    { "USER32.dll!EnumDisplayDevicesA",           u_EnumDisplayDevicesA },
    { "USER32.dll!EnumDisplaySettingsExA",        u_EnumDisplaySettingsExA },
    { "USER32.dll!ChangeDisplaySettingsExA",      u_ChangeDisplaySettingsExA },
    { "USER32.dll!EnumDisplaySettingsA",          u_EnumDisplaySettingsA },
    { "USER32.dll!GetMessageA",                   u_GetMessageA },
    { "USER32.dll!RegisterDeviceNotificationA",   u_RegisterDeviceNotificationA },
    { "USER32.dll!GetRawInputDeviceInfoA",        u_GetRawInputDeviceInfoA },
    { "USER32.dll!UnregisterClassA",              u_UnregisterClassA },
    { "USER32.dll!UnregisterDeviceNotification",  u_UnregisterDeviceNotification },
    { "USER32.dll!CreateWindowExA",               u_CreateWindowExA },
    { "USER32.dll!RegisterClassExA",              u_RegisterClassExA },
    { "USER32.dll!GetRawInputDeviceList",         u_GetRawInputDeviceList },
    { "USER32.dll!AdjustWindowRectEx",            u_AdjustWindowRectEx },
    { "USER32.dll!GetKeyState",                   u_GetKeyState },
    { "USER32.dll!DispatchMessageA",              u_DispatchMessageA },
    { "USER32.dll!GetMenu",                       u_GetMenu },
    { "USER32.dll!LoadCursorA",                   u_LoadCursorA },
    { "USER32.dll!DestroyWindow",                 u_DestroyWindow },
    { "USER32.dll!WindowFromPoint",               u_WindowFromPoint },
    { "USER32.dll!ShowWindow",                    u_ShowWindow },
    { "USER32.dll!SetClassLongA",                 u_SetClassLongA },
    { "USER32.dll!GetAsyncKeyState",              u_GetAsyncKeyState },
};

#define U_NTBL ((int)(sizeof u_table / sizeof u_table[0]))

/* The import list is a hard constraint: drop one and the game will miss a symbol on some startup path. Pinned at compile time. */
_Static_assert(sizeof u_table / sizeof u_table[0] == 59u,
               "USER32 dispatch table must cover exactly 59 imports");

/* On a hit, run it, write c->eax, clean the stack and return 1; on a miss return 0 (the framework reports the symbol name). */
int host_user32_dispatch(CPU *__restrict c, const char *name)
{
    int i;
    if (!c || !name) return 0;
    for (i = 0; i < U_NTBL; i++) {
        if (strcmp(u_table[i].name, name) == 0) {
            u_table[i].fn(c);
            return 1;
        }
    }
    return 0;
}

int host_user32_count(void)
{
    return U_NTBL;
}
