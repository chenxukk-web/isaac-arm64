/* Android CRT layer -- implementation of the MSVC runtime imports
 *
 *
 * All arguments are on the guest stack (x86 cdecl); return values go in eax / xmm0 / edx:eax.
 * Allocations must land in the low 4GB: guest pointers are uint32_t.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>

#include "guest.h"

/* Heap history ring (for diagnostics): records the block address and size of the
 * last 512 malloc/free calls, so heap_hist_dump(target) can tell who gave/freed a block. */
#define HIST_N 512
static struct { uintptr_t p; size_t sz; int is_free; } g_hist[HIST_N];
static unsigned g_hist_n;
static void hist_rec(uintptr_t p, size_t sz, int is_free)
{
    g_hist[g_hist_n % HIST_N].p = p;
    g_hist[g_hist_n % HIST_N].sz = sz;
    g_hist[g_hist_n % HIST_N].is_free = is_free;
    g_hist_n++;
}
void heap_hist_dump(uintptr_t target)
{
    fprintf(stderr, "[heap] history of address %08lx (hits among last %u ops):\n",
            (unsigned long)target, g_hist_n);
    unsigned start = g_hist_n > HIST_N ? g_hist_n - HIST_N : 0;
    int found = 0;
    for (unsigned k = start; k < g_hist_n; k++) {
        uintptr_t p = g_hist[k % HIST_N].p;
        size_t sz = g_hist[k % HIST_N].sz;
        if (target >= p && target < p + sz) {
            fprintf(stderr, "[heap]   #%u %s base=%08lx size=%zu (target offset +0x%lx)\n",
                    k, g_hist[k % HIST_N].is_free ? "FREE" : "ALLOC",
                    (unsigned long)p, sz, (unsigned long)(target - p));
            found++;
        }
    }
    if (!found) fprintf(stderr, "[heap]   no hit (block did not come from this allocator)\n");
}
#include "host_path.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cdecl_ret(CPU *c) { (void)gpop(c); }
static char *G(uint32_t a) { return (char *)(uintptr_t)a; }


/* ============================================================================
 * Write-protection watch (for hunting "object header overwritten")
 *
 * Scenario: the header of object [0x652b2010] in the resource cache was overwritten by
 * a glyph array, yet three guards prove it was never freed, never double allocated and
 * never clobbered by VirtualAlloc, leaving only "someone writes straight into this memory".
 *
 * Approach: mark this 4KB page read-only. After that **any** write (whether from
 * host memcpy/memset or from inlined st32/st8 in gen/) triggers SIGSEGV,
 * and pc points straight at the culprit.
 *
 * Timing:
 *   allocation hits the watch address -> allow writes first (object construction)
 *   next malloc                       -> set read-only (constructed, guard period begins)
 *   free touches that block           -> restore writable first (header needs marking)
 *
 * Usage: HEAP_WATCH=652b2000 ./isaac_android_test
 * ==========================================================================*/
uintptr_t g_watch_page;             /* page base; 0 = disabled (the signal handler needs it) */
static int       g_watch_pending;   /* 1 = allocated, arm protection at the next malloc */
int              g_watch_armed;     /* 1 = the page is currently read-only */

/* Non-fatal mode: after a hit, restore writability and keep running while
 * recording every distinct writer. The goal is to catch "who wrote a data
 * pointer into a table slot" -- that spot is usually a legitimate vector append. */
int g_watch_nonfatal;
#define WATCH_SEEN_N 64
uintptr_t g_watch_seen_pc[WATCH_SEEN_N];
uint32_t  g_watch_seen_val[WATCH_SEEN_N];
unsigned  g_watch_seen_n;

static void watch_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    if (getenv("HEAP_WATCH_NONFATAL")) g_watch_nonfatal = 1;
    const char *e = getenv("HEAP_WATCH");
    if (e && *e) {
        g_watch_page = (uintptr_t)strtoul(e, NULL, 16) & ~(uintptr_t)0xfff;
        fprintf(stderr, "[watch] watch page = %08lx (HEAP_WATCH=%s)\n",
                (unsigned long)g_watch_page, e);
    }
}

static unsigned long g_n_malloc, g_n_free, g_n_realloc, g_n_calloc, g_n_free_bad;
static int g_chain_corrupt;

/* ★★ 2026-11-11: UAF pointer registry.
 *
 * Crashes are timing sensitive (the same .so sometimes crashes and sometimes does not),
 * so "does it crash" cannot be the criterion. Instead, record every object that is freed
 * yet still in the resource table and replay them all at crash time. One crash shows all. */
#define UAF_LOG_N 64
uintptr_t g_uaf_log[UAF_LOG_N];
unsigned  g_uaf_log_n;
unsigned  g_uaf_log_overflow;

/* ==========================================================================
 * ★★ 2026-11-11: UAF suppression switch (decisive A/B experiment)
 *
 * Symptom: on the device, around "enter game", a batch of objects **still in
 * the resource table** was freed; std::terminate then hit at glDeleteProgram.
 * The UAF count is a steady 8, but crashing depends on timing -- a textbook
 * use-after-free: our heap instantly reuses the block, so the reader sees another object's data.
 *
 * Experiment: when <game>/keepres exists, objects **still in the resource
 * table** are leaked instead of really freed, leaving the memory untouched.
 *   · no crash    -> the root cause is this batch of UAFs; fix by deferring release / retaining refs
 *   · still crash -> unrelated to this batch; go back to the glDeleteProgram path itself
 *
 * Same idea as ISAAC_VITA_IMAGE_RETAIN in repentogxm:
 * it also keeps ImageManager from freeing sprite tables, holding them in the LRU for the game.
 *
 * Safety cap: retain at most 512 objects / 16 MB, then fall back to normal
 * freeing so the diagnostic switch itself cannot eat all the memory.
 * ========================================================================== */
static int      g_keepres = -1;         /* -1 not probed / 0 off / 1 on */
static unsigned g_keepres_n;            /* number of objects retained */
static size_t   g_keepres_bytes;        /* bytes retained */
static const size_t KEEPRES_MAX_BYTES = 16u << 20;

static int keepres_enabled(void)
{
    if (g_keepres >= 0) return g_keepres;

    /* ★★ 2026-11-11: **on by default**.
     *
     * Rationale and trade-offs:
     *   · 4 of 5 crashing runs correlate exactly with "the game requests glDeleteProgram";
     *     the UAF (freeing objects still in the resource table) is 8 times in **every** run.
     *   · With these objects retained (measured at 06:31) the same load stage (the 1.1s
     *     free stall at frame #101) never took that path and it ran to frame 1200 without crashing.
     *   · The measured cost is only **72 KB / 18 objects**, negligible.
     *   · The 512 objects / 16 MB cap rules out eating all the memory.
     *
     * How to disable: an existing <game>/no_keepres turns it off (for triage).
     * This keeps the default stable without losing reproducibility. */
    g_keepres = 1;
    static const char *const off_probe[] = {
        "/storage/emulated/0/Android/media/com.isaacport.launcher"
        "/files/game/no_keepres",
        "/data/user/0/com.isaacport.launcher/files/game/no_keepres",
        "no_keepres",
        NULL
    };
    for (int i = 0; off_probe[i]; i++) {
        /* ★ Use fopen, not access(): on Android FUSE, access reports false failures. */
        FILE *fp = fopen(off_probe[i], "rb");
        if (!fp) continue;
        fclose(fp);
        g_keepres = 0;
        break;
    }
    fprintf(stderr,
        "[keepres] UAF suppression = %s (default on; cap 512 objects/16MB)\n",
        g_keepres ? "on" : "off (found no_keepres)");
    fflush(NULL);
    return g_keepres;
}
int res_table_contains(uintptr_t p);   /* defined below */
void res_table_audit(int deep);        /* defined below (RES_AUDIT=1) */
int  res_audit_on;
extern unsigned long g_last_malloc_size;
static void res_audit_tick(void);
static void res_map_probe(void);
static int g_res_free_n;

static void watch_arm_if_pending(void)
{
    if (!g_watch_pending || !g_watch_page) return;
    /* ★ Only arm the protection once the object really entered the resource table
     * (construction and insertion both done). The old "arm at the next malloc"
     * caught legal writes from construction (measured: import #840000 hit a write
     * to edi=652b2018), which is the object initializing itself, not an overwrite. */
    /* Object pointer = block base + 16 (block header). That is what the table stores. */
    if (!g_watch_nonfatal && !res_table_contains(g_watch_page + 16u))
        return;   /* not in the table yet = construction incomplete, do not arm */
    g_watch_pending = 0;
    if (mprotect((void *)g_watch_page, 4096, PROT_READ) == 0) {
        static int armed_n;
        g_watch_armed = 1;
        if (armed_n++ < 40)
            fprintf(stderr, "[watch] page %08lx set read-only (armed %d times)\n",
                    (unsigned long)g_watch_page, armed_n);
    }
}

static void watch_note_alloc(uintptr_t p)
{
    if (g_watch_page && p == g_watch_page && !g_watch_armed) {
        g_watch_pending = 1;
        fprintf(stderr, "[watch] block %08lx allocated (malloc #%lu, table hit=%d)\n",
                (unsigned long)p, g_n_malloc,
                res_table_contains(p + 16u) ? 1 : 0);   /* 16 = block header, object pointer = base + 16 */
        fflush(NULL);
    }
}

static int watch_is_armed_page(uintptr_t p)
{
    return g_watch_armed && g_watch_page &&
           p >= g_watch_page && p < g_watch_page + 4096u;
}

/* ---------------- low 4GB heap ----------------
 *
 * Lesson from 2026-11-02: this used to be a pure bump allocator; low_free only
 * read the block header, did "(void)sz;", and gave not a single byte back. After
 * resource unpacking the game began real malloc/free and the 624 MB cap filled up in half a minute -> operator new threw std::bad_alloc.
 *
 * It is now an allocator with size-class free lists: block sizes always round up
 * to a power of two; frees go back on their class list so the next same-size request reuses them.
 *
 * The block header is still 16 bytes and [ptr-4] is still the block base -- both
 * are conventions of MSVC aligned-free (see below) and must not change.
 *   p+0 .. p+7   size        total block size (power of two, page aligned)
 *   p+8 .. p+11  free next   (offset from HEAP_BASE + 1; 0 = end of list)
 *   p+12.. p+15  low 32 bits of the block base (MSVC checks read here)
 */
#define HEAP_BASE 0x48000000UL
/* ★★ 2026-11-10: heap cap raised from 0xB0000000 to 0xF0000000.
 *
 * Device crash chain:
 *   [heap] ✗ low 4GB heap exhausted: bump used 1664.0 MB / cap 1664.0 MB
 *   [cxx]  type[0] = ".?AVbad_alloc@std@@"
 *   [SEGV] abort
 *   meanwhile [texstat] counted 940 textures, 320 of them all zero
 *
 * Both are **the same root cause**: not enough address space.
 *   · old range 0x48000000-0xB0000000 = 1.66 GB
 *   · 384 MB of it is held by ART dalvik-free (already skipped)
 *   · about 1.28 GB really usable, while Isaac needs ~1.25 GB live
 *   · once exceeded: malloc returns NULL -> the decoder abandons its write -> textures all zero
 *     -> finally throws bad_alloc -> abort
 *
 * A PC has 4-8 GB, so this path never surfaced there.
 *
 * New cap 0xF0000000 = 2.62 GB (minus ART's 384 MB ≈ 2.2 GB), which leaves
 * enough headroom. The ART JIT cache lower down at 0xC8C00000-0xD0C00000 is
 * handled by the existing "skip ahead when mmap fails" logic. */
#define HEAP_END  0xF0000000UL

/* ★ Reserved range: [0x7D000000, 0x80000000) is the "fake address" space.
 *
 * GL function tokens start at 0x7D000000, dynamic export tokens at 0x7E000000,
 * XInput tokens at 0x7F000000 (see DYN_BASE in host_android.c and the gl tokens
 * in host_gl4es.c). These addresses are identifiers only and hold no data, but
 * they **must never really be mapped to the heap** -- otherwise a stray guest
 * dereference would read garbage, and crash diagnostics would misread a token as
 * a heap address. The heap jumps straight to 0x80000000 over this range. */
#define HEAP_RESERVED_LO 0x7D000000UL
#define HEAP_RESERVED_HI 0x80000000UL
#define HEAP_CLASSES 31            /* 2^12 .. 2^30 (power-of-two sizes up to 1 GB) */
#define HEAP_MIN_BLK 4096u

typedef struct heap_blk {
    size_t   size;
    uint32_t next_off;              /* used by the free lists only */
    uint32_t base_lo;
} heap_blk;

static uintptr_t g_heap_cur = HEAP_BASE;

/* Heap segment table: crash diagnostics use it to tell whether a bad address lies in a segment that should be mapped.
 *
 * ★★ 2026-11-10: switched from a linear array to a hash table. This was the worst performance bottleneck at the time.
 *
 * Old implementation:
 *     void heap_seg_del(uintptr_t base) {
 *         for (i = 0; i < 65536 && i < g_seg_n; i++) { ... }
 *     }
 * And g_seg_n is a **cumulative** insert counter (it never shrinks), so past 65536
 * every free ran a full 65536 comparisons.
 *
 * Device data: malloc=1318868 free=1092573
 *   -> 1.09M frees x 33K comparisons on average ≈ 35.7 billion memory accesses
 *
 * In the log this shows up as the repeated:
 *     [stall] single frame 422.2 ms  prev import=...!free  imports during=323230
 * Paused, the game allocates and frees heavily (menus, logging), so the stall is worst there.
 *
 * New implementation: open addressing + linear probing, O(1) amortized.
 *   g_segs[i].base == 0  empty slot
 *   g_segs[i].base == 1  tombstone (deleted, keep searching)
 *   size == 0            also a tombstone (kept for compatibility) */
#define HEAP_SEG_N 131072          /* power of two, easy to mask; twice the old 65536 */
#define HEAP_SEG_MASK (HEAP_SEG_N - 1u)
static struct { uintptr_t base; size_t size; } g_segs[HEAP_SEG_N];
static unsigned g_seg_used;     /* occupied slots (tombstones included), for the load factor */

/* ★★ 2026-11-10 incident record: never delete this function.
 *
 * When I reworked the hash table in the previous version I also deleted heap_seg_count(),
 * since this file no longer used it. But **runtime/guest.c still calls it** (it
 * prints the segment count in crash diagnostics), and guest.o is prebuilt so a
 * header change does not rebuild it. The linker reports no undefined symbols for
 * a -shared link, so: compiled -> linked -> only the device dlopen fails:
 *   UnsatisfiedLinkError: cannot locate symbol "heap_seg_count"
 * The game could not load at all.
 *
 * Lesson: before deleting any non-static function, search **all** directories
 * (including runtime/ and prebuilt .o files), not just the *.c files here.
 *
 * Semantics: returns the number of **currently live** heap segments (not the
 * cumulative insert count -- that one only ever grew in the old ring design). */
static unsigned g_seg_live;     /* number of live segments */

unsigned heap_seg_count(void) { return g_seg_live; }

static inline unsigned seg_hash(uintptr_t base)
{
    /* base is always page aligned, so the low 12 bits are 0; shift first, then mix.
     * Knuth multiplicative hashing, which behaves well for power-of-two capacities. */
    uint64_t h = (uint64_t)(base >> 12) * 2654435761ull;
    return (unsigned)(h >> 20) & HEAP_SEG_MASK;
}

void heap_seg_add(uintptr_t base, size_t size)
{
    if (!base) return;
    unsigned i = seg_hash(base);
    for (unsigned n = 0; n < HEAP_SEG_N; n++) {
        uintptr_t b = g_segs[i].base;
        if (b == base) {                 /* duplicate add: only update the size */
            g_segs[i].size = size;
            return;
        }
        if (b == 0 || b == 1) {
            if (b == 0) g_seg_used++;
            g_segs[i].base = base;
            g_segs[i].size = size;
            g_seg_live++;
            return;
        }
        i = (i + 1) & HEAP_SEG_MASK;
    }
    /* Table full: drop it. (diagnostic only, should not happen) */
}

void heap_seg_del(uintptr_t base)
{
    if (!base) return;
    unsigned i = seg_hash(base);
    for (unsigned n = 0; n < HEAP_SEG_N; n++) {
        uintptr_t b = g_segs[i].base;
        if (b == 0) return;              /* empty slot: not present */
        if (b == base) {
            g_segs[i].base = 1;          /* tombstone, keep searching */
            g_segs[i].size = 0;
            if (g_seg_live) g_seg_live--;
            return;
        }
        i = (i + 1) & HEAP_SEG_MASK;
    }
}

int heap_seg_lookup(uintptr_t addr, uintptr_t *base, size_t *size)
{
    unsigned i = seg_hash(addr);
    for (unsigned n = 0; n < HEAP_SEG_N; n++) {
        uintptr_t b = g_segs[i].base;
        if (b == 0) return 0;            /* empty slot: it cannot be further on */
        size_t sz = g_segs[i].size;
        if (b != 1 && sz && addr >= b && addr < b + sz) {
            if (base) *base = b;
            if (size) *size = sz;
            return 1;
        }
        i = (i + 1) & HEAP_SEG_MASK;
    }
    return 0;
}
/* Used by the audit: memory below the bump cursor is always already mmap'ed (every allocation maps). */
uintptr_t heap_bump_cursor(void) { return g_heap_cur; }
uint32_t g_free_caller;
static int g_double_free_reported;
static uint32_t  g_free_head[HEAP_CLASSES];
static size_t    g_heap_live;       /* bytes currently outstanding */
static size_t    g_heap_peak;

/* Returns the class c with 2^c >= sz; -1 when even the largest class is too small.
 *
 * ★ Root cause of 2026-11-03: there used to be no "too small" exit here, and the
 *   cap was HEAP_CLASSES-1 = 23 (8 MB). One fread needed 8,561,852 bytes, the
 *   allocator handed out an 8,388,608-byte block, fread still wrote 8,561,836
 *   bytes -- 173 KB past the end, smearing the two neighbouring 4 KB resource
 *   object blocks into font data: "2 of 488 cache elements have obj at a glyph array".
 *   Failing the caller with NULL beats ever returning a block smaller than requested. */
static int heap_class(size_t sz)
{
    int c = 12;
    while (c < HEAP_CLASSES - 1 && ((size_t)1 << c) < sz) c++;
    if (((size_t)1 << c) < sz) return -1;
    return c;
}

/* Count of consecutive bad nodes skipped on the free list: see the two "64 consecutive
 * bad nodes" comments below. It must be static -- the low-level `return low_malloc(n)`
 * gets tail-call optimized (jumping back to the function head), so a local is reset. */
static unsigned g_skip_streak;

static void *low_malloc(size_t n)
{
    g_n_malloc++;
    g_last_malloc_size = n;
    watch_init();
    {
        static int audit_init;
        if (!audit_init) {
            audit_init = 1;
            const char *e = getenv("RES_AUDIT");
            res_audit_on = e ? atoi(e) : 0;
            if (res_audit_on == 0 && e) res_audit_on = 1;
        }
        res_audit_tick();
        res_map_probe();
        {
            static int dump_at = -1;
            if (dump_at < 0) {
                const char *d = getenv("RES_MAP_DUMP");
                dump_at = d ? atoi(d) : 0;
            }
            extern void anm_dump_map(void);
            if (dump_at > 0 && g_n_malloc == (unsigned long)dump_at) anm_dump_map();
        }
    }
    if (!n) n = 1;
    /* Large-request probe: when bad_alloc hits we need to know how big the request was. */
    if (0 && n >= (size_t)(64u << 20)) {
        static int big_n;
        if (big_n++ < 10) {
            extern uint32_t g_malloc_caller;
            fprintf(stderr, "[heap] large malloc(%zu) ≈ %.1f MB  caller=%08x\n",
                    n, (double)n / 1048576.0, g_malloc_caller);
            fflush(NULL);
        }
    }
    size_t need = (n + sizeof(heap_blk) + HEAP_MIN_BLK - 1) & ~(size_t)(HEAP_MIN_BLK - 1);
    int c = heap_class(need);
    if (c < 0) {
        static int reported_big;
        if (!reported_big) {
            reported_big = 1;
            fprintf(stderr, "[heap] ✗ malloc(%zu) exceeds the largest size class"
                            " (2^%d); returning NULL instead of a short block\n",
                    n, HEAP_CLASSES - 1);
            fflush(NULL);
        }
        return NULL;
    }
    size_t bsz = (size_t)1 << c;
    if (bsz < need) return NULL;        /* belt and braces: never hand out a short block */

    uint32_t off = g_free_head[c];
    if (off) {
        uintptr_t p = HEAP_BASE + (size_t)(off - 1u);
        heap_blk *b = (heap_blk *)p;
        /* ★ Blocks on the free list must carry the "freed" marker (bit0). A block
         * without the marker yet linked in = list corruption: the same block was
         * linked twice, or something overwrote it. The result is one 4KB block
         * handed to two objects, each writing into it ("resource vtable becomes glyph data"). */
        if (!(b->size & 1)) {
            if (g_chain_corrupt < 8) {
                g_chain_corrupt++;
                fprintf(stderr,
                    "[heap] ★★ free-list corruption: class=%d off=%08x base=%08lx "
                    "size=%zu(no freed marker) request=%zu\n",
                    c, off, (unsigned long)p, b->size, n);
                fflush(NULL);
            }
            /* Skip this bad node and keep looking, to avoid a direct double allocation.
             *
             * ★ 2026-11-06: skipping **must be bounded**.
             * Measured: the free list can form a cycle (a bad node's next_off points back
             * at itself or loops), and this used to walk on unconditionally -> endless loop.
             * Sampled profile: 99.4% of CPU stuck in low_malloc, the game fully frozen,
             * with only 8 "free-list corruption" lines in the log (printing is capped).
             *
             * Now: 64 consecutive skips declare the whole list untrustworthy and clear it,
             * falling through to a fresh mmap block. Bad blocks leak, the game keeps running. */
            if (++g_skip_streak > 64u) {
                fprintf(stderr,
                    "[heap] ★ free-list class=%d had 64 consecutive bad nodes; clearing the list (avoiding an infinite loop)\n", c);
                g_free_head[c] = 0;
                g_skip_streak = 0;
                /* do not return; fall through to a fresh mmap block */
            } else {
                g_free_head[c] = b->next_off;
                return low_malloc(n);
            }
        }
        /* ★ A free-list block's size must be **exactly** 2^c for its class. A
         * mismatch means the header was overwritten or the block is on the wrong
         * list -- the same block goes to two objects and they overwrite each other. */
        if ((b->size & ~(size_t)1) != bsz) {
            static int szbad;
            if (szbad++ < 8) {
                fprintf(stderr,
                    "[heap] ★★ free-list size mismatch: class=%d base=%08lx "
                    "header size=%zu expected=%zu request=%zu\n",
                    c, (unsigned long)p, b->size & ~(size_t)1, bsz, n);
                fflush(NULL);
            }
            if (++g_skip_streak > 64u) {
                fprintf(stderr,
                    "[heap] ★ free-list class=%d had 64 consecutive size mismatches; clearing the list\n", c);
                g_free_head[c] = 0;
                g_skip_streak = 0;
            } else {
                g_free_head[c] = b->next_off;   /* skip this bad block */
                return low_malloc(n);
            }
        }
        g_skip_streak = 0;              /* good block found, reset the skip count */
        b->size &= ~(size_t)1;          /* clear the "freed" marker */
        hist_rec(p, b->size, 0);
        g_free_head[c] = b->next_off;
        b->base_lo = (uint32_t)p;
        g_heap_live += b->size;
        if (g_heap_live > g_heap_peak) g_heap_peak = g_heap_live;
        watch_note_alloc(p);
        return (void *)(p + sizeof(heap_blk));
    }

    /* ★★ 2026-11-10: skip the fake-address guard region (0x7D000000-0x80000000).
     * See the HEAP_RESERVED_LO comment. This sits before the exhaustion check,
     * else a cur parked at the region entry would look like running out. */
    if (g_heap_cur < HEAP_RESERVED_HI &&
        g_heap_cur + bsz > HEAP_RESERVED_LO) {
        fprintf(stderr, "[heap] skipping fake-address guard region 0x%08lx..0x%08lx\n",
                (unsigned long)HEAP_RESERVED_LO,
                (unsigned long)HEAP_RESERVED_HI);
        fflush(NULL);
        g_heap_cur = HEAP_RESERVED_HI;
    }

    if (g_heap_cur + bsz > HEAP_END) {
        static int reported;
        if (!reported) {
            reported = 1;
            fprintf(stderr,
                "[heap] ✗ low-4GB heap exhausted: bump used %.1f MB / cap %.1f MB,"
                " %.1f MB outside (peak %.1f MB), request %zu bytes\n"
                "[heap]   malloc=%lu free=%lu(bad pointers %lu) realloc=%lu calloc=%lu\n",
                (double)(g_heap_cur - HEAP_BASE) / 1048576.0,
                (double)(HEAP_END - HEAP_BASE) / 1048576.0,
                (double)g_heap_live / 1048576.0,
                (double)g_heap_peak / 1048576.0, n,
                g_n_malloc, g_n_free, g_n_free_bad, g_n_realloc, g_n_calloc);
            fflush(NULL);
        }
        return NULL;
    }
    void *p = mmap((void *)g_heap_cur, bsz, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    /* ★★ 2026-11-07: on mmap failure **skip** the occupied range instead of stalling.
     *
     * Device crash chain (native.log):
     *     [heap] ✗ mmap(0x88900000, 4096) failed, heap pointer stuck in place
     *     [cxx] _CxxThrowException  type[0] = ".?AVbad_alloc@std@@"
     *     [SEGV] sig=6  abort
     *
     * The cause is that ART already owns blocks in this address space:
     *     08900000-38900000  [anon:dalvik-main]   768 MB
     *     88900000-a0900000  [anon:dalvik-free]   384 MB  ← 0x88900000 is in here
     * and the guest heap range was hardcoded to 0x48000000-0xB0000000 (1.7 GB), so it collided.
     *
     * The old comment worried that "advancing leaves a hole". Note the difference:
     *   · hole = an allocation was recorded but never mapped (later objects SIGSEGV)
     *   · skip = this range is **not ours**, so no object can point into it
     * No pointer of ours can live in ART-owned pages, so skipping over them is safe.
     *
     * The bump allocator in host_android.c was already changed for the same reason;
     * this is the same fix: on failure move on 1 MB and retry, up to 512 MB total. */
    if (p != (void *)g_heap_cur) {
        static int reported_skip;
        uintptr_t base = g_heap_cur;
        int i;
        int done = 0;
        for (i = 0; i < 512; i++) {
            uintptr_t try_at = base + (uintptr_t)i * 0x100000u;
            if (try_at + bsz > HEAP_END) break;
            void *q = mmap((void *)try_at, bsz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                           -1, 0);
            if (q == (void *)try_at) {
                fprintf(stderr,
                    "[heap] ⚠ skipping occupied region 0x%08lx..0x%08lx; allocating at 0x%08lx instead"
                    " (%d MB)\n",
                    (unsigned long)base, (unsigned long)try_at,
                    (unsigned long)try_at, i);
                fflush(NULL);
                g_heap_cur = try_at;
                p = q;
                done = 1;
                break;
            }
        }
        if (!done) {
            static int reported;
            if (!reported) { reported = 1;
                fprintf(stderr, "[heap] ✗ mmap(%p, %zu) failed and no hole within 512 MB;"
                        "heap pointer left in place\n", (void *)g_heap_cur, bsz); }
            return NULL;
        }
        (void)reported_skip;
    }
    heap_blk *b = (heap_blk *)p;
    b->size = bsz;
    /* ★ MSVC aligned-free looks for the "allocation block base" at [ptr-4] and
     * checks (ptr - base - 4) <= 0x1f before freeing. stringbuf's destructor
     * (RVA 0x40f0a0) follows that convention:
     *     0xf0da  mov ecx,[eax-4]      ; ecx = base
     *     0xf0e0  sub eax,ecx
     *     0xf0e2  add eax,-4
     *     0xf0e5  cmp eax,0x1f
     *     0xf0e8  ja  _invalid_parameter_noinfo_noreturn
     * Our block header is 16 bytes, so the base sits at [r-4] and (r-p-4)=12 passes the check.
     * Leaving it unwritten reads 0, the check fails -> the game aborts. */
    b->base_lo = (uint32_t)(uintptr_t)p;
    /* Uninitialized-memory probe (HEAP_POISON=1):
     * every new block is filled with 0xCD. If the "wild pointer" at the crash
     * becomes 0xCDCDCDCD, an uninitialized field was read; if it stays a
     * concrete value, that value was really written (from a file or computed).
     * Debug only -- it memsets the whole block on every allocation, very costly. */
    {
        static int poison = -1;
        if (poison < 0) poison = getenv("HEAP_POISON") != NULL;
        if (poison) memset((void *)(p + sizeof(heap_blk)), 0xCD,
                           bsz - sizeof(heap_blk));
    }
    heap_seg_add((uintptr_t)p, bsz);
    hist_rec((uintptr_t)p, bsz, 0);
    g_heap_cur += bsz;
    g_heap_live += bsz;
    if (g_heap_live > g_heap_peak) g_heap_peak = g_heap_live;
    watch_note_alloc((uintptr_t)p);
    return (void *)((uintptr_t)p + sizeof(heap_blk));
}

/* Diagnostics: current state of the block holding an address. 0=not in heap 1=in use 2=free */
int heap_block_state(uintptr_t addr, size_t *sz_out)
{
    uintptr_t p = addr & ~(uintptr_t)(HEAP_MIN_BLK - 1);
    if (p < HEAP_BASE || p >= HEAP_END) return 0;
    heap_blk *b = (heap_blk *)p;
    size_t raw = b->size & ~(size_t)1;
    if (raw < HEAP_MIN_BLK || raw > (size_t)1 << (HEAP_CLASSES - 1)) return 0;
    if (sz_out) *sz_out = raw;
    return (b->size & 1) ? 2 : 1;
}

/* Check whether a pointer is the obj of some element in the global resource table (0x3c7e7d00).
 * The game uses it as a cache, so an element should only be freed after removal;
 * freeing one that is still in the table makes the next access a UAF. Returns 1 = in the table. */
int res_table_contains(uintptr_t p)
{
    /* ★★ 2026-11-11: two fatal problems both live here.
     *
     * 1. **Performance**: this function is called on **every free** (see heap_free),
     *    and it walks each of the 32 buckets. Measured on device ([stall]):
     *    a single "prev import=free" frame costs **4994 ms / 10858 ms** --
     *    free stalls for tens of seconds and drags loading and frame rate down.
     *    If some bucket's beg/end are garbage values (a huge range), one free
     *    would walk hundreds of millions of entries.
     *
     * 2. **Out-of-bounds reads**: beg/end come straight from guest memory with no
     *    bound check. Once garbage is read, it sweeps many unmapped addresses -> SEGV.
     *    The local log line `[SEGV] sig=11 fault_addr=0x307e7d00` comes from
     *    exactly this access (0x307e7d00 = 0x3c7e7d00 - 0x0c000000).
     *
     * Fix: cap every bucket at 1 MB (131072 entries) and bound the total scan.
     * A resource-table bucket cannot be 1 MB -- anything bigger means the bucket
     * structure is invalid, and skipping it is far safer than scanning garbage. */
    uint32_t tbl = ld32(0x3c7e7d00u);
    if (tbl < 0x10004u || tbl + 32u * 12u >= 0xC0000000u) return 0;

    enum { BUCKET_MAX_BYTES = 0x100000u, SCAN_BUDGET = 262144u };
    uint32_t budget = SCAN_BUDGET;

    for (unsigned b = 0; b < 32u; b++) {
        uint32_t beg = ld32(tbl + b * 12u);
        uint32_t end = ld32(tbl + b * 12u + 4u);
        if (beg == end || end < beg || end >= 0xC0000000u) continue;
        if (end - beg > (uint32_t)BUCKET_MAX_BYTES) continue;   /* ★ garbage bucket */
        for (uint32_t q = beg; q + 8u <= end; q += 8u) {
            if ((uintptr_t)ld32(q) == p) return 1;
            if (--budget == 0u) return 0;                        /* ★ total budget exhausted */
        }
    }
    return 0;
}

/* Walk AnmCache's map (MSVC std::map / red-black tree) and print every key.
 *
 * Node layout (MSVC 32-bit):
 *   +0x00 _Left   +0x04 _Parent   +0x08 _Right
 *   +0x0c _Color  +0x0d _Isnil
 *   +0x10 _Myval  = pair<const string, T>, string being a 0x18-byte SSO
 * Container layout: map+0 = _Myhead, map+4 = _Mysize.
 * The disassembly of 0x90ad (`cmp eax,[esi+0x10]`) confirms the key at +0x10. */
static void anm_dump_key(uint32_t node)
{
    uint32_t sp = node + 0x10u;          /* std::string */
    uint32_t len = ld32(sp + 0x10u);     /* _Mysize */
    uint32_t cap = ld32(sp + 0x14u);
    uint32_t data = (cap >= 0x10u) ? ld32(sp) : sp;
    if (len > 200u) { fprintf(stderr, "[anm]   <len=%u abnormal>\n", len); return; }
    char buf[220]; int k = 0;
    for (; k < (int)len && k < 210; k++) {
        uint8_t ch = ld8(data + (uint32_t)k);
        buf[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
    }
    buf[k] = 0;
    fprintf(stderr, "[anm]   %s\n", buf);
}

void anm_dump_map(void)
{
    uint32_t mapobj = 0x3c7fec30u;
    uint32_t head = ld32(mapobj);
    uint32_t size = ld32(mapobj + 4u);
    /* First dump the raw bytes of the first node to locate the key's real offset.
     * No guessing: MSVC's _Tree_node may carry alignment padding in 32-bit builds. */
    if (head) {
        uint32_t first = ld32(head);
        if (first && first != head) {
            fprintf(stderr, "[anm] node %08x raw bytes:\n", first);
            for (unsigned row = 0; row < 7; row++) {
                fprintf(stderr, "[anm]   +%02x:", row * 16u);
                for (unsigned col = 0; col < 16; col++) {
                    uint32_t a = first + row * 16u + col;
                    fprintf(stderr, " %02x", ld8(a));
                }
                fprintf(stderr, "  |");
                for (unsigned col = 0; col < 16; col++) {
                    uint8_t ch = ld8(first + row * 16u + col);
                    fprintf(stderr, "%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
                }
                fprintf(stderr, "|\n");
            }
        }
    }
    fprintf(stderr, "[anm] map size=%u head=%08x all keys follow:\n", size, head);
    if (!head) return;
    uint32_t node = ld32(head);           /* head->_Left */
    unsigned guard = 0;
    while (node && node != head && guard++ < 4096u) {
        if (ld8(node + 0xdu)) break;      /* _Isnil */
        anm_dump_key(node);
        /* in-order successor */
        uint32_t r = ld32(node + 8u);
        if (r != head) {
            node = r;
            while (ld32(node) != head) node = ld32(node);
        } else {
            uint32_t par = ld32(node + 4u);
            while (node == ld32(par + 8u)) { node = par; par = ld32(par + 4u); }
            node = par;
        }
    }
    fprintf(stderr, "[anm] traversed %u entries\n", guard);
    fflush(NULL);
}

static int g_uaf_reports;

/* ============================================================================
 * Resource-table consistency audit (RES_AUDIT=1)
 *
 * The question: were those 2 bad elements bad at insert time, or did they go bad later?
 * Criterion: obj must be readable and [obj+0] must be a vtable (0x30xxxxxx range).
 * 486 of 488 normal elements satisfy it; the two bad ones have int16 glyph data at [obj+0].
 * Trigger: whenever the element count changes (i.e. after each push_back), plus a
 * deep scan every 2048 mallocs.
 * ==========================================================================*/
static unsigned g_audit_last_count = 0xffffffffu;
static unsigned g_audit_tick;
static unsigned long g_audit_last_ok_malloc;   /* malloc# of the last fully clean table scan */
unsigned long g_audit_new_bad;                 /* number of new bad elements found by the audit */
unsigned long g_last_malloc_size;              /* size of the current malloc request */

static int audit_readable(uintptr_t p, size_t n)
{
    /* Image / read-only range. ★ 2026-11-07: this used to read `p + n <= 0x38000000u`,
     * but the previous condition already requires p >= 0x3c000000, and 0x3c000000 > 0x38000000,
     * so that test was **always false** and the audit never treated the image as readable.
     * Switched to the 0x3c900000 bound that probe uses. */
    if (p >= 0x3c000000u && p + n <= 0x3c900000u) return 1;   /* image / read-only range */
    if (p >= 0x40000000u && p + n <= 0x40400000u) return 1;   /* guest stack */
    /* Heap: memory below the bump cursor is always already mmap'ed, so skip the 65536-entry table */
    if (p >= HEAP_BASE && p + n <= heap_bump_cursor()) return 1;
    uintptr_t base; size_t sz;
    if (heap_seg_lookup(p, &base, &sz) && p + n <= base + sz) return 1;
    return 0;
}

static uint32_t g_audit_seen[64];
static unsigned g_audit_seen_n;

static void audit_report_slot(unsigned b, unsigned idx, uint32_t slot,
                              uint32_t obj, uint32_t aux, unsigned long nm)
{
    for (unsigned i = 0; i < g_audit_seen_n; i++)
        if (g_audit_seen[i] == slot) return;      /* report each slot only once */
    if (g_audit_seen_n < 64) g_audit_seen[g_audit_seen_n++] = slot;
    g_audit_new_bad++;
    uint32_t v0 = 0, v3c = 0;
    if (audit_readable(obj, 0x40)) {
        v0  = *(volatile uint32_t *)(uintptr_t)obj;
        v3c = *(volatile uint32_t *)(uintptr_t)(obj + 0x3c);
    }
    size_t bsz = 0;
    int st = heap_block_state(obj, &bsz);
    fprintf(stderr,
        "[res-audit] bad element bucket=%u index=%u slot=%08x obj=%08x aux=%08x "
        "[obj]=%08x [obj+3c]=%08x block state=%d block size=%zu "
        "malloc#=%lu (last clean sweep=%lu) request=%lu\n",
        b, idx, slot, obj, aux, v0, v3c, st, bsz, nm, g_audit_last_ok_malloc,
        g_last_malloc_size);
    fflush(NULL);
}

void res_table_audit(int deep)
{
    uint32_t tbl = ld32(0x3c7e7d00u);
    if (tbl < 0x10004u || tbl + 32u * 12u >= 0xC0000000u) return;
    unsigned total = 0;
    int any_bad = 0;
    for (unsigned b = 0; b < 32u; b++) {
        uint32_t beg = ld32(tbl + b * 12u);
        uint32_t end = ld32(tbl + b * 12u + 4u);
        if (beg == end || end < beg || end >= 0xC0000000u) continue;
        unsigned idx = 0;
        for (uint32_t q = beg; q + 8u <= end; q += 8u, idx++) {
            total++;
            if (!deep) continue;
            uint32_t obj = ld32(q), aux = ld32(q + 4u);
            if (!obj) continue;
            int bad = 0;
            if (!audit_readable(obj, 0x40)) bad = 1;
            else {
                uint32_t vt = *(volatile uint32_t *)(uintptr_t)obj;
                /* [obj]==0 is the normal transient of "object just malloc'ed, vtable not
                 * written yet" and is not bad; a real bad value is glyph data (nonzero, outside the vtable range). */
                if (vt != 0 && (vt < 0x3c000000u || vt >= 0x31000000u)) bad = 1;
            }
            if (bad) { any_bad = 1; audit_report_slot(b, idx, q, obj, aux, g_n_malloc); }
        }
    }
    if (deep && !any_bad && total) g_audit_last_ok_malloc = g_n_malloc;
    if (total != g_audit_last_count) {
        fprintf(stderr, "[res-audit] table element count %u -> %u (malloc#=%lu)\n",
                g_audit_last_count, total, g_n_malloc);
        fflush(NULL);
        g_audit_last_count = total;
    }
}

/* Called once per malloc. RES_AUDIT=1 scans deeply every time (window = one malloc);
 * RES_AUDIT=2 keeps the light mode (scan only on table size change or every 2048 calls). */
static void res_audit_tick(void)
{
    if (!res_audit_on) return;
    g_audit_tick++;
    if (res_audit_on >= 2) {
        uint32_t tbl = ld32(0x3c7e7d00u);
        if (tbl < 0x10004u || tbl + 32u * 12u >= 0xC0000000u) return;
        unsigned total = 0;
        for (unsigned b = 0; b < 32u; b++) {
            uint32_t beg = ld32(tbl + b * 12u);
            uint32_t end = ld32(tbl + b * 12u + 4u);
            if (beg == end || end < beg || end >= 0xC0000000u) continue;
            total += (end - beg) / 8u;
        }
        if (total != g_audit_last_count) { res_table_audit(1); return; }
        if ((g_audit_tick & 2047u) == 0) res_table_audit(1);
        return;
    }
    res_table_audit(1);
}

/* External-write audit: called when fread / memcpy writes a big chunk into guest memory.
 * If the written range covers an element's obj in the resource table, that is hard
 * evidence of "the loader stepped on cached objects" -- an order of magnitude sharper. */
extern unsigned long g_read_reports;
unsigned long g_read_reports;
void res_write_audit(uint32_t beg, uint32_t len, uint32_t caller, const char *who)
{
    if (!len) return;
    uint32_t end = beg + len;
    if (end < beg) return;                    /* overflow */
    uint32_t tbl = ld32(0x3c7e7d00u);
    if (tbl < 0x10004u || tbl + 32u * 12u >= 0xC0000000u) return;
    for (unsigned b = 0; b < 32u; b++) {
        uint32_t bb = ld32(tbl + b * 12u);
        uint32_t ee = ld32(tbl + b * 12u + 4u);
        if (bb == ee || ee < bb || ee >= 0xC0000000u) continue;
        unsigned idx = 0;
        for (uint32_t q = bb; q + 8u <= ee; q += 8u, idx++) {
            uint32_t obj = ld32(q);
            if (obj >= beg && obj < end) {
                if (g_read_reports < 32) {
                    g_read_reports++;
                    fprintf(stderr,
                        "[res-write] ★★ %s write to [%08x,%08x) overwrote a resource-table element "
                        "bucket=%u index=%u obj=%08x (slot=%08x) caller=%08x\n",
                        who, beg, end, b, idx, obj, q, caller);
                    fflush(NULL);
                }
            }
        }
    }
}

/* ============================================================================
 * AnmCache container probe (RES_MAP_PROBE=1)
 *
 * RVA 0x8fa0 is AnmCache's "add reference": it walks the 4-byte element container
 * 0xbfeda4..0xbfeda8 and does a find in the map (0xbfec30) for each element.
 * When find returns end() it prints "cannot add reference" (confirmed from the
 * disassembly: 0x90ba cmp esi,ecx / 0x90bc jne on success / else 0x90e6 prints).
 *
 * So watching whether the map size grows tells us "did loading insert anything".
 * Runtime address = 0x3c000000 + (RVA - 0x400000):
 *   map object 0x3c7fec30   [+0]=_Myhead  [+4]=_Mysize
 *   vector     0x3c7feda4 begin / 0x3c7feda8 end
 * ==========================================================================*/
static int g_map_probe = -1;
static unsigned g_map_last_size = 0xffffffffu;
static uint32_t g_map_last_begin, g_map_last_end;

static void res_map_probe(void)
{
    if (g_map_probe < 0) g_map_probe = getenv("RES_MAP_PROBE") != NULL;
    if (!g_map_probe) return;
    uint32_t mapobj = 0x3c7fec30u;
    uint32_t size   = ld32(mapobj + 4u);
    uint32_t beg    = ld32(0x3c7feda4u);
    uint32_t end    = ld32(0x3c7feda8u);
    if (size == g_map_last_size && beg == g_map_last_begin && end == g_map_last_end)
        return;
    g_map_last_size = size; g_map_last_begin = beg; g_map_last_end = end;
    fprintf(stderr,
        "[anm] map size=%u  head=%08x  vec=[%08x,%08x) count=%d  malloc#=%lu\n",
        size, ld32(mapobj), beg, end,
        (beg && end && end >= beg) ? (int)((end - beg) / 4u) : -1,
        g_n_malloc);
    fflush(NULL);
}

/* A thin wrapper: only consider arming protection **after** the allocation returns.
 * Arming at the low_malloc entry meant the block-header write right after it landed
 * on the page just made read-only -- the "writer" caught was the allocator itself
 * (low_malloc+0x160), a wasted round. */
static void *low_malloc_armed(size_t n)
{
    void *p = low_malloc(n);
    watch_arm_if_pending();
    return p;
}

static void low_free(void *ptr)
{
    if (!ptr) return;
    g_n_free++;
    /* ★★ 2026-11-11: this used to call res_table_contains on **every free**, and
     * the UAF block below called it again for the same ptr -- one guest table scanned twice.
     * Measured on device: a "prev import=free" frame stalls 4994 ms / 10858 ms, a main cause.
     *
     * Now: merged into a single lookup, and the table is **no longer scanned once the
     * diagnostic quota is spent**. Both reports only print the first 10 / 8 entries. */
    {
        uintptr_t up = (uintptr_t)ptr;
        int kp = keepres_enabled();
        int want_res = (g_res_free_n < 10);
        int want_uaf = (g_uaf_reports < 8) &&
                       ((up & 0xfff00000UL) >= 0x60000000UL);
        /* ★ keepres must be **independent of the diagnostic quota**, or it stops
         * working once that quota is spent (it used to sit inside want_res||want_uaf, capped at 10). */
        int want_keep = kp && (g_keepres_n < 512u) &&
                        ((up & 0xfff00000UL) >= 0x40000000UL);
        if (want_res || want_uaf || want_keep) {
            int in_res = res_table_contains(up);
            if (in_res && want_res) {
                g_res_free_n++;
                fprintf(stderr,
                    "[heap] freed an object still present in the resource table %p (occurrence %d) caller=%08x\n",
                    ptr, g_res_free_n, g_free_caller);
                fflush(NULL);
            }
            if (in_res && want_uaf) {
                if (g_uaf_log_n < (unsigned)UAF_LOG_N)
                    g_uaf_log[g_uaf_log_n++] = up;
                else
                    g_uaf_log_overflow++;
                g_uaf_reports++;
                fprintf(stderr,
                    "[heap] ★★ UAF warning: free(%p) but the pointer is still a resource-table obj;"
                    "caller=%08x\n", ptr, g_free_caller);
                fflush(NULL);
            }
            /* ★ A/B experiment: still in the resource table means do not really free (see keepres_enabled). */
            if (in_res && want_keep) {
                size_t sz = 0;
                heap_block_state((uintptr_t)ptr, &sz);  /* block size */
                if (g_keepres_n < 512u && g_keepres_bytes + sz <= KEEPRES_MAX_BYTES) {
                    g_keepres_n++;
                    g_keepres_bytes += sz;
                    static unsigned shown_kr;
                    if (shown_kr < 20u || (g_keepres_n % 50u) == 0u) {
                        shown_kr++;
                        fprintf(stderr,
                            "[keepres] retaining %p (%zu bytes, #%u, %.1f KB total)"
                            " - not freeing\n", ptr, sz, g_keepres_n,
                            (double)g_keepres_bytes / 1024.0);
                        fflush(NULL);
                    }
                    return;          /* ★ no free: memory and contents stay as they are */
                }
            }
        }
    }
    if (watch_is_armed_page((uintptr_t)ptr)) {
        fprintf(stderr, "[watch] free(%p) during the guard window - freeing it would trip on the header write; restoring writability first\n", ptr);
        mprotect((void *)g_watch_page, 4096, PROT_READ | PROT_WRITE);
        g_watch_armed = 0;
        fflush(NULL);
    }
    uintptr_t p = (uintptr_t)ptr - sizeof(heap_blk);
    if (p < HEAP_BASE || p >= HEAP_END) { g_n_free_bad++; return; }
    heap_blk *b = (heap_blk *)p;
    size_t raw0 = b->size & ~(size_t)1;
    if (raw0 < HEAP_MIN_BLK || raw0 > (size_t)1 << (HEAP_CLASSES - 1)) { g_n_free_bad++; return; }
    size_t raw = b->size;
    if (raw & 1) {
        /* ★ Double free. The old implementation (free was a no-op) hid this problem
         * completely; once blocks really go back on the free list the same block is
         * handed to two objects, so their data overwrites -- a container field becomes string content. */
        extern uint32_t g_free_caller;
        if (g_double_free_reported < 5) {
            g_double_free_reported++;
            fprintf(stderr,
                "[heap] ★ double free: block=%08lx size=%zu caller=%08x\n",
                (unsigned long)p, raw & ~(size_t)1, g_free_caller);
            fflush(NULL);
        }
        return;
    }
    raw &= ~(size_t)1;
    int c = heap_class(raw);
    if (c < 0) { g_n_free_bad++; return; }   /* bad block size, do not link it */
    hist_rec(p, raw, 1);
    /* deadbeef probe (off by default): fill a freed block with 0xA5 to detect UAF.
     * It memsets the whole block on every free, very costly; enable it when chasing UAFs.
     * This round already used it to rule UAFs out, so it stays but defaults off. */
#if defined(HEAP_DEADBEEF)
    memset((void *)(p + sizeof(heap_blk)), 0xA5, raw - sizeof(heap_blk));
#endif
    b->size = raw | 1;                  /* mark as "freed" */
    b->next_off = g_free_head[c];
    g_free_head[c] = (uint32_t)(p - HEAP_BASE) + 1u;
    if (g_heap_live >= raw) g_heap_live -= raw;
}

/* ---------------- memory / strings ---------------- */
static void h_memcpy(CPU *c) {
    uint32_t d = A(c,0), s2 = A(c,1), n = A(c,2);
    if (n >= 0x01000000u) {
        fprintf(stderr, "[memcpy] ★ huge copy dst=%08x src=%08x n=%u(0x%x) caller=%08x\n",
                d, s2, n, n, ld32(c->esp));
        fflush(NULL);
    }
    if (d && s2 && n) memcpy(G(d), G(s2), n);
    c->eax = d; cdecl_ret(c);
}
static void h_memmove(CPU *c) {
    uint32_t d = A(c,0), s = A(c,1), n = A(c,2);
    if (d && s && n) memmove(G(d), G(s), n);
    c->eax = d; cdecl_ret(c);
}
static void h_memset(CPU *c) {
    uint32_t d = A(c,0), v = A(c,1), n = A(c,2);
    /* A huge memset almost always means a bad length field (measured 0x0c000000 = 201 MB,
     * writing from 0x802bc010 must run into unmapped pages). Print the caller for tracing. */
    if (n >= 0x01000000u) {
        fprintf(stderr, "[memset] ★ huge fill dst=%08x val=%u n=%u(0x%x) caller=%08x\n",
                d, v, n, n, ld32(c->esp));
        /* Backtracking from the scene: this memset comes from a hash-table rehash (RVA 0x565800);
         * that function keeps this in [ebp-4] and the original bucket count in [ebp-0x28].
         * Digging out the container object and the frame variables shows which field went bad. */
        {
            uint32_t ebp = c->ebp;
            uint32_t thisp = ld32(ebp + (uint32_t)(int32_t)(-4));
            fprintf(stderr, "[memset]   rehash this=%08x  [ebp-0x28]=%08x\n",
                    thisp, ld32(ebp + (uint32_t)(int32_t)(-0x28)));
            if (thisp >= 0x10000u && thisp < 0xC0000000u) {
                fprintf(stderr,
                        "[memset]   container [+0]=%08x [+4]=%08x [+8]=%08x [+c]=%08x"
                        " [+10]=%08x [+14]=%08x\n",
                        ld32(thisp), ld32(thisp + 4), ld32(thisp + 8),
                        ld32(thisp + 12), ld32(thisp + 16), ld32(thisp + 20));
            }
            fprintf(stderr, "[memset]   stack frame ebp-0x30..ebp: ");
            for (int i = -0x30; i <= 0; i += 4)
                fprintf(stderr, "%08x ", ld32(ebp + (uint32_t)(int32_t)i));
            fprintf(stderr, "\n");
            /* Walk up the ebp chain and print every return address in the caller frames.
             * In translated code ebp is a standard frame pointer, so the chain stays valid. */
            if (thisp >= 0x10000u && thisp < 0xC0000000u) {
                char sbuf[128]; int k = 0;
                for (; k < 120; k++) {
                    uint8_t ch = ld8(thisp + (uint32_t)k);
                    if (!ch) break;
                    sbuf[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '.');
                }
                sbuf[k] = 0;
                fprintf(stderr, "[memset]   container head = \"%s\"  (len=%d)\n", sbuf, k);
                { extern void heap_hist_dump(uintptr_t); heap_hist_dump(thisp);
                  extern int heap_block_state(uintptr_t, size_t *);
                  size_t bs = 0; int stt = heap_block_state(thisp, &bs);
                  fprintf(stderr, "[memset]   block state = %s (size=%zu)\n",
                          stt == 0 ? "not on heap" : stt == 1 ? "in use" : "★ free (UAF!)", bs); }
            }
            fprintf(stderr, "[memset]   call chain: ");
            {
                uint32_t f = ebp;
                for (int depth = 0; depth < 10; depth++) {
                    if (f < 0x1000u || f + 8u >= 0xC0000000u) break;
                    uint32_t ret = ld32(f + 4u);
                    uint32_t nxt = ld32(f);
                    if (ret >= 0x3c000000u && ret < 0x3c800000u)
                        ret -= 0x3c000000u;         /* remove the guest base */
                    fprintf(stderr, "%08x ", ret);
                    if (nxt <= f || nxt >= 0xC0000000u) break;
                    f = nxt;
                }
            }
            fprintf(stderr, "\n");
        }
        fflush(NULL);
    }
    if (d && n) memset(G(d), (int)v, n);
    c->eax = d; cdecl_ret(c);
}
static void h_memchr(CPU *c) {
    uint32_t d = A(c,0), v = A(c,1), n = A(c,2);
    void *r = (d && n) ? memchr(G(d), (int)v, n) : NULL;
    c->eax = (uint32_t)(uintptr_t)r; cdecl_ret(c);
}
static void h_strchr(CPU *c) {
    uint32_t s = A(c,0), v = A(c,1);
    char *r = s ? strchr(G(s), (int)v) : NULL;
    c->eax = (uint32_t)(uintptr_t)r; cdecl_ret(c);
}
static void h_strstr(CPU *c) {
    uint32_t h = A(c,0), n = A(c,1);
    char *r = (h && n) ? strstr(G(h), G(n)) : NULL;
    c->eax = (uint32_t)(uintptr_t)r; cdecl_ret(c);
}
static void h_strpbrk(CPU *c) {
    uint32_t s = A(c,0), a = A(c,1);
    char *r = (s && a) ? strpbrk(G(s), G(a)) : NULL;
    c->eax = (uint32_t)(uintptr_t)r; cdecl_ret(c);
}
static void h_strncpy(CPU *c) {
    uint32_t d = A(c,0), s = A(c,1), n = A(c,2);
    /* anm2's spritesheet path building goes through here (n=260, see RVA 0x259d84).
     * With PNC_TRACE=1 it prints source and destination, to check the built path. */
    {
        static int on = -1;
        if (on < 0) on = getenv("PNC_TRACE") != NULL;
        if (on && n == 260u && s) {
            static int cnt;
            if (cnt++ < 6) {
                char sb[128] = {0};
                for (int i = 0; i < 120; i++) {
                    char ch = G(s)[i];
                    if (!ch) break;
                    sb[i] = (ch >= 0x20 && ch < 0x7f) ? ch : '?';
                }
                fprintf(stderr, "[pnc] strncpy(dst=%08x, \"%s\", 260)\n", d, sb);
                /* also dump the two vectors at 0x2599c0, to see the split result */
                extern void *g_cur_cpu_diag;
                if (g_cur_cpu_diag) {
                    CPU *k = (CPU *)g_cur_cpu_diag;
                    for (int vi = 0; vi < 2; vi++) {
                        uint32_t base = k->ebp - (vi ? 0x94u : 0x84u);
                        uint32_t b = *(uint32_t *)(uintptr_t)base;
                        uint32_t e = *(uint32_t *)(uintptr_t)(base + 4u);
                        fprintf(stderr, "[pnc]   vec%d begin=%08x end=%08x count=%d\n",
                                vi, b, e, (int)((e - b) / 24));
                        for (uint32_t q = b; q + 24 <= e && q >= 0x10000u &&
                             q < 0xC0000000u; q += 24) {
                            uint32_t sz = *(uint32_t *)(uintptr_t)(q + 0x10);
                            uint32_t c2 = *(uint32_t *)(uintptr_t)(q + 0x14);
                            uint32_t dt = (c2 >= 0x10u) ? *(uint32_t *)(uintptr_t)q : q;
                            char t[70] = {0};
                            if (sz < 64u && dt >= 0x10000u && dt < 0xC0000000u)
                                for (uint32_t z = 0; z < sz; z++)
                                    t[z] = (char)*(uint8_t *)(uintptr_t)(dt + z);
                            fprintf(stderr, "[pnc]     [%08x] \"%s\"\n", q, t);
                        }
                    }
                }
                fflush(NULL);
            }
        }
    }
    if (d && s && n) strncpy(G(d), G(s), n);
    c->eax = d; cdecl_ret(c);
}
static void h_strncmp(CPU *c) {
    uint32_t a = A(c,0), b = A(c,1), n = A(c,2);
    c->eax = (uint32_t)((a && b) ? strncmp(G(a), G(b), n) : 0);
    cdecl_ret(c);
}
static void h__strnicmp(CPU *c) {
    uint32_t a = A(c,0), b = A(c,1), n = A(c,2);
    c->eax = (uint32_t)((a && b) ? strncasecmp(G(a), G(b), n) : 0);
    cdecl_ret(c);
}
static void h__strdup(CPU *c) {
    uint32_t s = A(c,0);
    if (!s) { c->eax = 0; cdecl_ret(c); return; }
    size_t n = strlen(G(s)) + 1;
    char *d = (char *)low_malloc_armed(n);
    if (d) memcpy(d, G(s), n);
    c->eax = (uint32_t)(uintptr_t)d; cdecl_ret(c);
}
static void h_strcpy_s(CPU *c) {
    uint32_t d = A(c,0), dn = A(c,1), s = A(c,2);
    if (!d || !s) { c->eax = 22; cdecl_ret(c); return; }
    size_t n = strlen(G(s));
    if (n + 1 > dn) { c->eax = 34; cdecl_ret(c); return; }
    memcpy(G(d), G(s), n + 1);
    c->eax = 0; cdecl_ret(c);
}
static void h_strncpy_s(CPU *c) {
    uint32_t d = A(c,0), dn = A(c,1), s = A(c,2), n = A(c,3);
    if (!d || !s) { c->eax = 22; cdecl_ret(c); return; }
    size_t sl = strlen(G(s));
    size_t lim = (n == 0xFFFFFFFFu) ? sl : n;
    if (lim >= dn) { c->eax = 34; cdecl_ret(c); return; }
    size_t cp = sl < lim ? sl : lim;
    memcpy(G(d), G(s), cp);
    G(d)[cp] = 0;
    c->eax = 0; cdecl_ret(c);
}
static void h_strcat_s(CPU *c) {
    uint32_t d = A(c,0), dn = A(c,1), s = A(c,2);
    if (!d || !s) { c->eax = 22; cdecl_ret(c); return; }
    size_t dl = strlen(G(d)), sl = strlen(G(s));
    if (dl + sl + 1 > dn) { c->eax = 34; cdecl_ret(c); return; }
    memcpy(G(d) + dl, G(s), sl + 1);
    c->eax = 0; cdecl_ret(c);
}
#define CTYPE1(fn, expr) \
    static void h_##fn(CPU *c) { c->eax = (uint32_t)(expr); cdecl_ret(c); }
CTYPE1(isdigit, A(c,0) < 256u && isdigit((int)A(c,0)) != 0)
CTYPE1(isspace, A(c,0) < 256u && isspace((int)A(c,0)) != 0)
CTYPE1(ispunct, A(c,0) < 256u && ispunct((int)A(c,0)) != 0)
CTYPE1(iswspace, A(c,0) < 256u && isspace((int)A(c,0)) != 0)
static void h_tolower(CPU *c) { c->eax = (uint32_t)(A(c,0) < 256u ? tolower((int)A(c,0)) : A(c,0)); cdecl_ret(c); }
static void h_toupper(CPU *c) { c->eax = (uint32_t)(A(c,0) < 256u ? toupper((int)A(c,0)) : A(c,0)); cdecl_ret(c); }

/* ---------------- heap ---------------- */
/* malloc records its caller for UAF backtracking */
#define MALLOC_TRACE_N 65536
static struct { uint32_t ptr; uint32_t caller; uint32_t size; } g_mt[MALLOC_TRACE_N];
static unsigned g_mt_n;
extern uint32_t g_last_free_caller;

uint32_t g_malloc_caller;

static void h_malloc(CPU *c)
{
    uint32_t n = A(c, 0);
    g_malloc_caller = ld32(c->esp);      /* return address of this malloc */
    unsigned long before = g_audit_new_bad;
    void *p = low_malloc_armed(n);
    uint32_t caller = ld32(c->esp);
    if (g_audit_new_bad != before) {
        fprintf(stderr,
            "[res-audit] ★ bad element appeared after malloc(#%lu): "
            "size=%u(0x%x) caller=%08x ptr=%p\n",
            g_n_malloc, n, n, caller, p);
        fflush(NULL);
    }
    if (p) {
        g_mt[g_mt_n % MALLOC_TRACE_N].ptr = (uint32_t)(uintptr_t)p;
        g_mt[g_mt_n % MALLOC_TRACE_N].caller = caller;
        g_mt[g_mt_n % MALLOC_TRACE_N].size = n;
        g_mt_n++;
    }
    c->eax = (uint32_t)(uintptr_t)p;
    cdecl_ret(c);
}

/* Trace back which caller last malloc'ed a given address */
void malloc_trace_dump(uint32_t addr)
{
    unsigned lim = g_mt_n < MALLOC_TRACE_N ? g_mt_n : MALLOC_TRACE_N;
    int shown = 0;
    fprintf(stderr, "[heap] malloc history for %08x (newest first, max 4):\n", addr);
    for (unsigned i = 0; i < lim && shown < 4; i++) {
        unsigned k = (g_mt_n - 1 - i) % MALLOC_TRACE_N;
        if (g_mt[k].ptr == addr) {
            fprintf(stderr, "[heap]   malloc(%u) caller=%08x\n",
                    g_mt[k].size, g_mt[k].caller);
            shown++;
        }
    }
    if (!shown) fprintf(stderr, "[heap]   no record\n");
}
static void h_calloc(CPU *c) {
    g_n_calloc++;
    uint32_t n = A(c,0), s = A(c,1);
    uint64_t t = (uint64_t)n * s;
    if (t > 0x10000000u) { c->eax = 0; cdecl_ret(c); return; }
    void *p = low_malloc_armed((size_t)t);
    /* ★ calloc must zero explicitly.
     *
     * This used to say "mmap memory is already 0" -- true only for a bump allocator.
     * With size-class reuse the same block is handed out again and again, holding
     * whatever the previous user left. Measured: a calloc(1,28) hash-table object got
     * a former std::string buffer whose [+0] was 0x746e6f66 ("font"), fed in
     * to rehash as a bucket count -> prime table overrun -> an 805 MB memset.
     */
    if (p) memset(p, 0, (size_t)t);
    c->eax = (uint32_t)(uintptr_t)p;
    cdecl_ret(c);
}
static void h_realloc(CPU *c) {
    g_n_realloc++;
    uint32_t p = A(c,0), n = A(c,1);
    if (!p) { c->eax = (uint32_t)(uintptr_t)low_malloc(n); cdecl_ret(c); return; }
    void *np = low_malloc_armed(n);
    if (np) {
        /* usable size = total block size - block header; never use the block size as data length */
        size_t old = ((heap_blk *)(uintptr_t)(p - sizeof(heap_blk)))->size
                     - sizeof(heap_blk);
        memcpy(np, (void *)(uintptr_t)p, old < n ? old : n);
        low_free((void *)(uintptr_t)p);
    }
    c->eax = (uint32_t)(uintptr_t)np; cdecl_ret(c);
}
static void h_free(CPU *c) { g_free_caller = ld32(c->esp); low_free((void *)(uintptr_t)A(c,0)); cdecl_ret(c); }
static void h__set_new_mode(CPU *c) { c->eax = 0; cdecl_ret(c); }
static void h__callnewh(CPU *c) { c->eax = 0; cdecl_ret(c); }

/* ---------------- conversions ---------------- */
/* ---------------- qsort ----------------
 * The comparator is a guest function pointer (cdecl: int cmp(const void*, const void*)).
 * This is a real implementation, not a stub: every comparison calls back into the guest
 * comparator via guest_call. Heapsort is used -- in place, no recursion, O(n log n) worst
 * case; host qsort_r is avoided because its signature differs between glibc and bionic. */
static int qsort_guest_cmp(CPU *__restrict c, uint32_t fn,
                           uint32_t a, uint32_t b)
{
    uint32_t saved = c->esp;
    gpush(c, b);
    gpush(c, a);
    gpush(c, 0xFFF1A11EU);
    guest_call(c, fn);
    c->esp = saved;                 /* cdecl: the caller cleans the stack */
    return (int)c->eax;
}

static void qsort_swap(uint32_t a, uint32_t b, uint32_t size)
{
    uint8_t *pa = (uint8_t *)(uintptr_t)a, *pb = (uint8_t *)(uintptr_t)b;
    for (uint32_t i = 0; i < size; i++) {
        uint8_t t = pa[i]; pa[i] = pb[i]; pb[i] = t;
    }
}

static void qsort_sift(CPU *__restrict c, uint32_t fn, uint32_t base,
                       uint32_t root, uint32_t n, uint32_t size)
{
    for (;;) {
        if (c->fault) return;
        uint32_t child = root * 2u + 1u;
        if (child >= n) break;
        if (child + 1u < n &&
            qsort_guest_cmp(c, fn, base + (child + 1u) * size,
                            base + child * size) > 0)
            child++;
        if (qsort_guest_cmp(c, fn, base + child * size,
                            base + root * size) <= 0)
            break;
        qsort_swap(base + root * size, base + child * size, size);
        root = child;
    }
}

static void h_qsort(CPU *c)
{
    uint32_t base = A(c, 0), nmemb = A(c, 1), size = A(c, 2), fn = A(c, 3);
    if (!base || nmemb < 2u || !size || !fn) { cdecl_ret(c); return; }
    /* Argument sanity: bad arguments must not sweep across guest memory */
    if (size > 4096u || (uint64_t)nmemb * size > 0x40000000ull) {
        fprintf(stderr, "[crt] qsort bad arguments base=%08x n=%u size=%u\n",
                base, nmemb, size);
        fflush(NULL);
        cdecl_ret(c);
        return;
    }
    for (uint32_t i = nmemb / 2u; i > 0u; i--)
        qsort_sift(c, fn, base, i - 1u, nmemb, size);
    for (uint32_t end = nmemb - 1u; end > 0u; end--) {
        qsort_swap(base, base + end * size, size);
        qsort_sift(c, fn, base, 0u, end, size);
    }
    cdecl_ret(c);
}

static void h_atoi(CPU *c) { uint32_t s=A(c,0); c->eax = s?(uint32_t)atoi(G(s)):0; cdecl_ret(c); }
static void h_strtol(CPU *c) {
    uint32_t s=A(c,0), e=A(c,1), b=A(c,2);
    char *end = NULL;
    long v = s ? strtol(G(s), e ? &end : NULL, (int)b) : 0;
    if (e) st32(e, (uint32_t)(uintptr_t)end);
    c->eax = (uint32_t)v; cdecl_ret(c);
}
static void h_strtoull(CPU *c) {
    uint32_t s=A(c,0), e=A(c,1), b=A(c,2);
    char *end = NULL;
    unsigned long long v = s ? strtoull(G(s), e ? &end : NULL, (int)b) : 0;
    if (e) st32(e, (uint32_t)(uintptr_t)end);
    c->eax = (uint32_t)v; c->edx = (uint32_t)(v >> 32); cdecl_ret(c);
}
/* The following are defined later in the file; forward declarations first */
static void   setd(CPU *c, double v);
static void   setf(CPU *c, float v);
static double argd(CPU *c, int i);

static void h_atof(CPU *c) {
    uint32_t s = A(c,0);
    double v = s ? atof(G(s)) : 0.0;
    setd(c, v);                           /* MSVC x86: a double is returned in ST(0) */
    cdecl_ret(c);
}

/* ---------------- math ---------------- */
/* float arguments are on the guest stack (cdecl); the result goes to xmm0 */
static float argf(CPU *c, int i) { uint32_t b = A(c, i); float v; memcpy(&v,&b,4); return v; }

/* ★ A double argument takes 8 bytes on the x86 stack (low 4 bytes first) */
static double argd(CPU *c, int i)
{
    uint64_t lo = A(c, i), hi = A(c, i + 1);
    uint64_t raw = lo | (hi << 32);
    double v; memcpy(&v, &raw, 8);
    return v;
}

/* ★ MSVC x86 returns float/double in **x87 ST(0)**, not in xmm0.
 *   Writing the result to xmm0 makes the guest's `fstp` read the stale top of the ST
 *   stack -- measured symptom: after modf returned, fault_addr = 0x3ff00000, exactly
 *   the high 32 bits of double 1.0, i.e. a double bit pattern used as a pointer. */
static void setd(CPU *c, double v) { fpush(c, v); }
static void setf(CPU *c, float v)  { fpush(c, (double)v); }

/* SSE2 variants (_libm_sse2_*): arguments and result all in xmm0/xmm1 */
static double argd_sse(CPU *c, int n) { return c->x[n].d[0]; }
static void setd_sse(CPU *c, double v) { c->x[0].d[0] = v; }

static void h_ceil(CPU *c)  { setd(c, ceil(argd(c,0)));  cdecl_ret(c); }
static void h_floor(CPU *c) { setd(c, floor(argd(c,0))); cdecl_ret(c); }
static void h_roundf(CPU *c){ setf(c, roundf(argf(c,0))); cdecl_ret(c); }
static void h_copysignf(CPU *c) { setf(c, copysignf(argf(c,0), argf(c,1))); cdecl_ret(c); }
static void h_nextafterf(CPU *c) { setf(c, nextafterf(argf(c,0), argf(c,1))); cdecl_ret(c); }
static void h__libm_sse2_sin_precise(CPU *c) { setd_sse(c, sin(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_cos_precise(CPU *c) { setd_sse(c, cos(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_sqrt_precise(CPU *c) { setd_sse(c, sqrt(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_log_precise(CPU *c) { setd_sse(c, log(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_log10_precise(CPU *c) { setd_sse(c, log10(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_exp_precise(CPU *c) { setd_sse(c, exp(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_pow_precise(CPU *c) { setd_sse(c, pow(argd_sse(c,0), argd_sse(c,1))); cdecl_ret(c); }
static void h__libm_sse2_acos_precise(CPU *c) { setd_sse(c, acos(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_asin_precise(CPU *c) { setd_sse(c, asin(argd_sse(c,0))); cdecl_ret(c); }
static void h__libm_sse2_atan_precise(CPU *c) { setd_sse(c, atan(argd_sse(c,0))); cdecl_ret(c); }
static void h__CIatan2(CPU *c) { setd(c, atan2(argd(c,0), argd(c,2))); cdecl_ret(c); }
static void h__CIfmod(CPU *c) { setd(c, fmod(argd(c,0), argd(c,2))); cdecl_ret(c); }
static void h_ldexp(CPU *c) {
    uint32_t b = A(c, 2);              /* (double x, int exp): x occupies 2 slots */
    setd(c, ldexp(argd(c, 0), (int)b)); cdecl_ret(c);
}
static void h_modf(CPU *c) {
    /* double modf(double x, double *iptr) -- x takes 2 slots, iptr is in slot 3 */
    uint32_t ip = A(c, 2);
    double f, i;
    f = modf(argd(c, 0), &i);
    if (ip) { uint64_t b; memcpy(&b, &i, 8);
              st32(ip, (uint32_t)b); st32(ip + 4, (uint32_t)(b >> 32)); }
    setd(c, f); cdecl_ret(c);
}
static void h__fdclass(CPU *c) { c->eax = 0; cdecl_ret(c); }
static void h___setusermatherr(CPU *c) { c->eax = 0; cdecl_ret(c); }

/* ---------------- runtime misc ---------------- */
static uint32_t g_fmode = 0;
static void h__set_fmode(CPU *c) { g_fmode = A(c,0); c->eax = 0; cdecl_ret(c); }

/* ★ These functions hand the guest a pointer to process state. It must be a low
 * 4GB guest address, never the address of a host static -- in a 64-bit PIE those sit
 * at 0x5xxxxxxxxx, truncate to garbage as uint32_t, and crash the guest on access.
 * This is the third time this class of bug appeared in this debugging round (TIB, TLS block, here). */
static uint32_t g_status_blk = 0;
static uint32_t *status_slot(int which)
{
    if (!g_status_blk) {
        uint32_t p = (uint32_t)(uintptr_t)low_malloc_armed(64);
        if (p) memset((void *)(uintptr_t)p, 0, 64);
        g_status_blk = p;
    }
    if (!g_status_blk) return NULL;
    return (uint32_t *)(uintptr_t)(g_status_blk + (uint32_t)which * 4);
}
static void h___p__commode(CPU *c) {
    uint32_t *p = status_slot(0);
    c->eax = p ? (uint32_t)(uintptr_t)p : 0; cdecl_ret(c);
}
static void h__errno(CPU *c) {
    uint32_t *p = status_slot(1);
    c->eax = p ? (uint32_t)(uintptr_t)p : 0; cdecl_ret(c);
}
static void h__set_errno(CPU *c) {
    uint32_t *p = status_slot(1);
    if (p) *p = A(c,0);
    c->eax = 0; cdecl_ret(c);
}
static void h___p___argc(CPU *c) {
    uint32_t *p = status_slot(2);
    if (p) *p = 1;
    c->eax = p ? (uint32_t)(uintptr_t)p : 0; cdecl_ret(c);
}
static void h___p___argv(CPU *c) {
    uint32_t *p = status_slot(3);       /* pointer to the argv pointer */
    if (p && !*p) {
        uint32_t arr = (uint32_t)(uintptr_t)low_malloc_armed(16);
        if (arr) { st32(arr, 0); st32(arr + 4, 0); }
        *p = arr;
    }
    c->eax = p ? (uint32_t)(uintptr_t)p : 0; cdecl_ret(c);
}
static void h__get_initial_narrow_environment(CPU *c) {
    c->eax = 0; cdecl_ret(c);
}
static void h__initialize_narrow_environment(CPU *c) { c->eax = 0; cdecl_ret(c); }
static void h__configure_narrow_argv(CPU *c) { c->eax = 0; cdecl_ret(c); }
static void h__controlfp_s(CPU *c) {
    uint32_t out = A(c,0), nv = A(c,1), mask = A(c,2);
    (void)nv; (void)mask;
    if (out) st32(out, 0x0008001Fu);
    c->eax = 0; cdecl_ret(c);
}
static void h__initialize_onexit_table(CPU *c) {
    uint32_t t = A(c,0);
    if (t) { st32(t,0); st32(t+4,0); st32(t+8,0); }
    c->eax = 0; cdecl_ret(c);
}
static void h__register_onexit_function(CPU *c) { (void)A(c,0); (void)A(c,1); c->eax = 0; cdecl_ret(c); }
static void h__crt_atexit(CPU *c) { (void)A(c,0); c->eax = 0; cdecl_ret(c); }
static void h__register_thread_local_exe_atexit_callback(CPU *c) { (void)A(c,0); c->eax = 0; cdecl_ret(c); }
/* abort() **does not return**. This used to call cdecl_ret(), so the guest kept
 * executing the bytes after the call site -- that "pretend return" pushes the crash
 * far away. Measured: after DINPUT8 failed to load the game called abort; we pretended
 * to return and it immediately called a NULL function pointer. */
static void h_abort(CPU *c) { (void)c; fprintf(stderr,"[host] guest abort()\n"); fflush(NULL); abort(); }
/* The exit family **does not return**. This used to pop the stack via cdecl_ret() and
 * return, making a "never returns" function pretend to return -- the guest then ran the
 * bytes after the call; measured this round as a wild indirect call to 0x26. */
static void h_exit(CPU *c)
{
    uint32_t code = A(c, 0);
    fprintf(stderr, "[host] guest exit(%u)\n", code);
    fflush(NULL);
    exit((int)code);
}
static void h__exit(CPU *c)
{
    uint32_t code = A(c, 0);
    fprintf(stderr, "[host] guest _exit(%u)\n", code);
    _exit((int)code);
}
/* _cexit: flush and close streams, but **return** (that is how it differs from exit) */
static void h__cexit(CPU *c) { fflush(NULL); cdecl_ret(c); }
/* _c_exit: clear CRT state only, run no atexit handlers, and do not return */
static void h__c_exit(CPU *c) { exit(0); }
static void h_terminate(CPU *c)
{
    fprintf(stderr, "[host] std::terminate - uncaught C++ exception\n");
    fflush(NULL);
    /* ★★ 2026-11-11: crash forensics (strictly read-only, no GL calls).
     *
     * All we knew was "terminate happens near glDeleteProgram", but the [glreq] log
     * had no fflush, so the ordering before abort is unreliable. Two additions here:
     *   1. the last 64 GL entry points that were **actually dispatched**
     *   2. the return-address chain on the guest stack (which function called terminate)
     */
    {
        extern void host_gl_dump_ring(const char *);
        host_gl_dump_ring("crashed in terminate");
    }
    fprintf(stderr, "[host]   guest esp=%08x ebp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n",
            c->esp, c->ebp, c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi);
    /* Return addresses on the stack (10 starting at esp).
     * ★★ 2026-11-11: **esp must be validated first**.
     * With the local TERMINATE_SELFTEST firing at esp=0, this did ld32(0) -> SIGSEGV,
     * so the forensic code became the second crash source -- the error that broke the last device build. */
    if (c->esp >= 0x10000u && c->esp < 0xC0000000u) {
        fprintf(stderr, "[host]   guest stack:");
        for (int i = 0; i < 10 && c->esp + (uint32_t)i * 4u + 4u <= 0xC0000000u; i++) {
            uint32_t v = ld32(c->esp + (uint32_t)i * 4u);
            fprintf(stderr, " %08x", v);
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[host]   guest stack skipped (esp=%08x implausible)\n", c->esp);
    }
    /* ebp chain (unreliable with FPO, for reference only) */
    fprintf(stderr, "[host]   ebp chain:");
    {
        uint32_t bp = c->ebp;
        for (int i = 0; i < 8 && bp > 0x40000000u && bp < 0x40400000u; i++) {
            uint32_t ret = ld32(bp + 4u);
            fprintf(stderr, " %08x", ret);
            bp = ld32(bp);
        }
    }
    fprintf(stderr, "\n");

    /* ★★ 2026-11-11: **full stack scan** for return addresses.
     *
     * The ebp chain often breaks under MSVC /O2 because of frame-pointer omission. So we
     * scan the whole guest stack: any value falling in the PE code range (0x3c000000..0x3d000000,
     * an image of 0x889000 bytes) that **follows a call instruction** is a candidate return address.
     * Far more reliable than the ebp chain; it costs a few hundred memory reads, irrelevant on a crash. */
    {
        uint32_t lo = c->stack_floor, hi = c->stack_ceiling;
        uint32_t begin = c->esp;
        /* ★ Address sanity guard: **no scan** when esp sits at a low process address or above the stack top.
         * Otherwise ld32(0) and friends SIGSEGV outright, turning terminate forensics into a second crash. */
        if (begin < 0x10000u || begin >= 0xC0000000u) {
            fprintf(stderr, "[host]   full stack scan skipped (esp=%08x implausible)\n", begin);
            goto uaf_replay;
        }
        if (!hi || hi <= begin) hi = begin + 16384u;
        uint32_t end = (hi - begin > 16384u) ? begin + 16384u : hi;
        fprintf(stderr, "[host]   full stack scan [%08x,%08x), candidate return addresses inside code segments:\n",
                begin, end);
        unsigned found = 0;
        for (uint32_t a = begin; a + 4u <= end; a += 4u) {
            uint32_t v = ld32(a);
            if (v >= 0x3c000000u && v < 0x3c889000u) {
                fprintf(stderr, "[host]     %08x -> rva %06x\n", a, v - 0x3c000000u);
                if (++found >= 24u) { fprintf(stderr, "[host]     ...\n"); break; }
            }
        }
        if (!found) fprintf(stderr, "[host]     (none)\n");
    }

uaf_replay:
    /* ★ Replay objects that were freed yet are still in the resource table -- direct proof of a lifetime bug. */
    {
        extern uintptr_t g_uaf_log[64];
        extern unsigned  g_uaf_log_n, g_uaf_log_overflow;
        extern int res_table_contains(uintptr_t p);
        fprintf(stderr, "[host]   %u UAF records (+%u overflowed):\n",
                g_uaf_log_n, g_uaf_log_overflow);
        for (unsigned i = 0; i < g_uaf_log_n; i++) {
            uintptr_t q = g_uaf_log[i];
            /* Still in the table? Then it was never removed -- a classic leak-style UAF */
            uint32_t head = 0;
            if (q >= 0x10000u && q < 0xC0000000u) head = ld32((uint32_t)q);
            fprintf(stderr, "[host]     %08lx still in resource table=%d  first 4 bytes now=%08x\n",
                    (unsigned long)q, res_table_contains(q), head);
        }
    }
    fflush(NULL);
    abort();
}
static void h__invalid_parameter_noinfo_noreturn(CPU *c)
{
    fprintf(stderr, "[host] _invalid_parameter_noinfo_noreturn\n");
    fflush(NULL);
    abort();
}
static void h__seh_filter_exe(CPU *c) { c->eax = 1; cdecl_ret(c); }
static void h___std_terminate(CPU *c) { cdecl_ret(c); }
static void h__purecall(CPU *c) { cdecl_ret(c); }
static void h__Thrd_yield(CPU *c) { cdecl_ret(c); }
static void h_perror(CPU *c) { (void)A(c,0); cdecl_ret(c); }
static void h__wassert(CPU *c) { cdecl_ret(c); }
/* _access: really ask the filesystem. It used to return -1 unconditionally, so KAGE's
 *   ContentManager treated every resource as "not there" without even trying fopen,
 *   leaving the log full of "Failed to open resources/packed/*.a". */
static void h__access(CPU *c)
{
    uint32_t p = A(c, 0);
    int mode = (int)A(c, 1);
    const char *path = p ? G(p) : NULL;
    if (path) { const char *r = host_path_resolve(path); if (r) path = r; }
    int rc = (path && access(path, mode) == 0) ? 0 : -1;
    fprintf(stderr, "[crt] _access(\"%s\", %d) -> %d\n", path ? path : "?", mode, rc);
    c->eax = (uint32_t)rc;
    cdecl_ret(c);
}
static void h_remove(CPU *c) { uint32_t p=A(c,0); c->eax = p?(uint32_t)remove(G(p)):0xFFFFFFFFu; cdecl_ret(c); }
static void h_getenv(CPU *c) { (void)A(c,0); c->eax = 0; cdecl_ret(c); }

/* _CxxThrowException: diagnostic version.
 *
 * With no SEH unwind tables nobody catches a throw -- the only hard evidence is in
 * _ThrowInfo: it points at the CatchableTypeArray, which points at a TypeDescriptor,
 * whose mangled type name sits at descriptor+8. Print the name before blowing up,
 * otherwise all we see is "FAULT at ...!..", which carries no information. */
static void h_CxxThrowException(CPU *c)
{
    uint32_t pObj = A(c, 0), pTI = A(c, 1);
    fprintf(stderr, "[cxx] _CxxThrowException(obj=%08x, throwInfo=%08x)\n", pObj, pTI);
    if (pTI >= 0x10000u && pTI < 0x80000000u) {
        uint32_t attr = ld32(pTI);
        uint32_t pCTA = (pTI + 12u < 0x80000000u) ? ld32(pTI + 12u) : 0u;
        fprintf(stderr, "[cxx]   attributes=%08x catchableArray=%08x\n", attr, pCTA);
        if (pCTA >= 0x10000u && pCTA < 0x80000000u) {
            int n = (int)ld32(pCTA);
            fprintf(stderr, "[cxx]   nCatchable=%d\n", n);
            for (int i = 0; i < n && i < 8; i++) {
                uint32_t pCT = ld32(pCTA + 4u + (uint32_t)i * 4u);
                if (pCT < 0x10000u || pCT >= 0x80000000u) continue;
                uint32_t pTD = ld32(pCT + 4u);
                if (pTD < 0x10000u || pTD >= 0x80000000u) continue;
                uint32_t namep = pTD + 8u;
                char buf[192]; int k = 0;
                for (; k < (int)sizeof buf - 1; k++) {
                    uint8_t ch = ld8(namep + (uint32_t)k);
                    if (!ch) break;
                    buf[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
                }
                buf[k] = 0;
                fprintf(stderr, "[cxx]   type[%d] = \"%s\"\n", i, buf);
            }
        }
    }
    /* MSVC 32-bit layout of rapidxml::parse_error:
     *   +0 vptr  +4 what(char*)  +8 where(char*)
     * Printing what and the bytes around the error position shows which character broke. */
    if (pObj >= 0x10000u && pObj < 0xC0000000u) {
        fprintf(stderr, "[cxx]   obj dwords:");
        for (int i = 0; i < 5; i++) fprintf(stderr, " %08x", ld32(pObj + (uint32_t)i * 4u));
        fprintf(stderr, "\n");
        /* MSVC layout: the std::exception base comes first (vptr,_Mywhat,_Mydofree, 0xc total),
         * and rapidxml::parse_error's own m_what sits at +0xc, m_where at +0x10. */
        uint32_t wp = ld32(pObj + 0xcu);
        if (!(wp >= 0x10000u && wp < 0xC0000000u)) wp = ld32(pObj + 4u);
        if (wp >= 0x10000u && wp < 0xC0000000u) {
            char b[256]; int k = 0;
            for (; k < (int)sizeof b - 1; k++) {
                uint8_t ch = ld8(wp + (uint32_t)k);
                if (!ch) break;
                b[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
            }
            b[k] = 0;
            fprintf(stderr, "[cxx]   what(@%08x) = \"%s\"\n", wp, b);
        }
        uint32_t where = ld32(pObj + 0x10u);
        if (!(where >= 0x10000u && where < 0xC0000000u)) where = ld32(pObj + 8u);
        fprintf(stderr, "[cxx]   where(@%08x)\n", where);
        if (where >= 0x10000u && where < 0xC0000000u) {
            uint32_t beg = where > 64u ? where - 64u : 0u;
            fprintf(stderr, "[cxx]   nearby bytes: ");
            for (uint32_t a = beg; a < where + 64u; a++) {
                uint8_t ch = ld8(a);
                fprintf(stderr, "%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
            }
            fprintf(stderr, "\n");
        }
    }
    fflush(NULL);
    abort();
}

/* ==========================================================================
 * __RTDynamicCast -- the dynamic_cast implementation for MSVC 32-bit RTTI
 * (added 2026-11-06)
 *
 * Why it must exist
 * --------------
 * It was missing from the dispatch table, so the call fell into guest_fault() -> longjmp
 * and cut straight through the game's main loop. Device log:
 *     last import = VCRUNTIME140.dll!__RTDynamicCast
 *     [host] std::terminate -- uncaught C++ exception
 *     [SEGV] sig=6 ... abort
 * In other words: **one dynamic_cast anywhere in the game brings it down**.
 *
 * Signature (cdecl, the VCRUNTIME C export):
 *     void *__RTDynamicCast(void *pObject, long VfDelta, void *Sink,
 *                           void *TargetType, int isReference);
 *
 * The four MSVC 32-bit RTTI structures (all fields are RVAs; add the image base):
 *
 *   RTTICompleteObjectLocator
 *     +0  signature        +4  offset (vfptr offset relative to the complete object)
 *     +8  cdOffset         +12 pTypeDescriptor
 *     +16 pClassDescriptor
 *
 *   TypeDescriptor          +0 pVFTable  +4 spare  +8 name(".?AVxxx@@")
 *   RTTIClassHierarchyDescriptor
 *     +0 signature  +4 attributes  +8 numBaseClasses  +12 pBaseClassArray
 *   RTTIBaseClassDescriptor
 *     +0 pTypeDescriptor  +4 numContainedBases
 *     +8 mdisp  +12 pdisp  +16 vdisp  +20 attributes  +24 pClassDescriptor
 *
 * Algorithm:
 *   1. vtbl   = *(void**)(pObject + VfDelta)
 *   2. col    = *(void**)(vtbl - 4)
 *   3. full   = pObject + VfDelta - col->offset     ← complete object start
 *   4. walk BaseClassArray looking for pTypeDescriptor == TargetType
 *   5. hit -> full + mdisp
 *
 * Adjusting pointers for virtual bases (pdisp/vdisp != -1) needs the vbtable; for now
 * only mdisp is applied -- Isaac's failures are plain inheritance chains, so that is enough.
 * ========================================================================== */
static uint32_t rt_rva(uint32_t v)
{
    /* The image is relocated to 0x3c000000 and RVAs go up to about 0x85f000.
     * Small values are treated as RVAs, large ones as already-computed absolute addresses. */
    if (v && v < 0x1000000u) return v + 0x3c000000u;
    return v;
}

static void rt_name(uint32_t td, char *buf, int cap)
{
    int k = 0;
    buf[0] = 0;
    if (!td) return;
    for (; k < cap - 1; k++) {
        uint8_t ch = ld8(td + 8u + (uint32_t)k);
        if (!ch) break;
        buf[k] = (char)((ch >= 0x20 && ch < 0x7f) ? ch : '?');
    }
    buf[k] = 0;
}

static void h___RTDynamicCast(CPU *c)
{
    uint32_t pObject = A(c, 0);
    uint32_t VfDelta = A(c, 1);
    uint32_t Sink    = A(c, 2);
    uint32_t Target  = A(c, 3);
    uint32_t isRef   = A(c, 4);
    (void)Sink;

    uint32_t result = 0;
    char src_name[160], dst_name[160];

    if (!pObject) {
        c->eax = 0;
        cdecl_ret(c);
        return;
    }

    uint32_t vtbl = ld32(pObject + VfDelta);
    if (!vtbl) {
        c->eax = 0;
        cdecl_ret(c);
        return;
    }
    uint32_t col = rt_rva(ld32(vtbl - 4u));
    if (!col) {
        c->eax = 0;
        cdecl_ret(c);
        return;
    }

    uint32_t obj_off   = ld32(col + 4u);
    uint32_t td_self   = rt_rva(ld32(col + 12u));
    uint32_t classdesc = rt_rva(ld32(col + 16u));
    uint32_t full      = pObject + VfDelta - obj_off;

    rt_name(td_self, src_name, (int)sizeof src_name);
    rt_name(rt_rva(Target), dst_name, (int)sizeof dst_name);

    if (classdesc) {
        uint32_t n   = ld32(classdesc + 8u);
        uint32_t arr = rt_rva(ld32(classdesc + 12u));
        if (n < 256u && arr) {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t bcd = rt_rva(ld32(arr + i * 4u));
                if (!bcd) continue;
                uint32_t td = rt_rva(ld32(bcd + 0u));
                if (!td) continue;
                if (td == rt_rva(Target)) {
                    int32_t mdisp = (int32_t)ld32(bcd + 8u);
                    result = full + (uint32_t)mdisp;
                    break;
                }
            }
        }
    }

    {
        static unsigned shown, warned;
        if (shown++ < 8u)
            fprintf(stderr, "[rtti] dynamic_cast \"%s\" -> \"%s\" = %s (%08x)\n",
                    src_name, dst_name, result ? "OK" : "NULL", result);
        if (!result && isRef && warned++ < 4u)
            fprintf(stderr, "[rtti] ★ reference cast failed (the game may crash): \"%s\" -> \"%s\"\n",
                    src_name, dst_name);
        if (result || shown <= 8u) fflush(NULL);
    }

    c->eax = result;
    cdecl_ret(c);
}

static void h__configthreadlocale(CPU *c) { (void)A(c,0); c->eax = 0; cdecl_ret(c); }
static void h_terminate_h(CPU *c) { cdecl_ret(c); }

/* ★★ 2026-11-11: **local self-test entry** for terminate forensics.
 *
 * The last device crash happened because I shipped code that never ran locally.
 * This provides an entry point that can be triggered locally, to confirm the
 * full stack scan / UAF replay inside terminate cannot crash a second time.
 * Called by host_gl4es.c only when TERMINATE_SELFTEST=1. */
void host_terminate_selftest(void)
{
    CPU dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.esp = 0;              /* deliberately implausible esp, to exercise the guard branch */
    fprintf(stderr, "[selftest] forcing the terminate forensics path\n");
    h_terminate(&dummy);
}

typedef struct { const char *n; void (*f)(CPU *); } E;
static const E TBL[] = {
    {"VCRUNTIME140.dll!memcpy", h_memcpy},
    {"VCRUNTIME140.dll!memmove", h_memmove},
    {"VCRUNTIME140.dll!memset", h_memset},
    {"VCRUNTIME140.dll!memchr", h_memchr},
    {"VCRUNTIME140.dll!strchr", h_strchr},
    {"VCRUNTIME140.dll!strstr", h_strstr},
    {"api-ms-win-crt-string-l1-1-0.dll!strpbrk", h_strpbrk},
    {"api-ms-win-crt-string-l1-1-0.dll!strncpy", h_strncpy},
    {"api-ms-win-crt-string-l1-1-0.dll!strncmp", h_strncmp},
    {"api-ms-win-crt-string-l1-1-0.dll!_strnicmp", h__strnicmp},
    {"api-ms-win-crt-string-l1-1-0.dll!_strdup", h__strdup},
    {"api-ms-win-crt-string-l1-1-0.dll!strcpy_s", h_strcpy_s},
    {"api-ms-win-crt-string-l1-1-0.dll!strncpy_s", h_strncpy_s},
    {"api-ms-win-crt-string-l1-1-0.dll!strcat_s", h_strcat_s},
    {"api-ms-win-crt-string-l1-1-0.dll!isdigit", h_isdigit},
    {"api-ms-win-crt-string-l1-1-0.dll!isspace", h_isspace},
    {"api-ms-win-crt-string-l1-1-0.dll!ispunct", h_ispunct},
    {"api-ms-win-crt-string-l1-1-0.dll!iswspace", h_iswspace},
    {"api-ms-win-crt-string-l1-1-0.dll!tolower", h_tolower},
    {"api-ms-win-crt-string-l1-1-0.dll!toupper", h_toupper},
    {"api-ms-win-crt-heap-l1-1-0.dll!malloc", h_malloc},
    {"api-ms-win-crt-heap-l1-1-0.dll!calloc", h_calloc},
    {"api-ms-win-crt-heap-l1-1-0.dll!realloc", h_realloc},
    {"api-ms-win-crt-heap-l1-1-0.dll!free", h_free},
    {"api-ms-win-crt-heap-l1-1-0.dll!_set_new_mode", h__set_new_mode},
    {"api-ms-win-crt-heap-l1-1-0.dll!_callnewh", h__callnewh},
    {"api-ms-win-crt-utility-l1-1-0.dll!qsort", h_qsort},
    {"api-ms-win-crt-convert-l1-1-0.dll!atoi", h_atoi},
    {"api-ms-win-crt-convert-l1-1-0.dll!atof", h_atof},
    {"api-ms-win-crt-convert-l1-1-0.dll!strtol", h_strtol},
    {"api-ms-win-crt-convert-l1-1-0.dll!strtoull", h_strtoull},
    {"api-ms-win-crt-math-l1-1-0.dll!ceil", h_ceil},
    {"api-ms-win-crt-math-l1-1-0.dll!floor", h_floor},
    {"api-ms-win-crt-math-l1-1-0.dll!roundf", h_roundf},
    {"api-ms-win-crt-math-l1-1-0.dll!copysignf", h_copysignf},
    {"api-ms-win-crt-math-l1-1-0.dll!nextafterf", h_nextafterf},
    {"api-ms-win-crt-math-l1-1-0.dll!ldexp", h_ldexp},
    {"api-ms-win-crt-math-l1-1-0.dll!modf", h_modf},
    {"api-ms-win-crt-math-l1-1-0.dll!_fdclass", h__fdclass},
    {"api-ms-win-crt-math-l1-1-0.dll!__setusermatherr", h___setusermatherr},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_sin_precise", h__libm_sse2_sin_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_cos_precise", h__libm_sse2_cos_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_sqrt_precise", h__libm_sse2_sqrt_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_log_precise", h__libm_sse2_log_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_log10_precise", h__libm_sse2_log10_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_exp_precise", h__libm_sse2_exp_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_pow_precise", h__libm_sse2_pow_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_acos_precise", h__libm_sse2_acos_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_asin_precise", h__libm_sse2_asin_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_libm_sse2_atan_precise", h__libm_sse2_atan_precise},
    {"api-ms-win-crt-math-l1-1-0.dll!_CIatan2", h__CIatan2},
    {"api-ms-win-crt-math-l1-1-0.dll!_CIfmod", h__CIfmod},
    {"api-ms-win-crt-stdio-l1-1-0.dll!_set_fmode", h__set_fmode},
    {"api-ms-win-crt-stdio-l1-1-0.dll!__p__commode", h___p__commode},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_errno", h__errno},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_set_errno", h__set_errno},
    {"api-ms-win-crt-runtime-l1-1-0.dll!__p___argc", h___p___argc},
    {"api-ms-win-crt-runtime-l1-1-0.dll!__p___argv", h___p___argv},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_get_initial_narrow_environment", h__get_initial_narrow_environment},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_initialize_narrow_environment", h__initialize_narrow_environment},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_configure_narrow_argv", h__configure_narrow_argv},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_controlfp_s", h__controlfp_s},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_initialize_onexit_table", h__initialize_onexit_table},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_register_onexit_function", h__register_onexit_function},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_crt_atexit", h__crt_atexit},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_register_thread_local_exe_atexit_callback", h__register_thread_local_exe_atexit_callback},
    {"api-ms-win-crt-runtime-l1-1-0.dll!abort", h_abort},
    {"api-ms-win-crt-runtime-l1-1-0.dll!exit", h_exit},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_exit", h__exit},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_cexit", h__cexit},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_c_exit", h__c_exit},
    {"api-ms-win-crt-runtime-l1-1-0.dll!terminate", h_terminate},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_invalid_parameter_noinfo_noreturn", h__invalid_parameter_noinfo_noreturn},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_seh_filter_exe", h__seh_filter_exe},
    {"VCRUNTIME140.dll!__std_terminate", h___std_terminate},
    {"VCRUNTIME140.dll!_purecall", h__purecall},
    {"MSVCP140.dll!_Thrd_yield", h__Thrd_yield},
    {"api-ms-win-crt-runtime-l1-1-0.dll!perror", h_perror},
    {"api-ms-win-crt-runtime-l1-1-0.dll!_wassert", h__wassert},
    {"api-ms-win-crt-filesystem-l1-1-0.dll!_access", h__access},
    {"api-ms-win-crt-filesystem-l1-1-0.dll!remove", h_remove},
    {"api-ms-win-crt-environment-l1-1-0.dll!getenv", h_getenv},
    {"api-ms-win-crt-locale-l1-1-0.dll!_configthreadlocale", h__configthreadlocale},
    {"VCRUNTIME140.dll!_CxxThrowException", h_CxxThrowException},
    {"VCRUNTIME140.dll!__RTDynamicCast", h___RTDynamicCast},
};
#define NT ((int)(sizeof TBL / sizeof TBL[0]))

int host_crt_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    for (int i = 0; i < NT; i++)
        if (strcmp(TBL[i].n, name) == 0) { TBL[i].f(c); return 1; }
    return 0;
}
int host_crt_count(void) { return NT; }

/* Called from the signal handler: in non-fatal mode restore writability and return so the program continues */
int watch_handle_hit(uintptr_t pc, uint32_t val)
{
    for (unsigned i = 0; i < g_watch_seen_n; i++)
        if (g_watch_seen_pc[i] == pc) return 0;      /* already recorded, let it pass silently */
    if (g_watch_seen_n < WATCH_SEEN_N) {
        g_watch_seen_pc[g_watch_seen_n] = pc;
        g_watch_seen_val[g_watch_seen_n] = val;
        g_watch_seen_n++;
        fprintf(stderr, "[watch] writer pc=%08lx  value written=%08x\n",
                (unsigned long)pc, val);
        fflush(NULL);
    }
    mprotect((void *)g_watch_page, 4096, PROT_READ | PROT_WRITE);
    g_watch_armed = 0;
    g_watch_pending = 0;      /* do not re-arm, to avoid an endless loop */
    return 1;
}

/* ============================================================================
 * vtable guard rail: catches "object header overwritten"
 *
 * Each element's obj pointer in the resource table points at a 0x90-byte object whose [0] is a vtable.
 * The moment someone writes a non-vtable value to [0] (outside 0x30xxxxxx), that is the overwrite scene.
 * This is far more precise than write protection: field-by-field writes (like +0x48) are untouched.
 * ==========================================================================*/
/* ★ 2026-11-06: made it a global initialized by a constructor, so guest.h's st32 can test it inline.
 * It used to be "function-local static + lazy init", and since guest_vtable_guard_enabled()
 * is an external function called on every st32, sampling showed it costing 3.7% CPU. */
int g_vt_guard_on;
static int g_vt_hits;

__attribute__((constructor))
static void vt_guard_init(void)
{
    g_vt_guard_on = getenv("HEAP_GUARD_VTABLE") != NULL;
}

int guest_vtable_guard_enabled(void)
{
    return g_vt_guard_on;
}

int guest_is_res_obj_addr(uint32_t a)
{
    uint32_t tbl = ld32(0x3c7e7d00u);
    if (tbl < 0x10004u || tbl + 32u * 12u >= 0xC0000000u) return 0;
    for (unsigned b = 0; b < 32u; b++) {
        uint32_t beg = ld32(tbl + b * 12u);
        uint32_t end = ld32(tbl + b * 12u + 4u);
        if (beg == end || end < beg || end >= 0xC0000000u) continue;
        for (uint32_t q = beg; q + 8u <= end; q += 8u)
            if (ld32(q) == a) return 1;      /* a is the obj base of some element */
    }
    return 0;
}

void guest_vtable_guard_hit(uint32_t addr, uint32_t val)
{
    if (g_vt_hits >= 8) return;
    g_vt_hits++;
    fprintf(stderr,
        "[vt] ★★ object header overwritten: addr=%08x written=%08x (was=%08x) caller=%08x\n",
        addr, val, ld32(addr), g_free_caller);
    fflush(NULL);
}
