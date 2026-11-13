/* Android 宿主层 —— steam_api.dll（11 个导入）
 *
 * ★ 这不是假桩。
 *
 * 判据来自 PC 行为：一台没装 Steam / 没有 steam_appid.txt 的机器上，
 * SteamAPI_Init() 返回 false，游戏走离线降级路径继续跑。所以这里实现的
 * 是「没有 Steam 运行时」这一真实场景下的正确语义，而不是「假装成功」。
 * 假装成功才是错的：游戏会以为成就、云存档、好友可用，然后在一个
 * 半可用的状态里出各种错。
 *
 * 调用约定是逐条从调用点反汇编实测出来的，不是照 SDK 头文件抄的
 * （实测和头文件在 RegisterCallback 上就不一致）。判据：call 之后
 * 紧接 add esp,N → cdecl，N 是参数总字节；没有 add esp → 被调方清理
 * （stdcall），参数总字节由 call 前的 push 数量决定。
 *
 *   函数                             约定      参数字节  证据（RVA 0x）
 *   SteamAPI_Init                    stdcall    0        598c80 后接 test al,al
 *   SteamAPI_Shutdown                stdcall    0        598e91 后接 mov esi,...
 *   SteamAPI_RunCallbacks            stdcall    0        598d9e 后接 mov ecx,...
 *   SteamAPI_GetHSteamPipe           stdcall    0        18a32  后接 test eax,eax
 *   SteamAPI_GetHSteamUser           stdcall    0        18a55  后接 mov ebx,eax
 *   SteamAPI_RegisterCallback        cdecl      8        1dea   thunk 栈平衡 12/12
 *   SteamAPI_UnregisterCallback      cdecl      4        605a28 后接 add esp,4
 *   SteamAPI_RegisterCallResult      cdecl      12       3f808b 后接 add esp,0xc
 *   SteamAPI_UnregisterCallResult    cdecl      12       3f8055 后接 add esp,0xc
 *   SteamInternal_CreateInterface    cdecl      4        18a72  后接 add esp,4
 *   SteamInternal_ContextInit        cdecl      4        25b747 后接 add esp,4
 *
 * steamAppId / steam_appid.txt 也没有 —— 游戏读不到就不会连 Steam。
 *
 * 低 4GB 约束（本项目踩过 4 次的坑）：SteamInternal_ContextInit 必须返回
 * 一个 guest 能解引用的指针。调用点是
 *      push 0xbaa3c8 / call ContextInit / add esp,4 / cmp [eax+0x24],0
 * 返回 0 会立刻崩在读 [0x24]。所以返回一块低 4GB 的零填充结构：
 * [eax+0x24] == 0 正是「接口不可用」的表示，游戏会跳过 Steam 路径。
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "guest.h"

/* ---- 调用约定助手（与 host_android.c 一致） ---- */
static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
/* stdcall：弹出返回地址 + 清 n 字节参数 */
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}
/* cdecl：只弹返回地址，参数由调用方的 add esp,N 清理 */
static void Rc(CPU *__restrict c)
{
    (void)gpop(c);
}

/* ---- ContextInit 的返回块（低 4GB，零填充 = 无接口） ---- */
#define STEAM_CTX_ADDR 0x3A000000u
#define STEAM_CTX_SIZE 0x1000u

static uint32_t steam_ctx_addr;
static int      steam_ctx_state;   /* 0=未尝试 1=就绪 -1=失败 */

static uint32_t steam_ctx(void)
{
    if (steam_ctx_state == 1) return steam_ctx_addr;
    if (steam_ctx_state == -1) return 0;
    void *p = mmap((void *)(uintptr_t)STEAM_CTX_ADDR, STEAM_CTX_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)STEAM_CTX_ADDR) {
        fprintf(stderr,
                "[steam] cannot establish the context block at 0x%08x (see errno above);"
                "SteamInternal_ContextInit cannot hand out a low-4GB pointer\n", STEAM_CTX_ADDR);
        steam_ctx_state = -1;
        return 0;
    }
    /* mmap ANONYMOUS 已是全零 —— [ctx+0x24] == 0 就是「接口不可用」 */
    steam_ctx_addr = STEAM_CTX_ADDR;
    steam_ctx_state = 1;
    return steam_ctx_addr;
}

/* ---- 生命周期 ---- */
static void h_SteamAPI_Init(CPU *c)
{
    /* 没有 Steam 运行时 → false。游戏据此走离线路径。 */
    c->eax = 0;
    fprintf(stderr, "[steam] SteamAPI_Init -> false (no Steam runtime; offline mode)\n");
    R(c, 0);
}

static void h_SteamAPI_Shutdown(CPU *c)
{
    /* 从未初始化过，关掉也没什么可关的。 */
    R(c, 0);
}

static void h_SteamAPI_RunCallbacks(CPU *c)
{
    /* 没有 Steam 就没有回调被投递。空转是这里唯一正确的行为。 */
    R(c, 0);
}

/* ---- 句柄：无效值就是 0 ---- */
static void h_SteamAPI_GetHSteamPipe(CPU *c) { c->eax = 0; R(c, 0); }
static void h_SteamAPI_GetHSteamUser(CPU *c) { c->eax = 0; R(c, 0); }

/* ---- 回调注册 ----
 * Init 返回 false 之后游戏本来就不该依赖这些回调被调用，所以不记账。
 * 记账反而会撒谎：真记下来就得在 RunCallbacks 里调用它们，而那时
 * 回调对象里的 Steam 接口全是空指针，会炸在游戏自己的代码里。
 */
static void h_SteamAPI_RegisterCallback(CPU *c)
{
    (void)A(c, 0);   /* pCallback */
    (void)A(c, 1);   /* iCallback */
    Rc(c);           /* cdecl，参数 8 字节由调用方 add esp,8 清理 */
}

static void h_SteamAPI_UnregisterCallback(CPU *c)
{
    (void)A(c, 0);
    Rc(c);           /* cdecl，4 字节 */
}

static void h_SteamAPI_RegisterCallResult(CPU *c)
{
    (void)A(c, 0);   /* pCallback */
    (void)A(c, 1);   /* hAPICall 低 32 位 */
    (void)A(c, 2);   /* hAPICall 高 32 位 */
    Rc(c);           /* cdecl，12 字节 */
}

static void h_SteamAPI_UnregisterCallResult(CPU *c)
{
    (void)A(c, 0);
    (void)A(c, 1);
    (void)A(c, 2);
    Rc(c);           /* cdecl，12 字节 */
}

/* ---- 接口 ---- */
static void h_SteamInternal_CreateInterface(CPU *c)
{
    uint32_t ver = A(c, 0);
    const char *name = ver ? (const char *)(uintptr_t)ver : "(null)";
    /* 没有 Steam 进程，任何接口版本都拿不到 → NULL。调用点检查了返回值。 */
    fprintf(stderr, "[steam] CreateInterface(\"%s\") -> NULL\n", name);
    c->eax = 0;
    Rc(c);           /* cdecl，4 字节 */
}

static void h_SteamInternal_ContextInit(CPU *c)
{
    (void)A(c, 0);   /* pContextInitData —— 真 Steam 会往里写，这里不需要 */
    /* 必须返回可解引用的低 4GB 指针：[+0x24] == 0 表示接口不可用。 */
    c->eax = steam_ctx();
    Rc(c);           /* cdecl，4 字节 */
}

/* ---- 分发 ---- */
typedef struct { const char *name; void (*fn)(CPU *); } steam_entry;

static const steam_entry s_steam[] = {
    { "steam_api.dll!SteamAPI_Init",                  h_SteamAPI_Init },
    { "steam_api.dll!SteamAPI_Shutdown",              h_SteamAPI_Shutdown },
    { "steam_api.dll!SteamAPI_RunCallbacks",          h_SteamAPI_RunCallbacks },
    { "steam_api.dll!SteamAPI_GetHSteamPipe",         h_SteamAPI_GetHSteamPipe },
    { "steam_api.dll!SteamAPI_GetHSteamUser",         h_SteamAPI_GetHSteamUser },
    { "steam_api.dll!SteamAPI_RegisterCallback",      h_SteamAPI_RegisterCallback },
    { "steam_api.dll!SteamAPI_UnregisterCallback",    h_SteamAPI_UnregisterCallback },
    { "steam_api.dll!SteamAPI_RegisterCallResult",    h_SteamAPI_RegisterCallResult },
    { "steam_api.dll!SteamAPI_UnregisterCallResult",  h_SteamAPI_UnregisterCallResult },
    { "steam_api.dll!SteamInternal_CreateInterface",  h_SteamInternal_CreateInterface },
    { "steam_api.dll!SteamInternal_ContextInit",      h_SteamInternal_ContextInit },
};

/* 返回 1 = 已处理；0 = 不是本层负责的导入 */
int host_steam_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof s_steam / sizeof s_steam[0]; i++) {
        if (strcmp(s_steam[i].name, name) == 0) {
            s_steam[i].fn(c);
            return 1;
        }
    }
    return 0;
}

int host_steam_count(void)
{
    return (int)(sizeof s_steam / sizeof s_steam[0]);
}
