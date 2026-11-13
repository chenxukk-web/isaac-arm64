/* 宿主入口 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <ucontext.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include "guest.h"

unsigned g_host_import_calls = 0;
static const char *g_last_import_p = "(none)";

/* ★ 2026-11-10：给 GL 层看“慢帧时最后一个导入是什么”。
 * 用途见 host_gl_audio.c 的 ga_SwapBuffers：慢帧会把这个名字打出来，
 * 一眼就能判断卡顿是落在哪个 Win32 调用上。 */
const char *host_last_import(void) { return g_last_import_p; }

/* ★★ 2026-11-10：用于 [stall] 探针的“上一个**非** SwapBuffers 导入”。
 *
 * 原探针把 host_last_import() 写进 [stall] 行，但 SwapBuffers 自己
 * 就是导入，所以日志里永远是 "GDI32.dll!SwapBuffers" ——
 * 那是探针的错，不是现场。
 *
 * 修法：维护一份“跳过 SwapBuffers 的最后一个导入”。这样在
 * ga_SwapBuffers 里问到的就是真正的前一个调用，能区分：
 *   · 是某个桩在阻塞（名字直接命中）
 *   · 还是卡在 guest 翻译代码里（导入计数差几乎为 0） */
static const char *g_prev_import_p = "(none)";
const char *host_prev_import(void) { return g_prev_import_p; }
/* 每个导入的 guest 调用者返回地址 = 栈顶那个返回地址。
   这比 g_trace 有用：g_trace 只记录导入地址本身。 */
static uint32_t g_caller[128];
static unsigned g_caller_n = 0;
static uintptr_t g_base = 0;
CPU *g_cur_cpu = 0;
void *g_cur_cpu_diag = 0;

/* 调用轨迹：记录最近 256 个 guest 调用目标 */
#define TRACE_N 256
static uint32_t g_trace[TRACE_N];
static unsigned g_trace_n = 0;
void guest_trace_hit(uint32_t addr) {
    g_trace[g_trace_n % TRACE_N] = addr;
    g_trace_n++;
}

/* ============================================================================
 * 信号处理器里**不能**用 stdio / malloc / fopen。
 *
 * 2026-11-03 踩的坑：崩溃处理器里 fopen("/proc/self/maps") 会 malloc，
 * 而此时堆可能已被破坏 —— 处理器内部二次崩溃，后面所有诊断输出全丢，
 * 现象是"只打印到 [SEGV] 第一行"。同一原因也解释了以前几轮
 * "崩溃处理器没打印"。
 *
 * 这里给信号处理器一套只用 write(2) 的输出原语：
 *   sig_puts  —— 直接写字符串
 *   sig_printf—— vsnprintf 到栈缓冲再 write
 * 两者都不分配、不加锁。
 * ==========================================================================*/
static void sig_puts(const char *s)
{
    if (!s) return;
    size_t n = 0;
    while (s[n]) n++;
    { ssize_t r = write(2, s, n); (void)r; }
}

static void sig_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    { ssize_t r = write(2, buf, (size_t)n); (void)r; }
}

static void on_sig(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = (ucontext_t *)uc;
    /* ★ 写保护监视命中：这是我们自己设的陷阱，不是真崩。
     * 打印"谁写的"——栈回溯能一路走到 host 的 memcpy/memset，
     * 再往外就是 guest 的调用点。 */
    { extern int g_watch_armed;
      extern uintptr_t g_watch_page;
      uintptr_t a = (uintptr_t)si->si_addr;
      if (g_watch_armed && a >= g_watch_page && a < g_watch_page + 4096u) {
          fprintf(stderr,
              "\n[WATCH] ★★ caught the writer! wrote to guard page %08lx (offset +0x%lx)\n",
              (unsigned long)g_watch_page, (unsigned long)(a - g_watch_page));
          fprintf(stderr, "  si_code=%d (%s)\n", si->si_code,
                  si->si_code == 2 /*SEGV_ACCERR*/ ? "perm" : "other");
          fprintf(stderr, "  pc = %p   lr = %p\n",
                  (void *)u->uc_mcontext.pc, (void *)u->uc_mcontext.regs[30]);
          { Dl_info di;
            if (dladdr((void *)u->uc_mcontext.pc, &di) && di.dli_fname)
                fprintf(stderr, "  pc belongs to %s +0x%lx (%s)\n", di.dli_fname,
                        (unsigned long)((uintptr_t)u->uc_mcontext.pc -
                                        (uintptr_t)di.dli_fbase),
                        di.dli_sname ? di.dli_sname : "?");
            if (dladdr((void *)u->uc_mcontext.regs[30], &di) && di.dli_fname)
                fprintf(stderr, "  lr belongs to %s +0x%lx (%s)\n", di.dli_fname,
                        (unsigned long)((uintptr_t)u->uc_mcontext.regs[30] -
                                        (uintptr_t)di.dli_fbase),
                        di.dli_sname ? di.dli_sname : "?");
          }
          { extern CPU *g_cur_cpu;
            if (g_cur_cpu) {
              CPU *k = g_cur_cpu;
              fprintf(stderr, "  guest: eax=%08x ebx=%08x ecx=%08x edx=%08x "
                              "esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                      k->eax,k->ebx,k->ecx,k->edx,k->esi,k->edi,k->ebp,k->esp);
            } }
          fprintf(stderr, "  last import = %s   import calls = %u\n",
                  g_last_import_p, g_host_import_calls);
          fprintf(stderr, "  -- last 12 call targets --\n");
          { unsigned stt = g_trace_n > 12 ? g_trace_n - 12 : 0;
            for (unsigned i = stt; i < g_trace_n; i++)
                fprintf(stderr, "    [%u] 0x%08x\n", i, g_trace[i % TRACE_N]); }
          fflush(NULL);
          {   /* 非致命模式：记录后恢复可写，让程序继续 */
            extern int g_watch_nonfatal;
            extern int watch_handle_hit(uintptr_t, uint32_t);
            if (g_watch_nonfatal) {
                watch_handle_hit((uintptr_t)u->uc_mcontext.pc, 0);
                return;                   /* 非致命模式：永远继续执行 */
            }
          }
          _exit(70);   /* 用独立退出码，和真崩区分 */
      } }

    fprintf(stderr, "\n[SEGV] sig=%d fault_addr=%p\n", sig, si->si_addr);
    { extern void sb_report(void); sb_report(); }
    /* ── 崩溃诊断（全部用 write(2)，不用 stdio/malloc） ──────────────
     *
     * 1) 崩溃地址有没有映射？直接 open/read /proc/self/maps（syscall）。
     * 2) 这个"值"在堆、镜像、栈里出现在哪里？纯内存读，安全。
     */
    { uintptr_t fa = (uintptr_t)si->si_addr;
      extern uintptr_t heap_bump_cursor(void);

      /* ---- 1) maps ---- */
      {
          int fd = open("/proc/self/maps", O_RDONLY);
          if (fd >= 0) {
              static char mbuf[1 << 16];
              ssize_t got = read(fd, mbuf, sizeof mbuf - 1);
              close(fd);
              sig_puts("  -- mappings near fault_addr --\n");
              int hit = 0;
              if (got > 0) {
                  mbuf[got] = 0;
                  char *p = mbuf;
                  while (*p) {
                      char *nl = p;
                      while (*nl && *nl != '\n') nl++;
                      unsigned long lo = 0, hi = 0;
                      char perms[8] = {0};
                      if (sscanf(p, "%lx-%lx %7s", &lo, &hi, perms) == 3) {
                          if (fa >= lo && fa < hi) {
                              sig_printf("  ★ hit %.300s\n", p);
                              hit = 1;
                          } else if (!hit && lo > fa && lo - fa < 0x1000000UL) {
                              sig_printf("  > next %.300s\n", p);
                              hit = 2;
                          }
                      }
                      if (!*nl) break;
                      p = nl + 1;
                  }
              }
              if (!hit) sig_printf("  ★ %p unmapped\n", (void *)fa);
          }
      }

      /* ---- 2) 值来源扫描：按 maps 给出的**实际可读段**逐段扫 ----
       *
       * 上一版硬编码 0x3c000000..0x30860000 扫镜像，结果那段并没有全部
       * 映射（.text 是 r-x，.data 在 0x3c7a9000 但中间有洞），
       * 信号处理器在未映射页上二次崩溃，整个诊断静默失败。
       * 正确做法：先读 maps 拿到每段的 [lo,hi) 和权限，只在可读段里扫。 */
      if (fa >= 0x10000UL && fa < 0xC0000000UL) {
          uint32_t needle = (uint32_t)fa;
          int fd2 = open("/proc/self/maps", O_RDONLY);
          if (fd2 >= 0) {
              static char mbuf2[1 << 16];
              ssize_t got2 = read(fd2, mbuf2, sizeof mbuf2 - 1);
              close(fd2);
              if (got2 > 0) {
                  mbuf2[got2] = 0;
                  char *p = mbuf2;
                  int scanned = 0, found = 0;
                  while (*p && found < 5) {
                      char *nl = p;
                      while (*nl && *nl != '\n') nl++;
                      unsigned long lo = 0, hi = 0;
                      char perms[8] = {0};
                      if (sscanf(p, "%lx-%lx %7s", &lo, &hi, perms) == 3 &&
                          perms[0] == 'r' && hi > lo) {
                          /* 只扫 guest 地址空间（低 3GB）；宿主映射不相关 */
                          if (lo < 0xC0000000UL) {
                              unsigned long a = lo;
                              while (a + 4 <= hi) {
                                  uint32_t v; memcpy(&v, (void *)a, 4);
                                  /* 存的是"基址"，解引用 +0/+4/+8 才等于 fault_addr */
                                  if (v == needle || v + 4u == needle ||
                                      v + 8u == needle) {
                                      sig_printf("  ★★ segment %08lx-%08lx %s contains the value @%08lx"
                                                 " (delta %+ld)\n",
                                                 lo, hi, perms, a,
                                                 (long)needle - (long)v);
                                      if (++found >= 5) break;
                                  }
                                  a += 4;
                              }
                              scanned++;
                          }
                      }
                      if (!*nl) break;
                      p = nl + 1;
                  }
                  sig_printf("  (scanned %d readable segments)\n", scanned);
                  if (!found) sig_puts("  ★ the value is in no readable segment\n");
              }
          }
      }
    }

    /* pc 很可能落在 dlopen 进来的 GPU 驱动里，符号化到库名+偏移 */
    { Dl_info di;
      void *pc = (void *)u->uc_mcontext.pc;
      if (dladdr(pc, &di) && di.dli_fname)
          fprintf(stderr, "  pc belongs to %s +0x%lx  (symbol %s)\n",
                  di.dli_fname,
                  (unsigned long)((uintptr_t)pc - (uintptr_t)di.dli_fbase),
                  di.dli_sname ? di.dli_sname : "?");
      else
          fprintf(stderr, "  pc does not belong to any loaded module\n"); }
    fprintf(stderr, "  lr = %p\n", (void *)u->uc_mcontext.regs[30]);
    fprintf(stderr, "  last import = %s\n", g_last_import_p);
    { extern CPU *g_cur_cpu;
      if (g_cur_cpu) {
        CPU *k = g_cur_cpu;
        fprintf(stderr, "  guest: eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
                k->eax,k->ebx,k->ecx,k->edx);
        fprintf(stderr, "         esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                k->esi,k->edi,k->ebp,k->esp);
      } }
    fprintf(stderr, "  import calls = %u\n", g_host_import_calls);
    {   /* 最近执行过的 Lua guest 回调 */
        extern uint32_t g_lua_cb_ring[16];
        extern unsigned g_lua_cb_n;
        if (g_lua_cb_n) {
            unsigned st = g_lua_cb_n > 16 ? g_lua_cb_n - 16 : 0;
            fprintf(stderr, "  recent Lua callbacks:");
            for (unsigned i = st; i < g_lua_cb_n; i++)
                fprintf(stderr, " %08x", g_lua_cb_ring[i & 15u]);
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "  guest_call count = %u\n", g_trace_n);
    fprintf(stderr, "  -- caller return addresses of the last 10 imports --\n");
    { unsigned st = g_caller_n > 20 ? g_caller_n - 20 : 0;
      for (unsigned i = st; i < g_caller_n; i++)
          fprintf(stderr, "    import#%u <- 0x%08x\n", i + 1, g_caller[i % 128]); }
    fprintf(stderr, "  -- last 40 call targets --\n");
    unsigned start = g_trace_n > 40 ? g_trace_n - 40 : 0;
    for (unsigned i = start; i < g_trace_n; i++)
        fprintf(stderr, "    [%u] 0x%08x\n", i, g_trace[i % TRACE_N]);
    /* ★ stderr 被重定向到文件时是全缓冲，不 flush 就 _exit 会丢掉
     * 最后一批输出 —— 之前好几轮"崩溃处理器没打印"都是这个原因。 */
    fflush(NULL);
    _exit(139);
}
static int g_miss_printed = 0;

int host_android_dispatch(CPU *__restrict c, const char *name);
int host_android_count(void);
int host_crt_dispatch(CPU *__restrict c, const char *name);
int host_crt_count(void);
int host_android_dynamic(CPU *__restrict c, uint32_t addr);
int host_lua_dispatch(CPU *__restrict c, const char *name);
int host_lua_count(void);
int host_user32_dispatch(CPU *__restrict c, const char *name);
int host_user32_count(void);
int host_gl_dispatch(CPU *__restrict c, const char *name);
int host_gl_count(void);
int host_audio_dispatch(CPU *__restrict c, const char *name);
int host_audio_count(void);
int host_steam_dispatch(CPU *__restrict c, const char *name);
int host_steam_count(void);
int host_crt_stdio_dispatch(CPU *__restrict c, const char *name);
int host_crt_stdio_count(void);
int host_crt_file_dispatch(CPU *__restrict c, const char *name);
int host_crt_file_count(void);
int host_sys_dispatch(CPU *__restrict c, const char *name);
int host_sys_count(void);
int host_gl4es_init(int width, int height);
int host_thread_dispatch(CPU *__restrict c, const char *name);
int host_thread_count(void);
int host_msvcp_dispatch(CPU *__restrict c, const char *name);
int host_msvcp_count(void);
int host_crt_time_dispatch(CPU *__restrict c, const char *name);
int host_crt_time_count(void);

/* 导入逐条日志默认关闭：一次 5 分钟的启动会打印 4000 万行 / 3.3 GB，
 * 光写盘就把游戏拖慢一个数量级。诊断时 IMP_TRACE=1 再打开。
 * 「最后导入 / 最近调用者」的环形记录始终维护，崩溃 dump 不受影响。 */
static int g_imp_trace = -1;

/* ★★ 2026-11-10：慢导入探针。
 *
 * 用来定位那个周期性 ~1000 ms 的帧尖峰（perf 里反复出现的
 * “最大 1005-1026 ms”）。如果尖峰是某个导入桩里的阻塞（sleep/
 * 等锁/写盘），这里会直接把名字和时间打出来。
 *
 * 用 CLOCK_MONOTONIC_COARSE：走 vDSO、无系统调用，~1-4 ms 精度。
 * 对找 1000 ms 级尖峰完全够，而每次导入只花几十纳秒。
 * 全量打开的开销：~36 亿次导入 × 2 次取时间 ≈ 秒级（整个会话累计），
 * 比之前 IMP_TRACE 的 3.3 GB 日志小几个数量级。
 * SLOW_IMP_MS=1 可关闭（设成很大的数即可）。 */
static int g_slow_ms = -1;
static inline uint64_t g_now_coarse_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ★★ 2026-11-10：性能修复 —— 这是启动慢的主要原因之一。
 *
 * 旧代码（我上一轮加的诊断）每次导入都做：
 *     strstr(name, "SwapBuffers")      ~30 ns
 *     snprintf(g_prev_import, 128, ...)  ~150-400 ns  ← 元凶
 *     snprintf(g_last_import,  128, ...)  ~150-400 ns  ← 元凶
 *     clock_gettime(COARSE) × 2          ~40 ns
 *
 * 而导入调用量是**上亿次**（Isaac 加载资源时 fread 就有 2400 万次）。
 * 保守估计 2 个 snprintf × 1 亿次 × 300ns ≈ **60 秒** —— 实际观测到
 * 启动耗时 21.5 秒（从第一帧到游戏开始读手柄），大部分是这里。
 *
 * 修复：导入名都来自静态表（编译期字符串常量），**永远不会失效**，
 * 所以直接存指针，不做任何拷贝。开销从 ~600ns 降到 ~2ns。
 *
 * SwapBuffers 判断也改成免 strstr：先用指针去重（同一导入连续调用
 * 是最常见情况），只有名字真的变了才做一次 strstr。 */
static int is_swapbuffers(const char *n)
{
    /* 只在名字变化时调用，不是热路径 */
    return n && strstr(n, "SwapBuffers") != NULL;
}

int guest_host_import(CPU *__restrict c, const char *name) {
    if (g_slow_ms < 0) {
        const char *e = getenv("SLOW_IMP_MS");
        /* ★★ 2026-11-10：默认从 50ms **改成 0（关闭）**。
         *
         * 每次导入两次 clock_gettime(COARSE) 约 40 ns，
         * 而上亿次导入 → 约 4 秒。已知的卡顿根因（heap_seg_del
         * 线性遍历、Sleep(1000)）都已修复，不再需要常备探针。
         * 需要诊断时： SLOW_IMP_MS=50 环境变量开启。 */
        g_slow_ms = e ? atoi(e) : 0;
        if (g_slow_ms < 0) g_slow_ms = 0;
    }
    uint64_t t0 = g_slow_ms ? g_now_coarse_ms() : 0;

    g_host_import_calls++;
    /* ★ 指针赋值，不拷贝。见上面的性能说明。 */
    if (name && name != g_last_import_p) {
        /* 名字变了（不是同一静态串）→ 更新 prev，并算一次 SwapBuffers。 */
        if (!is_swapbuffers(name)) g_prev_import_p = name;
    }
    g_last_import_p = name ? name : "?";
    { uint32_t ret = ld32(c->esp);
      g_caller[g_caller_n % 128] = ret;
      g_caller_n++;
      if (g_imp_trace < 0) g_imp_trace = getenv("IMP_TRACE") != NULL;
      if (g_imp_trace)
          fprintf(stderr, "[imp %u] %-42s <- 0x%08x  ebp=%08x esp=%08x\n",
                  g_host_import_calls, name ? name : "?", ret, c->ebp, c->esp); }
    if (host_android_dispatch(c, name)) goto done;
    if (host_crt_dispatch(c, name)) goto done;
    if (host_lua_dispatch(c, name)) goto done;
    if (host_user32_dispatch(c, name)) goto done;
    if (host_gl_dispatch(c, name)) goto done;
    if (host_audio_dispatch(c, name)) goto done;
    if (host_msvcp_dispatch(c, name)) goto done;
    if (host_thread_dispatch(c, name)) goto done;
    if (host_sys_dispatch(c, name)) goto done;
    if (host_crt_file_dispatch(c, name)) goto done;
    if (host_crt_time_dispatch(c, name)) goto done;
    if (host_crt_stdio_dispatch(c, name)) goto done;
    if (host_steam_dispatch(c, name)) goto done;
    if (g_miss_printed < 20) {
        g_miss_printed++;
        fprintf(stderr, "[MISS] %s\n", name ? name : "?");
    }
    return 0;

done:
    /* ★ 慢调用报告：只打印超过阈值的，所以日志不会被冲爆。 */
    if (g_slow_ms) {
        uint64_t dt = g_now_coarse_ms() - t0;
        if (dt >= (uint64_t)g_slow_ms) {
            fprintf(stderr, "[slow] %llu ms  %s  (import#%u)\n",
                    (unsigned long long)dt, name ? name : "?",
                    g_host_import_calls);
            fflush(NULL);
        }
    }
    return 1;
}
int guest_host_dynamic(CPU *__restrict c, uint32_t token) {
    return host_android_dynamic(c, token);
}
int mainCRTStartup(void) { return 0; }

void guest_register_all(void);
void guest_register_all_imports(void);


/* ==========================================================================
 * 启动阶段计时（2026-11-06）
 *
 * 用户问"启动能不能快一点"。回答这个问题之前先要能测量 —— 之前只有
 * 一堆没时间戳的日志，看不出 100 秒花在哪一段。
 * 每次启动在关键点打一行 "[boot] T+xxxx ms  阶段名"。
 * ========================================================================== */
static struct timespec g_boot_t0;
static int g_boot_t0_set;

void host_boot_mark(const char *what)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!g_boot_t0_set) { g_boot_t0 = ts; g_boot_t0_set = 1; }
    double ms = (double)(ts.tv_sec - g_boot_t0.tv_sec) * 1000.0
              + (double)(ts.tv_nsec - g_boot_t0.tv_nsec) / 1e6;
    fprintf(stderr, "[boot] T+%8.1f ms  %s\n", ms, what);
    fflush(NULL);
}

/* ==========================================================================
 * 低 4GB 占用探测（2026-11-07）
 *
 * 背景：.so 化在真机上失败，报
 *     guest: VirtualAlloc at 0x3c000000 size 0x85f000 failed
 * 原因是 app 进程里 0x3c000000 被别的映射占了（很可能是 ART 的堆）。
 * 而 gen 代码里地址是编译期常量，镜像**必须**落在编译时选定的基址。
 *
 * 所以在加载镜像之前，直接把低 4GB 的占用情况打出来：
 *   · 当前基址可用吗
 *   · 候选基址哪一个可用
 *   · 谁占着当前基址
 *
 * 输出进 stderr —— 进程内模式下它被重定向到 <game>/inproc.log，
 * 子进程模式下就是 run.log。两边都能看到。
 * ========================================================================== */
static uint32_t detect_image_size(void)
{
    /* 直接用 loader 的常量，避免和 guest.h 里的值漂移 */
    extern const unsigned guest_pe_expected_image_size_dummy;
    (void)guest_pe_expected_image_size_dummy;
    return 0x900000u;            /* 9 MB，够覆盖 0x85f000 */
}

static void probe_low4g(void)
{
    static const uint32_t cand[] = {
        0x3c000000u,   /* 当前基址 */
        0x3c000000u, 0x28000000u, 0x38000000u, 0x18000000u,
        0x10000000u, 0x58000000u, 0x60000000u,
    };
    const uint32_t need = detect_image_size();
    FILE *m;
    char line[512];
    uint32_t rlo[512], rhi[512];
    int n = 0, i;

    m = fopen("/proc/self/maps", "r");
    if (!m) { fprintf(stderr, "[probe] cannot open /proc/self/maps\n"); return; }
    while (fgets(line, sizeof line, m) && n < 512) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        if (lo >= 0x100000000ull) continue;         /* 只看低 4GB */
        if (hi > 0x100000000ull) hi = 0x100000000ull;
        rlo[n] = (uint32_t)lo; rhi[n] = (uint32_t)hi; n++;
    }
    fclose(m);

    fprintf(stderr, "[probe] -- low 4GB occupancy (%d segments, need %u KB) --\n", n, need / 1024u);
    {
        uint32_t prev_end = 0;
        int shown = 0;
        for (i = 0; i < n && shown < 28; i++) {
            if (rlo[i] > prev_end && prev_end >= 0x10000000u) {
                fprintf(stderr, "[probe]   hole %08x..%08x  (%u KB)\n",
                        prev_end, rlo[i], (rlo[i] - prev_end) / 1024u);
                shown++;
            }
            if (rlo[i] >= 0x10000000u && rlo[i] < 0x80000000u) {
                fprintf(stderr, "[probe]   used %08x..%08x  (%u KB)\n",
                        rlo[i], rhi[i], (rhi[i] - rlo[i]) / 1024u);
                shown++;
            }
            if (rhi[i] > prev_end) prev_end = rhi[i];
        }
    }

    /* 候选基址逐个判定 */
    for (i = 0; i < (int)(sizeof cand / sizeof cand[0]); i++) {
        uint32_t lo = cand[i], hi = cand[i] + need;
        int hit = 0, k;
        for (k = 0; k < n; k++) {
            if (rlo[k] < hi && rhi[k] > lo) { hit = 1; break; }
        }
        fprintf(stderr, "[probe] candidate %08x..%08x  %s%s\n",
                lo, hi, hit ? "occupied" : "free",
                (cand[i] == GUEST_IMAGE_BASE) ? "   <- current compile-time base" : "");
    }
    fflush(NULL);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    host_boot_mark("main entered");
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);   /* 崩溃输出绝不能因缓冲丢失 */
    setvbuf(stderr, NULL, _IONBF, 0);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    /* SIGILL 以前没有安装。guest 跳到 0xCC 填充或翻译出来的非法序列时
     * 进程直接 "Illegal instruction"，什么现场都没有 —— 白跑一轮。 */
    sigaction(SIGILL, &sa, 0); sigaction(SIGTRAP, &sa, 0);
    sigaction(SIGABRT, &sa, 0);
    { Dl_info di;
      if (dladdr((void*)&main, &di) && di.dli_fbase)
          g_base = (uintptr_t)di.dli_fbase;
      else g_base = (uintptr_t)&main; }
    printf("=== The Binding of Isaac: Repentance - Android arm64 (repentogxm) ===\n");
    printf("base(&main)=%p\n", (void*)g_base);
    printf("layer entries implemented: %d (with overlap, 366 of 413 imports covered)\n"
           "  KERNEL32 %d  CRT %d  Lua %d  USER32 %d  GL %d  Audio %d\n"
           "  Steam %d  stdio %d  file %d  time %d  sys %d\n\n",
           host_android_count() + host_crt_count() + host_lua_count()
             + host_user32_count() + host_gl_count() + host_audio_count()
             + host_steam_count() + host_crt_stdio_count()
             + host_crt_file_count() + host_crt_time_count() + host_sys_count(),
           host_android_count(), host_crt_count(), host_lua_count(),
           host_user32_count(), host_gl_count(), host_audio_count(),
           host_steam_count(), host_crt_stdio_count(),
           host_crt_file_count(), host_crt_time_count(), host_sys_count());


    const char *pe = "/data/data/com.termux/files/home/work/analysis/isaac-ng.exe.unpacked";

    guest_register_all_imports();
    /* 只在进程内模式（或显式要求）时探测：子进程模式下地址空间本来就干净，
     * 打这一堆只有噪音。 */
    {
        extern int host_gl4es_window_mode(void) __attribute__((weak));
        if ((host_gl4es_window_mode && host_gl4es_window_mode()) ||
            getenv("PROBE_LOW4G"))
            probe_low4g();
    }
    if (guest_image_load(pe) != 0) {
        puts("image load failed");
        /* 加载失败时无论如何都要把布局打出来 —— 这就是排查的全部依据 */
        probe_low4g();
        return 1;
    }
    printf("✓ image loaded base=0x%08x\n", (unsigned)GUEST_IMAGE_BASE);
    host_boot_mark("PE image loaded");
    /* GL 后端：必须在任何 guest GL 调用之前建好 EGL 上下文。
     * 用 pbuffer 离屏跑；APK 阶段会换成 SurfaceView 的 window surface。
     *
     * ★ 2026-11-03：尺寸必须跟游戏自己以为的窗口尺寸一致。
     *   原来这里硬编码 1280x720，而游戏从 options.ini 读到 960x540，
     *   于是它只往左下角 960x540 的视口里画，剩下的 320x180 全是
     *   未初始化内存 —— 抓帧看到的就是"白矩形 + 黑边 + 彩色噪声"。
     *   正确做法是读游戏自己的配置。读不到再退回 1280x720。 */
    {
        int gw = 0, gh = 0;
        FILE *f = fopen("./Documents/My Games/Binding of Isaac Repentance/options.ini", "rb");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                int v = 0;
                if (sscanf(line, "WindowWidth=%d", &v) == 1) { gw = v; continue; }
                if (sscanf(line, "WindowHeight=%d", &v) == 1) { gh = v; continue; }
            }
            fclose(f);
        }
        /* ★ 2026-11-06：窗口模式（.so 化）下**不能**用 options.ini 的尺寸。
         *
         * options.ini 里根本没有 WindowWidth/WindowHeight（实测），所以
         * gw/gh 会退到 1280x720。而窗口模式下真正的后备缓冲尺寸是
         * SurfaceView 的像素尺寸（比如 2400x1080）。
         *
         * 如果这里把 1280x720 覆盖回去：
         *   set_getmainfbsize() 返回 1280x720 → 游戏按这个设 glViewport
         *   → 只画左上角一块，其余是黑的。
         *
         * 窗口模式下尺寸已经由 JNI 的 host_gl4es_set_fb_size() 设好，
         * 这里传 0 表示"别覆盖"。 */
        {
            extern int host_gl4es_window_mode(void) __attribute__((weak));
            extern int host_gl4es_fb_w(void);
            extern int host_gl4es_fb_h(void);
          if (host_gl4es_window_mode && host_gl4es_window_mode()) {
            /* ★ 2026-11-07：尺寸已由 JNI 层按 options.ini 设好（渲染尺寸，
             * 不是 Surface 尺寸），这里传 0 表示不覆盖。
             * 详见 jni_bridge.c 里"渲染尺寸与 Surface 尺寸分离"那段注释。 */
            fprintf(stderr, "[gl4es] window mode: reusing render size %dx%d (from options.ini)\n",
                    host_gl4es_fb_w(), host_gl4es_fb_h());
            if (host_gl4es_init(0, 0) != 0)
                fprintf(stderr, "[gl4es] backend init failed; GL calls will return 0\n");
          } else {
            if (gw <= 0 || gh <= 0) { gw = 1280; gh = 720; }
            fprintf(stderr, "[gl4es] pbuffer set to %dx%d per options.ini\n", gw, gh);
            if (host_gl4es_init(gw, gh) != 0)
                fprintf(stderr, "[gl4es] backend init failed; GL calls will return 0\n");
          }
        }
    }

    guest_register_all();
    {
        uint32_t probe = 0x3c000000u + 0x606258u;
        uint32_t v = ld32(probe);
        printf("IAT slot 0x%08x content = 0x%08x  %s\n", probe, v,
               v == probe ? "(tagged ✓)" : "(not patched ✗)");
    }

    CPU c;
    g_cur_cpu = &c;
    g_cur_cpu_diag = &c;
    guest_cpu_init(&c);
    if (guest_stack_init(&c) != 0) { puts("stack init failed"); return 1; }
    printf("✓ stack esp=0x%08x\n", c.esp);

    host_boot_mark("GL/host init complete; entering the entry point");

    uint32_t entry = GUEST_IMAGE_BASE + 0x005EB83E;
    printf("\nexecuting entry point 0x%08x ...\n\n", entry);
    fflush(stdout);
    /* 采样型 CPU 画像（PROF=1 时启用）。用来回答"那 106 秒花在哪"。 */
    {
        extern void host_prof_start(uintptr_t) __attribute__((weak));
        extern void host_prof_stop(void) __attribute__((weak));
        if (host_prof_start && getenv("PROF"))
            host_prof_start((uintptr_t)&main);

        guest_call(&c, entry);

        if (host_prof_stop) host_prof_stop();
    }
    printf("\n[entry point returned]\nimport calls: %u\n", g_host_import_calls);
    return 0;
}
