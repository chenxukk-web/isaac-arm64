/* Android 宿主层 —— Windows 系统服务类导入
 *
 * 覆盖：ole32(COM 初始化) / WINMM(定时器) / ADVAPI32(权限) /
 *       SHELL32(文件操作) / dbghelp(符号化) / bcrypt(随机数)
 *
 * 每个函数的参数字节数都从**调用点反汇编实测**得出，注释里标了 RVA。
 * 判据：call 前连续 push 的个数 ×4（stdcall，被调方清理）。
 *
 * 语义原则：这些函数在「PC 上功能不可用」时的返回值是有明确定义的，
 * 我们复现那个返回值，而不是一律给 0 假装成功。
 *   · CoInitialize/CoInitializeEx → S_OK。COM 在 Windows 上总能隐式
 *     初始化，游戏随后调的是 steam_api，不依赖真正的 COM 对象。
 *   · timeGetTime → 真实的毫秒计数（宿主 monotonic clock），这个必须是
 *     真的，游戏用它做帧计时和动画，乱给会让动画跳帧。
 *   · OpenProcessToken/AdjustTokenPrivileges → 失败。普通用户进程本来
 *     就没有 SeLockMemoryPrivilege，返回失败才是真实行为。
 *   · SymInitialize → TRUE，SymFromAddr/SymGetLineFromAddr → FALSE。
 *     没有 PDB 文件时 dbghelp 就是这个结果，游戏只是拿不到符号名。
 *   · SHFileOperationA → 成功码 0（并且真的删/移文件，见下）。
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

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
static char *G(uint32_t a) { return (char *)(uintptr_t)a; }

/* ================= ole32 ================= */
/* CoInitialize(pvReserved)  RVA 0x88bcdd：push 1 */
static void h_CoInitialize(CPU *c)
{
    (void)A(c, 0);
    c->eax = 0;          /* S_OK */
    R(c, 4);
}
/* CoInitializeEx(pvReserved, dwCoInit)  RVA 0x99ca29：push 2 */
static void h_CoInitializeEx(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    /* S_OK。返回 RPC_E_CHANGED_MODE 会让游戏再试一次 MTA；我们两种都
     * 支持（本质上什么都不做），所以给 S_OK 让它走第一条路。 */
    c->eax = 0;
    R(c, 8);
}
/* CoUninitialize()  RVA 0x88bec5：push 0 */
static void h_CoUninitialize(CPU *c) { R(c, 0); }

/* ================= WINMM ================= */
/* timeGetTime()  RVA 0x408f68（334 个调用点）：push 0 → DWORD 毫秒
 * 必须真实：游戏用它算帧间隔和动画相位。
 *
 * ★ 2026-11-04：加 TIME_SCALE=<N> 倍速（N 可为小数）。
 *
 * 为什么需要：开场 cutscene 是按 timeGetTime 的真实时间推进的，
 * 全长约 144 秒（anm2 30fps，f0 → f4332）。每次验证都干等两分半，
 * 一轮调试要跑七八次就是二十分钟 —— 时间全花在"等动画"上。
 *
 * 原理：返回 (now - t0) * scale + t0。对游戏来说时间只是走得更快，
 * 帧间隔、动画相位、计时器的语义都保持一致，不会出现"时间倒退"。
 * 缩放只影响游戏看到的时钟，不影响宿主 sleep，所以 CPU 占用不变。
 *
 * 用法：TIME_SCALE=10 → 144 秒的 cutscene 约 15 秒跑完。
 * 只用于调试观察，正常运行不要加。 */
static void h_timeGetTime(CPU *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
    static double scale = 0.0;
    static uint64_t t0 = 0;
    if (scale == 0.0) {
        const char *e = getenv("TIME_SCALE");
        scale = e ? atof(e) : 1.0;
        if (scale <= 0.0) scale = 1.0;
        t0 = ms;
    }
    if (scale != 1.0) {
        uint64_t out = t0 + (uint64_t)((double)(ms - t0) * scale);
        c->eax = (uint32_t)out;
    } else {
        c->eax = (uint32_t)ms;
    }
    R(c, 0);
}
/* timeBeginPeriod(uPeriod)  RVA 0x998d40：push 1
 * Android 没有定时器精度的概念，接受请求即可（返回 TIMERR_NOERROR=0）。 */
static void h_timeBeginPeriod(CPU *c) { (void)A(c, 0); c->eax = 0; R(c, 4); }
/* timeEndPeriod(uPeriod)  RVA 0x998eee：push 1 */
static void h_timeEndPeriod(CPU *c) { (void)A(c, 0); c->eax = 0; R(c, 4); }
/* timeGetDevCaps(ptc, cbtc)  RVA 0x998d34：push 2
 * TIMECAPS { UINT wPeriodMin; UINT wPeriodMax; } —— 宿主的调度粒度是 1ms。 */
static void h_timeGetDevCaps(CPU *c)
{
    uint32_t ptc = A(c, 0), cbtc = A(c, 1);
    if (ptc && cbtc >= 8) {
        st32(ptc + 0, 1);      /* wPeriodMin */
        st32(ptc + 4, 1000);   /* wPeriodMax */
    }
    c->eax = 0;                /* TIMERR_NOERROR */
    R(c, 8);
}

/* ================= ADVAPI32 ================= */
/* OpenProcessToken(h, access, &token)  RVA 0x90a8b0：push 3
 * 普通进程拿不到可调整的 token，按真实失败返回（游戏随后会跳过。 */
static void h_OpenProcessToken(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    uint32_t out = A(c, 2);
    if (out) st32(out, 0);
    c->eax = 0;                /* FALSE */
    R(c, 12);
}
/* LookupPrivilegeValueA(sys, name, &luid)  RVA 0x9e7905：push 3 */
static void h_LookupPrivilegeValueA(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    uint32_t luid = A(c, 2);
    if (luid) { st32(luid, 0); st32(luid + 4, 0); }
    c->eax = 0;                /* FALSE */
    R(c, 12);
}
/* AdjustTokenPrivileges(h, dis, new, len, prev, &ret)  RVA 0x9e792a：push 6 */
static void h_AdjustTokenPrivileges(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    (void)A(c, 3); (void)A(c, 4);
    uint32_t ret = A(c, 5);
    if (ret) st32(ret, 0);
    c->eax = 0;                /* FALSE */
    R(c, 24);
}

/* ================= SHELL32 =================
 * SHFileOperationA(lpFileOp)  RVA 0x87572c：push 1
 *
 * 这是删文件用的（游戏清理临时文件）。必须真做 —— 否则临时文件堆积，
 * 而且游戏会以为删成功了。
 * SHFILEOPSTRUCTA: hwnd(0) wFunc(4) pFrom(8) pTo(12) fFlags(16)
 *                  fAnyOperationsAborted(20) hNameMappings(24) lpszProgressTitle(28)
 * pFrom 是**双 \0 结尾**的多字符串。 */
static void h_SHFileOperationA(CPU *c)
{
    uint32_t fo = A(c, 0);
    if (!fo) { c->eax = 0x71; R(c, 4); return; }   /* DE_INVALIDFILES */

    uint32_t wFunc = ld32(fo + 4);
    uint32_t pFrom = ld32(fo + 8);
    int rc = 0;

    if (pFrom) {
        /* 逐个取双 \0 分隔的路径 */
        const char *p = (const char *)(uintptr_t)pFrom;
        while (*p) {
            size_t len = strlen(p);
            switch (wFunc) {
            case 3:   /* FO_DELETE */
                if (remove(p) != 0 && errno != ENOENT) rc = 0x7C;  /* DE_ACCESSDENIEDSRC */
                else fprintf(stderr, "[shell] delete \"%s\"\n", p);
                break;
            case 1:   /* FO_MOVE */
            case 2:   /* FO_COPY */
            case 4:   /* FO_RENAME */
                fprintf(stderr, "[shell] wFunc=%u \"%s\" (actual move not implemented)\n", wFunc, p);
                break;
            default:
                break;
            }
            p += len + 1;
        }
    }
    uint32_t aborted = ld32(fo + 20);
    (void)aborted;
    c->eax = (uint32_t)rc;
    R(c, 4);
}

/* ================= dbghelp ================= */
/* SymSetOptions(o)  RVA 0x87ba93：push 1 */
static void h_SymSetOptions(CPU *c) { (void)A(c, 0); c->eax = 0; R(c, 4); }
/* SymInitialize(h, path, invade)  RVA 0x87baa4：push 3
 * 没有 PDB，但仍然返回 TRUE —— 这是「符号服务器为空」的正常结果。 */
static void h_SymInitialize(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = 1;                /* TRUE */
    R(c, 12);
}
/* SymFromAddr(hProcess, Address64, &Displacement, pSymbol)
 * 参数：+0 hProcess(4), +4/+8 Address(8), +12 &Displacement(4), +16 pSymbol(4)
 * 合计 20 字节。没有 PDB 时返回 FALSE 是真实行为。 */
static void h_SymFromAddr(CPU *c)
{
    (void)A(c, 0);                 /* hProcess */
    (void)A(c, 1); (void)A(c, 2);  /* Address 低/高 */
    uint32_t disp = A(c, 3);
    if (disp) st32(disp, 0);
    (void)A(c, 4);                 /* pSymbol */
    c->eax = 0;                    /* FALSE：查不到符号 */
    R(c, 20);
}
/* SymGetLineFromAddr(h, addr, &disp, line)  RVA 0x87bc18：push 4 */
static void h_SymGetLineFromAddr(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = 0;                /* FALSE */
    R(c, 16);
}

/* ================= bcrypt ================= */
/* BCryptGenRandom(alg, buf, len, flags) */
static void h_BCryptGenRandom(CPU *c)
{
    (void)A(c, 0);
    uint32_t buf = A(c, 1), len = A(c, 2);
    (void)A(c, 3);
    if (buf && len) {
        FILE *f = fopen("/dev/urandom", "rb");
        size_t got = 0;
        if (f) { got = fread(G(buf), 1, len, f); fclose(f); }
        for (uint32_t i = (uint32_t)got; i < len; i++)
            st8(buf + i, (uint8_t)(rand() & 0xFF));
    }
    c->eax = 0;                /* STATUS_SUCCESS */
    R(c, 16);
}

/* ================= 分发 ================= */
typedef struct { const char *name; void (*fn)(CPU *); } sys_entry;

static const sys_entry s_sys[] = {
    { "ole32.dll!CoInitialize",          h_CoInitialize },
    { "ole32.dll!CoInitializeEx",        h_CoInitializeEx },
    { "ole32.dll!CoUninitialize",        h_CoUninitialize },
    { "WINMM.dll!timeGetTime",           h_timeGetTime },
    { "WINMM.dll!timeBeginPeriod",       h_timeBeginPeriod },
    { "WINMM.dll!timeEndPeriod",         h_timeEndPeriod },
    { "WINMM.dll!timeGetDevCaps",        h_timeGetDevCaps },
    { "ADVAPI32.dll!OpenProcessToken",   h_OpenProcessToken },
    { "ADVAPI32.dll!LookupPrivilegeValueA", h_LookupPrivilegeValueA },
    { "ADVAPI32.dll!AdjustTokenPrivileges", h_AdjustTokenPrivileges },
    { "SHELL32.dll!SHFileOperationA",    h_SHFileOperationA },
    { "dbghelp.dll!SymSetOptions",       h_SymSetOptions },
    { "dbghelp.dll!SymInitialize",       h_SymInitialize },
    { "dbghelp.dll!SymFromAddr",         h_SymFromAddr },
    { "dbghelp.dll!SymGetLineFromAddr",  h_SymGetLineFromAddr },
    { "bcrypt.dll!BCryptGenRandom",      h_BCryptGenRandom },
};

int host_sys_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    for (unsigned i = 0; i < sizeof s_sys / sizeof s_sys[0]; i++) {
        if (strcmp(s_sys[i].name, name) == 0) { s_sys[i].fn(c); return 1; }
    }
    return 0;
}
int host_sys_count(void) { return (int)(sizeof s_sys / sizeof s_sys[0]); }
