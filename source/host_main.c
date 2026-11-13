/* host entry point */
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

/* ★ 2026-11-10: lets the GL layer see "what was the last import during a slow frame".
 * See ga_SwapBuffers in host_gl_audio.c: a slow frame prints that name, so you can
 * tell at a glance which Win32 call the stall lands on. */
const char *host_last_import(void) { return g_last_import_p; }

/* ★★ 2026-11-10: the "previous **non**-SwapBuffers import", used by the [stall] probe.
 *
 * The original probe wrote host_last_import() into the [stall] line, but SwapBuffers is
 * itself an import, so the log always said "GDI32.dll!SwapBuffers" --
 * that was the probe's bug, not the actual state.
 *
 * Fix: keep a separate "last import, skipping SwapBuffers". Asking from inside
 * ga_SwapBuffers then yields the genuinely previous call, which distinguishes:
 *   . a stub is blocking (the name matches directly)
 *   . or it is stuck in translated guest code (the import count delta is ~0) */
static const char *g_prev_import_p = "(none)";
const char *host_prev_import(void) { return g_prev_import_p; }
/* The guest caller return address for each import = the return address on top of the stack.
   This is more useful than g_trace: g_trace only records the import address itself. */
static uint32_t g_caller[128];
static unsigned g_caller_n = 0;
static uintptr_t g_base = 0;
CPU *g_cur_cpu = 0;
void *g_cur_cpu_diag = 0;

/* Call trace: records the last 256 guest call targets */
#define TRACE_N 256
static uint32_t g_trace[TRACE_N];
static unsigned g_trace_n = 0;
void guest_trace_hit(uint32_t addr) {
    g_trace[g_trace_n % TRACE_N] = addr;
    g_trace_n++;
}

/* ============================================================================
 * Signal handlers must **not** use stdio / malloc / fopen.
 *
 * Trap hit on 2026-11-03: doing fopen("/proc/self/maps") inside the crash handler
 * mallocs, and by then the heap may already be corrupted -- so the handler crashes a
 * second time and every diagnostic output after it is lost. The symptom was "only the
 * first [SEGV] line gets printed". The same cause explains the earlier rounds where
 *
 * "the crash handler printed nothing". Signed handlers get their own primitives based
 * only on write(2):
 *   sig_puts   -- write a string directly
 *   sig_printf -- vsnprintf into a stack buffer, then write
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
    /* ★ write-protect watch hit: this is a trap we set ourselves, not a real crash.
     * Printing "who wrote it" -- the stack walk reaches the host's memcpy/memset,
     * and beyond that the guest call site. */
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
          {   /* non-fatal mode: record it, then restore writability and let the program continue */
            extern int g_watch_nonfatal;
            extern int watch_handle_hit(uintptr_t, uint32_t);
            if (g_watch_nonfatal) {
                watch_handle_hit((uintptr_t)u->uc_mcontext.pc, 0);
                return;                   /* non-fatal mode: always keep going */
            }
          }
          _exit(70);   /* distinct exit code to tell it apart from a real crash */
      } }

    fprintf(stderr, "\n[SEGV] sig=%d fault_addr=%p\n", sig, si->si_addr);
    { extern void sb_report(void); sb_report(); }
    /* -- crash diagnostics (all via write(2), no stdio/malloc) --------------
     *
     * 1) Is the crash address mapped? open/read /proc/self/maps directly (syscall).
     * 2) Where does this "value" appear in the heap, image and stack? Pure memory reads, safe.
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

      /* ---- 2) value-origin scan: walk each **actually readable** segment from maps ----
       *
       * The previous version hard-coded 0x3c000000..0x30860000 to scan the image, but
       * that range is not fully mapped (.text is r-x, .data is at 0x3c7a9000 with
       * holes in between), so the signal handler crashed again on an unmapped page and
       * the whole diagnostic failed silently. Correct approach: read maps for each segment's [lo,hi) and permissions, and scan only the readable ones. */
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
                          /* only scan guest address space (low 3GB); host mappings are irrelevant */
                          if (lo < 0xC0000000UL) {
                              unsigned long a = lo;
                              while (a + 4 <= hi) {
                                  uint32_t v; memcpy(&v, (void *)a, 4);
                                  /* stores the "base"; only +0/+4/+8 dereferenced equals fault_addr */
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

    /* pc most likely lands in a dlopen'd GPU driver; symbolicate down to library name + offset */
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
    {   /* most recent executed Lua guest callback */
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
    /* ★ stderr is fully buffered when redirected to a file, so _exit without a flush drops
     * the last batch of output -- that explains several earlier "crash handler printed nothing". */
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

/* Per-import logging is off by default: one 5-minute startup prints 40 million lines / 3.3 GB,
 * and the disk writes alone slow the game by an order of magnitude. Enable with IMP_TRACE=1.
 * The "last import / recent caller" ring is always maintained, so crash dumps are unaffected. */
static int g_imp_trace = -1;

/* ★★ 2026-11-10: slow-import probe.
 *
 * Used to locate that periodic ~1000 ms frame spike (the one that keeps
 * appearing in perf as "max 1005-1026 ms"). If the spike is a block inside some
 * import stub (sleep / lock wait / disk write), this prints the name and the duration directly.
 *
 * Uses CLOCK_MONOTONIC_COARSE: goes through vDSO, no syscall, ~1-4 ms precision.
 * Plenty for finding 1000 ms spikes, while each import costs only tens of nanoseconds.
 * Cost of enabling it fully: ~3.6 billion imports x 2 time reads ≈ seconds in total,
 * several orders of magnitude less than the 3.3 GB of IMP_TRACE logging.
 * SLOW_IMP_MS can turn it off (set it very large). */
static int g_slow_ms = -1;
static inline uint64_t g_now_coarse_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ★★ 2026-11-10: performance fix -- this was one of the main causes of slow startup.
 *
 * The old code (diagnostics I added in a previous round) did this on every import:
 *     strstr(name, "SwapBuffers")      ~30 ns
 *     snprintf(g_prev_import, 128, ...)  ~150-400 ns  <- the culprit
 *     snprintf(g_last_import,  128, ...)  ~150-400 ns  <- the culprit
 *     clock_gettime(COARSE) × 2          ~40 ns
 *
 * And import calls number in the **hundreds of millions** (fread alone runs 24 million times).
 * Conservatively 2 snprintf x 100M x 300ns ≈ **60 seconds** -- and startup was
 * measured at 21.5 s (first frame to the game reading the pad), most of it right here.
 *
 * Fix: import names all come from a static table (compile-time string constants) and
 * **never go stale**, so store the pointer with no copying. Cost drops from ~600ns to ~2ns.
 *
 * The SwapBuffers check also drops strstr: dedupe by pointer first (the same import
 * called repeatedly is the common case), and only strstr when the name really changed. */
static int is_swapbuffers(const char *n)
{
    /* only called when the name changes, not a hot path */
    return n && strstr(n, "SwapBuffers") != NULL;
}

int guest_host_import(CPU *__restrict c, const char *name) {
    if (g_slow_ms < 0) {
        const char *e = getenv("SLOW_IMP_MS");
        /* ★★ 2026-11-10: default changed from 50ms **to 0 (off)**.
         *
         * Two clock_gettime(COARSE) per import is ~40 ns, and hundreds of millions of
         * imports -> ~4 seconds. The known stall root causes (heap_seg_del linear walk,
         * Sleep(1000)) are all fixed, so the always-on probe is no longer needed.
         * To diagnose: enable with the SLOW_IMP_MS=50 environment variable. */
        g_slow_ms = e ? atoi(e) : 0;
        if (g_slow_ms < 0) g_slow_ms = 0;
    }
    uint64_t t0 = g_slow_ms ? g_now_coarse_ms() : 0;

    g_host_import_calls++;
    /* ★ pointer assignment, no copy. See the performance note above. */
    if (name && name != g_last_import_p) {
        /* name changed (not the same static string) -> update prev, and do one SwapBuffers check. */
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
    /* ★ slow-call report: prints only those above the threshold, so the log cannot flood. */
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
 * Startup phase timing (2026-11-06)
 *
 * The user asked "can startup be faster". Before answering, one has to be able to
 * measure -- previously there was only a pile of timestamp-less logs with no way to
 * tell where 100 seconds went. Every startup prints "[boot] T+xxxx ms  phase" at key points.
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
 * Low-4GB occupancy probe (2026-11-07)
 *
 * Background: .so mode failed on device, reporting
 *     guest: VirtualAlloc at 0x3c000000 size 0x85f000 failed
 * The cause was that 0x3c000000 was already mapped by something else in the app
 * But addresses in the generated code are compile-time constants, so the image **must** land on the base chosen at build time.
 *
 * So before loading the image, print the low-4GB occupancy outright:
 *   . is the current base available
 *   . which candidate base is available
 *   . who is occupying the current base
 *
 * Output goes to stderr -- in in-process mode that is redirected to <game>/inproc.log,
 * in child-process mode it is run.log. Visible either way.
 * ========================================================================== */
static uint32_t detect_image_size(void)
{
    /* use the loader's constant directly, to avoid drifting from the value in guest.h */
    extern const unsigned guest_pe_expected_image_size_dummy;
    (void)guest_pe_expected_image_size_dummy;
    return 0x900000u;            /* 9 MB, enough to cover 0x85f000 */
}

static void probe_low4g(void)
{
    static const uint32_t cand[] = {
        0x3c000000u,   /* current base */
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
        if (lo >= 0x100000000ull) continue;         /* only look at the low 4GB */
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

    /* evaluate each candidate base in turn */
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
    setvbuf(stderr, NULL, _IONBF, 0);   /* crash output must never be lost to buffering */
    setvbuf(stderr, NULL, _IONBF, 0);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0);
    /* SIGILL was not installed before. When guest code jumps into 0xCC padding or a
     * translated illegal sequence, the process just dies with "Illegal instruction" and leaves no scene at all -- a wasted run. */
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
    /* Probe only in in-process mode (or when explicitly requested): in child-process mode
     * the address space is clean anyway, and printing all this is just noise. */
    {
        extern int host_gl4es_window_mode(void) __attribute__((weak));
        if ((host_gl4es_window_mode && host_gl4es_window_mode()) ||
            getenv("PROBE_LOW4G"))
            probe_low4g();
    }
    if (guest_image_load(pe) != 0) {
        puts("image load failed");
        /* when loading fails the layout must be printed regardless -- it is the entire evidence base */
        probe_low4g();
        return 1;
    }
    printf("✓ image loaded base=0x%08x\n", (unsigned)GUEST_IMAGE_BASE);
    host_boot_mark("PE image loaded");
    /* GL backend: the EGL context must exist before any guest GL call.
     * Runs offscreen on a pbuffer; the APK stage swaps in the SurfaceView window surface.
     *
     * ★ 2026-11-03: the size must match the window size the game itself believes in.
     *   This used to hard-code 1280x720 while the game read 960x540 from options.ini,
     *   so it drew only into the lower-left 960x540 viewport and the remaining 320x180
     *   was uninitialized memory -- frame capture showed "white rectangle + black border + coloured noise".
     *   The right thing is to read the game's own config, falling back to 1280x720. */
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
        /* ★ 2026-11-06: in window mode (.so) the options.ini size must **not** be used.
         *
         * options.ini has no WindowWidth/WindowHeight at all (measured), so gw/gh would
         * fall back to 1280x720. But in window mode the real back buffer size is the
         * SurfaceView's pixel size (e.g. 2400x1080).
         *
         * If 1280x720 were written back here:
         *   set_getmainfbsize() returns 1280x720 -> the game sets glViewport from it
         *   -> only a patch in the top-left is drawn, the rest is black.
         *
         * In window mode the size is already set by JNI's host_gl4es_set_fb_size();
         * passing 0 here means "do not override". */
        {
            extern int host_gl4es_window_mode(void) __attribute__((weak));
            extern int host_gl4es_fb_w(void);
            extern int host_gl4es_fb_h(void);
          if (host_gl4es_window_mode && host_gl4es_window_mode()) {
            /* ★ 2026-11-07: the size is already set by the JNI layer from options.ini
             * (the render size, not the Surface size); passing 0 here means no override.
             * See the "render size and Surface size are separated" notes in jni_bridge.c. */
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
    /* Sampling CPU profiler (enabled with PROF=1). Answers "where did those 106 seconds go". */
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
