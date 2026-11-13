/* host_user32.c —— Android 宿主层的 USER32.dll 实现（全部 59 个静态导入）
 *
 * 背景
 * ----
 * 游戏是 PC x86 PE32，经 repentogxm 静态翻译成 C 再编成 arm64。guest 的指针是
 * uint32_t，所以任何交给 guest 使用的地址都必须落在低 4GB；本文件只用三种地址：
 *   1) guest 自己传进来的缓冲区（我们只写它，地址原样仍是 32 位）；
 *   2) 我们自己 mmap 在 0x3B000000 附近的低 4GB 区块（见 u_low_alloc）；
 *   3) 小整数"句柄令牌"（HWND/HDC/HICON/...），guest 只把它们当不透明值回传。
 *
 * 调用约定
 * --------
 * 59 个符号全是 WINAPI = stdcall：调用者压参数，被调者用 ret N 清栈。
 *   uA(c, i)  = 第 i 个参数（c->esp 指向返回地址）
 *   uR(c, n)  = 弹出返回地址并清 n 字节参数
 * 每个函数末尾的 uR 字节数按 Win32 真实签名写死，并在函数头注释里标了签名。
 * 清理字节数错一个就会让栈失衡、崩在很远的地方，所以每个数字都要能对着签名核。
 *
 * 这一层刻意不做的事
 * ------------------
 * 不创建 Android 窗口 / EGL surface。目标是让游戏继续往下跑，看到它下一步要什么。
 * 因此：
 *   · 窗口是"虚拟窗口"：CreateWindowExA 记一笔账（外框矩形、style、wndproc），
 *     GetClientRect / SetWindowPos / ShowWindow / GetWindowLongA 都操作这本账，
 *     返回的是自洽的真实数据，而不是常量垃圾。
 *   · 没有任何消息产生：PeekMessageA 返回 0（没有消息），GetMessageA 按任务要求
 *     返回 1 + 填 WM_NULL（永远不返回 0，因为 0 == WM_QUIT 会让消息循环退出、
 *     游戏直接关窗），并睡 RGXM_USER32_IDLE_US 微秒避免 100% 空转。
 *   · 基本不调用 guest WndProc：我们从不合成需要窗口过程处理的消息（WM_NULL 的
 *     默认处理就是"什么也不做"），所以 DispatchMessageA 直接返回 0。
 *
 * 三类处理（每个函数头注释里注明属于哪类）
 *   [真实] 真的算/真的存/真的填结构
 *   [桩]   返回一个能让流程继续的合法值，并说明为什么这个值不会骗到 guest
 *   [失败] 如实返回失败（0 / -1），让调用方走它的错误分支
 *
 * 命名：文件内所有静态符号都带 u 前缀。这样万一将来有人把本文件 #include 进
 * host_android.c（那里已经有 static 的 A/R/low_alloc），不会撞符号。语义仍与
 * 现有代码的 A/R 完全一致。
 *
 * 参数字节数总表（供审计）：
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
 *   WindowFromPoint 8 字节按值 ->8          ShowWindow 2->8
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

/* MAP_FIXED_NOREPLACE 是 Linux 4.17 才有的；老内核头文件里没有这个宏。
 * 缺了就自己给一个同值的定义（0x100000），这样即使在旧平台上本文件也能编译。
 * 注意：如果内核真的不支持这个 flag，mmap 会返回 EINVAL，u_low_alloc 会
 * 往后挪并最终如实返回 0 —— 不会静默覆盖别人已经映射的区块。 */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* 没有消息时 GetMessageA 的让步时间（微秒）。1ms => 最多 ~1000 圈/秒。
 * 用 -DRGXM_USER32_IDLE_US=0 可以关掉，让循环全力空转。 */
#ifndef RGXM_USER32_IDLE_US
#define RGXM_USER32_IDLE_US 1000
#endif

/* ---------------------------------------------------------------- 调用约定 -- */

/* 第 i 个参数。stdcall：c->esp 指向返回地址，参数从 esp+4 开始。 */
static uint32_t uA(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}

/* stdcall 返回：弹出返回地址 + 清 n 字节参数。 */
static void uR(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ------------------------------------------------------------ 指针合法性 -- */

/* guest 指针只保证是 32 位；截断、0、越界都要挡住，不然写 4 字节就崩。
 * 这里不验证"是否已映射"（那要查 /proc/self/maps），但截断和空指针是历史上
 * 真正出过事的两类，必须先挡。 */
static int u_ptr_ok(uint32_t p, uint32_t n)
{
    if (p < 0x10000u) return 0;              /* 空指针与 DOS 页 */
    if (n > 0xFFFFFFFFu - p) return 0;       /* 32 位回绕 */
    if (p > 0xFFFF0000u) return 0;
    return 1;
}

static int u_w32(uint32_t p, uint32_t v)
{
    if (!u_ptr_ok(p, 4u)) return 0;
    st32(p, v);
    return 1;
}

/* 读 guest 的 C 字符串到宿主缓冲。返回 1 表示读到了完整的 NUL 结尾串。
 * 逐字节检查范围：调用方给的指针可能正好贴着映射末尾。 */
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
    return 0;                                 /* 没有结尾 NUL，或超长 */
}

static uint32_t u_hash_name(const char *s)
{
    uint32_t h = 2166136261u;                 /* FNV-1a，只要稳定就够 */
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

/* ------------------------------------------------------- 低 4GB 内存分配器 -- */

/* 地址规划（见 CONTEXT_FOR_AGENTS.md）：镜像 0x3c000000、TIB/TLS 0x32000000、
 * 栈 0x40000000、CRT 堆 0x48000000、Lua 堆 0x50000000。
 * 我们取 0x3B000000 —— 在 TIB 区上沿与栈之间，且与 host_android.c 的
 * 0x38000000 区分开，避免两个层的 bump 指针互相踩。
 * MAP_FIXED_NOREPLACE：如果这块已经被别人映射了，mmap 会失败而不是覆盖，
 * 于是往后挪 64KB 再试；全部失败就返回 0（调用方必须处理 0，绝不返回一个
 * 高地址再让 guest 截断成垃圾）。 */
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

/* ------------------------------------------------------------ Win32 常量 -- */

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
#define U_GWL_USERDATA       (-21)    /* GWLP_USERDATA：GLFW 存 window 指针 */

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

/* 虚拟句柄令牌：小整数，非 0，guest 只会原样回传给我们。
 * 绝不能是宿主指针 —— 那会在 0x5xxxxxxxxx，截断成 uint32_t 就是垃圾。 */
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

/* ------------------------------------------------------- 虚拟窗口 / 类表 -- */

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
    int32_t  win_x, win_y;        /* 外框左上角（屏幕坐标） */
    int32_t  win_cx, win_cy;      /* 外框尺寸 */
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

/* 与 AdjustWindowRectEx 共用的边框度量。窗口是虚拟的，但尺寸账必须自洽：
 * GLFW 用 AdjustWindowRectEx 由客户区算外框，再把外框传给 CreateWindowExA，
 * 我们在这里做同一套算术的逆运算，GetClientRect 才能还原成游戏要的 1280x720。 */
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

/* 客户区尺寸：对外框做 AdjustWindowRectEx 的逆运算。 */
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

/* 其余全局状态 */
static uint32_t g_capture_hwnd;
static uint32_t g_focus_hwnd;
static uint32_t g_active_hwnd;
static uint32_t g_cursor_handle;
static int32_t  g_cursor_x = 640, g_cursor_y = 360;   /* 屏幕中心 */
static int      g_show_cursor_count;                  /* Win32 的显示计数器 */
static int      g_clip_open;
static uint32_t g_foreground_lock_timeout = 200000u;  /* Windows 默认值 */
static int      g_warned_spi;
static int      g_warned_unknown_class;

#define U_CLIP_MAX 8
static struct { int used; uint32_t fmt; uint32_t handle; } g_clip[U_CLIP_MAX];

/* 屏幕参数：整个宿主层按 1280x720 设计（游戏的目标分辨率）。 */
#define U_SCREEN_W 1280
#define U_SCREEN_H 720

static const char *const u_display_name = "\\\\.\\DISPLAY1";
static const char *const u_display_desc = "Android virtual display";

/* ============================ ① 消息 ==================================== */

/* BOOL PeekMessageA(LPMSG, HWND, UINT, UINT, UINT) — stdcall，清 20 字节。
 * [真实] 我们没有任何消息源（没有窗口、没有键鼠注入），所以"队列是空的"是
 * 事实而不是谎报：返回 FALSE，且按 Win32 语义不碰 lpMsg（调用者只在 TRUE 时
 * 才读它）。GLFW 的 `while (PeekMessageA(...))` 因此立刻退出，不做任何事。 */
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

/* BOOL GetMessageA(LPMSG, HWND, UINT, UINT) — stdcall，清 16 字节。
 * [权衡] 两个选项：
 *   (a) 返回 0 —— Win32 语义是 WM_QUIT，glfwWindowShouldClose 立刻变真，
 *       游戏退出。绝对不能用。
 *   (b) 返回 1 + 一条 WM_NULL —— 消息循环继续，DispatchMessageA(WM_NULL) 是
 *       定义上的空操作，游戏不会看到假事件。
 * 选 (b)。代价是没有消息时循环会立即再转一圈，所以这里睡
 * RGXM_USER32_IDLE_US 微秒（默认 1ms），把空转压到 ~1000 圈/秒，既不占满
 * CPU 也不至于让 glfwWaitEvents 饿死渲染。
 * MSG 结构（32 位，28 字节）：hwnd@0 message@4 wParam@8 lParam@12 time@16
 * pt.x@20 pt.y@24。time 用单调毫秒，与 GetMessageTime 同一个时基。 */
static void u_GetMessageA(CPU *__restrict c)
{
    uint32_t msg = uA(c, 0);
    struct timespec ts;

    (void)uA(c, 1);                            /* hWnd 过滤：没有消息 */
    (void)uA(c, 2);                            /* wMsgFilterMin */
    (void)uA(c, 3);                            /* wMsgFilterMax */

    if (u_ptr_ok(msg, 28u)) {
        memset((void *)(uintptr_t)msg, 0, 28u);
        st32(msg + 0u, g_active_hwnd);          /* 归属窗口 */
        st32(msg + 4u, U_WM_NULL);              /* WM_NULL：空操作消息 */
        st32(msg + 8u, 0u);
        st32(msg + 12u, 0u);
        st32(msg + 16u, u_now_ms());
        st32(msg + 20u, (uint32_t)g_cursor_x);
        st32(msg + 24u, (uint32_t)g_cursor_y);
    }

    ts.tv_sec = 0;
    ts.tv_nsec = (long)RGXM_USER32_IDLE_US * 1000L;
    (void)nanosleep(&ts, NULL);

    c->eax = 1;                                /* 有消息：循环不要退出 */
    uR(c, 16u);
}

/* BOOL TranslateMessage(const MSG*) — stdcall，清 4 字节。
 * [真实] 没有消息被翻译过，如实返回 FALSE。我们从不产生 WM_KEYDOWN，
 * 所以也不会有 WM_CHAR 需要投递。 */
static void u_TranslateMessage(CPU *__restrict c)
{
    (void)uA(c, 0);
    c->eax = 0;
    uR(c, 4u);
}

/* LRESULT DispatchMessageA(const MSG*) — stdcall，清 4 字节。
 * [桩] 返回 0。理由：唯一可能到达这里的是 GetMessageA 合成的 WM_NULL，
 * 而 WM_NULL 的窗口过程处理就是"返回 0、什么都不做"。我们因此不需要回调
 * guest WndProc —— 在一个没有真实输入的世界里调用它只会给游戏送假事件。
 * 将来真要投递消息时，改这一处（wndproc 已登记在 u_vcls / u_vwin 里）即可。 */
static void u_DispatchMessageA(CPU *__restrict c)
{
    (void)uA(c, 0);
    c->eax = 0;
    uR(c, 4u);
}

/* LONG GetMessageTime(void) — stdcall，清 0 字节。
 * [真实] 没有消息队列，但"上一条消息的时间"如果返回 0，调用方做时间差会得到
 * 巨大的负数/回绕。返回单调时钟毫秒，保证单调不减。 */
static void u_GetMessageTime(CPU *__restrict c)
{
    c->eax = u_now_ms();
    uR(c, 0u);
}

/* LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) — stdcall，清 16 字节。
 * [桩] 返回 0。我们从不把消息转给 guest 的 WndProc，所以这个默认过程实际上
 * 不会被调用；若将来被调用，0 是 WM_NULL/WM_SETICON/WM_SIZE 等消息的默认结果。
 * 提醒后来的实现者：一旦开始真的调用 WndProc，WM_NCCREATE 必须返回 TRUE，
 * 否则窗口创建会被判失败。 */
static void u_DefWindowProcA(CPU *__restrict c)
{
    (void)uA(c, 0);
    (void)uA(c, 1);
    (void)uA(c, 2);
    (void)uA(c, 3);
    c->eax = 0;
    uR(c, 16u);
}

/* LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM) — stdcall，清 16 字节。
 * [桩] 返回 0 = 未处理。我们不会同步调用 guest WndProc（那需要 guest_call 和
 * 正确的调用帧），所以任何同步查询都得到"默认值"；游戏用它设置图标
 * (WM_SETICON) 时 0 正是正确返回值。其它消息打到 stderr，方便看到游戏想说什么。 */
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

/* int MessageBoxA(HWND, LPCSTR, LPCSTR, UINT) — stdcall，清 16 字节。
 * [桩] 真弹一个 Android 对话框会让无头运行卡死；但静默返回 IDOK 会丢掉唯一的
 * 诊断信息。折中：把标题和正文打到 stderr，并按按钮类型返回"用户点了第一个
 * 肯定按钮"（MB_OK->IDOK，MB_YESNO->IDYES），让流程继续。
 * 按钮位是 type & 0x0F：0=OK 1=OKCANCEL 3=YESNOCANCEL 4=YESNO。 */
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

/* ============================ ② 窗口创建/类 ============================= */

/* HWND CreateWindowExA(DWORD, LPCSTR, LPCSTR, DWORD, int,int,int,int,
 *                      HWND, HMENU, HINSTANCE, LPVOID) — stdcall，清 48 字节。
 * [桩+真实] 建一个虚拟窗口：句柄是非 0 的小整数令牌（U_HWND_BASE 起），
 * 同时记下外框矩形、style/exstyle、类 atom、菜单、wndproc，后续
 * GetClientRect / SetWindowPos / ShowWindow / GetWindowLongA / GetMenu 都从
 * 这本账里读，所以游戏拿到的尺寸和样式是自洽的数据，不是常量垃圾。
 * 客户区尺寸按 AdjustWindowRectEx 的逆运算还原：GLFW 传进来的是外框尺寸
 * （它先算过 AdjustWindowRectEx），减掉边框/标题/菜单后正好是 1280x720。
 * 类名没注册过时照建（wndproc=0）并警告一次 —— 在我们的世界里 WndProc 从不
 * 被调用，为此拒绝创建只会让启动停在这里。
 * 但 lpClassName 本身为 NULL 时如实返回 0：Win32 也是失败，而且凭空造一个
 * 无类窗口只会让后续 GetClientRect/WindowFromPoint 指向一个游戏不知道的东西。 */
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
    (void)param;                               /* 没有 WM_CREATE/WM_NCCREATE */

    /* 类：既可能是字符串指针，也可能是 RegisterClassA 返回的 MAKEINTATOM */
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

    (void)parent;                              /* 没有父子 z 序需要维护 */

    /* 新窗口成为活动/焦点窗口：GLFW 建完窗口就假设它能拿到焦点 */
    g_active_hwnd = w->hwnd;
    g_focus_hwnd  = w->hwnd;

    c->eax = w->hwnd;
    uR(c, 48u);
}

/* RegisterClassA / RegisterClassExA 的公共实现。
 * WNDCLASSA（32 位，40 字节）：style@0, lpfnWndProc@4, cbClsExtra@8,
 * cbWndExtra@12, hInstance@16, hIcon@20, hCursor@24, hbrBackground@28,
 * lpszMenuName@32, lpszClassName@36。
 * WNDCLASSEXA（48 字节）：cbSize@0, style@4, lpfnWndProc@8, cbClsExtra@12,
 * cbWndExtra@16, hInstance@20, hIcon@24, hCursor@28, hbrBackground@32,
 * lpszMenuName@36, lpszClassName@40, hIconSm@44。 */
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
        if (cb != 48u) {                       /* Win32 要求 cbSize 精确匹配 */
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
    /* 同名重复注册：复用已有 atom（Win32 会返回 0，但那只会让启动停在这里） */
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

/* ATOM RegisterClassA(const WNDCLASSA*) — stdcall，清 4 字节。
 * [真实] 解析结构、存类表、返回非 0 atom。 */
static void u_RegisterClassA(CPU *__restrict c)
{
    u_register_class_common(c, uA(c, 0), 0);
}

/* ATOM RegisterClassExA(const WNDCLASSEXA*) — stdcall，清 4 字节。
 * [真实] 同上，cbSize 必须为 48；不等则如实返回 0。 */
static void u_RegisterClassExA(CPU *__restrict c)
{
    u_register_class_common(c, uA(c, 0), 1);
}

/* BOOL UnregisterClassA(LPCSTR, HINSTANCE) — stdcall，清 8 字节。
 * [真实] 找到就从类表里删掉并返回 TRUE；找不到如实返回 FALSE（Win32 行为）。 */
static void u_UnregisterClassA(CPU *__restrict c)
{
    uint32_t name_ptr = uA(c, 0);
    u_vcls *vc;
    char name[128];

    (void)uA(c, 1);                            /* hInstance：按名字查即可 */
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

/* BOOL DestroyWindow(HWND) — stdcall，清 4 字节。
 * [真实] 只销毁我们发过的句柄；并清掉指向它的焦点/捕获/活动窗口，
 * 免得后面 GetActiveWindow 返回一个已死的句柄。 */
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

/* BOOL ShowWindow(HWND, int) — stdcall，清 8 字节。
 * [真实] 更新可见标志与 style 的 WS_VISIBLE 位，返回"之前是否可见"
 * （Win32 返回值就是这个），所以调用方据此判断状态变化是准的。 */
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

/* BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) — stdcall，清 28 字节。
 * [真实] 按 SWP_NOMOVE / SWP_NOSIZE 更新虚拟外框矩形。z 序只有一层，忽略
 * hWndInsertAfter；SWP_SHOWWINDOW 顺带把窗口标成可见（Win32 也这么做）。 */
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

/* BOOL GetClientRect(HWND, LPRECT) — stdcall，清 8 字节。
 * [真实] RECT = 16 字节（left,top,right,bottom）。客户区原点恒为 (0,0)，
 * 尺寸由外框减去边框/标题/菜单得到 —— 与 AdjustWindowRectEx 严格互逆，
 * 所以 1280x720 的客户区请求会原样得到 1280x720。
 * 未知窗口按 Win32 返回 FALSE 并且不写缓冲区。 */
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

/* BOOL AdjustWindowRectEx(LPRECT, DWORD, BOOL, DWORD) — stdcall，清 16 字节。
 * [真实] 按我们自己的边框度量把客户区矩形扩成外框：左右各加 border，
 * 上边加 border+caption+menu，下边加 border（有 WS_EX_CLIENTEDGE 再各加 2）。
 * 这两个函数必须成对自洽，否则 GLFW 算出来的窗口尺寸和 GetClientRect 打架。 */
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

/* ============================ ③ 窗口状态 =============================== */

/* BOOL BringWindowToTop(HWND) — stdcall，清 4 字节。
 * [桩] 只有一层 z 序，提升没有可观察效果；句柄有效就返回 TRUE。 */
static void u_BringWindowToTop(CPU *__restrict c)
{
    c->eax = u_win_find(uA(c, 0)) ? 1u : 0u;
    uR(c, 4u);
}

/* HWND SetCapture(HWND) — stdcall，清 4 字节。
 * [真实] 返回上一个捕获窗口（Win32 语义），并记住新的；无效句柄等同释放。 */
static void u_SetCapture(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    uint32_t prev = g_capture_hwnd;
    g_capture_hwnd = u_win_find(hwnd) ? hwnd : 0u;
    c->eax = prev;
    uR(c, 4u);
}

/* BOOL ReleaseCapture(void) — stdcall，清 0 字节。
 * [真实] 后置条件（没有捕获窗口）一定成立，返回 TRUE，不制造"释放失败"的假象。 */
static void u_ReleaseCapture(CPU *__restrict c)
{
    g_capture_hwnd = 0;
    c->eax = 1;
    uR(c, 0u);
}

/* HCURSOR SetCursor(HCURSOR) — stdcall，清 4 字节。
 * [真实] 记录并返回旧光标句柄（初始 0 = 未设置，与 Win32 一致）。 */
static void u_SetCursor(CPU *__restrict c)
{
    uint32_t cur = uA(c, 0);
    uint32_t prev = g_cursor_handle;
    g_cursor_handle = cur;
    c->eax = prev;
    uR(c, 4u);
}

/* HWND SetFocus(HWND) — stdcall，清 4 字节。
 * [真实] 返回上一个焦点窗口；有效句柄同时成为活动窗口。 */
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

/* HWND GetActiveWindow(void) — stdcall，清 0 字节。
 * [真实] 返回我们这个线程的活动窗口（建窗时设置）。没有窗口时如实返回 0。 */
static void u_GetActiveWindow(CPU *__restrict c)
{
    c->eax = g_active_hwnd;
    uR(c, 0u);
}

/* BOOL SetForegroundWindow(HWND) — stdcall，清 4 字节。
 * [桩] Android 没有"前台窗口仲裁"，把前台状态记到活动窗口上并返回 TRUE。 */
static void u_SetForegroundWindow(CPU *__restrict c)
{
    uint32_t hwnd = uA(c, 0);
    if (hwnd && u_win_find(hwnd)) g_active_hwnd = hwnd;
    c->eax = 1;
    uR(c, 4u);
}

/* BOOL ClipCursor(const RECT*) — stdcall，清 4 字节。
 * [桩] 陷阱区域只在真光标上才有意义；我们没有真光标。指针为空 = 解除陷阱，
 * 非空但非法时如实返回 FALSE。 */
static void u_ClipCursor(CPU *__restrict c)
{
    uint32_t rect = uA(c, 0);
    c->eax = (!rect || u_ptr_ok(rect, 16u)) ? 1u : 0u;
    uR(c, 4u);
}

/* int ShowCursor(BOOL) — stdcall，清 4 字节。
 * [真实] Win32 是一个计数器：TRUE 加一、FALSE 减一，返回新的计数；
 * 计数 >= 0 时可见。照实现即可，不需要真的画箭头。 */
static void u_ShowCursor(CPU *__restrict c)
{
    int32_t show = (int32_t)uA(c, 0);
    if (show) g_show_cursor_count++;
    else      g_show_cursor_count--;
    c->eax = (uint32_t)g_show_cursor_count;
    uR(c, 4u);
}

/* BOOL SetCursorPos(int, int) — stdcall，清 8 字节。
 * [真实] 记录到虚拟光标位置；GetCursorPos / GetMessageA 的 pt 都读它。 */
static void u_SetCursorPos(CPU *__restrict c)
{
    g_cursor_x = (int32_t)uA(c, 0);
    g_cursor_y = (int32_t)uA(c, 1);
    c->eax = 1;
    uR(c, 8u);
}

/* BOOL GetCursorPos(LPPOINT) — stdcall，清 4 字节。
 * [真实] 写 POINT{x,y}（8 字节），初始在屏幕中心 640,360。 */
static void u_GetCursorPos(CPU *__restrict c)
{
    uint32_t out = uA(c, 0);
    if (!u_ptr_ok(out, 8u)) { c->eax = 0; uR(c, 4u); return; }
    st32(out + 0u, (uint32_t)g_cursor_x);
    st32(out + 4u, (uint32_t)g_cursor_y);
    c->eax = 1;
    uR(c, 4u);
}

/* BOOL ClientToScreen(HWND, LPPOINT) — stdcall，清 8 字节。
 * [真实] 客户区原点 = 外框左上角（虚拟窗口不带装饰位移，只在尺寸上记边框），
 * 所以加法就是 win_x/win_y。未知窗口返回 FALSE。 */
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

/* HWND WindowFromPoint(POINT) — stdcall，8 字节按值传参，清 8 字节。
 * [真实] 只有一层窗口：点落在可见外框内就返回它，否则 0（桌面）。 */
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

/* HMENU GetMenu(HWND) — stdcall，清 4 字节。
 * [真实] 返回建窗时登记的菜单句柄；GLFW 传的是 NULL，所以正常路径得到 0，
 * 与"窗口没有菜单"这一事实一致。 */
static void u_GetMenu(CPU *__restrict c)
{
    u_vwin *w = u_win_find(uA(c, 0));
    c->eax = w ? w->menu : 0u;
    uR(c, 4u);
}

/* LONG GetWindowLongA(HWND, int) — stdcall，清 8 字节。
 * [真实] 从虚拟窗口里取值：GWL_STYLE(-16)/GWL_EXSTYLE(-20)/GWL_WNDPROC(-4)/
 * GWL_HINSTANCE(-6)/GWL_USERDATA(-21，GLFW 用它存 window 指针)。未知索引按
 * Win32 返回 0；未知窗口也返回 0（Win32 置 ERROR_INVALID_WINDOW_HANDLE）。
 * 注意：必须支持 -21，否则 GLFW 的窗口↔对象映射拿不到指针会直接崩。 */
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
    case U_GWL_HWNDPARENT: c->eax = 0;           break;   /* 无父窗口 */
    case U_GWL_ID:         c->eax = 0;           break;
    default:               c->eax = 0;           break;
    }
    uR(c, 8u);
}

/* LONG SetWindowLongA(HWND, int, LONG) — stdcall，清 12 字节。
 * [真实] 写入虚拟窗口并返回旧值（Win32 语义）。改 style 后客户区尺寸会跟着
 * 变 —— GetClientRect 每次按当前 style 重算，所以全屏切换这类操作是自洽的。 */
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

/* DWORD SetClassLongA(HWND, int, LONG) — stdcall，清 12 字节。
 * [真实] 写类表里对应的字段并返回旧值。游戏用它换窗口图标(GCL_HICON -14)，
 * 旧值 0 表示之前没有图标 —— 这是真的。未知窗口返回 0。 */
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
    case U_GCL_MENUNAME:      old = 0;             break;   /* 无菜单资源 */
    default:                  old = 0;             break;
    }
    c->eax = old;
    uR(c, 12u);
}

/* ============================ ④ 图标 / 光标资源 ========================= */

/* HICON LoadIconA(HINSTANCE, LPCSTR) — stdcall，清 8 字节。
 * [桩] 我们不解 PE 资源、也不加载图标位图；但返回 0 会被 GLFW 当成"图标加载
 * 失败"进而当成致命错误。所以按请求内容派生一个稳定、非 0、且绝不与宿主指针
 * 混淆的令牌：整数资源号 (HIWORD==0) -> 0xE000|id，字符串名 -> 0xE800|hash。
 * 同一请求永远得到同一令牌，guest 若做相等比较也成立。 */
static void u_LoadIconA(CPU *__restrict c)
{
    uint32_t name = uA(c, 1);
    (void)uA(c, 0);                            /* hInstance：资源在内置镜像里 */

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

/* HCURSOR LoadCursorA(HINSTANCE, LPCSTR) — stdcall，清 8 字节。
 * [桩] 与 LoadIconA 同理：GLFW 用 LoadCursorA(NULL, IDC_ARROW=32512) 填类的
 * hCursor，返回 0 会让窗口创建被判失败。令牌 0xC000|(id&0x7FF)，IDC_ARROW
 * 得到 0x0000CF00。 */
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

/* HANDLE LoadImageA(HINSTANCE, LPCSTR, UINT, int, int, UINT) — stdcall，清 24。
 * [桩] 不解码位图；按 (类型, 资源号) 派生稳定非 0 令牌。类型只接受
 * IMAGE_BITMAP(0)/IMAGE_ICON(1)/IMAGE_CURSOR(2)，其它如实返回 0。
 * cx/cy/fuLoad 只影响解码尺寸，这里不关心（以后接真解码时再读）。 */
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

/* ============================ ⑤ 系统度量 / 显示 ======================== */

/* int GetSystemMetrics(int) — stdcall，清 4 字节。
 * [真实] 按 1280x720 单屏给出与 AdjustWindowRectEx 自洽的度量（SM_CXFRAME=8,
 * SM_CYCAPTION=23, SM_CYMENU=19 ...）。未知选择器返回 0 = "该特性不存在"，
 * 这是 Win32 对不支持选择器的合法应答，不会让调用方拿到垃圾尺寸。 */
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
        /* 未知选择器 = 该特性不存在。0 是 Win32 的合法回答，调用方会走
         * "没有这个功能"的分支，而不是拿一个编出来的尺寸。 */
        v = 0;
        break;
    }
    c->eax = (uint32_t)v;
    uR(c, 4u);
}

/* BOOL EnumDisplayDevicesA(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD) — stdcall，16。
 * [真实] 我们只有一个显示适配器：iDevNum==0 返回 "\\\\.\\DISPLAY1"，
 * StateFlags=ATTACHED_TO_DESKTOP|PRIMARY_DEVICE(0x5)，iDevNum>0 如实返回 FALSE。
 * DISPLAY_DEVICEA 布局（32 位，424 字节）：cb@0, DeviceName[32]@4,
 * DeviceString[128]@36, StateFlags@164, DeviceID[128]@168, DeviceKey[128]@296。
 * 调用方的 cb 小于 424 时按 Win32 的 ERROR_INSUFFICIENT_BUFFER 语义返回 FALSE。 */
static void u_EnumDisplayDevicesA(CPU *__restrict c)
{
    uint32_t dev_num = uA(c, 1);
    uint32_t out     = uA(c, 2);
    uint32_t cb;

    (void)uA(c, 0);                            /* lpDevice：NULL = 全部适配器 */
    (void)uA(c, 3);                            /* dwFlags：接口名标志无意义 */

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

/* 填一个"当前显示模式"的 DEVMODEA（32 位，156 字节）。
 * 偏移：dmDeviceName[32]@0, dmSpecVersion@32, dmDriverVersion@34, dmSize@36,
 * dmDriverExtra@38, dmFields@40, dmBitsPerPel@104, dmPelsWidth@108,
 * dmPelsHeight@112, dmDisplayFlags@116, dmDisplayFrequency@120。 */
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

/* BOOL EnumDisplaySettingsA(LPCSTR, DWORD, DEVMODEA*) — stdcall，清 12 字节。
 * [真实] 只有一种模式：ENUM_CURRENT_SETTINGS(0xFFFFFFFF) 或 0 返回
 * 1280x720@60 32bpp；其它索引如实返回 FALSE（没有更多模式）。 */
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

/* BOOL EnumDisplaySettingsExA(LPCSTR, DWORD, DEVMODEA*, DWORD) — stdcall，16。
 * [真实] 同上；EDS_RAWMODE 不额外提供模式（我们只有桌面模式），故行为一致。 */
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

/* LONG ChangeDisplaySettingsExA(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID) — 20。
 * [真实] Android 的 surface 尺寸由宿主决定，没有可切换的显示模式；但
 * "1280x720"正是我们已处于的状态，所以返回 DISP_CHANGE_SUCCESSFUL(0)。
 * 请求别的模式同样返回 0 并注明：只要图形后端按 1280x720 输出就自洽。
 * lpDevMode 指针非法时返回 DISP_CHANGE_BADMODE(-2)。 */
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

/* BOOL SystemParametersInfoA(UINT, UINT, PVOID, UINT) — stdcall，清 16 字节。
 * [真实] 只实现游戏实际会用的动作，其余如实返回 FALSE（并打一次警告，避免
 * "静默成功"让后面的行为无从解释）：
 *   SPI_GETFOREGROUNDLOCKTIMEOUT(0x2000) -> 写 200000（Windows 默认）
 *   SPI_SETFOREGROUNDLOCKTIMEOUT(0x2001) -> 记住新值（pvParam 是值不是指针）
 *   SPI_GETWORKAREA(0x0030)              -> 1280x720 的 RECT（无任务栏）
 *   SPI_GET/SETSCREENSAVEACTIVE/TIMEOUT  -> 无屏保：写 0 / 接受
 * 忏悔在窗口激活时会读改写这个前台锁超时；没有桌面就不需要仲裁，但 GET/SET
 * 往返必须自洽，否则游戏会以为自己改动失败。 */
static void u_SystemParametersInfoA(CPU *__restrict c)
{
    uint32_t action  = uA(c, 0);
    uint32_t uiparam = uA(c, 1);
    uint32_t param   = uA(c, 2);
    uint32_t flags   = uA(c, 3);

    (void)uiparam;                             /* 这些动作要求 uParam = 0 */
    (void)flags;

    switch (action) {
    case U_SPI_GETFOREGROUNDLOCKTIMEOUT:
        c->eax = u_w32(param, g_foreground_lock_timeout) ? 1u : 0u;
        break;
    case U_SPI_SETFOREGROUNDLOCKTIMEOUT:
        g_foreground_lock_timeout = param;     /* 泛型 API 文档：这里是值 */
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
        c->eax = u_w32(param, 0u) ? 1u : 0u;    /* 没有屏保 */
        break;
    case U_SPI_SETSCREENSAVEACTIVE:
        c->eax = 1;                             /* 接受，但从不触发 */
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
        c->eax = 0;                             /* 如实失败，不假装成功 */
        break;
    }
    uR(c, 16u);
}

/* HDC GetDC(HWND) — stdcall，清 4 字节。
 * [桩] 没有真实 GDI 表面可给；返回不透明令牌 0x0000A001（非 0，guest 只会把它
 * 回传给 ReleaseDC）。真正的绘制走 GDI32/OPENGL32 层，不经过这里。 */
static void u_GetDC(CPU *__restrict c)
{
    (void)uA(c, 0);
    c->eax = U_HDC_TOKEN;
    uR(c, 4u);
}

/* int ReleaseDC(HWND, HDC) — stdcall，清 8 字节。
 * [真实] 我们的 DC 是令牌，没有资源可释放；返回 1 = "已释放"（后置条件成立）。 */
static void u_ReleaseDC(CPU *__restrict c)
{
    (void)uA(c, 0);
    (void)uA(c, 1);
    c->eax = 1;
    uR(c, 8u);
}

/* ============================ ⑥ 输入 ================================== */

/* 外部输入桥的前向声明（实现见下方 GetAsyncKeyState 之前）。 */
static int u_key_down(uint32_t vk);

/* SHORT GetKeyState(int) — stdcall，清 4 字节。
 * [真实] 没有键盘/鼠标输入源，键状态恒为"抬起、未切换" = 0。
 * 0 是真实状态而不是伪造的高位（0x8000 会被当成"按下了"）。 */
static void u_GetKeyState(CPU *__restrict c)
{
    uint32_t vk = uA(c, 0);
    c->eax = u_key_down(vk) ? 0x8000u : 0u;
    uR(c, 4u);
}

/* ==========================================================================
 * 外部输入桥（2026-11-04）
 *
 * 为什么需要
 * ----------
 * DirectInput 的 CreateDevice 恒返回 DIERR_DEVICENOTREG（见 host_dinput.c），
 * 游戏拿不到 DInput 键盘/手柄设备，只能走 GetAsyncKeyState 这条 Win32 路径。
 * 而这个桩原来恒返回 0 —— 游戏**完全看不到任何输入**：
 *   · 开场动画按任意键跳过 → 没反应
 *   · 主菜单 PRESS START → 进不去
 *   · 手柄/键盘全无效
 *
 * Android 上 /dev/input 需要 root（Termux 实测 Permission denied），
 * 而 Java 层的 KeyEvent/MotionEvent 只能在本进程拿到，子进程读不到。
 * 所以走一条最朴素的桥：Java 侧把按键状态按行写进 TCP:127.0.0.1:7137，
 * 宿主侧一个线程收下来更新状态表，GetAsyncKeyState 查表返回。
 *
 * 协议（纯文本，一行一条，容错解析）
 * ----------------------------------
 *      <VK十进制> <1|0>\n     按下/松开，例如 "32 1" = 空格按下
 *      R\n                    清零全部按键（手柄断开/Activity 暂停时用）
 *
 * 为什么用"状态"而不是"事件"：GetAsyncKeyState 是电平语义（现在按住吗），
 * 游戏每帧轮询。发事件还得宿主维护队列和边沿，反而更容易错。
 *
 * 线程安全：状态表是 volatile uint8_t[256]，单字节写天然原子；
 * 读侧只读，不需要锁。这是整个方案里最不容易出错的部分 —— 刻意如此。
 * ========================================================================== */
#define IN_BRIDGE_PORT 7137
static volatile unsigned char u_key_state[256];
static int u_bridge_listen = -1;

/* ---- 手柄状态（XInput 用）----
 *
 * 2026-11-06：游戏的主输入路径是 XInput，不是 DInput 键盘。
 * Java 侧把虚拟手柄的**完整状态**按下面这行发过来：
 *
 *     G <buttons十六进制> <lx> <ly> <rx> <ry> <lt> <rt>\n
 *
 * 轴是 -32768..32767（Y 向上为正，跟 XInput 一致），扳机 0..255。
 * host_xinput.c 通过 u_pad_get() 读取。用互斥锁而不是 volatile 数组，
 * 因为一次读要取 7 个字段，必须是一致的快照（摇杆和按钮不能来自两帧）。 */
static pthread_mutex_t u_pad_mu = PTHREAD_MUTEX_INITIALIZER;

/* 活跃连接计数。多个 Activity 实例可能同时连进来（重开 app 时旧连接
 * 还没超时），断开时必须知道是不是最后一个。 */
static pthread_mutex_t u_bridge_mu = PTHREAD_MUTEX_INITIALIZER;
static int u_bridge_conns;

/* ★★ 2026-11-07：延迟清零。
 *
 * 真机症状：手柄“按几次都不能正确输入”，app.log 里 728 次重连。
 *
 * 原因就是断开时的 `u_pad_clear()`：Java 重连的**空窗期**（close 旧
 * socket → 建新的 → 写第一行）虽然只有几十毫秒，但旧连接断开那
 * 一刻 conns 归零，于是刚设好的摇杆/按钮被整张清掉，游戏下一帧
 * 读到的就是“全部松开”。用户想按着左走，实际效果是每秒被松几十次。
 *
 * 修法：断开时**只记时间，不清表**；真正清零交给宽限期：
 * 连续 1500 ms 没有任何连接才视为全部释放。 */
static uint32_t u_bridge_zero_at;      /* 最后一个连接断开的时刻（ms） */

static void u_bridge_tick(void);       /* 定义在 u_pad_clear 之后 */
static struct {
    unsigned buttons;
    int lx, ly, rx, ry;   /* -32768..32767 */
    int lt, rt;           /* 0..255 */
} u_pad;

int u_pad_get(unsigned *buttons, int *lx, int *ly, int *rx, int *ry,
              int *lt, int *rt)
{
    u_bridge_tick();   /* 宽限期到了才清，不是断开就清 */
    pthread_mutex_lock(&u_pad_mu);
    if (buttons) *buttons = u_pad.buttons;
    if (lx) *lx = u_pad.lx;
    if (ly) *ly = u_pad.ly;
    if (rx) *rx = u_pad.rx;
    if (ry) *ry = u_pad.ry;
    if (lt) *lt = u_pad.lt;
    if (rt) *rt = u_pad.rt;
    pthread_mutex_unlock(&u_pad_mu);
    return 1;   /* 虚拟手柄始终存在；没收到过数据就是全零状态 */
}

static void u_pad_clear(void)
{
    pthread_mutex_lock(&u_pad_mu);
    memset(&u_pad, 0, sizeof u_pad);
    pthread_mutex_unlock(&u_pad_mu);
}

/* 宽限期结束后才真正清零。u_pad_get / host_key_table 都会调。
 *
 * 为什么延后：见 u_bridge_zero_at 的注释。断开后 1500ms 内不清，
 * 让 Java 的重连空窗期盖在里面。 */
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
    if ((uint32_t)(now - at) < 1500u) return;   /* 宽限期内：保留最后状态 */

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

/* 活跃连接计数与宽限期状态已在 u_pad_mu 旁边定义（见 u_bridge_zero_at 的注释）。 */

/* GET_ASYNC_KEY_STATE 的高位约定：
 *   0x8000 = 当前按下，0x0001 = 自上次查询后按下过（"刚按下"）。
 * 这里两个都置，因为游戏既可能查电平也可能查边沿。 */
/* ==========================================================================
 * 单个连接的服务体（2026-11-06 新增）
 *
 * ★ 为什么必须拆出来：原来是"accept 之后在本线程里阻塞 read 直到断开"，
 * 也就是**单线程串行 accept**。
 *
 * 真机上出现的后果（run.log 有 7 次"连接建立"、app.log 说 connected，
 * 但 native 侧"手柄状态"收到 0 条）：
 *
 *   · Java 每次重连都新建 socket，旧的**不关闭**，inOut 指向最新的那个
 *   · native 卡在第一个连接的 read 上，永远回去不了 accept
 *   · 于是数据写进第 N 个 socket，native 在读第 1 个 —— 永远收不到
 *
 * 现在：accept 到 fd 就丢给一个 detached 线程，主循环立刻回去 accept。
 * 哪个连接发数据都能收到，跟连接顺序无关。
 * ========================================================================== */
static void *u_bridge_serve(void *arg)
{
    int fd = (int)(intptr_t)arg;
    pthread_mutex_lock(&u_bridge_mu);
    u_bridge_conns++;
    int nconn = u_bridge_conns;
    pthread_mutex_unlock(&u_bridge_mu);
    /* ★ 2026-11-07：限流。真机上曾有 728 次连接/断开，而这份日志是
     * 写到 /sdcard（FUSE）并每次 fflush —— 同步落盘，毫秒级。
     * 光日志就能拖慢主循环。头 20 次足够诊断，之后静默。 */
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
        /* ★★ 2026-11-07：EINTR 必须重试，不能当成断开。
         *
         * 真机症状：app.log 里 `input bridge connected` 出现 131 次，
         * 每触摸一次就重连一次（每轮重连要 500 ms 超时）。
         *
         * 根因就在这里：进程里随时可能有信号到达（SIGPROF 采样、
         * ART 的 RCU/GC 信号、我们的 crash handler 机制……），
         * read() 会被中断并返回 -1，errno= EINTR。
         * 原来 `if (n <= 0) break;` 把 EINTR 当成对端关闭 —— 直接 break，
         * 关掉 fd。Java 下次写就 EPIPE，于是重连。
         *
         * 这条也解释了交接文档里那个悬了很久的
         * "124 次连接、native 收到 0 条数据" —— 从来不是 JVM 的锅。
         *
         * 正确语义：
         *   n > 0   有数据
         *   n == 0  对端正常关闭 → break
         *   n < 0   EINTR 重试；其他错误才 break */
        if (n < 0) {
            if (errno == EINTR) continue;
            /* ★ 2026-11-07：诊断。728 次重连必须查清是"对端关"还是"读错"。 */
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
            /* 对端正常关闭。若 "剩余"很快归零就会清空整张键表，
             * 那正是"按了没反应"的直接原因。 */
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
                /* 手柄状态行：G <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt> */
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
            len = 0;   /* 超长行直接丢弃，防止流错位后一直卡住 */
        }
    }
    close(fd);

    pthread_mutex_lock(&u_bridge_mu);
    if (u_bridge_conns > 0) u_bridge_conns--;
    int still = u_bridge_conns;
    if (still == 0) u_bridge_zero_at = u_now_ms();   /* 开宽限期 */
    pthread_mutex_unlock(&u_bridge_mu);
    {
        static unsigned shown_disc;
        if (shown_disc++ < 20u) {
            fprintf(stderr, "[input] connection closed (%d remaining, %u total)\n",
                    still, shown_disc);
            fflush(NULL);
        }
    }
    /* ★★ 2026-11-07：这里原来直接 u_pad_clear() + 清键表。
     *
     * 后果：Java 重连的空窗期（close 旧 → 建新 → 写第一行，几十毫秒）
     * 里 conns 会短暂归零，于是刚设好的摇杆/按钮被抹掉。
     * 用户按住不放，游戏却看到每秒几十次“松手”—— 手柄完全不灵。
     *
     * 现在只记时间，真正清零交给 u_bridge_tick()：连续 1500 ms
     * 没有任何连接才清。重连空窗期完全盖在宽限期里。 */
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
        /* Android 缺 INTERNET 权限时这里必然失败 —— 手柄会完全无效 */
        fprintf(stderr, "[input] ✗ bind(%d) failed errno=%d (%s)\n",
                IN_BRIDGE_PORT, errno, strerror(errno));
        fflush(NULL);
        close(u_bridge_listen); u_bridge_listen = -1; return;
    }
    /* ★ backlog 从 1 提到 8：Java 侧可能瞬间建多个连接
     * （启动一次 + 写失败触发的重连线程），backlog=1 会直接拒掉。 */
    if (listen(u_bridge_listen, 8) != 0) {
        fprintf(stderr, "[input] ✗ listen failed errno=%d (%s)\n",
                errno, strerror(errno));
        fflush(NULL);
        close(u_bridge_listen); u_bridge_listen = -1; return;
    }
    fprintf(stderr, "[input] ✓ listening on 127.0.0.1:%d\n", IN_BRIDGE_PORT);
    fflush(NULL);

    /* ★ 每个连接一个线程，主循环立刻回去 accept。
     * 见 u_bridge_serve 上方的长注释：串行 accept 会导致
     * "Java 说 connected、native 收 0 条数据"。 */
    for (;;) {
        int fd = accept(u_bridge_listen, NULL, NULL);
        if (fd < 0) { usleep(200000); continue; }
        {
            pthread_t t;
            if (pthread_create(&t, NULL, u_bridge_serve,
                               (void *)(intptr_t)fd) == 0) {
                pthread_detach(t);
            } else {
                close(fd);          /* 起不了线程就关掉，别泄漏 fd */
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

/* 给 DirectInput 假设备用（host_dinput.c）。
 *
 * ★ 2026-11-05：游戏**从不调用** GetAsyncKeyState（实测 0 次），
 * 它读输入走的是 DirectInput 的 GetDeviceData。所以键盘状态表
 * 必须能被那边访问 —— 把整张表导出，由对方 diff 成事件。
 *
 * ★ 同时把 KEY_HELD 调试注入合并进表里。
 *   原来 KEY_HELD 只在 GetAsyncKeyState 分支生效，而游戏根本不调
 *   那个函数，导致“注入的键永远看不到”。现在合并到源头，
 *   两个消费者（GetAsyncKeyState 和 DInput）都能看到。
 *   真实输入（u_key_state 由 TCP 输入桥写）优先级更高，不会被覆盖为 0。 */
/* 前向声明：这两个函数定义在本文件靠后的位置，且都是 static。 */
static int key_held_match(uint32_t vk);
static int key_hold_active(void);

const unsigned char *host_key_table(void)
{
    u_bridge_tick();   /* 宽限期到了才清状态（见 u_bridge_tick 的注释） */
    for (uint32_t vk = 0; vk < 256u; vk++) {
        if (key_held_match(vk) && key_hold_active())
            u_key_state[vk] = 1;
    }
    return (const unsigned char *)u_key_state;
}

/* SHORT GetAsyncKeyState(int) — stdcall，清 4 字节。
 *
 * ★ 2026-11-04：加 KEY_HELD=<vk>[,<vk>...] 注入按键。
 *
 * 为什么需要：DirectInput 的 CreateDevice 返回 DIERR_DEVICENOTREG
 * （见 host_dinput.c），游戏拿不到 DInput 键盘设备，只能走
 * GetAsyncKeyState 这条 Win32 路径。而这个桩原来恒返回 0，
 * 于是**游戏完全看不到任何按键** —— 表现在"开场动画按任意键跳过"
 * 完全没反应，每次验证都得干等 144 秒。
 *
 * 返回 0x8000（最高位 = 当前按下）。低字节的"上次按下"位不置，
 * 这样模拟的是"一直按住"而不是"刚按下"；两者在取边沿的逻辑里
 * 都能触发跳过。
 *
 * 用法：KEY_HELD=0x20（空格） / KEY_HELD=0x1B（ESC）
 *       KEY_HELD=0x20,0x0D 一次多个键。 */
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

/* KEY_HOLD_MS=N：按键只在"首次查询后的 N 毫秒内"算按下，之后自动松开。
 *
 * 为什么需要：一直按住会让游戏跳过 cutscene 之后停在等"键释放"的状态
 * （实测：一直按空格 → Cutscene 帧推进 0 条，日志卡在 HQX4 size 不动）。
 * 真实玩家是"按一下就松"，所以必须在宿主侧模拟一次点击。 */
static int key_hold_active(void)
{
    static int inited, hold_ms;
    static uint64_t t_first;
    if (!inited) {
        inited = 1;
        const char *e = getenv("KEY_HOLD_MS");
        hold_ms = e ? atoi(e) : -1;      /* -1 = 一直按住（旧行为） */
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
        c->eax = 0x8001u;      /* 按住 + 刚按下 */
    else if (key_held_match(vk) && key_hold_active())
        c->eax = 0x8000u;      /* KEY_HELD 调试注入 */
    else
        c->eax = 0u;
    uR(c, 4u);
}

/* UINT MapVirtualKeyA(UINT uCode, UINT uMapType) — stdcall，清 8 字节。
 * [真实(部分)] MAPVK_VK_TO_CHAR(2) 给出可打印字符的 ASCII（A-Z/0-9/空格等），
 * 其余情况返回 0 = "无对应翻译"，这是 Win32 的合法结果（不是垃圾值）。
 * 不需要扫描码表：没有真实键盘设备，MAPVK_VK_TO_VSC(0) 返回 0。 */
static void u_MapVirtualKeyA(CPU *__restrict c)
{
    uint32_t vk   = uA(c, 0);
    uint32_t type = uA(c, 1);
    uint32_t ch = 0;

    if (type == 2u) {                          /* MAPVK_VK_TO_CHAR */
        if (vk >= 0x41u && vk <= 0x5Au)       ch = vk;                 /* A-Z */
        else if (vk >= 0x30u && vk <= 0x39u)  ch = vk;                 /* 0-9 */
        else if (vk >= 0x60u && vk <= 0x69u)  ch = vk - 0x60u + 0x30u; /* 小键盘 */
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

/* UINT SendInput(UINT, LPINPUT, int) — stdcall，清 12 字节。
 * [桩] 我们没有合成输入的通道（没有真窗口、没有 Android InputQueue 事件）。
 * 但返回 0 会让"注入失败"变成硬错误，而这里的语义其实是"事件被消费但没有
 * 观察者"。折中：先校验数组（cbSize 必须是 28 = sizeof(INPUT)，指针可读），
 * 合法就返回 cInputs 表示全部消费掉。非法参数如实返回 0（Win32 也返回 0）。 */
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

/* BOOL TrackMouseEvent(LPTRACKMOUSEEVENT) — stdcall，清 4 字节。
 * [桩] 受理登记（cbSize 必须 == 16，hwndTrack 必须是我们的窗口），返回 TRUE；
 * 但因为我们从不产生鼠标消息，也永远不会投递 WM_MOUSELEAVE。对没有输入源的
 * 环境这是自洽的：事件从不开始，也就无所谓结束。 */
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

/* HDEVNOTIFY RegisterDeviceNotificationA(HANDLE, LPVOID, DWORD) — 清 12 字节。
 * [桩] 返回非 0 令牌，让设备通知的注册步骤通过；我们从不投递 WM_DEVICECHANGE。
 * 指针非法如实返回 0。 */
static void u_RegisterDeviceNotificationA(CPU *__restrict c)
{
    uint32_t filter = uA(c, 1);
    (void)uA(c, 0);
    (void)uA(c, 2);
    c->eax = (filter && u_ptr_ok(filter, 1u)) ? U_NOTIFY_TOKEN : 0u;
    uR(c, 12u);
}

/* BOOL UnregisterDeviceNotification(HDEVNOTIFY) — stdcall，清 4 字节。
 * [真实] 只认我们发出去的令牌。 */
static void u_UnregisterDeviceNotification(CPU *__restrict c)
{
    c->eax = (uA(c, 0) == U_NOTIFY_TOKEN) ? 1u : 0u;
    uR(c, 4u);
}

/* UINT GetRawInputDeviceList(PRAWINPUTDEVICELIST, PUINT, UINT) — 清 12 字节。
 * [真实] 这台机器上没有任何 raw input 设备（没有 USB HID 直通），所以返回 0
 * 个设备并把数量写进 *puiNumDevices —— 这是"真的没有"，不是"我们不知道"。
 * GLFW 的 init 看到 devCount==0 就跳过手柄初始化，不会去解引用空列表。
 * cbSize 必须等于 sizeof(RAWINPUTDEVICELIST)=8，否则按 Win32 返回 (UINT)-1。 */
static void u_GetRawInputDeviceList(CPU *__restrict c)
{
    uint32_t list  = uA(c, 0);
    uint32_t count = uA(c, 1);
    uint32_t cbsize = uA(c, 2);

    (void)list;                                /* 0 个设备，没东西可写 */
    if (cbsize != 8u || !u_ptr_ok(count, 4u)) {
        c->eax = 0xFFFFFFFFu;                  /* (UINT)-1 */
        uR(c, 12u);
        return;
    }
    st32(count, 0u);
    c->eax = 0u;
    uR(c, 12u);
}

/* UINT GetRawInputDeviceInfoA(HANDLE, UINT, LPVOID, PUINT) — 清 12 字节。
 * [失败] 没有设备句柄会有效，所以任何查询都如实返回 (UINT)-1
 * （Win32 对无效 hDevice 的行为），调用方会走错误分支而不是读到垃圾名称。 */
static void u_GetRawInputDeviceInfoA(CPU *__restrict c)
{
    (void)uA(c, 0);
    (void)uA(c, 1);
    (void)uA(c, 2);
    (void)uA(c, 3);
    c->eax = 0xFFFFFFFFu;
    uR(c, 16u);
}

/* ============================ ⑦ 剪贴板 ================================= */

/* 把 guest 的一段文本拷进我们自己的低 4GB 区块，返回低地址句柄。
 * 为什么要拷：SetClipboardData 收的是 HGLOBAL，KERNEL32 层的 GlobalLock 是
 * "原样返回句柄"（identity），返回 caller 的句柄也能工作；但我们更希望剪贴板
 * 里的字节归我们所有（调用方随后释放自己的块也不会让 GetClipboardData 悬空）。
 * 宽字符按 UTF-16 单元计，最多个 2048 个单元 / 最多个 4096 字节。 */
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

/* BOOL OpenClipboard(HWND) — stdcall，清 4 字节。
 * [真实] 进程内单持有者模型：已打开时返回 FALSE（Win32 同）。 */
static void u_OpenClipboard(CPU *__restrict c)
{
    (void)uA(c, 0);
    if (g_clip_open) { c->eax = 0; uR(c, 4u); return; }
    g_clip_open = 1;
    c->eax = 1;
    uR(c, 4u);
}

/* BOOL CloseClipboard(void) — stdcall，清 0 字节。 */
static void u_CloseClipboard(CPU *__restrict c)
{
    g_clip_open = 0;
    c->eax = 1;
    uR(c, 0u);
}

/* BOOL EmptyClipboard(void) — stdcall，清 0 字节。
 * [真实] 清空所有格式；未打开时如实返回 FALSE。 */
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

/* HANDLE GetClipboardData(UINT) — stdcall，清 4 字节。
 * [真实] 返回本进程内登记过的格式句柄；没有该格式 / 未打开剪贴板返回 0
 * （Windows 同样返回 NULL）。绝不返回宿主地址。 */
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

/* HANDLE SetClipboardData(UINT, HANDLE) — stdcall，清 8 字节。
 * [真实] 文本格式(CF_TEXT/CF_UNICODETEXT)在低 4GB 区块里留一份自己的副本，
 * 其余格式只记句柄。成功返回登记后的句柄（Win32 语义），未打开剪贴板或句柄为
 * 0 时返回 0。 */
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
        stored = handle;                        /* 位图/自定义格式：只记句柄 */
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

/* ============================== 分发表 ================================== */

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

/* 导入清单是硬约束：少一个游戏就会在某条启动路径里缺符号。编译期钉死。 */
_Static_assert(sizeof u_table / sizeof u_table[0] == 59u,
               "USER32 dispatch table must cover exactly 59 imports");

/* 命中就执行、写 c->eax、清栈，返回 1；未命中返回 0（框架会报出符号名）。 */
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
