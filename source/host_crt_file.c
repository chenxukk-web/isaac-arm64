/* Android CRT layer -- the stdio file family
 *
 * The game must create the save directory, read config, and open resource files early in
 * startup, so this layer is a hard requirement, not an optional extra.
 *
 * Core constraint: the guest FILE* is a uint32_t and must point into low 4GB memory. So here we
 * do not return a host FILE*; we return a small low-4GB guest_file struct that holds the
 * host FILE* and the fd. All stdio imports are routed through it.
 *
 * Paths are guest pointers too (ANSI, relative to the current working directory). The game uses
 * relative paths like "./Documents/...", so the runtime cwd must be correct -- see continue_android_build.sh.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>
#include <strings.h>
#include "guest.h"
#include "host_path.h"

static uint32_t A(CPU *__restrict c, int i)
{
    return ld32(guest_stack_address(c, c->esp + 4u + (uint32_t)i * 4u, 4u, 0u));
}
static void cdecl_ret(CPU *c) { (void)gpop(c); }
static char *G(uint32_t a) { return (char *)(uintptr_t)a; }

/* ---- low-4GB guest FILE table ---- */
#define GF_BASE 0x3A100000u
#define GF_MAX  512u
#define GF_SIZE 0x10000u

typedef struct {
    uint32_t host_fp;      /* truncating a host FILE* to 32 bits loses data, so index back into the table */
    int32_t  hindex;       /* index into g_hostfp[]; -1 means closed */
    int32_t  fd;
    int32_t  eof;
    int32_t  err;
    int32_t  in_use;
    int32_t  xml_mode;    /* 1 = this stream carries XML and needs quote substitution (see h_fread) */
    int32_t  cache_slot;  /* >=0 means this handle points at a persistent cache slot; -1 means a private FILE* */
} gfile;

/* ★ 2026-11-05: the handle table must be locked.
 *
 * The game has 2 worker threads, and fopen/fclose come in from different threads.
 * gf_alloc used to do `for (...) if (!in_use) { in_use = 1; return; }`
 * allocating without a lock -- two threads could see the same free slot, get the **same guest
 * FILE address**, then overwrite each other's host_fp / hindex, and finally interleave
 * fseek/read on one FILE*, showing up as corrupted data or even a deadlock.
 *
 * This lock only covers the small "allocate/recycle a slot" section, not I/O, so the cost is negligible. */
static pthread_mutex_t g_gf_mu = PTHREAD_MUTEX_INITIALIZER;

static gfile   *g_gtab;                /* guest side (low 4GB) */
static FILE   **g_hostfp;              /* host side, indexed by hindex */
static unsigned g_hfp_n;               /* upper bound of the hindex values used so far */
static unsigned *g_free_hidx;          /* stack of recycled hindex values */
static unsigned  g_free_n;
static int      g_tab_ready;

/* ==========================================================================
 * persistent file cache (2026-11-05)
 *
 * Symptom: in on-device/local logs the archive open count matches the entry count **one to one**
 *       afterbirthp.a  5512 entries → 5513 fopen calls
 *       repentance.a   4180 entries → 4181 fopen calls
 *       one run totals 15573 fopen/fclose calls.
 *
 * This is the behavior of the game's own KAGE Archive::extract: for every entry it takes, it
 * opens the archive file, seeks to the index, reads the record, seeks to the data, inflates, closes.
 * On Windows the system file cache makes that nearly free; on Android's FUSE it is expensive.
 *
 * Approach: keep the N most recently used FILE* handles, do not really close them on fclose, and on the
 * next open of the same path just rewind and reuse. The semantics match open/close exactly (read from
 * the beginning); it only degrades when "the same path is opened twice at once" -- that case allocates
 * its own FILE*, because slots carry a refs count and an in-use slot is not reused.
 *
 * Limits: 16 fixed slots, 256-byte paths. There are only 9 archives, so that is enough.
 * ========================================================================== */
#define FCACHE_N 16
#define FCACHE_PATH 256

typedef struct {
    char  path[FCACHE_PATH];
    FILE *fp;
    int   refs;      /* >0 means a guest handle is currently using it */
} fcache_ent;

static fcache_ent g_fcache[FCACHE_N];
static unsigned   g_fcache_next;

/* find a usable slot: prefer the same path and idle; otherwise find an empty slot */
static int fcache_acquire(const char *path)
{
    int empty = -1;
    for (int i = 0; i < FCACHE_N; i++) {
        if (!g_fcache[i].path[0]) { if (empty < 0) empty = i; continue; }
        if (g_fcache[i].refs == 0 && strcmp(g_fcache[i].path, path) == 0) {
            if (!g_fcache[i].fp) { g_fcache[i].path[0] = 0; if (empty < 0) empty = i; continue; }
            return i;
        }
    }
    if (empty >= 0) return empty;
    /* all full: evict an idle one (refs==0) */
    for (int k = 0; k < FCACHE_N; k++) {
        int i = (int)((g_fcache_next + (unsigned)k) % FCACHE_N);
        if (g_fcache[i].refs == 0) {
            if (g_fcache[i].fp) fclose(g_fcache[i].fp);
            g_fcache[i].fp = NULL;
            g_fcache[i].path[0] = 0;
            return i;
        }
    }
    return -1;      /* all in use, give up on caching */
}

static int gf_init(void)
{
    if (g_tab_ready) return g_tab_ready > 0 ? 0 : -1;
    void *p = mmap((void *)(uintptr_t)GF_BASE, GF_SIZE,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)GF_BASE) { g_tab_ready = -1; return -1; }
    g_gtab = (gfile *)(uintptr_t)GF_BASE;
    g_hostfp = calloc(GF_MAX, sizeof *g_hostfp);
    g_free_hidx = calloc(GF_MAX, sizeof *g_free_hidx);
    if (!g_hostfp || !g_free_hidx) { g_tab_ready = -1; return -1; }
    g_tab_ready = 1;
    return 0;
}

/* allocate a guest FILE, return the guest address (0 = failure) */
static uint32_t gf_alloc(FILE *hf, int fd)
{
    if (gf_init() != 0) return 0;
    /* hindex must be recyclable: it used to only grow, so after 512 cumulative file opens
     * allocation failed forever, and the failure branch printed nothing -- it showed up as "after the
     * 512th open no resource can be read any more", which is very hard to trace back to the handle table. */
    unsigned hidx;
    if (g_free_n) hidx = g_free_hidx[--g_free_n];
    else if (g_hfp_n < GF_MAX) hidx = g_hfp_n++;
    else return 0;

    pthread_mutex_lock(&g_gf_mu);
    for (unsigned i = 0; i < GF_MAX; i++) {
        if (g_gtab[i].in_use) continue;
        g_hostfp[hidx] = hf;
        g_gtab[i].host_fp = 0;
        g_gtab[i].hindex  = (int32_t)hidx;
        g_gtab[i].fd      = fd;
        g_gtab[i].eof = g_gtab[i].err = 0;
        g_gtab[i].xml_mode = 0;
        g_gtab[i].cache_slot = -1;
        g_gtab[i].in_use  = 1;
        pthread_mutex_unlock(&g_gf_mu);
        return GF_BASE + i * (uint32_t)sizeof(gfile);
    }
    pthread_mutex_unlock(&g_gf_mu);
    /* got a hidx but no slot: impossible, but hand the hidx back anyway so it does not leak */
    if (g_free_n < GF_MAX) g_free_hidx[g_free_n++] = hidx;
    return 0;
}

static gfile *gf_get(uint32_t guest_fp)
{
    if (gf_init() != 0 || !guest_fp) return NULL;
    if (guest_fp < GF_BASE || guest_fp >= GF_BASE + GF_MAX * sizeof(gfile))
        return NULL;
    unsigned idx = (guest_fp - GF_BASE) / (uint32_t)sizeof(gfile);
    if (idx >= GF_MAX) return NULL;
    gfile *g = &g_gtab[idx];
    if (!g->in_use || g->hindex < 0) return NULL;
    return g;
}

static FILE *gf_file(gfile *g)
{
    if (!g || g->hindex < 0 || (unsigned)g->hindex >= g_hfp_n) return NULL;
    return g_hostfp[g->hindex];
}

/* ==========================================================================
 * ★★ 2026-11-12: case-insensitive fopen fallback -- **root-cause fix for the missing fonts**
 *
 * Evidence chain:
 *   1. **9 of** the 12 .fnt files reference PNG names whose case does not match the actual file:
 *        teammeatfont16.fnt says       "TeamMeatFont16_0.png"
 *        the actual file name is            "teammeatfont16_0.png"
 *      (luamini / pftempesta / upheaval / teammeat* are all in this situation)
 *   2. When the font atlas is uploaded, **RGB and alpha are both 0** -- they were not cleared by premultiply,
 *      it is that ImagePng never read any pixels at all.
 *   3. The ports (Windows / iOS) are fine -- NTFS and APFS are **case-insensitive**,
 *      so the same name finds the file. **Android's ext4/F2FS is case-sensitive.**
 *
 * Symptom: the font atlas is an empty image → the shader samples alpha=0 → every character lands on the same spot
 *       → all HUD digits render as the same µ; the main menu list is entirely blank.
 *
 * Fix: when fopen fails, walk the path directory by directory and use readdir for a case-insensitive match.
 * Only **when the original open fails** does it take this path; the normal path costs nothing.
 * ========================================================================== */
static FILE *ci_fopen(const char *path, const char *mode)
{
    if (!path || !*path) return NULL;
    if (!strchr(mode, 'r')) return NULL;   /* only fall back for read modes */

    /* at most 16 directory levels; path length limit is 512 */
    char cur[512];
    if (strlen(path) >= sizeof cur) return NULL;

    /* absolute paths start at "/", relative paths start at "." */
    const char *p = path;
    if (p[0] == '/') { snprintf(cur, sizeof cur, ""); }
    else             { snprintf(cur, sizeof cur, "."); }

    int levels = 0;
    while (*p && levels < 16) {
        /* skip repeated '/' */
        while (*p == '/') p++;
        if (!*p) break;

        /* take out this level's component */
        char comp[256];
        size_t n = 0;
        while (*p && *p != '/' && n + 1 < sizeof comp) comp[n++] = *p++;
        comp[n] = 0;
        if (n == 0) break;

        char next[512];
        snprintf(next, sizeof next, "%s/%s", cur, comp);

        /* an exact hit uses the exact name */
        struct stat st;
        if (stat(next, &st) == 0) {
            snprintf(cur, sizeof cur, "%s", next);
            levels++;
            continue;
        }

        /* otherwise look for a case-insensitive match in this directory */
        DIR *d = opendir(cur[0] ? cur : "/");
        if (!d) return NULL;
        char found[256];
        int hit = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == 0 ||
                 (de->d_name[1] == '.' && de->d_name[2] == 0))) continue;
            if (strcasecmp(de->d_name, comp) == 0) {
                snprintf(found, sizeof found, "%s", de->d_name);
                hit = 1;
                break;
            }
        }
        closedir(d);
        if (!hit) return NULL;
        snprintf(cur, sizeof cur, "%s/%s", cur, found);
        levels++;
    }

    return fopen(cur, mode);
}

/* ---- open / close ---- */
static void h_fopen(CPU *c)
{
    uint32_t pathp = A(c, 0), modep = A(c, 1);
    if (!pathp || !modep) { c->eax = 0; cdecl_ret(c); return; }
    /* Windows semantics: case-insensitive. If resolution fails, try the original string once,
     * so absolute paths / special files (like /dev/null) are not broken. */
    const char *resolved = host_path_resolve(G(pathp));
    const char *use = resolved ? resolved : G(pathp);
    /* a directory is not a file: on Linux fopen(dir, "rb") succeeds and the following fread fails,
     * so the caller gets a buffer that was never written -- that failure is much harder to trace than ENOENT. */
    {
        struct stat st;
        if (stat(use, &st) == 0 && S_ISDIR(st.st_mode)) {
            fprintf(stderr, "[crt] fopen(\"%s\") resolved to directory \"%s\"; refusing\n",
                    G(pathp), use);
            c->eax = 0; cdecl_ret(c); return;
        }
    }
    /* check the persistent cache first: repeated open/close of the same path can reuse the FILE* directly.
     * Only applies to read-only modes -- write modes have different semantics and cannot share a stream. */
    {
        const char *mode = G(modep);
        int readonly = mode && (mode[0] == 'r') && strchr(mode, '+') == NULL
                       && strchr(mode, 'w') == NULL && strchr(mode, 'a') == NULL;
        /* ★ 2026-11-05: disable the persistent handle cache.
         *
         * On-device symptom: complete freeze after entering the main screen (draw stops at #16800, fread and the frame stream
         * both stall, but the process is still alive -- a deadlock).
         *
         * Cause: the game has 2 worker threads (the runtime log's thread-start line appears twice),
         * and this cache has **no lock at all**. When two threads fopen the same archive at the same time:
         *   · fcache_acquire may return the same slot
         *   · both threads share one FILE*, interleaving fflush/fseek/read
         *   · the internal FILE* locks block each other → deadlock
         *
         * And measurement showed it brought no benefit: fopen went from 15573 down to 2,
         * but load time did not change at all (already recorded in Section 22). Zero benefit plus high risk
         * -- so turn it off and return to the known-good behavior.
         *
         * If it is ever re-enabled, the preconditions are: add a pthread_mutex to g_fcache,
         * and refs may only mean "exclusive open" semantics (two simultaneous opens of the same path
         * must each get their own FILE*, never a shared one). */
        if (0 && readonly && strlen(use) < FCACHE_PATH) {
            int slot = fcache_acquire(use);
            if (slot >= 0) {
                fcache_ent *e = &g_fcache[slot];
                if (!e->fp) {
                    e->fp = fopen(use, mode);
                    if (e->fp) {
                        strncpy(e->path, use, FCACHE_PATH - 1);
                        e->path[FCACHE_PATH - 1] = 0;
                    }
                } else {
                    /* reuse: go back to the start of the file. This is exactly the open/close semantics. */
                    rewind(e->fp);
                    clearerr(e->fp);
                }
                if (e->fp) {
                    uint32_t gp = gf_alloc(e->fp, fileno(e->fp));
                    if (gp) {
                        gfile *gg = gf_get(gp);
                        if (gg) gg->cache_slot = slot;
                        e->refs++;
                        c->eax = gp;
                        cdecl_ret(c);
                        return;
                    }
                }
                /* if the cache path fails, fall through to the plain fopen below */
            }
        }
    }

    FILE *hf = fopen(use, G(modep));
    /* ★★ 2026-11-12: record the **real path** of every fopen (first 200).
     * The case fallback had 0 hits, so the fonts are not read as loose files through fopen --
     * we need to see exactly which paths the game requests.
     * Cap at 200 lines so the loading phase is not flooded. */
    {
        static unsigned n;
        if (n < 200u) {
            n++;
            fprintf(stderr, "[fopen] %s \"%s\"\n", hf ? "OK " : "FAIL", use);
            fflush(NULL);
        } else if (n == 200u) {
            n++;
            fprintf(stderr, "[fopen] ... (silent from here on)\n");
            fflush(NULL);
        }
    }
    if (!hf) {
        /* ★ 2026-11-12: case-insensitive fallback (see the comment on ci_fopen).
         * The font .fnt files reference "TeamMeatFont16_0.png" while the actual file is lowercase --
         * Android is case-sensitive and Windows/iOS are not, so this is the root cause of the missing fonts. */
        hf = ci_fopen(use, G(modep));
        if (hf) {
            static unsigned ci_n;
            if (ci_n++ < 24u) {
                fprintf(stderr, "[crt] case-insensitive fallback hit: \"%s\"\n", G(pathp));
                fflush(NULL);
            }
        }
    }
    if (!hf) {
        fprintf(stderr, "[crt] fopen(\"%s\", \"%s\") -> NULL (errno=%d)\n",
                G(pathp), G(modep), errno);
        c->eax = 0; cdecl_ret(c); return;
    }
    int fd = fileno(hf);
    uint32_t gp = gf_alloc(hf, fd);
    if (!gp) {
        fprintf(stderr, "[crt] ✗ fopen(\"%s\") host open succeeded but the guest handle table is full\n",
                G(pathp));
        fclose(hf); c->eax = 0; cdecl_ret(c); return;
    }
    /* ★ 2026-11-05: silent by default.
     *
     * It used to print a line on every successful fopen. Measured on device, one run does 23029 fopens
     * (9 archives × one per entry), which is 23029 stderr lines;
     * and stderr is forwarded by Java into run.log, and that file lives on /storage/emulated/0
     * (FUSE, an order of magnitude slower than ext4). That means one slow disk write
     * attached to every archive entry opened -- which is itself a huge burden on the loading phase.
     *
     * Use FOPEN_TRACE=1 when tracing path problems. */
    {
        static int on = -1;
        if (on < 0) on = getenv("FOPEN_TRACE") != NULL;
        static unsigned long n_ok;
        n_ok++;
        /* print progress every 5000 calls.
         * The point is not path tracing but making the "archive loading phase" visible on screen --
         * that phase takes 2~3 minutes with no output at all, and looks like a hang. */
        if (on)
            fprintf(stderr, "[crt] fopen(\"%s\", \"%s\") -> FILE* %08x\n",
                    G(pathp), G(modep), gp);
        else if ((n_ok % 5000UL) == 0UL)
            fprintf(stderr, "[crt] %lu files opened so far...\n", n_ok);
    }
    c->eax = gp;
    cdecl_ret(c);
}

static void h_fclose(CPU *c)
{
    uint32_t fp = A(c, 0);
    gfile *g = gf_get(fp);
    if (!g) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }

    int r = 0;
    if (g->cache_slot >= 0 && g->cache_slot < FCACHE_N) {
        /* cache slot: keep the FILE* open, only decrement the refcount.
         * The next fopen of the same path rewinds and reuses it -- saving one open/close. */
        fcache_ent *e = &g_fcache[g->cache_slot];
        if (e->refs > 0) e->refs--;
        g->cache_slot = -1;
    } else {
        FILE *hf = gf_file(g);
        r = hf ? fclose(hf) : -1;
    }

    pthread_mutex_lock(&g_gf_mu);
    if (g->hindex >= 0) {
        g_hostfp[g->hindex] = NULL;
        if (g_free_n < GF_MAX) g_free_hidx[g_free_n++] = (unsigned)g->hindex;
    }
    g->hindex = -1;
    g->in_use = 0;
    pthread_mutex_unlock(&g_gf_mu);
    c->eax = (uint32_t)r;
    cdecl_ret(c);
}

/* ---- fread range recording (for the GL layer to decide where texture data comes from) ---- */
#define FREC_N 4096
static struct { uint32_t dst; uint32_t size; } g_frec[FREC_N];
static unsigned g_frec_n;

void fread_record(uint32_t dst, uint32_t size)
{
    if (!size) return;
    g_frec[g_frec_n % FREC_N].dst  = dst;
    g_frec[g_frec_n % FREC_N].size = size;
    g_frec_n++;
}

/* whether ptr falls inside the destination range of one of the recent freads.
 * Returns 1 for "in range" (meaning this memory was covered by a file read),
 * 0 for "not in range" (data built into the PE, or never fread).
 * Only the last 512 are checked, to avoid long loops. */
int fread_covers(uint32_t ptr)
{
    unsigned lim = g_frec_n < FREC_N ? g_frec_n : FREC_N;
    if (lim > 512u) lim = 512u;
    for (unsigned i = 0; i < lim; i++) {
        unsigned k = (g_frec_n - 1u - i) % FREC_N;
        uint32_t d = g_frec[k].dst, sz = g_frec[k].size;
        if (sz && ptr >= d && ptr < d + sz) return 1;
    }
    return 0;
}

/* ---- read / write ---- */
static void h_fread(CPU *c)
{
    uint32_t dst = A(c, 0), sz = A(c, 1), n = A(c, 2), fp = A(c, 3);
    gfile *g = gf_get(fp);
    FILE *hf = gf_file(g);
    if (!hf || !dst || !sz || !n) { c->eax = 0; cdecl_ret(c); return; }
    /* timing probe: see whether the time really goes to I/O or elsewhere. One line every 2000 calls. */
    static unsigned long n_read, n_bytes_note;
    static unsigned long long us_total;
    static unsigned long n_png, n_zero;   /* PNG signature block / all-zero large block counts */

    /* ★★ 2026-11-10: faster startup -- heavy probes now off by default.
     *
     * The old code did this on every fread:
     *   · 2 clock_gettime(CLOCK_MONOTONIC) calls     ~50 ns
     *   · PNG signature check (4-byte compare)
     *   · when bytes>=1024, scan the first 32 bytes for all-zero      ~20 ns
     * And one Isaac load calls fread **24 million times**:
     *   24M × 70ns ≈ 1.7 seconds, pure diagnostic overhead.
     *
     * These probes have served their purpose (confirmed PNG signature blocks 16,
     * all-zero large blocks 80, no problem in the data layer). They are now off by default,
     * turn them on with IO_PROBE=1 when needed.
     *
     * What is kept is only the byte accumulator (a few additions, negligible),
     * because "how many MB were read" is useful for judging whether resource loading finished. */
    static int g_io_probe = -1;
    if (g_io_probe < 0) g_io_probe = getenv("IO_PROBE") ? 1 : 0;

    size_t got;
    if (g_io_probe) {
        struct timespec _t0, _t1;
        clock_gettime(CLOCK_MONOTONIC, &_t0);
        got = fread(G(dst), (size_t)sz, (size_t)n, hf);
        clock_gettime(CLOCK_MONOTONIC, &_t1);
        {
            long long ns = (long long)(_t1.tv_sec - _t0.tv_sec) * 1000000000ll
                         + (long long)(_t1.tv_nsec - _t0.tv_nsec);
            if (ns < 0) ns = 0;
            us_total += (unsigned long long)(ns / 1000);
        }
    } else {
        got = fread(G(dst), (size_t)sz, (size_t)n, hf);
    }
    n_bytes_note += got * (size_t)sz;
    /* ★★ 2026-11-10: record the fread destination range, for the GL layer to decide "where texture data comes from".
     *
     * Problem: 103 textures are all zero (21 of them 512x512, 7 of them 1024x512),
     * while the same 64x64 lava texture (id=41) is perfectly fine -- so the decode pipeline works,
     * but the large texture data is zero. We need to know whether those zeros are **read from the file**
     * or **written by inflate/decode**. The two point in completely different directions:
     *   · zero right out of the file → the archive read/seek is broken
     *   · the file has data but it is zero after decode → the inflater or some import stub has a bug
     *
     * Approach: ring-record (destination, size) of the last 4096 freads. When the GL layer hits an
     * all-zero texture it looks back to see whether ptr falls inside one of these ranges.
     * Cost: two assignments per fread, about 50ms for 24 million calls, acceptable. */
    fread_record(dst, (uint32_t)(got * (size_t)sz));
    {
        if (++n_read % 200000UL == 0UL) {
            fprintf(stderr, "[io] fread total %lu calls, %lu MB, host time %llu ms "
                    "| PNG signature blocks=%lu all-zero blocks=%lu\n",
                    n_read, n_bytes_note / (1024UL * 1024UL), us_total / 1000ull,
                    n_png, n_zero);
            fflush(NULL);
        }
        /* ★ 2026-11-06: second-layer probe for all-zero textures.
         *
         * Known: with HEAP_POISON=1 the glTexImage2D pixels are still 0x00 (not 0xCD),
         * which means the decoder really did write zeros -- so we must see whether it **took in zeros** or **spat out zeros**.
         *
         * Here we look at 4 bytes after every fread:
         *   · PNG signature block  = how many times 89 50 4E 47 was read
         *   · all-zero large block = how many times a single read of >= 1KB had its first 32 bytes all zero
         * The ratio between the two directly answers "is the input zero, or is the decode wrong".
         * It reads only 4~32 bytes, so the cost is negligible. */
        if (g_io_probe && got && dst) {
            const uint8_t *p = (const uint8_t *)G(dst);
            size_t bytes = got * (size_t)sz;
            if (bytes >= 4u && p[0] == 0x89u && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
                if (n_png++ < 3UL)
                    fprintf(stderr, "[io] PNG signature block #%lu: %zu bytes dst=%08x\n",
                            n_png, bytes, dst);
            } else if (bytes >= 1024u) {
                int allzero = 1;
                for (int k = 0; k < 32; k++) if (p[k]) { allzero = 0; break; }
                if (allzero) {
                    if (n_zero++ < 3UL)
                        fprintf(stderr, "[io] all-zero block #%lu: %zu bytes dst=%08x\n",
                                n_zero, bytes, dst);
                }
            }
        }
    }
    /* ====================================================================
     * XML quote substitution (workaround)
     *
     * Section 13 found that when "an attribute value ends with /", the parser misreads the trailing '/' as
     * the self-closing marker "/>", jumps straight to the end of the element, and swallows all following attributes.
     * <backdrops gfxroot="gfx/backdrop/" gridgfxroot="gfx/grid/"> → only the first one survives.
     *
     * The root of the symptom is an off-by-one at the value boundary (the start does not skip the separator, the length is computed after skipping).
     * With single quotes the value boundary character differs, which sidesteps this path -- and in XML
     * ' and " are fully equivalent, so the semantics do not change.
     *
     * Only applies to streams that "look like XML": the stream is locked once a block starting with '<' is read.
     * Substitution must happen in **every** 4-byte block -- these XML files are read as a stream block by block,
     * and the quote is a single byte, so it never straddles blocks.
     * ==================================================================== */
    if (0) {   /* temporarily disabled for investigation */
        static int xml_quirk = -1;
        /* off by default: a 2026-11-03 measurement showed it really did let parsing get further (no longer crashing in attribute lookup),
         * but it introduced the next bug -- a malloc(1879 MB) somewhere → bad_alloc → abort.
         * That shows the "value boundary off-by-one" direction is right, but swapping quotes only sidesteps the boundary character,
         * it does not really fix the pointer offset. Use XML_QUOTE_FIX=1 to reproduce this path. */
        if (xml_quirk < 0) xml_quirk = getenv("XML_QUOTE_FIX") != NULL;
        if (xml_quirk && g) {
            size_t bytes = got * (size_t)sz;
            if (!g->xml_mode) {
                if (*(uint8_t *)(uintptr_t)dst == '<') {
                    g->xml_mode = 1;
                    { static int shown; if (shown++ < 12)
                        fprintf(stderr, "[xmlfix] stream %08x entered XML mode (first byte '<')\n", fp); }
                }
            }
            if (g->xml_mode) {
                uint8_t *b = (uint8_t *)(uintptr_t)dst;
                for (size_t k = 0; k < bytes; k++)
                    if (b[k] == '"') b[k] = '\'';
            }
        }
    }
    {
        extern void res_write_audit(uint32_t, uint32_t, uint32_t, const char *);
        extern int heap_block_state(uintptr_t, size_t *);
        size_t bytes = (size_t)sz * (size_t)n;
        res_write_audit(dst, (uint32_t)bytes, ld32(c->esp), "fread");
        if (bytes >= (size_t)(1u << 20)) {
            size_t bsz = 0;
            int st = heap_block_state(dst, &bsz);
            fprintf(stderr,
                "[fread] dst=%08x size=%u n=%u bytes=%zu(class needs>=%zu) "
                "block state=%d block size=%zu caller=%08x\n",
                dst, sz, n, bytes, bytes + 16u, st, bsz, ld32(c->esp));
            fflush(NULL);
        }
    }
    /* XML probe: if the inflated/read content starts with '<', check that head and tail are intact.
     * Truncated XML makes rapidxml build a malformed node tree, so later traversal
     * reads a wild pointer -- but the error point is far away from the real cause. */
    {
        static int xshown;
        uint8_t c0 = dst ? *(uint8_t *)(uintptr_t)dst : 0;
        if (dst && c0 == '<' && xshown < 8) {
            size_t bytes = (size_t)sz * (size_t)n;
            size_t have  = (size_t)sz * got;
            xshown++;
            fprintf(stderr,
                "[xml] dst=%08x requested=%zu obtained=%zu complete=%s\n",
                dst, bytes, have, (have == bytes) ? "yes" : "★ no (truncated)");
            /* first 48 bytes + last 48 bytes (to tell whether it ends with '>') */
            fprintf(stderr, "[xml]   head:");
            for (int z = 0; z < 48 && (size_t)z < have; z++) {
                uint8_t ch = *(uint8_t *)(uintptr_t)(dst + (uint32_t)z);
                fprintf(stderr, "%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
            }
            fprintf(stderr, "\n[xml]   tail:");
            for (size_t z = (have > 48 ? have - 48 : 0); z < have; z++) {
                uint8_t ch = *(uint8_t *)(uintptr_t)(dst + (uint32_t)z);
                fprintf(stderr, "%c", (ch >= 0x20 && ch < 0x7f) ? ch : '.');
            }
            fprintf(stderr, "\n");
            fflush(NULL);
        }
    }
    if (got < n) g->eof = feof(hf) ? 1 : g->eof;
    if (ferror(hf)) g->err = 1;
    c->eax = (uint32_t)got;
    cdecl_ret(c);
}

static void h_fwrite(CPU *c)
{
    uint32_t src = A(c, 0), sz = A(c, 1), n = A(c, 2), fp = A(c, 3);
    gfile *g = gf_get(fp);
    FILE *hf = gf_file(g);
    if (!hf || !src || !sz || !n) { c->eax = 0; cdecl_ret(c); return; }
    size_t put = fwrite(G(src), (size_t)sz, (size_t)n, hf);
    if (ferror(hf)) g->err = 1;
    c->eax = (uint32_t)put;
    cdecl_ret(c);
}

/* ---- seek ---- */
static void h_fseek(CPU *c)
{
    uint32_t fp = A(c, 0);
    int32_t off = (int32_t)A(c, 1);
    int whence = (int32_t)A(c, 2);
    gfile *g = gf_get(fp);
    FILE *hf = gf_file(g);
    if (!hf) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }
    int r = fseek(hf, (long)off, whence);
    if (r == 0) g->eof = 0;
    c->eax = (uint32_t)r;
    cdecl_ret(c);
}

static void h_ftell(CPU *c)
{
    uint32_t fp = A(c, 0);
    gfile *g = gf_get(fp);
    FILE *hf = gf_file(g);
    if (!hf) { c->eax = (uint32_t)-1; cdecl_ret(c); return; }
    long p = ftell(hf);
    c->eax = (uint32_t)p;
    c->edx = (uint32_t)((uint64_t)(int64_t)p >> 32);   /* 64-bit return value */
    cdecl_ret(c);
}

static void h_fflush(CPU *c)
{
    uint32_t fp = A(c, 0);
    if (!fp) {
        /* fflush(NULL) = flush everything */
        c->eax = (uint32_t)fflush(NULL);
        cdecl_ret(c);
        return;
    }
    gfile *g = gf_get(fp);
    FILE *hf = gf_file(g);
    c->eax = (uint32_t)(hf ? fflush(hf) : -1);
    cdecl_ret(c);
}

static void h_fileno(CPU *c)
{
    uint32_t fp = A(c, 0);
    gfile *g = gf_get(fp);
    c->eax = (uint32_t)(g ? g->fd : -1);
    cdecl_ret(c);
}

/* ---- stdin/stdout/stderr ---- */
static void h_acrt_iob_func(CPU *c)
{
    uint32_t idx = A(c, 0);
    static uint32_t cached[3];
    if (idx > 2) { c->eax = 0; cdecl_ret(c); return; }
    if (!cached[idx]) {
        FILE *hf = idx == 0 ? stdin : (idx == 1 ? stdout : stderr);
        cached[idx] = gf_alloc(hf, idx == 0 ? 0 : (idx == 1 ? 1 : 2));
    }
    c->eax = cached[idx];
    cdecl_ret(c);
}

/* ---- low-level write used by __stdio_common_vfprintf ---- */
int host_crt_stdio_write(uint32_t guest_fp, const void *buf, uint32_t n);
int host_crt_stdio_write(uint32_t guest_fp, const void *buf, uint32_t n)
{
    gfile *g = gf_get(guest_fp);
    FILE *hf = gf_file(g);
    if (!hf) return -1;
    return (int)fwrite(buf, 1, n, hf);
}

int host_crt_file_dispatch(CPU *__restrict c, const char *name)
{
    if (!name) return 0;
    const char *n = strchr(name, '!');
    n = n ? n + 1 : name;
    if      (!strcmp(n, "fopen"))          { h_fopen(c);        return 1; }
    else if (!strcmp(n, "fclose"))         { h_fclose(c);       return 1; }
    else if (!strcmp(n, "fread"))          { h_fread(c);        return 1; }
    else if (!strcmp(n, "fwrite"))         { h_fwrite(c);       return 1; }
    else if (!strcmp(n, "fseek"))          { h_fseek(c);        return 1; }
    else if (!strcmp(n, "ftell"))          { h_ftell(c);        return 1; }
    else if (!strcmp(n, "fflush"))         { h_fflush(c);       return 1; }
    else if (!strcmp(n, "_fileno"))        { h_fileno(c);       return 1; }
    else if (!strcmp(n, "__acrt_iob_func")){ h_acrt_iob_func(c);return 1; }
    return 0;
}

int host_crt_file_count(void) { return 9; }
