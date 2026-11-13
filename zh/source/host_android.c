/* Android 宿主层 —— KERNEL32 导入实现
 *
 * 机制照抄 host_vita_first_fault.c：
 *   host_arg(c, i)  取第 i 个参数（stdcall，esp 指向返回地址）
 *   host_ret(c, n)  弹出返回地址并清理 n 字节参数（stdcall 语义）
 *
 * 原则：
 *   · 能用 libc 实现的就真实现（时间、内存、线程锁、文件）
 *   · Android 没有概念的返回一个让游戏继续的值
 *   · 绝不静默返回 0 假装成功 —— 除非那个 0 就是正确的返回值
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

#include "guest.h"
#include "host_path.h"

extern unsigned g_host_import_calls;

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void R(CPU *__restrict c, uint32_t n)
{
    (void)gpop(c);
    (void)guest_stack_adjust(c, n, 0u);
}
static uint64_t ret64(CPU *__restrict c, uint64_t v)
{
    c->eax = (uint32_t)(v & 0xFFFFFFFFu);
    c->edx = (uint32_t)(v >> 32);
    return v;
}
/* 低 4GB 的 malloc —— guest 的 uint32_t 指针不能指向高地址
 *
 * ★★ 2026-11-07：真机崩溃的根因就在这里。
 *
 * 原来写死 `g_bump = 0x38000000`，上限 `0x3C000000`。但在 app 进程里
 * ART 的 dalvik-main 堆占着 0x08900000..0x38900000 —— **0x38000000 正在
 * 里面**。所以：
 *   · 第一次 mmap(MAP_FIXED_NOREPLACE) 就失败
 *   · 失败分支不推进 g_bump（「避免留洞」），于是此后**每一次**
 *     low_alloc 都同样失败、同样返回 0
 *   · 上层拿到 NULL，Lua 的 luaL_requiref 直接崩在 0xd555c128
 *
 * 子进程模式从来不出这个问题：那里没有 ART，0x38000000 是空的。
 * 这就是「子进程能跑、进程内必崩」的完整解释。
 *
 * 修法两层：
 *   1. 候选基址全部避开 dalvik-main，优先落在
 *      镜像尾部 0x3C900000 与 guest 堆头 0x48000000 之间的真空
 *   2. mmap 失败时**挪 64KB 重试**（借 host_user32.c 的成熟做法），
 *      而不是原地卡死；窗口用尽就换下一个候选 */
#define BUMP_WINDOW (32u << 20)          /* 每个候选给 32 MB */

static const uint32_t k_bump_cand[] = {
    0x3D000000u,   /* 首选：镜像 0x3C900000 之后、guest 堆 0x48000000 之前 */
    0x44000000u,   /* 同上区域的后半段 */
    0x39000000u,   /* 空洞开头；会撞 steam 0x3A000000 / user32 0x3B000000，
                    * 但有 NOREPLACE + 挪移，会自己跳过去 */
    /* ★ 2026-11-10：0x90000000 / 0xC0000000 两个候选已删除。
     * 堆范围扩到 0xF0000000 后，那两个地址落入 guest 堆。
     * 虽然 MAP_FIXED_NOREPLACE 会拒绝，但会让 VirtualAlloc 失败
     * 而堆还在用那块地 —— 没必要制造这种模糊状态。
     * 低 4GB 里未被占用的安全区只剩镜像下方这一段。 */
    0x33000000u,   /* 镜像之前；会撞 dalvik-main（跳过后会自己找缝）*/
    0x68000000u,   /* 假地址保护区 0x7D000000 之前、堆中段的空白 */
};
static int      g_cand_idx;
static uint32_t g_bump;          /* 当前游标；0 = 尚未初始化 */
static uint32_t g_bump_end;

static uint32_t low_alloc(uint32_t n)
{
    /* mmap 要求地址和长度都按页对齐：原来只按 16 字节对齐，第二次
     * 分配就从 0x38000010 起 mmap，EINVAL，整个 bump 从此卡死。 */
    if (!n) n = 16u;
    n = (n + 4095u) & ~4095u;

    if (!g_bump) {
        g_bump = k_bump_cand[0];
        g_bump_end = g_bump + BUMP_WINDOW;
    }

    for (;;) {
        if (g_bump + n > g_bump_end) {
            /* 当前候选窗口用尽 → 换下一个 */
            if (++g_cand_idx >= (int)(sizeof k_bump_cand / sizeof k_bump_cand[0])) {
                static int reported;
                if (!reported) { reported = 1;
                    fprintf(stderr, "[bump] ✗ all candidate bases exhausted (need %u bytes)\n", n); }
                return 0;
            }
            g_bump = k_bump_cand[g_cand_idx];
            g_bump_end = g_bump + BUMP_WINDOW;
            if (g_bump_end < g_bump) g_bump_end = 0xFFFFFFFFu;  /* 溢出保护 */
            fprintf(stderr, "[bump] switching candidate base to 0x%08x\n", g_bump);
            continue;
        }

        void *p = mmap((void *)(uintptr_t)g_bump, n, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == (void *)(uintptr_t)g_bump) {
            uint32_t r = g_bump;
            g_bump += n;
            return r;
        }

        /* 被占用：挪 64 KB 再试。绝不原地卡死 */
        g_bump += 0x10000u;
    }
}

/* ---- 时间 ---- */
static void h_GetSystemTimeAsFileTime(CPU *c) {
    uint32_t out = A(c, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* FILETIME: 100ns 单位，起点 1601-01-01 */
    uint64_t v = (uint64_t)ts.tv_sec * 10000000ull + 116444736000000000ull
               + (uint64_t)(ts.tv_nsec / 100);
    if (out) { st32(out, (uint32_t)v); st32(out + 4, (uint32_t)(v >> 32)); }
    R(c, 4);
}
static void h_GetLocalTime(CPU *c) {
    uint32_t out = A(c, 0);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (out) {
        memset((void *)(uintptr_t)out, 0, 16);
        st16(out + 0, (uint16_t)tm.tm_year + 1900);
        st16(out + 2, (uint16_t)tm.tm_mon + 1);
        st16(out + 6, (uint16_t)tm.tm_mday);
        st16(out + 8, (uint16_t)tm.tm_hour);
        st16(out + 10, (uint16_t)tm.tm_min);
        st16(out + 12, (uint16_t)tm.tm_sec);
    }
    R(c, 4);
}
static void h_QueryPerformanceCounter(CPU *c) {
    uint32_t out = A(c, 0);
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t v = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (out) { st32(out, (uint32_t)v); st32(out + 4, (uint32_t)(v >> 32)); }
    c->eax = 1; R(c, 4);
}
static void h_QueryPerformanceFrequency(CPU *c) {
    uint32_t out = A(c, 0);
    if (out) { st32(out, 1000000000u); st32(out + 4, 0); }
    c->eax = 1; R(c, 4);
}

/* ---- 内存 ---- */
static void h_VirtualAlloc(CPU *c) {
    uint32_t addr = A(c, 0), size = A(c, 1);
    (void)A(c, 2); (void)A(c, 3);
    if (size == 0) { c->eax = 0; R(c, 16); return; }
    size = (size + 4095u) & ~4095u;
    if (addr) {
        void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void *)(uintptr_t)addr) {
            fprintf(stderr, "[valloc] ✗ fixed address %08x + %u failed (errno=%d)\n",
                    addr, size, errno);
            c->eax = 0; R(c, 16); return;
        }
        c->eax = addr;
    } else {
        c->eax = low_alloc(size);
    }
    R(c, 16);
}
static void h_VirtualFree(CPU *c) {
    uint32_t addr = A(c, 0), size = A(c, 1), type = A(c, 2);
    if (addr) {
        if (type & 0x8000u) {
            fprintf(stderr, "[vfree] %08x + %u (type=%x)\n", addr, size, type);
            munmap((void *)(uintptr_t)addr, size ? size : 4096);
        }
        /* MEM_DECOMMIT: 保持映射，只是内容失效 */
    }
    c->eax = 1; R(c, 12);
}
/* ==========================================================================
 * VirtualQuery —— 真实现（2026-11-07）
 *
 * 原来是个恒返回「MEM_FREE / 64KB」的桩。后果：guest_pe_load 在
 * 「固定地址分配失败」时会遍历这段区间打印占用者，而桩让每个查询
 * 都说"空闲"，于是那一段诊断输出**完全是空的**。
 *
 * 真机上 .so 化失败（0x3c000000 分配不到）时，正是靠这段输出才能知道
 * 是谁占的 —— 桩把它废掉了，等于把最关键的线索扔了。
 *
 * 现在读 /proc/self/maps 回答。语义要点：
 *   · RegionSize 必须让调用方的游标单调前进，否则那段 for 循环会死循环
 *   · 命中映射 → State=MEM_COMMIT，Type 按文件/匿名区分
 *   · 未命中   → State=MEM_FREE，RegionSize 到下一个映射起点
 * ========================================================================== */
#define VQ_MEM_COMMIT   0x00001000u
#define VQ_MEM_FREE     0x00010000u
#define VQ_MEM_PRIVATE  0x00020000u
#define VQ_MEM_IMAGE    0x01000000u
#define VQ_MEM_MAPPED   0x00040000u

typedef struct { uint32_t lo, hi; int is_file; } vq_rng;

/* 每次查询都重读 maps 很慢，但这条路径只在出错时走几次，值得。 */
int vq_scan_compat(uint32_t addr, uint32_t *base, uint32_t *size,
                   uint32_t *state, uint32_t *type)
{
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    uint32_t next_lo = 0xFFFFFFFFu;   /* 下一个映射的起点（用于 MEM_FREE 的空隙） */
    int found = 0;

    if (!m) return 0;
    while (fgets(line, sizeof line, m)) {
        unsigned long lo = 0, hi = 0;
        char perm[8];
        const char *p;
        int is_file;
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3) continue;
        if (lo > 0xFFFFFFFFul) continue;
        is_file = (strchr(line, '/') != NULL);
        /* 记录 addr 之后最近的一个映射起点 */
        if (lo > addr && lo < next_lo) next_lo = (uint32_t)lo;
        if (addr >= (uint32_t)lo && addr < (uint32_t)hi) {
            *base  = (uint32_t)lo;
            *size  = (uint32_t)hi - (uint32_t)lo;
            *state = VQ_MEM_COMMIT;
            p = perm;
            *type  = is_file ? VQ_MEM_IMAGE : VQ_MEM_PRIVATE;
            (void)p;
            found = 1;
            break;
        }
    }
    fclose(m);

    if (!found) {
        /* next_lo 是 addr 之后最近的映射起点；没有就用 4GB 顶部。
         * 注意不能写 0x100000000ull —— 截断成 uint32_t 会变 0。 */
        uint32_t end = (next_lo != 0xFFFFFFFFu) ? next_lo : 0xFFFFFFFFu;
        if (end <= addr) end = addr + 0x10000u;
        *base  = addr;
        *size  = end - addr;
        *state = VQ_MEM_FREE;
        *type  = 0;
        found  = 1;
    }
    /* 单调前进保险：RegionSize 绝不能是 0，否则调用方死循环 */
    if (*size == 0) *size = 0x1000u;
    return found;
}

/* 给 windows.h 的 VirtualQuery 兜底（真实现）。
 *
 * guest_pe_load 在固定基址分配失败时会调用 VirtualQuery 遍历区间、
 * 打印占用者。这是排查 .so 化地址冲突的唯一依据。
 *
 * 返回 1 = 成功填了 MEMORY_BASIC_INFORMATION。
 * 注意 RegionSize 必须非 0 且让游标前进，否则调用方死循环。
 */
int host_virtual_query_shim(uintptr_t addr, void *mbi, size_t n)
{
    extern int vq_scan_compat(uint32_t addr, uint32_t *base, uint32_t *size,
                              uint32_t *state, uint32_t *type);
    uint32_t base = (uint32_t)addr, size = 0x1000u, state = 0x10000u, type = 0;
    struct {
        void *BaseAddress; void *AllocationBase; uint32_t AllocationProtect;
        size_t RegionSize; uint32_t State; uint32_t Protect; uint32_t Type;
    } *m = (void *)mbi;

    if (!mbi || n < sizeof *m) return 0;
    if (!vq_scan_compat((uint32_t)addr, &base, &size, &state, &type)) return 0;
    m->BaseAddress = (void *)(uintptr_t)base;
    m->AllocationBase = (void *)(uintptr_t)base;
    m->AllocationProtect = (state == 0x1000u) ? 0x04u : 0u;
    m->RegionSize = size;
    m->State = state;
    m->Protect = m->AllocationProtect;
    m->Type = type;
    return 1;
}

static void h_VirtualQuery(CPU *c) {
    uint32_t addr = A(c, 0), out = A(c, 1), len = A(c, 2);
    uint32_t base = addr, size = 0x1000u, state = VQ_MEM_FREE, type = 0;
    if (!out || len < 28) { c->eax = 0; R(c, 12); return; }
    vq_scan_compat(addr, &base, &size, &state, &type);
    memset((void *)(uintptr_t)out, 0, 28);
    st32(out + 0,  base);                       /* BaseAddress */
    st32(out + 4,  base);                       /* AllocationBase */
    st32(out + 8,  state == VQ_MEM_COMMIT ? 0x04u : 0u);  /* AllocationProtect = PAGE_READWRITE */
    st32(out + 12, size);                       /* RegionSize */
    st32(out + 16, state);                      /* State */
    st32(out + 20, state == VQ_MEM_COMMIT ? 0x04u : 0u);  /* Protect */
    st32(out + 24, type);                       /* Type */
    c->eax = 28;
    R(c, 12);
}
static void h_VirtualUnlock(CPU *c) { (void)A(c,0); (void)A(c,1); c->eax = 1; R(c, 8); }
static void h_GetLargePageMinimum(CPU *c) { c->eax = 0; R(c, 0); }

/* ---- 进程 / 线程 ---- */
static void h_GetCurrentProcess(CPU *c) { c->eax = 0xFFFFFFFFu; R(c, 0); }
static void h_GetCurrentProcessId(CPU *c) { c->eax = (uint32_t)getpid(); R(c, 0); }
static void h_GetCurrentThreadId(CPU *c) { c->eax = (uint32_t)(uintptr_t)pthread_self(); R(c, 0); }
static void h_GetCurrentProcessorNumber(CPU *c) { c->eax = 0; R(c, 0); }
static void h_IsDebuggerPresent(CPU *c) { c->eax = 0; R(c, 0); }
static void h_IsProcessorFeaturePresent(CPU *c) {
    uint32_t f = A(c, 0);
    /* PF_XMMI64_INSTRUCTIONS_AVAILABLE(10) / SSE 系列都报支持 */
    c->eax = (f == 10u || f == 6u || f == 7u || f == 12u) ? 1u : 0u;
    R(c, 4);
}
static void h_TerminateProcess(CPU *c) { (void)A(c,0); (void)A(c,1); exit(0); }
static void h_TerminateThread(CPU *c) { (void)A(c,0); (void)A(c,1); R(c, 8); c->eax = 1; }
static void h_GetProcessTimes(CPU *c) {
    for (int i = 0; i < 4; i++) { uint32_t o = A(c, 1 + i); if (o) { st32(o,0); st32(o+4,0);} }
    c->eax = 1; R(c, 20);
}
static void h_K32GetProcessMemoryInfo(CPU *c) {
    uint32_t out = A(c, 1), len = A(c, 2);
    if (out && len >= 40) memset((void *)(uintptr_t)out, 0, 40);
    c->eax = 1; R(c, 12);
}
static void h_SetUnhandledExceptionFilter(CPU *c) { (void)A(c,0); c->eax = 0; R(c, 4); }
static void h_UnhandledExceptionFilter(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }
static void h_RaiseException(CPU *c) {
    fprintf(stderr, "[host] RaiseException code=%#x\n", A(c, 0));
    R(c, 16);
}
/* ★★ 2026-11-10：统一的“宿主睡眠限流”。
 *
 * 真机证据：
 *     [slow] 1004 ms  KERNEL32.dll!Sleep   × 1192 次
 * 而帧时间尖峰恰是 1017 ms（每次 = 1004ms 睡眠 + 17ms 帧）。
 *
 * 我们在**单线程串行执行 guest 代码**。睡眠期间游戏不会有任何
 * 进展 —— 真实 Windows 上那些并行工作线程我们根本没跑。
 * 所以长睡眠是纯浪费墙钟时间。
 *
 * 规则：
 *   · <= cap（默认 16ms）原样保留 —— 用于帧率节流，
 *     截掉会让游戏空转烧 CPU。
 *   · >  cap 截到 1ms（cap=0 则完全不睡）。长睡眠几乎都是
 *     “等某个异步任务”，我们根本没跑那些任务，等再久也等不到。
 * 留 1ms 是为了仍然让出调度器，避免与 ART / 音频 / 输入线程互饿。
 *
 * 可用 SLEEP_CAP_MS=<n> 覆盖。 */
static void host_sleep_ms(uint32_t ms)
{
    static int cap = -2;
    if (cap == -2) {
        const char *e = getenv("SLEEP_CAP_MS");
        cap = e ? atoi(e) : 16;
        if (cap < 0) cap = 0;
    }
    if (!ms || ms == 0xFFFFFFFFu) return;   /* 0 = 不睡；INFINITE = 不死等 */
    uint32_t eff = ms;
    if ((int)ms > cap) eff = (uint32_t)(cap > 0 ? 1 : 0);
    if (eff) usleep(eff * 1000u);
}

/* `KERNEL32.dll!Sleep` —— 帧率的头号杀手，见 host_sleep_ms 的注释。 */
static void h_Sleep(CPU *c)
{
    host_sleep_ms(A(c, 0));
    R(c, 4);
}
static void h_SetThreadExecutionState(CPU *c) { (void)A(c,0); c->eax = 0x80000000u; R(c, 4); }
static void h_SetThreadPriority(CPU *c) { (void)A(c,0); (void)A(c,1); c->eax = 1; R(c, 8); }
static void h_GetSystemInfo(CPU *c) {
    uint32_t out = A(c, 0);
    if (out) {
        /* 32 位 SYSTEM_INFO 是 36 字节，字段偏移是
         *   0 arch/oemid, 4 page size, 8 min addr, 12 max addr,
         *   16 processor mask, 20 processor count, 24 processor type,
         *   28 allocation granularity, 32 level, 34 revision.
         * 原来这里 memset 64 字节，而 guest 侧只开了 `sub esp,0x38`
         * （56 字节）的局部缓冲区，多写的 8 字节正好落在 [ebp]
         * 保存槽和返回地址上 —— 这就是 2026-11-02 栈帧失衡的根因：
         * sub_005e77d0 的 pop ebp 取到 0，之后所有 [ebp-…] 访问崩塌。 */
        memset((void *)(uintptr_t)out, 0, 36);
        st32(out + 0, 0);            /* ProcessorArchitecture = x86 */
        st32(out + 4, 4096);         /* PageSize */
        st32(out + 20, 1);           /* NumberOfProcessors */
        st32(out + 24, 586);         /* ProcessorType = Pentium Pro */
        st32(out + 28, 0x10000);     /* AllocationGranularity */
    }
    R(c, 4);
}
static void h_CreateThread(CPU *c) {
    /* 游戏用它跑音频/后台线程。先返回一个假句柄，让它继续。 */
    (void)A(c,0); (void)A(c,1); (void)A(c,2); (void)A(c,3); (void)A(c,4); (void)A(c,5);
    static uint32_t fake = 0x1000;
    c->eax = fake++; R(c, 24);
}

/* ---- 同步 ---- */
static void h_InitializeCriticalSection(CPU *c) {
    uint32_t p = A(c, 0);
    if (p) memset((void *)(uintptr_t)p, 0, 24);
    R(c, 4);
}
static void h_InitializeCriticalSectionAndSpinCount(CPU *c) {
    uint32_t p = A(c, 0);
    if (p) memset((void *)(uintptr_t)p, 0, 24);
    c->eax = 1; R(c, 8);
}
static void h_DeleteCriticalSection(CPU *c) { (void)A(c,0); R(c, 4); }
static void h_EnterCriticalSection(CPU *c) { (void)A(c,0); R(c, 4); }
static void h_LeaveCriticalSection(CPU *c) { (void)A(c,0); R(c, 4); }
static void h_TryEnterCriticalSection(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }
static void h_InitializeSListHead(CPU *c) {
    uint32_t p = A(c, 0);
    if (p) { st32(p, 0); st32(p + 4, 0); }
    R(c, 4);
}
static uint32_t g_evt = 0x2000;
static void h_CreateEventW(CPU *c) {
    (void)A(c,0); (void)A(c,1); (void)A(c,2); (void)A(c,3);
    c->eax = g_evt++; R(c, 16);
}
static void h_SetEvent(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }
static void h_ResetEvent(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }
static void h_WaitForSingleObject(CPU *c) {
    (void)A(c,0); uint32_t ms = A(c,1);
    host_sleep_ms(ms);
    c->eax = 0; R(c, 8);
}
static void h_WaitForSingleObjectEx(CPU *c) {
    (void)A(c,0); uint32_t ms = A(c,1); (void)A(c,2);
    host_sleep_ms(ms);
    c->eax = 0; R(c, 12);
}
static void h_LockFileEx(CPU *c) { for(int i=0;i<6;i++)(void)A(c,i); c->eax=1; R(c,24); }
static void h_UnlockFileEx(CPU *c) { for(int i=0;i<5;i++)(void)A(c,i); c->eax=1; R(c,20); }

/* ---- TLS / FLS ---- */
static uint32_t g_tls_next = 0;
static uint32_t g_tls_val[64];
static void h_TlsAlloc(CPU *c) { c->eax = g_tls_next++; R(c, 0); }
static void h_TlsFree(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }
static void h_TlsGetValue(CPU *c) {
    uint32_t i = A(c, 0);
    c->eax = (i < 64) ? g_tls_val[i] : 0; R(c, 4);
}
static void h_TlsSetValue(CPU *c) {
    uint32_t i = A(c, 0), v = A(c, 1);
    if (i < 64) g_tls_val[i] = v;
    c->eax = 1; R(c, 8);
}
static uint32_t g_fls_next = 0;
static uint32_t g_fls_val[64];
static void h_FlsAlloc(CPU *c) { (void)A(c,0); c->eax = 0x100 + g_fls_next++; R(c, 4); }
static void h_FlsFree(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }
static void h_FlsSetValue(CPU *c) {
    uint32_t i = A(c, 0) - 0x100, v = A(c, 1);
    if (i < 64) g_fls_val[i] = v;
    c->eax = 1; R(c, 8);
}

/* ---- 动态导出（GetProcAddress 的返回目标）----
 *
 * 游戏会 LoadLibrary("kernelbase.dll") 再 GetProcAddress 拿可选 API。
 * 返回值必须是一个 guest 能 call 的地址。repentogxm 用「动态 token」做这件事：
 * 地址不在 PE 镜像里、也不是 IAT 槽，guest_host_dynamic 拦截后分发。
 *
 * 这里划一段 0x7E000000 起的 token 空间，每项 4 字节。 */
#define DYN_BASE 0x7E000000u

static void dyn_SleepConditionVariableCS(CPU *c) {
    uint32_t ms = A(c, 2); (void)A(c,0); (void)A(c,1);
    host_sleep_ms(ms);
    c->eax = 1; R(c, 12);
}
static void dyn_WakeAllConditionVariable(CPU *c) { (void)A(c,0); R(c, 4); c->eax = 1; }
static void dyn_WakeConditionVariable(CPU *c) { (void)A(c,0); R(c, 4); c->eax = 1; }
static void dyn_SleepConditionVariableSRW(CPU *c) {
    uint32_t ms = A(c,2);
    host_sleep_ms(ms);
    c->eax = 1; R(c, 16);
}
static void dyn_VirtualAlloc2FromApp(CPU *c) {
    /* 转发到普通 VirtualAlloc（参数布局不同，取前两个即可） */
    uint32_t addr = A(c, 0), size = A(c, 1);
    (void)addr;
    c->eax = low_alloc(size); R(c, 16);
}
static void dyn_GetSystemTimePreciseAsFileTime(CPU *c) {
    h_GetSystemTimeAsFileTime(c);
}
static void dyn_GetCurrentThreadStackLimits(CPU *c) {
    uint32_t lo = A(c,0), hi = A(c,1);
    if (lo) st32(lo, 0x40000000u);
    if (hi) st32(hi, 0x40400000u);
    R(c, 8);
}

/* ---- WINMM：GLFW 会用 GetProcAddress 现取这几个 ----
 * GLFW 的 win32 初始化会 LoadLibraryA("winmm.dll") 再逐个 GetProcAddress。
 * 只查 DYN 表（不查常规导入表）时这些返回 0，GLFW 就认为平台初始化失败，
 * 整个 glfwInit() 返回 false —— 实测日志里就是
 * "[ASSERT] - GLFW failed to initialize"。
 *
 * timeGetTime 我们其实有实现（host_win_sys.c 的 h_timeGetTime），但那条路径
 * 是 IAT 分发，GetProcAddress 走不到。所以这里补一份动态导出版。 */
static void dyn_timeGetTime(CPU *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    c->eax = (uint32_t)((uint64_t)ts.tv_sec * 1000ull +
                        (uint64_t)(ts.tv_nsec / 1000000));
    R(c, 0);
}
static void dyn_timeBeginPeriod(CPU *c) { (void)A(c, 0); c->eax = 0; R(c, 4); }
static void dyn_timeEndPeriod(CPU *c)   { (void)A(c, 0); c->eax = 0; R(c, 4); }

/* JOYERR_UNPLUGGED = 167：诚实报告「没插手柄」，GLFW 会跳过该设备。 */
static void dyn_joyGetPos(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    c->eax = 167u;
    R(c, 8);
}
static void dyn_joyGetPosEx(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1);
    c->eax = 167u;
    R(c, 8);
}
/* JOYERR_NOCANDO = 165：连能力都拿不到 */
static void dyn_joyGetDevCapsW(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = 165u;
    R(c, 12);
}

/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter)
 *   stdcall 5 参数 = 20 字节，返回 HRESULT。
 *
 * ★ 必须返回 S_OK 并给出可用的接口对象，不能返回失败。
 *
 * 理由是游戏的假设：它调用完之后**无条件**把 0xc007de 置 1，然后在
 * sub_0059d3b0 里读 [0x803010]（就是这里的 ppvOut）当指针用：
 *      eax = [0x803010]; ecx = [eax]; call [ecx+0x10]   // EnumDevices
 * 返回失败 → ppvOut 保持 NULL → 读 [0] 崩（fault_addr=0x00000000）。
 *
 * 真机上 DINPUT8.dll 是系统组件，DirectInput8Create 必然成功；没有手柄
 * 时是 EnumDevices 枚举出 0 个设备。我们的假对象复现的正是这个状态。 */
extern uint32_t host_dinput_obj(void) __attribute__((weak));

static void dyn_DirectInput8Create(CPU *c)
{
    uint32_t ppvOut = A(c, 3);
    uint32_t obj = host_dinput_obj ? host_dinput_obj() : 0u;
    if (ppvOut) st32(ppvOut, obj);
    c->eax = obj ? 0u : 0x80004005u;   /* S_OK / E_FAIL */
    R(c, 20);
}

static void dyn_GetCurrentProcessorNumberEx(CPU *c) {
    uint32_t out = A(c,0);
    if (out) { st16(out,0); st8(out+2,0); st8(out+3,0); }
    R(c, 4);
}
static void dyn_GetNumaProcessorNodeEx(CPU *c) {
    uint32_t node = A(c,1);
    if (node) st16(node, 0);
    c->eax = 1; R(c, 8);
}
static void dyn_GetNumaNodeProcessorMaskEx(CPU *c) {
    uint32_t out = A(c,1);
    if (out) { st32(out,0); st32(out+4,0); }
    c->eax = 1; R(c, 8);
}
static void dyn_GetProcessInformation(CPU *c) { (void)A(c,0); (void)A(c,1); (void)A(c,2); c->eax=1; R(c,12); }
static void dyn_SetThreadInformation(CPU *c) { (void)A(c,0); (void)A(c,1); (void)A(c,2); (void)A(c,3); c->eax=1; R(c,16); }
static void dyn_GetSystemCpuSetInformation(CPU *c) { (void)A(c,0); (void)A(c,1); (void)A(c,2); (void)A(c,3); (void)A(c,4); c->eax=1; R(c,20); }

typedef struct { const char *name; void (*fn)(CPU *); } dynent;
static const dynent DYN[] = {
    {"SleepConditionVariableCS",                   dyn_SleepConditionVariableCS},
    {"SleepConditionVariableSRW",                  dyn_SleepConditionVariableSRW},
    {"WakeAllConditionVariable",                   dyn_WakeAllConditionVariable},
    {"WakeConditionVariable",                      dyn_WakeConditionVariable},
    {"VirtualAlloc2FromApp",                       dyn_VirtualAlloc2FromApp},
    {"VirtualAlloc2",                              dyn_VirtualAlloc2FromApp},
    {"GetSystemTimePreciseAsFileTime",             dyn_GetSystemTimePreciseAsFileTime},
    {"GetCurrentThreadStackLimits",                dyn_GetCurrentThreadStackLimits},
    {"GetCurrentProcessorNumberEx",                dyn_GetCurrentProcessorNumberEx},
    {"GetNumaProcessorNodeEx",                     dyn_GetNumaProcessorNodeEx},
    {"GetNumaNodeProcessorMaskEx",                 dyn_GetNumaNodeProcessorMaskEx},
    {"GetProcessInformation",                      dyn_GetProcessInformation},
    {"SetThreadInformation",                       dyn_SetThreadInformation},
    {"GetSystemCpuSetInformation",                 dyn_GetSystemCpuSetInformation},
    {"DirectInput8Create",                         dyn_DirectInput8Create},
    {"timeGetTime",                                dyn_timeGetTime},
    {"timeBeginPeriod",                            dyn_timeBeginPeriod},
    {"timeEndPeriod",                              dyn_timeEndPeriod},
    {"joyGetPos",                                  dyn_joyGetPos},
    {"joyGetPosEx",                                dyn_joyGetPosEx},
    {"joyGetDevCapsW",                             dyn_joyGetDevCapsW},
};
#define NDYN ((int)(sizeof DYN / sizeof DYN[0]))

static void *dyn_lookup(const char *name, uint32_t *out_addr)
{
    for (int i = 0; i < NDYN; i++) {
        if (strcmp(DYN[i].name, name) == 0) {
            *out_addr = DYN_BASE + (uint32_t)i * 4u;
            return (void *)DYN[i].fn;
        }
    }
    return NULL;
}

extern int host_gl_dynamic(CPU *__restrict c, uint32_t addr) __attribute__((weak));
extern int host_dinput_dynamic(CPU *__restrict c, uint32_t addr) __attribute__((weak));
extern int host_xinput_dynamic(CPU *__restrict c, uint32_t addr) __attribute__((weak));

int host_android_dynamic(CPU *__restrict c, uint32_t addr)
{
    /* token 段划分：0x7D = GL，0x7E = kernel32 动态导出，
     * 0x7F00/0x7F01 = DirectInput COM 方法，0x7F02 = XInput。互不重叠。 */
    if (host_gl_dynamic && host_gl_dynamic(c, addr)) return 1;
    if (host_dinput_dynamic && host_dinput_dynamic(c, addr)) return 1;
    if (host_xinput_dynamic && host_xinput_dynamic(c, addr)) return 1;

    if (addr < DYN_BASE) return 0;
    uint32_t i = (addr - DYN_BASE) / 4u;
    if (i >= (uint32_t)NDYN || (addr - DYN_BASE) % 4u) return 0;
    DYN[i].fn(c);
    return 1;
}

/* ---- 模块 / 库 ---- */
/* ★ 必须返回重定位后的基址。GPUEST_IMAGE_BASE 是 0x3c000000，
 * 不是 PE 头里的 0x400000 —— 镜像已经被 image.py 重定位过了。 */
static void h_GetModuleHandleA(CPU *c) { (void)A(c,0); c->eax = GUEST_IMAGE_BASE; R(c, 4); }
static void h_GetModuleHandleW(CPU *c) { (void)A(c,0); c->eax = GUEST_IMAGE_BASE; R(c, 4); }
/* GL 后端提供的名字解析（host_gl4es.c）。弱符号：没有 GL 后端时返回 0。 */
extern uint32_t host_gl_proc_resolve(const char *name) __attribute__((weak));
/* XInput 动态导出（host_xinput.c）。弱符号：没编进来时跳过。 */
extern int host_xinput_lookup(const char *name, uint32_t *out_addr) __attribute__((weak));
extern int host_xinput_lookup_ordinal(uint32_t ordinal, uint32_t *out_addr) __attribute__((weak));

static void h_GetProcAddress(CPU *c) {
    uint32_t mod = A(c, 0), nm = A(c, 1);

    /* ★ 2026-11-06：序号形式。游戏取 XInputGetStateEx 用的是
     *   GetProcAddress(hXInput, (LPCSTR)100)
     * 高 16 位为 0 的“指针”不是地址，是序号（Win32 文档）。
     * 旧代码把它当 char* 去 strcmp，会直接读地址 100 → 段错误。 */
    if (nm && nm < 0x10000u) {
        uint32_t tok = 0;
        if (host_xinput_lookup_ordinal && host_xinput_lookup_ordinal(nm, &tok)) {
            fprintf(stderr, "[host] GetProcAddress(ordinal %u) -> XInput token %08x\n",
                    nm, tok);
            c->eax = tok; R(c, 8); return;
        }
        fprintf(stderr, "[host] GetProcAddress(ordinal %u) -> 0\n", nm);
        c->eax = 0; R(c, 8); return;
    }

    const char *name = nm ? (const char *)(uintptr_t)nm : NULL;
    uint32_t out = 0;
    void *fn = name ? dyn_lookup(name, &out) : NULL;

    /* XInput：LoadLibraryA 成功之后游戏会按名字取这几个导出。 */
    if (!fn && name && host_xinput_lookup && host_xinput_lookup(name, &out)) {
        fprintf(stderr, "[host] GetProcAddress(%s) -> XInput token %08x\n",
                name, out);
        c->eax = out; R(c, 8); return;
    }

    /* 游戏对 GL 的取法：LoadLibraryA("OPENGL32") → GetProcAddress("gl*")。
     * 这里必须也查 GL 后端，否则 glGetString 会返回 0，游戏直接 exit(1)。
     * 只拦 gl 开头的和 wgl 开头的名字 —— 否则 kernel32 的未知名字
     * 也会被丢进 GL 表里报噪音。 */
    int looks_gl = name && (name[0]=='g' && name[1]=='l' && name[2] &&
                            (name[2]>='A' && name[2]<='Z'));
    int looks_wgl = name && !strncmp(name, "wgl", 3);
    if (!fn && name && host_gl_proc_resolve && (looks_gl || looks_wgl)) {
        uint32_t gltok = host_gl_proc_resolve(name);
        if (gltok) {
            fprintf(stderr, "[host] GetProcAddress(%s) -> GL token %08x\n",
                    name, gltok);
            c->eax = gltok; R(c, 8); return;
        }
    }

    fprintf(stderr, "[host] GetProcAddress(%s) -> %s\n",
            name ? name : "?", fn ? "dynamic token" : "0");
    c->eax = fn ? out : 0; R(c, 8);
}
/* LoadLibraryA 必须**只对真正提供的模块**返回非 NULL。
 *
 * 曾经这里对任何名字都返回 GUEST_IMAGE_BASE，于是游戏
 * LoadLibraryA("不存在的东西.dll") 拿到非 NULL → 认为 API 可用 →
 * GetProcAddress 拿到 0 → 存进函数指针 → 直接 call 0。
 * 实测崩在 fault_addr=0x64（NULL 当结构体用）。
 *
 * ★ 2026-11-06：XInput 已实现在 host_xinput.c（token 段 0x7F020000），
 * 所以 xinput1_4 / xinput1_3 / xinput9_1_0 必须**放行**。
 * 这也是离开「DInput only 降级路径」的唯一入口。 */
static const char *const s_provided_modules[] = {
    "kernel32", "kernelbase", "ntdll",
    "user32", "gdi32", "advapi32", "shell32", "ole32", "winmm",
    "opengl32", "dbghelp", "bcrypt", "dinput8",
    /* ★ 2026-11-06：XInput 是游戏手柄的**主路径**。不在这里放行，
     * 游戏就永远走 DInput 降级分支（日志里的
     * "Gamepad_init couldn't load XInput1_4.dll"）。
     * 三个名字都要：1.4 / 1.3 / bin\XInput1_3.dll。 */
    "xinput1_4", "xinput1_3", "xinput9_1_0",
    "msvcp140", "vcruntime140", "ucrtbase",
    "steam_api", "lua5.3.3", "openal32",
    NULL
};

static int module_provided(const char *name)
{
    char base[64];
    size_t i = 0;
    if (!name) return 0;
    /* 取文件名的基名部分（去掉路径和 .dll/.DLL 后缀） */
    const char *slash = strrchr(name, '\\');
    if (!slash) slash = strrchr(name, '/');
    const char *p = slash ? slash + 1 : name;
    while (p[i] && i < sizeof base - 1) {
        char ch = p[i];
        if (ch == '.') break;
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        base[i] = ch;
        i++;
    }
    base[i] = 0;

    for (int k = 0; s_provided_modules[k]; k++)
        if (strcmp(base, s_provided_modules[k]) == 0) return 1;
    /* api-ms-win-crt-* 系列我们实现了大部分 */
    if (strncmp(base, "api-ms-win-", 11) == 0) return 1;
    return 0;
}

static void h_LoadLibraryA(CPU *c) {
    uint32_t nm = A(c, 0);
    const char *name = nm ? (const char *)(uintptr_t)nm : NULL;
    int ok = module_provided(name);
    fprintf(stderr, "[host] LoadLibraryA(\"%s\") -> %s\n",
            name ? name : "?", ok ? "handle" : "NULL (not provided)");
    c->eax = ok ? GUEST_IMAGE_BASE : 0u;
    R(c, 4);
}
static void h_FreeLibrary(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }

/* ---- 文件 / 路径 ---- */
static void h_GetCurrentDirectoryA(CPU *c) {
    uint32_t len = A(c, 0), out = A(c, 1);
    char cwd[512]; getcwd(cwd, sizeof cwd);
    uint32_t n = (uint32_t)strlen(cwd);
    if (out && len > n) memcpy((void *)(uintptr_t)out, cwd, n + 1);
    c->eax = n; R(c, 8);
}
static void h_GetFullPathNameW(CPU *c) { (void)A(c,0); (void)A(c,1); (void)A(c,2); (void)A(c,3); c->eax = 0; R(c, 16); }
static void h_GetFileAttributesA(CPU *c) {
    uint32_t p = A(c, 0);
    const char *path = (const char *)(uintptr_t)p;
    struct stat st;
    if (path) { const char *r = host_path_resolve(path); if (r) path = r; }
    int ok = (path && stat(path, &st) == 0);
    fprintf(stderr, "[host] GetFileAttributesA(\"%s\") -> %s\n",
            path ? path : "?", ok ? (S_ISDIR(st.st_mode) ? "DIR" : "0x80") : "INVALID");
    c->eax = ok ? (S_ISDIR(st.st_mode) ? 0x10u : 0x80u) : 0xFFFFFFFFu;
    R(c, 4);
}
static void h_CreateDirectoryA(CPU *c) {
    uint32_t p = A(c, 0);
    const char *path = (const char *)(uintptr_t)p;
    int r = path ? mkdir(path, 0777) : -1;
    c->eax = (r == 0 || errno == EEXIST) ? 1u : 0u;
    R(c, 8);
}
static void h_GetStdHandle(CPU *c) { c->eax = A(c, 0) + 8; R(c, 4); }
static void h_GetEnvironmentVariableA(CPU *c) {
    uint32_t len = A(c, 1);
    if (len) ((char *)(uintptr_t)A(c, 1))[0] = 0;
    c->eax = 0; R(c, 12);
}
static void h_WriteConsoleA(CPU *c) {
    uint32_t buf = A(c, 1), n = A(c, 2), out = A(c, 4);
    if (buf && n) fwrite((const void *)(uintptr_t)buf, 1, n, stdout);
    if (out) st32(out, n);
    c->eax = 1; R(c, 20);
}
static void h_CloseHandle(CPU *c) { (void)A(c, 0); c->eax = 1; R(c, 4); }
static void h_FindClose(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }
/* ==========================================================================
 * FindFirstFileW / FindNextFileW / FindClose —— 真实现（2026-11-05）
 *
 * 在此之前它们是恒失败的桩，注释写着"实测调用 0 次，不影响启动"。
 * 那个结论对**启动**成立，但对 **mod 加载**不成立：
 * ModManager 枚举 mods/ 目录靠的就是这三个函数，桩一失败，
 * 列表永远是空的 —— 表现为日志里 `begin list mods` 之后什么都没有，
 * 用户放的 mod 完全不生效。
 *
 * WIN32_FIND_DATAW（x86 32 位）布局，共 592 字节：
 *   +0x00 DWORD    dwFileAttributes
 *   +0x04 FILETIME ftCreationTime
 *   +0x0C FILETIME ftLastAccessTime
 *   +0x14 FILETIME ftLastWriteTime
 *   +0x1C DWORD    nFileSizeHigh
 *   +0x20 DWORD    nFileSizeLow
 *   +0x24 DWORD    dwReserved0
 *   +0x28 DWORD    dwReserved1
 *   +0x2C WCHAR[260] cFileName
 *   +0x234 WCHAR[14] cAlternateFileName
 *
 * 通配符：路径末尾的 `*`、`*.*` 剥掉得到目录，其余当普通路径。
 * 过滤只支持最常见的两种：`*`（全部）和 `*.ext`（按后缀）。
 * 这覆盖了游戏侧全部实际用法（枚举 mods 目录、读 *.xml）。
 *
 * 编码：UTF-16 → UTF-8 只处理 BMP 里的 ASCII；非 ASCII 原样保留成 '?'。
 * 我们的资源路径全是 ASCII，够用；真有中文 mod 名再看。
 * ========================================================================== */

#define FIND_MAX 8
static DIR *g_find_dir[FIND_MAX];
static char g_find_pat[FIND_MAX][128];   /* 文件名过滤模式 */
static char g_find_dirpath[FIND_MAX][512]; /* 目录路径，FindNext 需要 */
static int  g_find_used[FIND_MAX];

static void u16_to_u8(uint32_t src, char *dst, int cap)
{
    int k = 0;
    if (!src) { if (cap > 0) dst[0] = 0; return; }
    for (int i = 0; i < cap - 1; i++) {
        uint16_t w = (uint16_t)ld16(src + (uint32_t)i * 2u);
        if (!w) break;
        dst[k++] = (w < 0x80) ? (char)w : '?';
    }
    dst[k] = 0;
}

static void u8_to_u16(const char *src, uint32_t dst, int maxchars)
{
    int i = 0;
    for (; i < maxchars - 1 && src[i]; i++)
        st16(dst + (uint32_t)i * 2u, (uint16_t)(unsigned char)src[i]);
    st16(dst + (uint32_t)i * 2u, 0);
}

/* 填一个 WIN32_FIND_DATAW。返回 1 表示有效。 */
static int fill_find_data(uint32_t data, const char *name, const struct stat *st)
{
    if (!data) return 0;
    for (int i = 0; i < 0x250; i++) st8(data + (uint32_t)i, 0);

    uint32_t attr = 0;
    if (st) {
        if (S_ISDIR(st->st_mode)) attr |= 0x10;          /* FILE_ATTRIBUTE_DIRECTORY */
        else                       attr |= 0x80;          /* FILE_ATTRIBUTE_NORMAL */
        st32(data + 0x1c, 0);
        st32(data + 0x20, (uint32_t)st->st_size);
    } else {
        attr = 0x10;
    }
    st32(data + 0x00, attr);
    u8_to_u16(name, data + 0x2c, 260);
    u8_to_u16(name, data + 0x234, 14);
    return 1;
}

/* 模式匹配：只支持 "*" 与 "*.ext" */
static int name_matches(const char *pat, const char *name)
{
    if (!pat[0] || strcmp(pat, "*") == 0 || strcmp(pat, "*.*") == 0) return 1;
    if (pat[0] == '*' && pat[1] == '.') {
        size_t nl = strlen(name), pl = strlen(pat + 1);   /* ".ext" */
        if (nl >= pl && strcasecmp(name + nl - pl, pat + 1) == 0) return 1;
        /* Windows 语义：`*.ext` 不匹配无扩展名的文件 */
        return 0;
    }
    return strcasecmp(pat, name) == 0;
}

static int find_slot(void)
{
    for (int i = 0; i < FIND_MAX; i++) if (!g_find_used[i]) return i;
    return -1;
}

static void h_FindFirstFileW(CPU *c)
{
    uint32_t patp = A(c, 0);
    uint32_t datap = A(c, 1);

    char wpath[512];
    u16_to_u8(patp, wpath, (int)sizeof wpath);

    /* 拆出目录和模式 */
    char dir[512], pat[128];
    strncpy(dir, wpath, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (!slash) slash = strrchr(dir, '\\');
    if (slash) {
        strncpy(pat, slash + 1, sizeof pat - 1); pat[sizeof pat - 1] = 0;
        *slash = 0;
    } else {
        strncpy(pat, dir, sizeof pat - 1); pat[sizeof pat - 1] = 0;
        strcpy(dir, ".");
    }
    if (!dir[0]) strcpy(dir, "/");

    int slot = find_slot();
    if (slot < 0) { c->eax = 0xFFFFFFFFu; R(c, 8); return; }

    const char *r = host_path_resolve(dir);
    DIR *d = opendir(r ? r : dir);
    if (!d) {
        fprintf(stderr, "[host] FindFirstFileW(\"%s\") -> cannot open directory\n", wpath);
        fflush(NULL);
        c->eax = 0xFFFFFFFFu; R(c, 8); return;
    }

    g_find_dir[slot] = d;
    g_find_used[slot] = 1;
    strncpy(g_find_dirpath[slot], dir, sizeof g_find_dirpath[slot] - 1);
    g_find_dirpath[slot][sizeof g_find_dirpath[slot] - 1] = 0;
    strncpy(g_find_pat[slot], pat, sizeof g_find_pat[slot] - 1);
    g_find_pat[slot][sizeof g_find_pat[slot] - 1] = 0;

    /* 找第一个匹配项 */
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == 0 || (ent->d_name[1] == '.' && ent->d_name[2] == 0)))
            continue;
        if (!name_matches(g_find_pat[slot], ent->d_name)) continue;

        struct stat st;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, ent->d_name);
        const char *fr = host_path_resolve(full);
        int ok = (stat(fr ? fr : full, &st) == 0);
        if (!fill_find_data(datap, ent->d_name, ok ? &st : NULL)) break;

        static int shown;
        if (shown++ < 4) {
            fprintf(stderr, "[host] FindFirstFileW(\"%s\") -> first match \"%s\"\n",
                    wpath, ent->d_name);
            fflush(NULL);
        }
        /* 句柄 = 槽位 + 1（0 与 INVALID_HANDLE_VALUE 都要避开） */
        c->eax = (uint32_t)(slot + 1);
        R(c, 8);
        return;
    }

    closedir(d);
    g_find_dir[slot] = NULL;
    g_find_used[slot] = 0;

    static int noise;
    if (noise++ < 4) {
        fprintf(stderr, "[host] FindFirstFileW(\"%s\") -> no match\n", wpath);
        fflush(NULL);
    }
    c->eax = 0xFFFFFFFFu;   /* INVALID_HANDLE_VALUE */
    R(c, 8);
}

static void h_FindNextFileW(CPU *c)
{
    uint32_t h = A(c, 0);
    uint32_t datap = A(c, 1);
    int slot = (int)h - 1;
    if (slot < 0 || slot >= FIND_MAX || !g_find_used[slot]) {
        c->eax = 0; R(c, 8); return;
    }

    /* 目录路径在 FindFirst 时没保存，重新从 DIR* 推不出来 —— 这里靠
     * readdir 的 d_name + stat 需要目录。为简化，FindFirst 时已把
     * 目录记在扩展表里。见 g_find_dirpath。 */
    struct dirent *ent;
    while ((ent = readdir(g_find_dir[slot])) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == 0 || (ent->d_name[1] == '.' && ent->d_name[2] == 0)))
            continue;
        if (!name_matches(g_find_pat[slot], ent->d_name)) continue;

        struct stat st;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", g_find_dirpath[slot], ent->d_name);
        const char *fr = host_path_resolve(full);
        int ok = (stat(fr ? fr : full, &st) == 0);
        if (!fill_find_data(datap, ent->d_name, ok ? &st : NULL)) { c->eax = 0; R(c, 8); return; }
        c->eax = 1;
        R(c, 8);
        return;
    }
    c->eax = 0;
    R(c, 8);
}

/* ---- 堆 ---- */
static void h_GlobalAlloc(CPU *c) { c->eax = low_alloc(A(c, 1)); R(c, 8); }
static void h_GlobalLock(CPU *c) { c->eax = A(c, 0); R(c, 4); }
static void h_GlobalUnlock(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }

/* ---- 错误码 ---- */
static uint32_t g_last_error = 0;
static void h_GetLastError(CPU *c) { c->eax = g_last_error; R(c, 0); }
static void h_SetLastError(CPU *c) { g_last_error = A(c, 0); R(c, 4); }

/* ---- NUMA（单节点机器）---- */
static void h_GetNumaHighestNodeNumber(CPU *c) {
    uint32_t out = A(c, 0);
    if (out) st32(out, 0);
    c->eax = 1; R(c, 4);
}
static void h_GetNumaProcessorNode(CPU *c) { (void)A(c,0); (void)A(c,1); c->eax = 1; R(c, 8); }
static void h_GetNumaNodeProcessorMask(CPU *c) {
    uint32_t out = A(c, 1);
    if (out) { st32(out, 1); st32(out+4, 0); }
    c->eax = 1; R(c, 8);
}

/* ---- CreateFileA：暂时返回无效句柄（后面接 Android 文件系统）---- */
static void h_CreateFileA(CPU *c) {
    uint32_t p = A(c, 0);
    fprintf(stderr, "[host] CreateFileA(%s) not implemented\n",
            p ? (const char *)(uintptr_t)p : "?");
    c->eax = 0xFFFFFFFFu; R(c, 28);
}


/* ---- CRT 静态初始化遍历 ----
 *
 * 这两个必须真实现：它们遍历 .CRT$XCU 段的函数指针数组并逐个调用。
 * 假桩会让所有 C++ 全局对象构造器不执行，后面必然崩。
 *
 * 实现照抄 host_win32.c 的同名函数，包括那个关键细节：
 * 翻译后的 RET 必须能在 guest 栈上弹到一个返回地址，即使控制流是通过
 * C 调用栈回到宿主垫片的。
 */
static void cdecl_ret(CPU *c) { (void)gpop(c); }

static int call_guest_init(CPU *__restrict c, uint32_t target)
{
    uint32_t saved = c->esp;
    gpush(c, 0xFFF1A11EU);          /* 放进 guest 栈的假返回地址 */
    guest_call(c, target);
    if (c->fault) return 0;
    if (c->esp != saved) {
        guest_fault(c, target, "CRT callback did not restore the cdecl stack");
        return 0;
    }
    return 1;
}

static void h__initterm_e(CPU *c)
{
    uint32_t cur = A(c, 0), end = A(c, 1), result = 0;
    if (cur > end || ((end - cur) & 3u)) {
        guest_fault(c, cur, "_initterm_e invalid range"); return;
    }
    while (cur < end) {
        uint32_t cb = ld32(cur);
        cur += 4u;
        if (!cb) continue;
        if (!call_guest_init(c, cb)) return;
        result = c->eax;
        if (result) break;
    }
    c->eax = result;
    cdecl_ret(c);
}

static int g_initterm_log = 3;

static void h__initterm(CPU *c)
{
    uint32_t cur = A(c, 0), end = A(c, 1);
    unsigned n = 0;
    if (cur > end || ((end - cur) & 3u)) {
        guest_fault(c, cur, "_initterm invalid range"); return;
    }
    if (g_initterm_log > 0) {
        g_initterm_log--;
        fprintf(stderr, "[crt] _initterm(%08x..%08x) has %u entries:",
                cur, end, (end - cur) / 4u);
        for (uint32_t q = cur; q < end; q += 4u) {
            uint32_t cb = ld32(q);
            if (cb) fprintf(stderr, " %08x", cb);
        }
        fprintf(stderr, "\n");
    }
    while (cur < end) {
        uint32_t cb = ld32(cur);
        cur += 4u;
        if (cb) { n++; if (!call_guest_init(c, cb)) return; }
    }
    if (n) fprintf(stderr, "[crt] _initterm ran %u constructors\n", n);
    cdecl_ret(c);
}

/* _set_app_type(int) —— cdecl，参数由调用方清理。
 * 见 host_win32.c 里的 ABI 说明：MSVC 把三次调用合并成一次 add esp,0xc，
 * 所以这里绝不能按 stdcall 清参数。 */
static void h__set_app_type(CPU *c) { c->eax = 0; cdecl_ret(c); }

/* ================================================================ 分发表 */
typedef struct { const char *name; void (*fn)(CPU *); } entry;
static const entry TBL[] = {
    {"KERNEL32.dll!GetSystemTimeAsFileTime", h_GetSystemTimeAsFileTime},
    {"KERNEL32.dll!GetLocalTime", h_GetLocalTime},
    {"KERNEL32.dll!QueryPerformanceCounter", h_QueryPerformanceCounter},
    {"KERNEL32.dll!QueryPerformanceFrequency", h_QueryPerformanceFrequency},
    {"KERNEL32.dll!VirtualAlloc", h_VirtualAlloc},
    {"KERNEL32.dll!VirtualFree", h_VirtualFree},
    {"KERNEL32.dll!VirtualQuery", h_VirtualQuery},
    {"KERNEL32.dll!VirtualUnlock", h_VirtualUnlock},
    {"KERNEL32.dll!GetLargePageMinimum", h_GetLargePageMinimum},
    {"KERNEL32.dll!GetCurrentProcess", h_GetCurrentProcess},
    {"KERNEL32.dll!GetCurrentProcessId", h_GetCurrentProcessId},
    {"KERNEL32.dll!GetCurrentThreadId", h_GetCurrentThreadId},
    {"KERNEL32.dll!GetCurrentProcessorNumber", h_GetCurrentProcessorNumber},
    {"KERNEL32.dll!IsDebuggerPresent", h_IsDebuggerPresent},
    {"KERNEL32.dll!IsProcessorFeaturePresent", h_IsProcessorFeaturePresent},
    {"KERNEL32.dll!TerminateProcess", h_TerminateProcess},
    {"KERNEL32.dll!TerminateThread", h_TerminateThread},
    {"KERNEL32.dll!GetProcessTimes", h_GetProcessTimes},
    {"KERNEL32.dll!K32GetProcessMemoryInfo", h_K32GetProcessMemoryInfo},
    {"KERNEL32.dll!SetUnhandledExceptionFilter", h_SetUnhandledExceptionFilter},
    {"KERNEL32.dll!UnhandledExceptionFilter", h_UnhandledExceptionFilter},
    {"KERNEL32.dll!RaiseException", h_RaiseException},
    {"KERNEL32.dll!Sleep", h_Sleep},
    {"KERNEL32.dll!SetThreadExecutionState", h_SetThreadExecutionState},
    {"KERNEL32.dll!SetThreadPriority", h_SetThreadPriority},
    {"KERNEL32.dll!GetSystemInfo", h_GetSystemInfo},
    {"KERNEL32.dll!CreateThread", h_CreateThread},
    {"KERNEL32.dll!InitializeCriticalSection", h_InitializeCriticalSection},
    {"KERNEL32.dll!InitializeCriticalSectionAndSpinCount", h_InitializeCriticalSectionAndSpinCount},
    {"KERNEL32.dll!DeleteCriticalSection", h_DeleteCriticalSection},
    {"KERNEL32.dll!EnterCriticalSection", h_EnterCriticalSection},
    {"KERNEL32.dll!LeaveCriticalSection", h_LeaveCriticalSection},
    {"KERNEL32.dll!TryEnterCriticalSection", h_TryEnterCriticalSection},
    {"KERNEL32.dll!InitializeSListHead", h_InitializeSListHead},
    {"KERNEL32.dll!CreateEventW", h_CreateEventW},
    {"KERNEL32.dll!SetEvent", h_SetEvent},
    {"KERNEL32.dll!ResetEvent", h_ResetEvent},
    {"KERNEL32.dll!WaitForSingleObject", h_WaitForSingleObject},
    {"KERNEL32.dll!WaitForSingleObjectEx", h_WaitForSingleObjectEx},
    {"KERNEL32.dll!LockFileEx", h_LockFileEx},
    {"KERNEL32.dll!UnlockFileEx", h_UnlockFileEx},
    {"KERNEL32.dll!TlsAlloc", h_TlsAlloc},
    {"KERNEL32.dll!TlsFree", h_TlsFree},
    {"KERNEL32.dll!TlsGetValue", h_TlsGetValue},
    {"KERNEL32.dll!TlsSetValue", h_TlsSetValue},
    {"KERNEL32.dll!FlsAlloc", h_FlsAlloc},
    {"KERNEL32.dll!FlsFree", h_FlsFree},
    {"KERNEL32.dll!FlsSetValue", h_FlsSetValue},
    {"KERNEL32.dll!GetModuleHandleA", h_GetModuleHandleA},
    {"KERNEL32.dll!GetModuleHandleW", h_GetModuleHandleW},
    {"KERNEL32.dll!GetProcAddress", h_GetProcAddress},
    {"KERNEL32.dll!LoadLibraryA", h_LoadLibraryA},
    {"KERNEL32.dll!FreeLibrary", h_FreeLibrary},
    {"KERNEL32.dll!GetCurrentDirectoryA", h_GetCurrentDirectoryA},
    {"KERNEL32.dll!GetFullPathNameW", h_GetFullPathNameW},
    {"KERNEL32.dll!GetFileAttributesA", h_GetFileAttributesA},
    {"KERNEL32.dll!CreateDirectoryA", h_CreateDirectoryA},
    {"KERNEL32.dll!GetStdHandle", h_GetStdHandle},
    {"KERNEL32.dll!GetEnvironmentVariableA", h_GetEnvironmentVariableA},
    {"KERNEL32.dll!WriteConsoleA", h_WriteConsoleA},
    {"KERNEL32.dll!CloseHandle", h_CloseHandle},
    {"KERNEL32.dll!FindClose", h_FindClose},
    {"KERNEL32.dll!FindFirstFileW", h_FindFirstFileW},
    {"KERNEL32.dll!FindNextFileW", h_FindNextFileW},
    {"KERNEL32.dll!GlobalAlloc", h_GlobalAlloc},
    {"KERNEL32.dll!GlobalLock", h_GlobalLock},
    {"KERNEL32.dll!GlobalUnlock", h_GlobalUnlock},
    {"KERNEL32.dll!GetLastError", h_GetLastError},
    {"KERNEL32.dll!SetLastError", h_SetLastError},
    {"KERNEL32.dll!GetNumaHighestNodeNumber", h_GetNumaHighestNodeNumber},
    {"KERNEL32.dll!GetNumaProcessorNode", h_GetNumaProcessorNode},
    {"KERNEL32.dll!GetNumaNodeProcessorMask", h_GetNumaNodeProcessorMask},
    {"KERNEL32.dll!CreateFileA", h_CreateFileA},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_initterm", h__initterm},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_initterm_e", h__initterm_e},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_set_app_type", h__set_app_type},
};
#define NTBL ((int)(sizeof TBL / sizeof TBL[0]))

int host_android_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    for (int i = 0; i < NTBL; i++) {
        if (strcmp(TBL[i].name, name) == 0) {
            TBL[i].fn(c);
            return 1;
        }
    }
    return 0;
}
int host_android_count(void) { return NTBL; }
