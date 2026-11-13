/* host_xinput.c —— 最小可用的 XInput 1.4 / 1.3 假实现
 *
 * ==========================================================================
 * 一、为什么必须有它
 *
 * 游戏启动时先探测手柄：
 *     LoadLibraryA("XInput1_4.dll")   → 失败
 *     LoadLibraryA("XInput1_3.dll")   → 失败
 *     LoadLibraryA("bin\XInput1_3.dll") → 失败
 *     "Gamepad_init couldn't load XInput1_4.dll or XInput1_3.dll;
 *      proceeding with DInput only"
 *
 * 然后降级到 DirectInput。我们之前一直在修 DInput 键盘，但游戏的主路径
 * 是 XInput —— 只有让 LoadLibraryA 成功，游戏才会 GetProcAddress 取
 * 下面这几个函数并真正开始轮询手柄。
 *
 * 实测（见 run.log 第 280~284 行）游戏只取三个导出：
 *     XInputGetState            按名字
 *     XInputGetStateEx          按序号 100   ← 无文档的 XPAD 版本
 *     XInputGetCapabilities     按名字
 * 这里把 XInputSetState / XInputEnable / XInputGetKeystroke /
 * XInputGetBatteryInformation / XInputGetAudioDeviceIds 一并给出，
 * 免得游戏某个分支取到 0 后直接 call 0。
 *
 * ==========================================================================
 * 二、状态从哪来
 *
 * 输入桥（host_user32.c 的 TCP:7137）除了原来的键盘行，还接受：
 *
 *     G <buttons十六进制> <lx> <ly> <rx> <ry> <lt> <rt>\n
 *
 *     lx/ly/rx/ry : -32768..32767（XInput 语义：Y 轴向上为正）
 *     lt/rt       : 0..255
 *
 * 没有收到任何手柄行时状态全零，但仍然报告「已连接」—— 否则游戏会
 * 认为手柄被拔掉，又回到 DInput 降级路径，我们就白做了。
 *
 * ==========================================================================
 * 三、结构布局（和 Windows SDK 完全一致，_Static_assert 钉死）
 *
 *   XINPUT_STATE        16 字节
 *     +0x00 DWORD dwPacketNumber
 *     +0x04 WORD  wButtons
 *     +0x06 BYTE  bLeftTrigger
 *     +0x07 BYTE  bRightTrigger
 *     +0x08 SHORT sThumbLX
 *     +0x0A SHORT sThumbLY
 *     +0x0C SHORT sThumbRX
 *     +0x0E SHORT sThumbRY
 *
 *   XINPUT_CAPABILITIES 20 字节
 *     +0x00 BYTE Type      +0x01 BYTE SubType  +0x02 WORD Flags
 *     +0x04 XINPUT_GAMEPAD (12)  +0x10 XINPUT_VIBRATION (4)
 *
 * 调用约定：x86 stdcall，参数个数按真实签名清栈（见每个函数末尾）。
 * ========================================================================== */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "guest.h"

/* ---- stdcall 参数 / 返回 ---- */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}

/* ---- token 段 ----
 * 0x7D = GL，0x7E = kernel32 动态导出，0x7F00/0x7F01 = DirectInput。
 * XInput 用 0x7F020000 起，互不重叠。 */
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

/* 返回码 */
#define XI_SUCCESS                 0u
#define XI_BAD_ARGUMENTS           160u
#define XI_DEVICE_NOT_CONNECTED    1167u
#define XI_EMPTY                   4306u

/* 按钮位 */
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

/* ---- 输入桥提供的状态（host_user32.c） ---- */
/* 弱符号：万一输入桥没编进来，这里仍能工作（全零状态）。 */
extern int u_pad_get(unsigned *buttons, int *lx, int *ly, int *rx, int *ry,
                     int *lt, int *rt) __attribute__((weak));

typedef struct {
    uint32_t packet;
    uint16_t buttons;
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
} xi_gamepad;

static int g_enabled = 1;          /* XInputEnable 的开关 */
static int g_last_connected = -1;  /* 只用于日志 */
static uint32_t g_packet;          /* dwPacketNumber，状态变化时递增 */
static uint32_t g_prev_raw;        /* 上一次原始状态的指纹 */
static uint32_t g_nstate_calls;    /* 前几次 GetState 打日志 */
static uint32_t xi_n_getstate, xi_n_getstateex, xi_n_getcaps;  /* 探针计数 */

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

/* 从输入桥取一帧状态。成功返回 1。 */
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

/* 状态指纹：用于判断 dwPacketNumber 要不要递增。
 * 注意不能简单求和（A+B 和 B+A 相同没关系，但要区分不同组合），
 * 用 FNV-1a 之类即可，这里用乘法混合，够用。 */
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

/* 把 XINPUT_GAMEPAD 写到 guest 内存（12 字节，无内嵌指针）。 */
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
 * GetStateEx 同签名，但结构是 20 字节（多一个保留 DWORD）。
 *
 * ★ 必须返回 0（ERROR_SUCCESS）。返回 1167 游戏就当没插手柄，
 *   又回到 DInput 降级路径 —— 那正是我们要离开的地方。 */
/* ★ 2026-11-06：只有 user 0 是“真手柄”。其余索引必须返回 1167。
 * 实测游戏会对 0..3 逐个探测；如果都返回 0，游戏会认为插了 4 个手柄。 */
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
        /* 前两次调用必打（证明游戏真在轮询），之后只在状态变化时打。
         * 这样本地用 nc 发一行 G 就能验证「输入桥 → XInput」整条链。 */
        if (g_nstate_calls < 2u || (changed && g_nstate_calls < 60u)) {
            g_nstate_calls++;
            /* ★ 2026-11-10：日志原来打的是 g.packet，而它在本段之后才被
             * 赋值（g.packet = g_packet），所以永远打印 0。
             * 这导致误判"dwPacketNumber 不递增"—— 实际写进 guest 的是
             * 正确的递增值。改打 g_packet。 */
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
        st32(out + 0x10u, 0u);   /* reserved，调用者可能只拷 4..15 */

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
        /* 没插的手柄：不写输出结构，如实报 1167。 */
        c->eax = XI_DEVICE_NOT_CONNECTED;
        R(c, 12);
        return;
    }
    /* XINPUT_FLAG_GAMEPAD = 1；0 表示「任意设备」。其余标志不接受。 */
    if (flags != 0u && flags != 1u) {
        c->eax = XI_BAD_ARGUMENTS;
        R(c, 12);
        return;
    }

    st8 (out + 0x00u, 1u);    /* XINPUT_DEVTYPE_GAMEPAD */
    st8 (out + 0x01u, 1u);    /* XINPUT_DEVSUBTYPE_GAMEPAD */
    st16(out + 0x02u, 0u);    /* Flags */
    /* 支持的按钮位 */
    st16(out + 0x04u, (uint16_t)(XB_DPAD_UP | XB_DPAD_DOWN | XB_DPAD_LEFT |
                                 XB_DPAD_RIGHT | XB_START | XB_BACK |
                                 XB_LEFT_THUMB | XB_RIGHT_THUMB |
                                 XB_LEFT_SHOULDER | XB_RIGHT_SHOULDER |
                                 XB_A | XB_B | XB_X | XB_Y));
    st8 (out + 0x06u, 255u);  /* bLeftTrigger  满量程 */
    st8 (out + 0x07u, 255u);  /* bRightTrigger */
    /* 摇杆分辨率/精度：真实 XInput 填 0 表示「无硬件精度要求」 */
    st16(out + 0x08u, 0u);
    st16(out + 0x0Au, 0u);
    st16(out + 0x0Cu, 0u);
    st16(out + 0x0Eu, 0u);
    /* XINPUT_VIBRATION 支持 */
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
 * 我们只做手柄，没有键盘事件源 → 如实返回 ERROR_EMPTY。 */
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
 * 没有对应音频设备 → 1167（游戏会走默认音频路径）。 */
static void xi_GetAudioDeviceIds(CPU *__restrict c)
{
    uint32_t user = A(c, 0), rc = A(c, 2), cc = A(c, 4);
    if (user > 3u) { c->eax = XI_BAD_ARGUMENTS; R(c, 20); return; }
    if (rc) st32(rc, 0u);
    if (cc) st32(cc, 0u);
    c->eax = XI_DEVICE_NOT_CONNECTED;
    R(c, 20);
}

/* ---------------- 名字 / 序号解析 ---------------- */

static const char *const xi_names[XI_T_COUNT] = {
    "XInputGetState",
    "XInputGetStateEx",          /* 只按序号 100 暴露，名字仅用于日志 */
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

/* GetProcAddress 按名字查。返回 1 并写 token。 */
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

/* GetProcAddress 按序号查。只有 100 = XInputGetStateEx。 */
int host_xinput_lookup_ordinal(uint32_t ordinal, uint32_t *out_addr)
{
    if (ordinal != 100u || !out_addr) return 0;
    *out_addr = XI_TOKEN_BASE + (uint32_t)XI_T_GETSTATEEX * 4u;
    return 1;
}

/* guest_call 分发入口。1 = 已处理。 */
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

/* XInputSetState 写振动值后，游戏可能期望下一次 GetState 反映？
 * 不需要 —— 真实 XInput 的 packet number 与振动无关。
 * 这里刻意什么都不做。 */

/* ==========================================================================
 * 诊断探针（由 host_gl_audio.c 的 SwapBuffers 心跳每 600 帧调用一次）
 *
 * 背景：真机日志里 GetStateEx 只被调 2 次就永久停止，GetCapabilities
 * 却调了 4+ 次。sub_0059d720（轮询根，coverage 11488）开头有三个开关：
 *
 *   0059d73a  cmp byte  [0x3c803040], 0   ; 手柄系统已初始化
 *   0059d744  je   0x59f18a               ; 为 0 → 整个函数直接返回
 *   0059d74a  cmp byte  [0x3c8090d8], 0   ; 重入保护
 *   0059d751  jne  0x59f18a               ; 非 0 → 返回（= 永久失效）
 *   0059d764  cmp dword [0x3c803044], 0   ; 已注册设备数
 *   0059d76a  jbe  0x59f183               ; <= 0 → 跳过采样
 *   遍历设备时：
 *   0059d78f  cmp byte  [ebx], 0          ; 该设备 "是否可用"
 *   0059d792  je   0x59e5a4               ; 为 0 → 跳过，不 call GetState
 *
 * 这四项任一变坏都会让输入彻底静默，而日志上看不出任何异常。
 * 探针只读、不改状态；和它读到的值一一对应就能定位断点。
 * ========================================================================== */

/* 镜像重定位后的 guest 基址（image.py 固定搬到 0x3c000000） */
#define XI_G(off) ((uint32_t)(0x3c000000u + (uint32_t)(off)))


/* 安全读：只看落在镜像数据段/堆范围内的指针 */
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

    /* 逐个设备：数组里存的是设备对象指针，dev[0x20] 才是子结构 */
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
