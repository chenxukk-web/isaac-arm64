/* ==========================================================================
 * host_gl_audio.c —— OPENGL32.dll(5) + GDI32.dll(6) + OpenAL32.dll(24) 宿主层
 *
 * 目标：Android arm64 上的《以撒的结合：忏悔》静态重编译版（repentogxm）。
 * 这一层的职责是让 guest 调到这些导入时**不崩、不死循环、拿到形状正确的值**，
 * 而不是真的渲染/播音。真正的 GL 后端（vitaGL / EGL）以后由 gl_bridge 接管。
 *
 * ---------------------------------------------------------------------------
 * 一、导入清单（全部经 grep ~/tmp/rgxm_build/gen/guest_table.c 核对）
 *
 *   GDI32.dll (6)  0x00606010 SetPixelFormat   0x00606014 GetDeviceCaps
 *                  0x00606018 DeleteDC         0x0060601c DescribePixelFormat
 *                  0x00606020 SwapBuffers      0x00606024 CreateDCA
 *   OPENGL32.dll(5) 0x006062f0 wglDeleteContext 0x006062f4 wglCreateContext
 *                  0x006062f8 wglShareLists    0x006062fc wglGetProcAddress
 *                  0x00606300 wglMakeCurrent
 *   OpenAL32.dll(24) 0x00606308..0x00606364，连续的 24 个 IAT 槽（见下表）
 *
 *   注意：游戏**没有**导入任何 gl* 函数（glClear/glTexImage2D…）。
 *   全部 GL 入口都经由 wglGetProcAddress 动态解析——所以「5 个 OPENGL32 导入
 *   全是 WGL 上下文管理 + 动态解析器」，而不是绘制调用。这一点决定了第二档
 *   的实现形状（见 §五）。
 *
 * ---------------------------------------------------------------------------
 * 二、调用约定（已核对，不是猜的）
 *
 *   OPENGL32/GDI32：x86 **stdcall**（WINAPI/APIENTRY）。
 *       证据：recomp/runtime/host_win32.c 里 wglGetProcAddress 的注释
 *       "`PROC WINAPI wglGetProcAddress(LPCSTR)` -- x86 stdcall, one argument."
 *       以及 GDI32/WGL 的 Win32 头文件声明。所以返回时 = pop 返回地址 + 清参数。
 *
 *   OpenAL32：x86 **cdecl**。
 *       证据：recomp/runtime/host_vita_audio.h 第 8-11 行原文
 *         "All entries use the Win32 OpenAL cdecl ABI, so an adapter removes
 *          only the return address; the translated caller removes its own
 *          arguments."
 *       与 host_vita_audio.c 的 vita_audio_cdecl_return()（只 gpop）一致。
 *       → 这里**只** gpop，绝不清参数；参数由 guest 自己的 add esp,N 清理。
 *       猜错的后果：栈每次调用偏移 N 字节，几百次调用后返回地址全错，
 *       崩在离现场很远的地方。这条已核对，无需冒险。
 *
 * ---------------------------------------------------------------------------
 * 三、低 4GB 约束
 *
 *   guest 的指针是 uint32_t。任何交给 guest 的**指针**必须是低 4GB 地址。
 *   本文件用 0x3C000000 起的 bump 分配器（另一个代理用 0x3B000000 给 user32，
 *   栈在 0x40000000，镜像在 0x3c000000，TIB 在 0x32xxxxxx），上界留到
 *   0x3FF00000（栈下 1 MiB 处停手），不会撞任何一段。
 *
 *   只有 ALCdevice* / ALCcontext* 是「真指针」形状（真 OpenAL 返回的就是
 *   堆指针），所以它们从低 4GB 分配并打 magic，销毁后 magic 失效。
 *   HDC / HGLRC 在 Win32 里本来就是内核小整数句柄、guest 从不解引用，
 *   所以用带 tag 的小整数（0x00DCxxxx / 0x00ACxxxx），既不会和镜像地址混，
 *   也绝不会被当成可解引用指针去踩内存。
 *
 * ---------------------------------------------------------------------------
 * 四、风险点（接线的人需要知道）
 *
 *   R1  wglGetProcAddress 目前返回 0。游戏用它解析全部 GL 入口；返回 0 会让
 *       其加载器把 0 存进函数指针表，第一次真正绘制时在 guest 侧触发
 *       "indirect call to untranslated address" 的**带名 fault**（可控、可知）。
 *       这是当前诚实的答案：没有 GL 后端时返回任何非 0 值都是伪造。
 *       接线后如需真实现：定义弱符号 host_gl_proc_resolve()（见 §五），
 *       并把返回的 token 接到 guest_host_dynamic 上。
 *   R2  GetDeviceCaps 的 110..121 号索引（PHYSICAL / VREFRESH / SHADEBLENDCAPS…）
 *       的值是照 wingdi.h 的索引表写的；手上没有 Windows SDK 头可以对照。
 *       0..44 段（HORZRES=8 等）是高置信的几十年稳定 ABI；低置信的那几个
 *       在 switch 里有单独标注，且游戏实测只查 HORZRES/VERTRES/BITSPIXEL/
 *       PLANES/LOGPIXELS*。
 *   R3  OpenAL 全部是「记账式实现」：句柄、ID、状态都真记，但不发声。
 *       因此 alGetSourcei(AL_BUFFERS_PROCESSED) 恒为 0 —— 刻意为之：
 *       若谎报已播完，音乐流会以 CPU 速度无限重灌 buffer（忙循环）。
 *       代价是静音，收益是不烧 CPU、不崩。见 ga_alGetSourcei 的注释。
 *   R4  RING 深度 16 / 源上限 256 是资源上限，超过会复用最老的槽位
 *       （老 ID 随后被判为无效）。Vita 版给 80，这里留了 3 倍余量。
 *
 * ---------------------------------------------------------------------------
 * 五、对外接口（父代理负责接线）
 *
 *   int host_gl_dispatch(CPU *__restrict c, const char *name);   GL+GDI，11 项
 *   int host_gl_count(void);                                     -> 11
 *   int host_audio_dispatch(CPU *__restrict c, const char *name); OpenAL，24 项
 *   int host_audio_count(void);                                  -> 24
 *
 *   name 是完整形式 "OPENGL32.dll!wglCreateContext"（精确 strcmp，和
 *   host_android.c 同一套约定；不乱猜大小写变体）。
 *
 *   可选扩展（不定义也能编过、能链过）：
 *     extern uint32_t host_gl_proc_resolve(const char *name);  // 弱符号
 *     定义它 = 给 wglGetProcAddress 一个真解析器（例如 gl_bridge 的 token）。
 *   可调外部变量（供接线方在启动时改分辨率）：
 *     unsigned host_gl_screen_width  = 1280;
 *     unsigned host_gl_screen_height = 720;
 * ========================================================================== */

#define _GNU_SOURCE
#include <unistd.h>   /* access(), F_OK —— 文件开关探测 */
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
/* 老内核/bionic 没有这个标志时退化成「提示地址」：下面的 got != want 检查
 * 会把没落在目标地址的映射 munmap 掉，语义仍然安全（只是更容易失败）。 */
#define MAP_FIXED_NOREPLACE 0
#endif

/* 接线方可改的显示尺寸。默认 1280x720（游戏 PC 版的默认窗口尺寸）。 */
unsigned host_gl_screen_width  = 1280U;
unsigned host_gl_screen_height = 720U;

/* ==========================================================================
 * 一、参数读取 / 返回（约定见文件头 §二）
 * ========================================================================== */

/* stdcall 第 i 个参数：esp 指向返回地址，参数从 esp+4 开始。 */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}

/* stdcall 返回：弹出返回地址 + 清 n 字节参数。 */
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* cdecl 返回：**只**弹出返回地址。OpenAL 的参数由 guest 自己 add esp,N 清，
 * 这里多清一个字节都会让下一次调用的返回地址错位。 */
static void cdecl_ret(CPU *__restrict c)
{
    (void)gpop(c);
}

/* ==========================================================================
 * 二、日志（有预算、不刷屏）
 *
 * 这一层每帧会被调用成百上千次（SwapBuffers / alSourcePlay）。全部打印会把
 * stderr 变成瓶颈，所以：预算 256 行，用完打一条 "suppressed" 就闭嘴。
 * 只有关键事件（句柄创建、未知参数、错误锁存）允许走这里。
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

/* 把 guest 低地址的 ASCII 名字拷出来（wglGetProcAddress 用）。
 * 返回 1 = 完整取到并以 0 结尾；0 = 空指针 / 非 ASCII / 没有终止符。
 * 只在 address 明显是用户区（> 0xFFFF）时读取，避免把 0/小整数当字符串踩。 */
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
        if (ch > 0x7fu) {         /* GL/WGL 符号名一定是 ASCII */
            out[i] = '\0';
            return 0;
        }
        out[i] = (char)ch;
    }
    out[cap - 1u] = '\0';
    return 0;                     /* 截断：调用方按「未知名字」处理 */
}

/* ==========================================================================
 * 三、低 4GB bump 分配器
 *
 * 0x3C000000 起，上界 0x3FF00000（guest 栈 0x40000000 下留 1 MiB）。
 * 不 free：这一层整个进程生命周期只分配 device/context 这种个位数对象。
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
        return 0u;                /* 调用方必须退化成 tag 句柄，绝不返回 NULL */
    }
    g_ga_bump += sz;
    return (uint32_t)(uintptr_t)want;
}

/* ==========================================================================
 * 四、GDI32（6）—— 假句柄 + 真数据
 * ========================================================================== */

/* Win32 里 HDC/HGLRC 是内核句柄（小整数），guest 从不解引用它们。
 * 用带 tag 的小整数：既能一眼看出是合成值，又绝不会被当作可读指针。 */
#define GA_HDC_TAG   0x00DC0000u
#define GA_HGLRC_TAG 0x00AC0000u
#define GA_HANDLE_KIND(h) ((h) & 0xFFFF0000u)

static uint32_t g_ga_hdc_next   = GA_HDC_TAG;
static uint32_t g_ga_hglrc_next = GA_HGLRC_TAG;

/* 句柄只用于日志分类，**从不**用它拒绝调用：HDC 也可能来自另一个代理的
 * user32 层（GetDC），那一层的编码不是我们能假设的。 */
static int ga_is_our_hdc(uint32_t hdc)
{
    return GA_HANDLE_KIND(hdc) == GA_HDC_TAG;
}

/* GetDeviceCaps 索引（wingdi.h）。索引值是 ABI 的一部分，不能改。 */
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

/* RASTERCAPS 位（wingdi.h）。写成 OR 形式，读的人不必去查魔数。 */
#define GA_RC_BITBLT      0x0001u
#define GA_RC_BITMAP64    0x0008u
#define GA_RC_DEVBITS     0x0020u
#define GA_RC_DI_BITMAP   0x0080u
#define GA_RC_DIBTODEV    0x0200u
#define GA_RC_STRETCHBLT  0x0800u
#define GA_RC_STRETCHDIB  0x2000u

/* 「本设备能力」的单一事实来源。index 未知时返回 -1（0 会被误读成
 * 「不支持」，-1 更显眼，和真实 GDI 对未知索引的行为也接近）。 */
static int32_t ga_device_caps(uint32_t index)
{
    uint32_t w = host_gl_screen_width ? host_gl_screen_width : 1280U;
    uint32_t h = host_gl_screen_height ? host_gl_screen_height : 720U;

    switch (index) {
    case GA_CAP_DRIVERVERSION:  return 0x0400;      /* 合成显示驱动版本 */
    case GA_CAP_TECHNOLOGY:     return 1;           /* DT_RASDISPLAY */
    /* 物理尺寸：按 96 DPI 从像素反推，inch = px/96, mm = inch*25.4 */
    case GA_CAP_HORZSIZE:       return (int32_t)((w * 254U) / 960U);
    case GA_CAP_VERTSIZE:       return (int32_t)((h * 254U) / 960U);
    case GA_CAP_HORZRES:        return (int32_t)w;
    case GA_CAP_VERTRES:        return (int32_t)h;
    case GA_CAP_BITSPIXEL:      return 32;          /* RGBA8888 后台缓冲 */
    case GA_CAP_PLANES:         return 1;           /* 现代显示设备恒为 1 */
    /* 16/18/20/22：0xFFFF = 设备侧不限量（GDI 自己模拟画刷/笔/字体）。 */
    case GA_CAP_NUMBRUSHES:     return 0xFFFF;
    case GA_CAP_NUMPENS:        return 0xFFFF;
    case GA_CAP_NUMMARKERS:     return 0xFFFF;
    case GA_CAP_NUMFONTS:       return 0xFFFF;
    /* NUMCOLORS：>8bpp 设备按 MSDN 返回 -1（"no color table"）。
     * 返回 0 会让游戏以为有 0 色调色板，返回 256 更糟，-1 才是标准答案。 */
    case GA_CAP_NUMCOLORS:      return -1;
    case GA_CAP_PDEVICESIZE:    return 0;           /* 无 CDC 私有块 */
    /* GDI 为显示驱动模拟全部曲线/直线/多边形能力。 */
    case GA_CAP_CURVECAPS:      return 0x01FF;      /* CC_* 全开 */
    case GA_CAP_LINECAPS:       return 0x01FF;      /* LC_* 全开 */
    case GA_CAP_POLYGONCAPS:    return 0x00FF;      /* PC_* 全开 */
    case GA_CAP_TEXTCAPS:       return 0x0001;      /* 仅 TC_OP_CHARACTER */
    case GA_CAP_CLIPCAPS:       return 1;           /* CP_RECTANGLE */
    case GA_CAP_RASTERCAPS:
        /* 无 RC_PALETTE（32bpp 无调色板）；位块传送/拉伸/DIB/devbits 都支持。 */
        return (int32_t)(GA_RC_BITBLT | GA_RC_BITMAP64 | GA_RC_DEVBITS |
                         GA_RC_DI_BITMAP | GA_RC_DIBTODEV |
                         GA_RC_STRETCHBLT | GA_RC_STRETCHDIB);
    case GA_CAP_ASPECTX:        return 36;          /* 画线用的像素纵横比 */
    case GA_CAP_ASPECTY:        return 36;
    case GA_CAP_ASPECTXY:       return 51;          /* round(36*sqrt2) */
    case GA_CAP_LOGPIXELSX:     return 96;          /* 96 DPI 逻辑坐标 */
    case GA_CAP_LOGPIXELSY:     return 96;
    case GA_CAP_SIZEPALETTE:    return 0;           /* 无调色板 */
    case GA_CAP_NUMRESERVED:    return 0;
    case GA_CAP_COLORRES:       return 24;          /* 24 位颜色分辨率 */
    /* 110..121：索引按 wingdi.h；值只是给游戏一个自洽的桌面几何。
     * 低置信度（见文件头 R2），但游戏实测不查这一段。 */
    case GA_CAP_PHYSICALWIDTH:   return (int32_t)w;
    case GA_CAP_PHYSICALHEIGHT:  return (int32_t)h;
    case GA_CAP_PHYSICALOFFSETX: return 0;
    case GA_CAP_PHYSICALOFFSETY: return 0;
    case GA_CAP_SCALINGFACTORX:  return 0;
    case GA_CAP_SCALINGFACTORY:  return 0;
    case GA_CAP_VREFRESH:        return 60;         /* 目标帧率 */
    case GA_CAP_DESKTOPVERTRES:  return (int32_t)h;
    case GA_CAP_DESKTOPHORZRES:  return (int32_t)w;
    case GA_CAP_BLTALIGNMENT:    return 4;          /* 4 字节对齐即可 */
    case GA_CAP_SHADEBLENDCAPS:  return 0;          /* 不做设备侧 alpha 混合 */
    case GA_CAP_COLORMGMTCAPS:   return 0;          /* 无 ICM */
    default:                     return -1;
    }
}

/* 合成 PIXELFORMATDESCRIPTOR：40 字节，32bpp RGBA + 24 位深度 + 8 位模板 +
 * 双缓冲 + 支持 OpenGL。游戏拿它只是看看格式合不合意，不会真去渲染。 */
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

/* `HDC WINAPI CreateDCA(LPCSTR, LPCSTR, LPCSTR, const DEVMODEA*)` —— stdcall, 16B。
 * 返回合成 HDC：游戏会把它传给 SetPixelFormat / wglMakeCurrent / SwapBuffers，
 * 那些函数都不解引用它，所以一个 tag 整数足够，且比伪造地址更安全。 */
static void ga_CreateDCA(CPU *__restrict c)
{
    uint32_t driver = A(c, 0);
    uint32_t device = A(c, 1);
    uint32_t port = A(c, 2);
    uint32_t devmode = A(c, 3);
    uint32_t hdc;

    (void)driver; (void)device; (void)port;   /* 不读 guest 字符串：无后端可配 */
    hdc = ++g_ga_hdc_next;
    /* 「不要返回 NULL 句柄却不记录」：每一次句柄创建都留痕。 */
    ga_note("CreateDCA -> HDC 0x%08x (synthetic, devmode=%s)\n",
            (unsigned)hdc, devmode ? "present (ignored)" : "NULL");
    c->eax = hdc;
    R(c, 16u);
}

/* `int WINAPI GetDeviceCaps(HDC, int index)` —— stdcall, 8B。
 * 这一档必须给**正确数据**：游戏拿它决定分辨率/色深/DPI，给错会走错初始化分支。 */
static void ga_GetDeviceCaps(CPU *__restrict c)
{
    uint32_t hdc = A(c, 0);
    uint32_t index = A(c, 1);
    int32_t value = ga_device_caps(index);

    if (!ga_is_our_hdc(hdc)) {
        /* 只记录一次，不拒绝：HDC 可能来自 user32 层。 */
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

/* `BOOL WINAPI DeleteDC(HDC)` —— stdcall, 4B。TRUE：删除合成句柄永远成功。 */
static void ga_DeleteDC(CPU *__restrict c)
{
    (void)A(c, 0);
    c->eax = 1u;
    R(c, 4u);
}

/* `int WINAPI SetPixelFormat(HDC, int format, const PIXELFORMATDESCRIPTOR*)`
 * —— stdcall, 12B。返回 TRUE：没有真上下文可以选格式，但拒绝会让游戏在
 * 创建窗口阶段直接退到错误分支。ppfd 为 NULL 只记录（仍 TRUE），
 * 因为这里是 bring-up 层，宁可多走一步也不要假失败。 */
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
 * —— stdcall, 16B。
 * 返回值是「像素格式总数」，这里只有 1 个合成格式，所以恒为 1；真实 GDI
 * 在 ppfd!=NULL 且 nBytes>0 时把第 format 号格式写进调用方缓冲。 */
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
        /* 纯查询「有几个格式」，没有越界写。 */
        static int noted;
        if (!noted) { noted = 1; ga_note("DescribePixelFormat format count query\n"); }
    }
    ga_note("DescribePixelFormat(format %u, nBytes %u) -> 1 synthetic format\n",
            (unsigned)format, (unsigned)bytes);
    c->eax = 1u;
    R(c, 16u);
}

/* `BOOL WINAPI SwapBuffers(HDC)` —— stdcall, 4B。TRUE：没有后台缓冲可翻，
 * 但返回 FALSE 会让游戏认为呈现失败并可能直接退出。只记账、不打印（每帧调）。 */
static uint32_t g_ga_swaps;

unsigned host_gl_swap_count(void) { return g_ga_swaps; }

static void ga_SwapBuffers(CPU *__restrict c)
{
    (void)A(c, 0);
    ++g_ga_swaps;
    /* ★ 2026-11-06：启动到第一帧的耗时 —— 用户能感知的"黑屏等待"。 */
    if (g_ga_swaps == 1u) {
        extern void host_boot_mark(const char *) __attribute__((weak));
        if (host_boot_mark) host_boot_mark("first frame rendered (SwapBuffers #1)");
    }
    if (g_ga_swaps == 1u)
        ga_note("SwapBuffers called for the first time (synthetic, no present)\n");

    /* ★★ 2026-11-10：纹理**最终状态**汇总。
     *
     * 为什么需要：现有 texstat 按调用次数统计，中间占位状态会污染
     * 数据 —— 同一 id 先传空、再传实，会被算成一次“全零”。
     * 以 300 帧（~5 秒）为节奏报告真正稳定的数字：
     *   · 已知状态的纹理 id 有多少
     *   · 其中最终内容仍是全零的多少
     *   · 当前帧绑定的那张是否全零（直接在画面上体现）
     * 这个数字才能和用户看到的“贴图缺失”对得上。 */
    if ((g_ga_swaps % 300u) == 0u) {
        extern unsigned char g_tex_zero[];
        extern unsigned g_tex_w[];
        extern unsigned g_tex_h[];
        static const unsigned N = 2048;
        unsigned known = 0, zero = 0;
        for (unsigned i = 1; i < N; i++) {
            if (g_tex_zero[i]) { known++; if (g_tex_zero[i] == 2) zero++; }
        }
        /* ★ 2026-11-10：FBO/占位纹理（g_tex_zero==0）不计入分母。
         * 它们的 glTexImage2D 传 NULL，CPU 侧必然读零 ——
         * 那是看不见，不是坏图。上一版把它们算进“全零”分母，
         * 报出 19-20% 的数字跟用户看到的对不上。 */
        extern int g_cur_tex_id;
        unsigned curz = 0;
        if (g_cur_tex_id > 0 && (unsigned)g_cur_tex_id < N)
            curz = g_tex_zero[g_cur_tex_id];
        /* ★ 本 300 帧内 draw 归属：屏幕 vs 离屏 RT */
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

        /* ★★ 2026-11-10（方案 B）：补传效果。
         *
         * “全零”这个数字现在会因为 draw 时的补传而下降 ——
         * 但下降可能是真修好了，也可能是把“别人稍后写入的内容”
         * 重放进去了（错图）。所以两个数要一起看：
         *   补传 N 张        → 有 N 张在 draw 时又上传了一次
         *   探测后仍未就绪 M → 这些的缓冲区至今还是零，无药可救
         * 配合同帧的 [repair] 行（带首 4 字节和 RGB）才能判定对错。 */
        {
            extern unsigned g_tex_repairs, g_tex_repair_wait;
            extern int host_tex_repair_enabled(void);
            /* 关闭时不打这一行 —— 否则每 300 帧刷一条“0 张”。
             * 开关自身的状态已在第一次探测时打过一次。 */
            if (host_tex_repair_enabled())
                fprintf(stderr,
                    "[texrepair] frame#%u  re-uploads so far %u, %u probes found data still not ready\n",
                    g_ga_swaps, g_tex_repairs, g_tex_repair_wait);
        }

        /* ★ 2026-11-10：前 3 次汇总时把全零的 id 列出来。
         * 只知道“23% 全零”没用 —— 需要知道具体是哪些图的哪些尺寸，
         * 才能判断它们是“本来就空白”（空道具槽、未用图集页）
         * 还是“解码失败”。 */
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

        /* ★★ 2026-11-10：通道分布汇总 —— 判定“数据层是否真的坏了”。
         *
         * 真机画面整体褪色发灰，而手动抽查的 6 张 32x32 纹理
         * （HUD 图标）全部是 R=35 G=0 B=0（G/B 精确为 0）。
         * 真实图像不会这样，所以要么数据只有单通道，
         * 要么格式声明与实际不符。
         *
         * 这一行给出全量分布，直接决定下一步方向：
         *   · “只有R”占比高 → 格式/通道问题，改上传参数
         *   · “彩色”占比高 → 数据没问题，问题在采样/混合
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

            /* ★★ 2026-11-10：列出"只有 R"的具体 id。
             *
             * 这批是可疑对象：真实图像即使红色主调，G/B 也会有
             * 抗锯齿/阴影带来的非零值。精确为 0 意味着那两个字节
             * 根本没写入。
             *
             * 知道具体 id 和尺寸，才能对照 [draw] 判断它们
             * 是不是 HUD/精灵用的那几张。 */
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

                    /* 顺便列出「纯灰度」的尺寸分布 —— 如果是大量
                     * 32x32，那可能就是 HUD 图标被当灰度处理了。 */
                    unsigned g32 = 0, g_other = 0;
                    for (unsigned i = 1; i < N; i++)
                        if (g_tex_class[i] == 4) {
                            if (g_tex_w[i] == 32 && g_tex_h[i] == 32) g32++;
                            else g_other++;
                        }
                    fprintf(stderr, "[gray-list] of the pure-gray ones 32x32=%u, other sizes=%u\n",
                            g32, g_other);

                    /* ★★ 2026-11-10：未翻译 stub 的命中计数。
                     * 这是判断“贴图全零是否因为 SIMD 解码函数未实现”
                     * 的唯一可靠手段 —— 不依赖日志缓冲，计数器必达。 */
                    {
                        extern int g_stub_hits[];
                        unsigned total = 0, distinct = 0;
                        for (int i = 0; i < 65536; i++)
                            if (g_stub_hits[i]) { total += g_stub_hits[i]; distinct++; }
                        fprintf(stderr, "[stubhits] frame#%u hits total=%u across %u distinct stubs\n",
                                g_ga_swaps, total, distinct);
                        if (total) {
                            /* 列出前 10 个命中最多的 */
                            for (int k = 0; k < 10; k++) {
                                int best = -1; int bestv = 0;
                                for (int i = 0; i < 65536; i++)
                                    if (g_stub_hits[i] > bestv) { bestv = g_stub_hits[i]; best = i; }
                                if (best < 0) break;
                                fprintf(stderr, "[stubhits]   0x%04x%04x -> %d hits\n",
                                        0x005d0000 + (best >> 8), best & 0xff, bestv);
                                g_stub_hits[best] = 0;   /* 只报一次 Top10 */
                            }
                        }
                    }
                }
            }
        }
        fflush(NULL);
    }
    /* 帧计数心跳：用来判定"到底有没有进主循环"。
     * 每 600 次打一行（60fps 约 10 秒），只在 FRAME_TRACE=1 时输出。 */
    /* 帧间隔统计：每 600 帧输出一次平均/最大帧时间。
     * 用来判断"50fps"是稳定限速还是掉帧。 */
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
            /* ★ 每帧都更新：只统计**本帧**内的导入次数，
             * 否则 delta 会从启动开始一直累加，失去意义。 */
            unsigned delta = g_host_import_calls - prev_calls;
            prev_calls = g_host_import_calls;

            /* ★★ 2026-11-10：慢帧现场。
             *
             * perf 里反复出现“最大 1005-1026 ms”的尖峰，而好帧只有
             * 17 ms —— 这不是均匀变慢，是周期性卡顿。
             *
             * 两个字段一起看就能定性：
             *   · 前一导入  = 卡顿前最后一个非 SwapBuffers 导入
             *   · 期间导入  = 这一帧内 guest 跑了多少次导入
             *
             * 判断：
             *   期间导入≈0 且 前一导入是个 sleep/等锁桩 → 就是它阻塞
             *   期间导入很大 → guest 在翻译代码里空转（需要 CPU 采样）
             *   前一导入是 gl* → GL 侧（eglSwapBuffers 阻塞） */
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
        /* ★ 2026-11-07：周期改小。原来 600 帧才输出一次，而已知帧率
         * 可能低到个位数 —— 600 帧要几分钟，日志里永远看不到 [perf]，
         * 等于没有测量手段。改成 60 帧（60fps 时 1 秒一次，2fps 时 30 秒一次）。 */
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
        /* ★ 2026-11-06：XInput 轮询探针。
         *
         * 真机日志显示 GetStateEx 只被调 2 次就永久停，而 GetCapabilities
         * 调了 4+ 次。sub_0059d720 的开头有三个开关决定它要不要真的采样：
         *   0x3c803040  手柄系统已初始化（byte）
         *   0x3c8090d8  重入保护（byte，必须为 0）
         *   0x3c803044  已注册设备数（dword，<=0 就整个跳过）
         * 设备对象里的第一个字节也是开关（为 0 就跳过该设备的 GetState）。
         * 把这几项打出来，一次就能定位是哪一层断了。 */
        {
            extern void host_xinput_probe(void) __attribute__((weak));
            if (host_xinput_probe && !getenv("XINPUT_PROBE_OFF"))
                host_xinput_probe();
            /* 资源加载要 100 秒左右；2400 帧后（约 60-80 秒）扫一次堆看解压产物 */
            if (g_ga_swaps >= 2400u) {
                extern void png_scan_heap_once_pub(void) __attribute__((weak));
                if (png_scan_heap_once_pub) png_scan_heap_once_pub();
            }
        }
    }
    /* ★★ 2026-11-11：抓帧必须放在 present_window **之前**。
     *
     * 踩过的坑：原来抓帧在 present 之后，而进程内窗口模式
     * （host_gl4es_present_window 返回 1）在这里直接 `return`，
     * 于是**真机一张都抓不到**，只有本地 pbuffer 模式能抓到。
     * 日志可以证实：真机 native.log 里 `[frame]` 出现 0 次。
     *
     * 语义上也更对：present 之后 back buffer 的内容不再保证有效，
     * 要在 swap 之前读。
     */
    /* 帧抓取：pbuffer 是离屏的，读回来就能看见游戏真正渲染了什么。
     * FRAME_DUMP_EVERY=N 在第 N、2N… 帧各存一张，最多 FRAME_DUMP_MAX 张（默认 5）；
     * FRAME_DUMP_AT=N    只在第 N 帧存一张；
     * FRAME_DUMP_FROM=N  只抓 SwapBuffers >= N 的帧（配合 EVERY 越过前段初始化）。
     *
     * 2026-11-04：上限从写死的 5 改成可配 —— 主菜单要跑到一万多帧，
     * 固定 5 张只覆盖到 cutscene 之前，看不到真正想看的画面。 */
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

            /* ★★ 2026-11-10：文件开关。
             *
             * app 由 Java 启动，拿不到环境变量，所以只能用文件。
             * 与 <game>/exec、<game>/fast、<game>/repair 同一约定。
             *
             * 只用**既有的** host_gl_dump_frame（glFinish + glReadPixels）。
             * ★ 绝不在这里 bind framebuffer、绝不调 glGetIntegerv ——
             * 上一次就是因为抓帧路径里加了 glBindFramebuffer，把
             * 状态改坏，真机崩在 glDeleteProgram → std::terminate。
             * 抓帧必须是纯读。 */
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
                    /* ★ 同上：用 fopen 而不是 access()。 */
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

    /* ★ 2026-11-06：进程内窗口模式 —— 直接把这一帧交给 SurfaceFlinger。
     *
     * 这是 .so 化的核心收益：零拷贝。以前每帧要
     *   glReadPixels(GPU→CPU) + RGB565 转换 + TCP + Java 拷贝 + drawBitmap
     * 现在只要一次 eglSwapBuffers。
     *
     * 返回值 1 = 已经呈现，下面所有读像素/推流/抓帧全部跳过。
     * 返回 0 = 还在 pbuffer 模式（Termux 里跑、或 Java 用了子进程 fallback），
     * 走原来的推流路径，行为完全不变。 */
    {
        extern int host_gl4es_present_window(void) __attribute__((weak));
        if (host_gl4es_present_window && host_gl4es_present_window()) {
            c->eax = 1u;
            R(c, 4u);
            return;
        }
    }

    /* 帧推流给 Java（SurfaceView 显示用）。FRAME_STREAM=N 时每 N 帧推一张。 */
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
 * 五、OPENGL32（5）—— 全是 WGL 上下文管理 + 动态解析器
 *
 * 注意 R1：这一档返回的「上下文」只是标签。真正的 GL 调用不经过这里，
 * 而是经 wglGetProcAddress 拿到地址后由 guest 直接 call。
 * ========================================================================== */

/* `HGLRC WINAPI wglCreateContext(HDC)` —— stdcall, 4B。
 * 返回非 NULL 合成上下文：返回 NULL 会让游戏认为「驱动不支持 OpenGL」并
 * 直接弹错误框/退出，那样后面什么都测不到。 */
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

/* `BOOL WINAPI wglDeleteContext(HGLRC)` —— stdcall, 4B。TRUE。 */
static void ga_wglDeleteContext(CPU *__restrict c)
{
    uint32_t hglrc = A(c, 0);

    ga_note("wglDeleteContext(HGLRC 0x%08x) -> TRUE\n", (unsigned)hglrc);
    c->eax = 1u;
    R(c, 4u);
}

/* `BOOL WINAPI wglMakeCurrent(HDC, HGLRC)` —— stdcall, 8B。
 * TRUE：游戏把「当前上下文」当成后续 GL 调用的前提，FALSE 会让它反复重试
 * 甚至放弃渲染路径。真正需要拒绝时（没有后端）由 R1 的解析器给 0 来体现。 */
static void ga_wglMakeCurrent(CPU *__restrict c)
{
    uint32_t hdc = A(c, 0);
    uint32_t hglrc = A(c, 1);

    ga_note("wglMakeCurrent(HDC 0x%08x, HGLRC 0x%08x) -> TRUE\n",
            (unsigned)hdc, (unsigned)hglrc);
    c->eax = 1u;
    R(c, 8u);
}

/* `BOOL WINAPI wglShareLists(HGLRC, HGLRC)` —— stdcall, 8B。TRUE。 */
static void ga_wglShareLists(CPU *__restrict c)
{
    uint32_t a = A(c, 0);
    uint32_t b = A(c, 1);

    ga_note("wglShareLists(0x%08x, 0x%08x) -> TRUE\n",
            (unsigned)a, (unsigned)b);
    c->eax = 1u;
    R(c, 8u);
}

/* 可选接缝：接线方定义了这个弱符号，wglGetProcAddress 就从它取 token
 * （例如 gl_bridge 的 0x7e…… 类型化 token，再由 guest_host_dynamic 分发）。
 * 没有定义时弱符号为 NULL，我们诚实地回答 0。 */
extern uint32_t host_gl_proc_resolve(const char *name) __attribute__((weak));

/* wglGetProcAddress 请求名的去重日志：只打印每个不同名字一次，最多 24 个。 */
#define GA_PROC_LOG 24u
#define GA_PROC_NAME_MAX 32u
static char     g_ga_proc_names[GA_PROC_LOG][GA_PROC_NAME_MAX];
static unsigned g_ga_proc_names_n;

static void ga_proc_note(const char *name, uint32_t token)
{
    unsigned i;

    for (i = 0u; i < g_ga_proc_names_n; ++i) {
        if (strcmp(g_ga_proc_names[i], name) == 0)
            return;                       /* 同名不重复打印 */
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

/* `PROC WINAPI wglGetProcAddress(LPCSTR)` —— stdcall, 4B。
 *
 * 这是整个 GL 面的**唯一入口**：游戏（libepoxy 风格加载器）在这里解析
 * glClear/glTexImage2D/... 全部入口。当前没有 GL 后端，所以：
 *   · 有 host_gl_proc_resolve 弱符号 → 用它的 token（低 4GB 空间的类型化 token，
 *     由 guest_host_dynamic 分发；直接返回宿主函数指针会让 guest 把它当 x86
 *     地址 call，必崩）
 *   · 否则返回 0，并**带名记录**请求过哪些函数
 * 返回 0 是诚实的：加载器会存 0，第一次真正绘制时在 guest 侧触发可归因的
 * "indirect call to untranslated address"。任何非 0 的伪造值都会把崩溃推远。
 * 猜大小写变体没有意义：guest 传的永远是编译期字符串。 */
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
 * 六、OpenAL32（24）—— 记账式实现
 *
 * 约定：全部 cdecl（只 gpop）。见文件头 §二 的证据。
 * 目标：句柄永不 NULL、ID 单调递增、错误锁存语义与真 OpenAL 一致，
 * 但**不发声**。所有 al* 都不抛错，除非调用本身明显非法（NULL 出参等）。
 * ========================================================================== */

/* AL 常量（al.h） */
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

/* ALC 对象块：低 4GB + magic 校验。64 字节足够，也留了以后放真后端指针的位置。 */
#define GA_ALC_DEVICE_MAGIC  0x414C4344u
#define GA_ALC_CONTEXT_MAGIC 0x414C4343u
#define GA_ALC_BLOCK_BYTES   64u

/* ALC 对象头：magic 在 0，kind 在 4，序号在 8。 */
static uint32_t g_ga_alc_dev_block;
static uint32_t g_ga_alc_ctx_block;
static uint32_t g_ga_alc_current;
static unsigned g_ga_alc_dev_seq;
static unsigned g_ga_alc_ctx_seq;

/* 分配失败时的退化句柄：tag 整数，非 NULL，绝不和真指针混淆。 */
#define GA_ALC_DEVICE_FALLBACK  0x00A1D001u
#define GA_ALC_CONTEXT_FALLBACK 0x00A1C001u

/* 身份优先校验：**先**比对这是我们自己发出的那一个块，再解引用。
 * 只做范围检查是不够的 —— guest 可能传一个 0x3Cxxxxxx 的随机值，而那段
 * 只有 bump 分配器真正映射过的页才可读，直接 ld32 会 SEGV。
 * 退化 tag 句柄（分配失败时用的）落在低 4GB 段之外，没有内存可查，
 * 但它仍是「我们发出的」，所以只做身份比对、不解引用。 */
static int ga_alc_valid(uint32_t block, uint32_t magic)
{
    uint32_t known = (magic == GA_ALC_DEVICE_MAGIC)
                     ? g_ga_alc_dev_block : g_ga_alc_ctx_block;

    if (!block || block != known)
        return 0;                  /* 不是我们发出的：绝不解引用 */
    if (block < GA_LOW_BASE || block >= GA_LOW_END)
        return 1;                  /* 退化 tag 句柄：算活，无内存可查 */
    if ((block & 7u) != 0u)
        return 0;                  /* 不该发生：bump 分配 16 字节对齐 */
    return ld32(block) == magic;    /* 已销毁则 magic 为 0 */
}

/* 销毁：只有真正落在低 4GB 段里的块才有内存可以清 magic。 */
static void ga_alc_invalidate(uint32_t block)
{
    if (block >= GA_LOW_BASE && block < GA_LOW_END && (block & 7u) == 0u)
        st32(block, 0u);
}

/* ---------------- 源 / buffer 记账 ---------------- */

#define GA_AL_MAX_SOURCES 256u
#define GA_AL_RING        16u

typedef struct {
    uint32_t id;          /* 0 = 空槽 */
    uint16_t state;       /* AL_INITIAL / AL_PLAYING / AL_PAUSED / AL_STOPPED */
    uint16_t queued;      /* 已排队 buffer 数 */
    uint16_t ring_head;
    uint16_t ring_count;  /* ring 里实际存了几个 id（<= GA_AL_RING） */
    uint32_t ring[GA_AL_RING];
} ga_al_source;

static ga_al_source g_ga_sources[GA_AL_MAX_SOURCES];

/* 源 ID 从 1 开始单调递增；buffer ID 从 0x00010001 开始（两套命名空间分开，
 * 便于日志里一眼分清，且两者都不会是 AL_NONE=0）。 */
static uint32_t g_ga_source_next;
static uint32_t g_ga_buffer_next  = 0x00010001u;
static uint32_t g_ga_buffers_made;
static uint64_t g_ga_audio_bytes;

/* AL 错误是「一次读走」的锁存器：多个错误只保留第一个，alGetError 清空。 */
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
        return NULL;    /* 从未发出，或者槽位已被更年轻的源复用 */
    return s;
}

/* ---------------- 设备 / 上下文（6 项） ---------------- */

/* `ALCdevice *alcOpenDevice(const ALCchar*)` —— cdecl。
 * 返回非 NULL：游戏把 NULL 当成「没有音频设备」，会跳过整个 Sound::Manager
 * 初始化。低 4GB 块 + magic，回来时可校验。 */
static void ga_alcOpenDevice(CPU *__restrict c)
{
    uint32_t name = A(c, 0);
    uint32_t device;

    (void)name;                    /* 不读设备名字符串：无后端可挑 */
    if (!g_ga_alc_dev_block) {
        g_ga_alc_dev_block = ga_low_alloc(GA_ALC_BLOCK_BYTES);
        if (g_ga_alc_dev_block) {
            st32(g_ga_alc_dev_block, GA_ALC_DEVICE_MAGIC);
            st32(g_ga_alc_dev_block + 4u, 1u);        /* kind = device */
            st32(g_ga_alc_dev_block + 8u, ++g_ga_alc_dev_seq);
        } else {
            /* 分配器失败也不返回 NULL —— 退化成 tag 句柄并留痕。 */
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

/* `ALCcontext *alcCreateContext(ALCdevice*, const ALCint*)` —— cdecl。
 * 返回非 NULL 同理：NULL 会让游戏认为「上下文创建失败」并放弃音频。 */
static void ga_alcCreateContext(CPU *__restrict c)
{
    uint32_t device = A(c, 0);
    uint32_t attrlist = A(c, 1);
    uint32_t context;

    (void)attrlist;                /* 采样率/声道数等属性：无后端，忽略 */
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

/* `ALCboolean alcMakeContextCurrent(ALCcontext*)` —— cdecl。
 * 返回 ALC_TRUE(1)。按规范传 NULL 表示「释放当前上下文」，同样返回 TRUE，
 * 所以这里恒为 TRUE，只改变我们自己记的 current。 */
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

/* `void alcDestroyContext(ALCcontext*)` —— cdecl。只让 magic 失效，
 * 不 munmap：这一层不值得为个位数对象做低 4GB 段的归还。 */
static void ga_alcDestroyContext(CPU *__restrict c)
{
    uint32_t context = A(c, 0);

    if (ga_alc_valid(context, GA_ALC_CONTEXT_MAGIC)) {
        ga_alc_invalidate(context); /* magic 清零 = 已销毁 */
        g_ga_alc_ctx_block = 0u;    /* 放开「发过一个上下文」的槽位 */
    }
    if (g_ga_alc_current == context)
        g_ga_alc_current = 0u;
    ga_note("alcDestroyContext(0x%08x)\n", (unsigned)context);
    cdecl_ret(c);
}

/* `ALCboolean alcCloseDevice(ALCdevice*)` —— cdecl。返回 TRUE：
 * 关闭一个我们自己的设备永远成功；不成功会让游戏在退出路径上卡住。 */
static void ga_alcCloseDevice(CPU *__restrict c)
{
    uint32_t device = A(c, 0);

    if (ga_alc_valid(device, GA_ALC_DEVICE_MAGIC)) {
        ga_alc_invalidate(device);
        g_ga_alc_dev_block = 0u;    /* 放开槽位，下次 alcOpenDevice 重新分配 */
    }
    ga_note("alcCloseDevice(0x%08x) -> TRUE\n", (unsigned)device);
    c->eax = GA_AL_TRUE;
    cdecl_ret(c);
}

/* `void alcProcessContext(ALCcontext*)` —— cdecl。no-op 即正确：
 * 我们的「上下文」不需要显式提交，本来就没有被挂起的概念。 */
static void ga_alcProcessContext(CPU *__restrict c)
{
    (void)A(c, 0);
    cdecl_ret(c);
}

/* ---------------- 源 / buffer 生成与删除（5 项） ---------------- */

/* `void alGenSources(ALsizei n, ALuint *sources)` —— cdecl。
 * 写 n 个**单调递增且非 0** 的 ID：0 是 AL_NONE，游戏会把它当「没有源」
 * 并跳过后续 alSourcei/alSourcePlay，等于悄悄关掉音频。 */
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

/* `void alGenBuffers(ALsizei n, ALuint *buffers)` —— cdecl。
 * 同 alGenSources：ID 非 0、单调递增，另起 0x00010001 段与源区分。 */
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

/* `void alDeleteSources(ALsizei n, const ALuint *sources)` —— cdecl。
 * 只接受本层发出的 ID；遇到陌生 ID 按真 OpenAL 语义置 AL_INVALID_NAME 并
 * 立即停止。这是少数会拒绝的路径之一：它用来暴露接线错误，而不是隐藏它们。 */
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
        s->id = 0u;                /* 槽位释放；排队状态一并清掉 */
        s->queued = 0u;
        s->ring_count = 0u;
        s->ring_head = 0u;
    }
    cdecl_ret(c);
}

/* `void alDeleteBuffers(ALsizei n, const ALuint *buffers)` —— cdecl。
 * buffer 没有逐条状态（流式音乐会造几千个），所以只做「发过的 ID 范围」检查：
 * 从未发出的 ID 视作 AL_INVALID_NAME。重复删除不报错（无害，见 R3）。 */
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

/* ---------------- 播放控制（4 项） ---------------- */

/* `void alSourcePlay(ALuint)` —— cdecl。状态改 AL_PLAYING：
 * 游戏轮询 AL_SOURCE_STATE，恒为 AL_INITIAL 会让它反复重新触发播放。 */
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

/* `void alSourceStop(ALuint)` —— cdecl。→ AL_STOPPED（可被重新 play）。 */
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

/* `void alSourcePause(ALuint)` —— cdecl。只有 PLAYING 才转 PAUSED，
 * 其它状态保持原样（和真 OpenAL 对非播放源 pause 的 no-op 行为一致）。 */
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

/* `void alSourceQueueBuffers(ALuint, ALsizei, const ALuint*)` —— cdecl。
 * 真记账：每个源的排队 ID 进环形缓冲（深 16），alGetSourcei(QUEUED) 与
 * alSourceUnqueueBuffers 都读这份记录。超过环深只加计数不存 ID。 */
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

/* `void alSourceUnqueueBuffers(ALuint, ALsizei, ALuint*)` —— cdecl。
 *
 * 这是我们和真 OpenAL 唯一的语义分歧，见 R3：因为不真的播放，
 * AL_BUFFERS_PROCESSED 恒为 0，游戏因此**不会**走到这里。若它还是调了，
 * 就从环头按 FIFO 退还 ID（不足补 0），保证出参总有定义值、guest 不会读到
 * 未初始化内存 —— 宁可「多退了一个 buffer」也不要让 guest 读垃圾。 */
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

/* ---------------- 查询（4 项） ---------------- */

/* `ALenum alGetError(void)` —— cdecl。返回锁存的错误（默认 AL_NO_ERROR=0）
 * 并清空。真 OpenAL 是每线程一个锁存器；本运行时是单 guest 线程同步执行，
 * 一个静态变量就够。 */
static void ga_alGetError(CPU *__restrict c)
{
    uint32_t error = g_ga_al_error;

    g_ga_al_error = GA_AL_NO_ERROR;
    c->eax = error;
    cdecl_ret(c);
}

/* `void alGetSourcei(ALuint, ALenum, ALint*)` —— cdecl。
 * 三个参数有真值：SOURCE_STATE / BUFFERS_QUEUED / SOURCE_TYPE；
 * 其余一律 0 —— 0 是这些枚举的合法值，不会让游戏读到垃圾。
 * AL_BUFFERS_PROCESSED 刻意恒为 0：谎报「已播完」会让音乐流以 CPU 全速
 * 无限重灌 buffer（忙循环）。见文件头 R3。 */
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
        st32(out, 0u);             /* 仍然填一个定义值，避免 guest 读垃圾 */
        cdecl_ret(c);
        return;
    }
    switch (param) {
    case GA_AL_SOURCE_STATE:      st32(out, (uint32_t)s->state); break;
    case GA_AL_BUFFERS_QUEUED:    st32(out, (uint32_t)s->queued); break;
    case GA_AL_BUFFERS_PROCESSED: st32(out, 0u); break;
    /* 有排队 buffer 说明是流式源；没有则 UNDETERMINED。两者都不影响播放
     * 语义，只是给游戏一个自洽的回答。 */
    case GA_AL_SOURCE_TYPE:       st32(out, GA_AL_UNDETERMINED); break;
    default:                      st32(out, 0u); break;
    }
    cdecl_ret(c);
}

/* `void alGetSourcef(ALuint, ALenum, ALfloat*)` —— cdecl。
 * 全部写 0.0f：没有混音器就没有增益/偏移/秒数可报。
 * 注意 0.0f 对 AL_GAIN/AL_PITCH 来说是「静音/零音高」，但这一层本来就不发声；
 * 若以后接真后端，这个函数必须改成读真值，否则音量控制会一直读到 0。 */
static void ga_alGetSourcef(CPU *__restrict c)
{
    uint32_t id = A(c, 0);
    uint32_t out;

    (void)A(c, 1);                 /* param：无值可查，一律 0.0f */
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

/* ---------------- 参数设置（5 项） ---------------- */

/* `void alSourcei(ALuint, ALenum, ALint)` —— cdecl。只接受不记账：
 * 没有混音器时这些参数无处可施，但它们不是错误，所以只在源无效时置错误码。 */
static void ga_alSourcei(CPU *__restrict c)
{
    uint32_t id = A(c, 0);

    (void)A(c, 1);
    (void)A(c, 2);
    if (!ga_al_slot(id))
        ga_al_set_error(GA_AL_INVALID_NAME);
    cdecl_ret(c);
}

/* `void alSourcef(ALuint, ALenum, ALfloat)` —— cdecl。
 * 浮点参数在 x86 栈上占 4 字节；这里不读（无混音器），但**不能错位**：
 * cdecl 下参数个数已由 guest 自己 add esp,N 清理。 */
static void ga_alSourcef(CPU *__restrict c)
{
    uint32_t id = A(c, 0);

    (void)A(c, 1);
    (void)A(c, 2);
    if (!ga_al_slot(id))
        ga_al_set_error(GA_AL_INVALID_NAME);
    cdecl_ret(c);
}

/* `void alSource3f(ALuint, ALenum, ALfloat, ALfloat, ALfloat)` —— cdecl。 */
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

/* `void alListener3f(ALenum, ALfloat, ALfloat, ALfloat)` —— cdecl。
 * 监听器位置/速度：无后端，接受即可（不置错误码，避免游戏进错误分支）。 */
static void ga_alListener3f(CPU *__restrict c)
{
    (void)A(c, 0);
    (void)A(c, 1);
    (void)A(c, 2);
    (void)A(c, 3);
    cdecl_ret(c);
}

/* `void alListenerfv(ALenum, const ALfloat*)` —— cdecl。
 * 唯一的检查是出参不能 NULL（真 OpenAL 会置 AL_INVALID_VALUE）。
 * 我们不解引用它（反正不发声），所以连读都不读，安全。 */
static void ga_alListenerfv(CPU *__restrict c)
{
    uint32_t values;

    (void)A(c, 0);
    values = A(c, 1);
    if (!values)
        ga_al_set_error(GA_AL_INVALID_VALUE);
    cdecl_ret(c);
}

/* `void alBufferData(ALuint, ALenum, const ALvoid*, ALsizei, ALsizei)` —— cdecl。
 * 真记账：累计上传字节数，供以后接真后端时核对（Vita 版就是走这条路上传）。
 * 这里**不拷贝**音频数据：没有混音器读它，拷贝只是浪费内存带宽。
 * 明显非法的调用（size>0 但 data=NULL，或 buffer ID 从未发出）只置错误码。 */
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
        /* 只记录前 16 次上传的形状：流式音乐会调用成千上万次。 */
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
 * 七、分发表 + 对外接口
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
    /* OPENGL32 (5) —— 全是 WGL，没有 gl* 绘制导入（见文件头 §一） */
    { "OPENGL32.dll!wglDeleteContext",  ga_wglDeleteContext },
    { "OPENGL32.dll!wglCreateContext",  ga_wglCreateContext },
    { "OPENGL32.dll!wglShareLists",     ga_wglShareLists },
    { "OPENGL32.dll!wglGetProcAddress", ga_wglGetProcAddress },
    { "OPENGL32.dll!wglMakeCurrent",    ga_wglMakeCurrent },
};

static const ga_entry GA_AUDIO_TABLE[] = {
    /* OpenAL32 (24) —— 顺序照 guest_table.c 的 IAT 顺序 */
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

/* 精确 strcmp，与 host_android.c 同一约定：名字来自编译期导入表，
 * 不需要大小写变体猜测（猜错会静默走到错误实现）。 */
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

    /* ★ 2026-11-07：先把 OpenAL32.dll 的导入转发给真正的 OpenAL Soft
     * （host_openal.c）。旧桩永远只是记账，从来没发出过声音。
     *
     * 返回 0 表示"真 OpenAL 不可用或不认识这个名字"，此时继续走下面的
     * 旧桩 —— 失败时行为与之前完全一致，不会引入新的崩溃点。 */
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
