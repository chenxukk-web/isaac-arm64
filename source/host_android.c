/* Android host layer -- KERNEL32 import implementations
 *
 * Mechanism copied from host_vita_first_fault.c:
 *   host_arg(c, i)  fetch argument i (stdcall, esp points at the return address)
 *   host_ret(c, n)  pop the return address and clear n bytes of arguments (stdcall semantics)
 *
 * Principles:
 *   . implement for real whatever libc can do (time, memory, thread locks, files)
 *   . things Android has no concept of return a value that lets the game continue
 *   . never silently return 0 pretending success -- unless 0 is the correct return value
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
/* low-4GB malloc -- the guest's uint32_t pointers cannot point high
 *
 * ★★ 2026-11-07: the root cause of the on-device crash is right here.
 *
 * It used to hard-code `g_bump = 0x38000000` with a ceiling of `0x3C000000`. But in an
 * app process ART's dalvik-main heap occupies 0x08900000..0x38900000 -- and **0x38000000
 * is inside it**. Therefore:
 *   . the very first mmap(MAP_FIXED_NOREPLACE) fails
 *   . the failure branch does not advance g_bump ("to avoid leaving holes"), so from then
 *     on **every** low_alloc fails the same way and returns 0
 *   . the caller gets NULL, and Lua's luaL_requiref crashes outright at 0xd555c128
 *
 * Child-process mode never had this problem: there is no ART there, and 0x38000000 is free.
 * That is the complete explanation for "works as a child process, always crashes in-process".
 *
 * The fix has two layers:
 *   1. all candidate bases avoid dalvik-main, preferring the vacuum between
 *      the image tail 0x3C900000 and the guest heap start 0x48000000
 *   2. on mmap failure **shift by 64KB and retry** (borrowing the proven approach from
 *      host_user32.c) instead of deadlocking in place; when the window is exhausted, move to the next candidate */
#define BUMP_WINDOW (32u << 20)          /* 32 MB per candidate */

static const uint32_t k_bump_cand[] = {
    0x3D000000u,   /* first choice: after the image at 0x3C900000, before the guest heap at 0x48000000 */
    0x44000000u,   /* second half of the same region */
    0x39000000u,   /* start of the hole; collides with steam at 0x3A000000 / user32 at 0x3B000000,
                    * but with NOREPLACE + shifting it hops past on its own */
    /* ★ 2026-11-10: the 0x90000000 / 0xC0000000 candidates were removed.
     * After the heap range was extended to 0xF0000000, those two addresses fall inside the guest heap.
     * MAP_FIXED_NOREPLACE would reject them, but that would make VirtualAlloc fail
     * while the heap still uses that land -- no need to create such an ambiguous state.
     * The only unoccupied safe region left in the low 4GB is the stretch below the image. */
    0x33000000u,   /* before the image; collides with dalvik-main (it finds a gap after hopping) */
    0x68000000u,   /* before the fake-address guard at 0x7D000000, a blank in the middle of the heap */
};
static int      g_cand_idx;
static uint32_t g_bump;          /* current cursor; 0 = not yet initialised */
static uint32_t g_bump_end;

static uint32_t low_alloc(uint32_t n)
{
    /* mmap requires both address and length to be page-aligned: it used to align to only
     * 16 bytes, so the second allocation mmap'd from 0x38000010, got EINVAL, and the whole bump allocator deadlocked from then on. */
    if (!n) n = 16u;
    n = (n + 4095u) & ~4095u;

    if (!g_bump) {
        g_bump = k_bump_cand[0];
        g_bump_end = g_bump + BUMP_WINDOW;
    }

    for (;;) {
        if (g_bump + n > g_bump_end) {
            /* current candidate window exhausted -> move to the next one */
            if (++g_cand_idx >= (int)(sizeof k_bump_cand / sizeof k_bump_cand[0])) {
                static int reported;
                if (!reported) { reported = 1;
                    fprintf(stderr, "[bump] ✗ all candidate bases exhausted (need %u bytes)\n", n); }
                return 0;
            }
            g_bump = k_bump_cand[g_cand_idx];
            g_bump_end = g_bump + BUMP_WINDOW;
            if (g_bump_end < g_bump) g_bump_end = 0xFFFFFFFFu;  /* overflow guard */
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

        /* Occupied: shift by 64 KB and retry. Never deadlock in place. */
        g_bump += 0x10000u;
    }
}

/* ---- time ---- */
static void h_GetSystemTimeAsFileTime(CPU *c) {
    uint32_t out = A(c, 0);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* FILETIME: 100ns units, epoch 1601-01-01 */
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

/* ---- memory ---- */
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
        /* MEM_DECOMMIT: keep the mapping, just invalidate the contents */
    }
    c->eax = 1; R(c, 12);
}
/* ==========================================================================
 * VirtualQuery -- real implementation (2026-11-07)
 *
 * It used to be a stub that always returned "MEM_FREE / 64KB". Consequence: when
 * guest_pe_load hit "fixed address allocation failed" it walked the range printing
 * "free", so that whole diagnostic output was **completely empty**.
 *
 * When .so mode failed on device (0x3c000000 could not be allocated), it was exactly this
 * output that would have told us who occupied it -- the stub voided it, throwing away the most
 *
 * It now answers by reading /proc/self/maps. Semantics that matter:
 *   . RegionSize must make the caller's cursor advance monotonically, or that for loop spins forever
 *   . hit a mapping -> State=MEM_COMMIT, Type distinguishes file-backed from anonymous
 *   . miss          -> State=MEM_FREE, RegionSize up to the next mapping start
 * ========================================================================== */
#define VQ_MEM_COMMIT   0x00001000u
#define VQ_MEM_FREE     0x00010000u
#define VQ_MEM_PRIVATE  0x00020000u
#define VQ_MEM_IMAGE    0x01000000u
#define VQ_MEM_MAPPED   0x00040000u

typedef struct { uint32_t lo, hi; int is_file; } vq_rng;

/* Re-reading maps on every query is slow, but this path only runs a few times on failure, so it is worth it. */
int vq_scan_compat(uint32_t addr, uint32_t *base, uint32_t *size,
                   uint32_t *state, uint32_t *type)
{
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    uint32_t next_lo = 0xFFFFFFFFu;   /* start of the next mapping (used for MEM_FREE gaps) */
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
        /* Record the nearest mapping start after addr */
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
        /* next_lo is the nearest mapping start after addr; if none, use the top of the 4GB.
         * Note it cannot be written as 0x100000000ull -- truncating to uint32_t yields 0. */
        uint32_t end = (next_lo != 0xFFFFFFFFu) ? next_lo : 0xFFFFFFFFu;
        if (end <= addr) end = addr + 0x10000u;
        *base  = addr;
        *size  = end - addr;
        *state = VQ_MEM_FREE;
        *type  = 0;
        found  = 1;
    }
    /* Monotonic-progress guarantee: RegionSize must never be 0, or the caller loops forever */
    if (*size == 0) *size = 0x1000u;
    return found;
}

/* Fallback for windows.h's VirtualQuery (real implementation).
 *
 * When fixed-base allocation fails guest_pe_load calls VirtualQuery to walk the range
 * and print the occupants. This is the only evidence for diagnosing .so address conflicts.
 *
 * Returns 1 = MEMORY_BASIC_INFORMATION filled in successfully.
 * Note RegionSize must be non-zero and advance the cursor, or the caller loops forever.
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

/* ---- process / thread ---- */
static void h_GetCurrentProcess(CPU *c) { c->eax = 0xFFFFFFFFu; R(c, 0); }
static void h_GetCurrentProcessId(CPU *c) { c->eax = (uint32_t)getpid(); R(c, 0); }
static void h_GetCurrentThreadId(CPU *c) { c->eax = (uint32_t)(uintptr_t)pthread_self(); R(c, 0); }
static void h_GetCurrentProcessorNumber(CPU *c) { c->eax = 0; R(c, 0); }
static void h_IsDebuggerPresent(CPU *c) { c->eax = 0; R(c, 0); }
static void h_IsProcessorFeaturePresent(CPU *c) {
    uint32_t f = A(c, 0);
    /* PF_XMMI64_INSTRUCTIONS_AVAILABLE(10) / report the whole SSE family as supported */
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
/* ★★ 2026-11-10: unified host sleep throttle.
 *
 * Device evidence:
 *     [slow] 1004 ms  KERNEL32.dll!Sleep   x 1192 times
 * and the frame-time spike was exactly 1017 ms (each = 1004ms sleep + 17ms frame).
 *
 * We execute guest code **single-threaded and serially**. During a sleep the game makes
 * no progress at all -- the parallel worker threads of real Windows are simply not running here.
 * So a long sleep is pure waste of wall-clock time.
 *
 * Rules:
 *   . <= cap (default 16ms) is kept as-is -- used for frame-rate throttling;
 *     truncating it would make the game spin and burn CPU.
 *   . >  cap is truncated to 1ms (cap=0 means do not sleep at all). Long sleeps are almost
 *     always "waiting for some async task", and since those tasks are not running, the wait never ends.
 * Keeping 1ms still yields the scheduler, avoiding mutual starvation with ART / audio / input threads.
 *
 * Override with SLEEP_CAP_MS=<n>. */
static void host_sleep_ms(uint32_t ms)
{
    static int cap = -2;
    if (cap == -2) {
        const char *e = getenv("SLEEP_CAP_MS");
        cap = e ? atoi(e) : 16;
        if (cap < 0) cap = 0;
    }
    if (!ms || ms == 0xFFFFFFFFu) return;   /* 0 = do not sleep; INFINITE = do not wait forever */
    uint32_t eff = ms;
    if ((int)ms > cap) eff = (uint32_t)(cap > 0 ? 1 : 0);
    if (eff) usleep(eff * 1000u);
}

/* `KERNEL32.dll!Sleep` -- the number one frame-rate killer; see the host_sleep_ms comment. */
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
        /* 32-bit SYSTEM_INFO is 36 bytes; field offsets are
         *   0 arch/oemid, 4 page size, 8 min addr, 12 max addr,
         *   16 processor mask, 20 processor count, 24 processor type,
         *   28 allocation granularity, 32 level, 34 revision.
         * This used to memset 64 bytes while the guest had only allocated `sub esp,0x38`
         * (56 bytes) of local buffer, so the extra 8 bytes landed exactly on the saved
         * [ebp] slot and the return address -- that is the root cause of the 2026-11-02 stack imbalance:
         * sub_005e77d0's pop ebp got 0, and every later [ebp-...] access collapsed. */
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
    /* The game uses it for audio/background threads. Return a fake handle so it continues. */
    (void)A(c,0); (void)A(c,1); (void)A(c,2); (void)A(c,3); (void)A(c,4); (void)A(c,5);
    static uint32_t fake = 0x1000;
    c->eax = fake++; R(c, 24);
}

/* ---- synchronisation ---- */
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

/* ---- dynamic exports (the targets GetProcAddress returns) ----
 *
 * The game does LoadLibrary("kernelbase.dll") then GetProcAddress for optional APIs.
 * The return value must be an address the guest can call. repentogxm uses "dynamic tokens" for this:
 * the address is neither in the PE image nor an IAT slot; guest_host_dynamic intercepts and dispatches it.
 *
 * A token space starting at 0x7E000000 is carved out here, 4 bytes per entry. */
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
    /* Forward to the ordinary VirtualAlloc (different argument layout; the first two suffice) */
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

/* ---- WINMM: GLFW fetches these on demand via GetProcAddress ----
 * GLFW's win32 init does LoadLibraryA("winmm.dll") then GetProcAddress one by one.
 * When only the DYN table is consulted (not the regular import table) these returned 0, so GLFW
 * the whole glfwInit() returned false -- which is exactly what the device log shows.
 * "[ASSERT] - GLFW failed to initialize"。
 *
 * We do implement timeGetTime (h_timeGetTime in host_win_sys.c), but that path
 * is IAT dispatch and GetProcAddress cannot reach it. So a dynamic-export copy is added here. */
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

/* JOYERR_UNPLUGGED = 167: honestly report "no pad plugged in"; GLFW skips that device. */
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
/* JOYERR_NOCANDO = 165: not even the capabilities can be obtained */
static void dyn_joyGetDevCapsW(CPU *c)
{
    (void)A(c, 0); (void)A(c, 1); (void)A(c, 2);
    c->eax = 165u;
    R(c, 12);
}

/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter)
 *   stdcall, 5 arguments = 20 bytes, returns HRESULT.
 *
 * ★ It must return S_OK with a usable interface object; it must not fail.
 *
 * The reason is the game's assumption: after the call it **unconditionally** sets 0xc007de to 1,
 * sub_0059d3b0 and uses it as a pointer:
 *      eax = [0x803010]; ecx = [eax]; call [ecx+0x10]   // EnumDevices
 * failure -> ppvOut stays NULL -> reading [0] crashes (fault_addr=0x00000000).
 *
 * On device DINPUT8.dll is a system component, so DirectInput8Create always succeeds; with no pad
 * EnumDevices simply enumerates 0 devices. Our fake object reproduces exactly that state. */
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
    /* token space layout: 0x7D = GL, 0x7E = kernel32 dynamic exports,
     * 0x7F00/0x7F01 = DirectInput COM methods, 0x7F02 = XInput. None overlap. */
    if (host_gl_dynamic && host_gl_dynamic(c, addr)) return 1;
    if (host_dinput_dynamic && host_dinput_dynamic(c, addr)) return 1;
    if (host_xinput_dynamic && host_xinput_dynamic(c, addr)) return 1;

    if (addr < DYN_BASE) return 0;
    uint32_t i = (addr - DYN_BASE) / 4u;
    if (i >= (uint32_t)NDYN || (addr - DYN_BASE) % 4u) return 0;
    DYN[i].fn(c);
    return 1;
}

/* ---- modules / libraries ---- */
/* ★ Must return the relocated base. GUEST_IMAGE_BASE is 0x3c000000,
 * not 0x400000 from the PE header -- the image has already been relocated by image.py. */
static void h_GetModuleHandleA(CPU *c) { (void)A(c,0); c->eax = GUEST_IMAGE_BASE; R(c, 4); }
static void h_GetModuleHandleW(CPU *c) { (void)A(c,0); c->eax = GUEST_IMAGE_BASE; R(c, 4); }
/* Name resolution provided by the GL backend (host_gl4es.c). Weak symbol: 0 when no GL backend. */
extern uint32_t host_gl_proc_resolve(const char *name) __attribute__((weak));
/* XInput dynamic exports (host_xinput.c). Weak symbol: skipped when not compiled in. */
extern int host_xinput_lookup(const char *name, uint32_t *out_addr) __attribute__((weak));
extern int host_xinput_lookup_ordinal(uint32_t ordinal, uint32_t *out_addr) __attribute__((weak));

static void h_GetProcAddress(CPU *c) {
    uint32_t mod = A(c, 0), nm = A(c, 1);

    /* ★ 2026-11-06: ordinal form. The game fetches XInputGetStateEx using
     *   GetProcAddress(hXInput, (LPCSTR)100)
     * a "pointer" whose high 16 bits are 0 -- that is not an address but an ordinal (Win32 docs).
     * The old code strcmp'd it as a char*, reading address 100 outright -> segfault. */
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

    /* XInput: after LoadLibraryA succeeds the game fetches these exports by name. */
    if (!fn && name && host_xinput_lookup && host_xinput_lookup(name, &out)) {
        fprintf(stderr, "[host] GetProcAddress(%s) -> XInput token %08x\n",
                name, out);
        c->eax = out; R(c, 8); return;
    }

    /* How the game fetches GL: LoadLibraryA("OPENGL32") -> GetProcAddress("gl*").
     * The GL backend must be consulted here too, or glGetString returns 0 and the game exits(1) outright.
     * Intercept only names starting with gl or wgl -- otherwise unknown kernel32 names
     * would also be thrown into the GL table and produce noise. */
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
/* LoadLibraryA must return non-NULL **only for modules actually provided**.
 *
 * This used to return GUEST_IMAGE_BASE for any name, so the game's
 * LoadLibraryA("nonexistent.dll") got non-NULL -> decided the API was available ->
 * GetProcAddress got 0 -> stored it in a function pointer -> called 0 outright.
 * Measured crash at fault_addr=0x64 (NULL used as a struct).
 *
 * ★ 2026-11-06: XInput is now implemented in host_xinput.c (token space 0x7F020000),
 * so xinput1_4 / xinput1_3 / xinput9_1_0 must **be let through**.
 * That is also the only way out of the "DInput only degraded path". */
static const char *const s_provided_modules[] = {
    "kernel32", "kernelbase", "ntdll",
    "user32", "gdi32", "advapi32", "shell32", "ole32", "winmm",
    "opengl32", "dbghelp", "bcrypt", "dinput8",
    /* ★ 2026-11-06: XInput is the game's **primary** controller path. Without letting it through here,
     * the game forever takes the DInput degraded branch (the one visible in the log as
     * "Gamepad_init couldn't load XInput1_4.dll"）。
     * All three names are needed: 1.4 / 1.3 / bin\XInput1_3.dll. */
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
    /* Take the basename part (strip path and .dll/.DLL suffix) */
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
    /* we implement most of the api-ms-win-crt-* family */
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

/* ---- files / paths ---- */
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
 * FindFirstFileW / FindNextFileW / FindClose -- real implementation (2026-11-05)
 *
 * Before this they were always-failing stubs, annotated "measured 0 calls, does not affect startup".
 * That conclusion held for **startup** but not for **mod loading**:
 * ModManager enumerates the mods/ directory using exactly these three functions; with the stub failing,
 * the list is always empty -- seen as nothing at all after `begin list mods` in the log,
 * so user-installed mods have no effect whatsoever.
 *
 * WIN32_FIND_DATAW (x86 32-bit) layout, 592 bytes total:
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
 * Wildcards: a trailing `*` or `*.*` is stripped to get the directory; anything else is a plain path.
 * Filtering supports only the two commonest forms: `*` (everything) and `*.ext` (by suffix).
 * That covers every actual use in the game (enumerating mods, reading *.xml).
 *
 * Encoding: UTF-16 -> UTF-8 handles only ASCII within the BMP; non-ASCII becomes '?'.
 * Our resource paths are all ASCII, which is enough; revisit if Chinese mod names appear.
 * ========================================================================== */

#define FIND_MAX 8
static DIR *g_find_dir[FIND_MAX];
static char g_find_pat[FIND_MAX][128];   /* filename filter pattern */
static char g_find_dirpath[FIND_MAX][512]; /* directory path, needed by FindNext */
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

/* Fill one WIN32_FIND_DATAW. Returns 1 when valid. */
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

/* Pattern matching: only "*" and "*.ext" are supported */
static int name_matches(const char *pat, const char *name)
{
    if (!pat[0] || strcmp(pat, "*") == 0 || strcmp(pat, "*.*") == 0) return 1;
    if (pat[0] == '*' && pat[1] == '.') {
        size_t nl = strlen(name), pl = strlen(pat + 1);   /* ".ext" */
        if (nl >= pl && strcasecmp(name + nl - pl, pat + 1) == 0) return 1;
        /* Windows semantics: `*.ext` does not match files without an extension */
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

    /* Split out the directory and the pattern */
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

    /* Find the first match */
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
        /* handle = slot + 1 (both 0 and INVALID_HANDLE_VALUE must be avoided) */
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

    /* The directory path is not saved in FindFirst and cannot be recovered from DIR* -- here we rely on
     * readdir's d_name + stat needing the directory. To keep it simple, FindFirst already recorded
     * the directory in the extension table. See g_find_dirpath. */
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

/* ---- heap ---- */
static void h_GlobalAlloc(CPU *c) { c->eax = low_alloc(A(c, 1)); R(c, 8); }
static void h_GlobalLock(CPU *c) { c->eax = A(c, 0); R(c, 4); }
static void h_GlobalUnlock(CPU *c) { (void)A(c,0); c->eax = 1; R(c, 4); }

/* ---- error codes ---- */
static uint32_t g_last_error = 0;
static void h_GetLastError(CPU *c) { c->eax = g_last_error; R(c, 0); }
static void h_SetLastError(CPU *c) { g_last_error = A(c, 0); R(c, 4); }

/* ---- NUMA (single-node machine) ---- */
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

/* ---- CreateFileA: returns an invalid handle for now (Android filesystem comes later) ---- */
static void h_CreateFileA(CPU *c) {
    uint32_t p = A(c, 0);
    fprintf(stderr, "[host] CreateFileA(%s) not implemented\n",
            p ? (const char *)(uintptr_t)p : "?");
    c->eax = 0xFFFFFFFFu; R(c, 28);
}


/* ---- CRT static initialiser walk ----
 *
 * These two must be real: they walk the function-pointer array in the .CRT$XCU segment and call each one.
 * A fake stub leaves every C++ global object constructor unrun, and a crash is then inevitable.
 *
 * Implemented following the same-named function in host_win32.c, including the key detail:
 * the translated RET must be able to pop a return address off the guest stack even when control flow
 * returns to the host shim through the C call stack.
 */
static void cdecl_ret(CPU *c) { (void)gpop(c); }

static int call_guest_init(CPU *__restrict c, uint32_t target)
{
    uint32_t saved = c->esp;
    gpush(c, 0xFFF1A11EU);          /* fake return address pushed onto the guest stack */
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

/* _set_app_type(int) -- cdecl, arguments cleared by the caller.
 * See the ABI notes in host_win32.c: MSVC merges three calls into one add esp,0xc,
 * so arguments must absolutely not be cleared stdcall-style here. */
static void h__set_app_type(CPU *c) { c->eax = 0; cdecl_ret(c); }

/* ================================================================ dispatch table */
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
