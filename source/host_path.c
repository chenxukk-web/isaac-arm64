/* Windows path semantics: case-insensitive.
 *
 * The game's resource paths were written on Windows (`gfx/ui/ScoreMenu.anm2`)
 * while filelist / in-archive names are lowercase (`gfx/ui/scoremenu.anm2`).
 * On Windows the two are equivalent; on Android's ext4/f2fs they are not -- so
 * fopen returns ENOENT outright, showing up as a wall of "cannot add reference
 * to ..." even though the file is right there on disk.
 *
 * This does case-insensitive resolution segment by segment: split on '/', look
 * for an exact match in the parent directory, and if that fails do one readdir
 * for a case-insensitive match. Results are cached (including negative ones),
 * so the second lookup of the same path is a single table probe.
 *
 * The returned pointer points into the cache and lives until that slot is
 * evicted; the caller must finish with it before the next call, or strdup it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>

#include "host_path.h"

#define CI_CACHE_N 1024

typedef struct {
    char *key;          /* original requested path */
    char *resolved;     /* path that actually exists; NULL means resolution failed (negative cache) */
} ci_ent;

static ci_ent g_ci[CI_CACHE_N];
static unsigned g_ci_n;
static unsigned g_ci_next;

static ci_ent *ci_find(const char *path)
{
    for (unsigned i = 0; i < g_ci_n; i++)
        if (g_ci[i].key && strcmp(g_ci[i].key, path) == 0) return &g_ci[i];
    return NULL;
}

static ci_ent *ci_store(const char *path, const char *resolved)
{
    ci_ent *e;
    if (g_ci_n < CI_CACHE_N) {
        e = &g_ci[g_ci_n++];
    } else {
        e = &g_ci[g_ci_next++ % CI_CACHE_N];
        free(e->key); free(e->resolved);
    }
    e->key = strdup(path);
    e->resolved = resolved ? strdup(resolved) : NULL;
    return e;
}

/* Find an entry in dir matching want case-insensitively; exact match wins */
static char *ci_lookup(const char *dir, const char *want)
{
    DIR *d = opendir(dir && *dir ? dir : ".");
    if (!d) return NULL;
    struct dirent *ent;
    char *found = NULL;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, want) == 0) { found = strdup(ent->d_name); break; }
        if (!found && strcasecmp(ent->d_name, want) == 0) found = strdup(ent->d_name);
    }
    closedir(d);
    return found;
}

/* ★ 2026-11-05: fast path.
 *
 * The original flow resolved **every** path segment by segment: split on '/',
 * then opendir + readdir the whole parent directory for a case-insensitive
 * match on each segment.
 *
 * Measurement: one round does 23029 fopens, and the vast majority of paths
 * already have the right case (filelist spells them the way the archive stores
 * them). Making all 20k lookups walk directories for the few mismatched ones is
 * a huge waste -- this was the main reason "loading is slower than translating"
 * on device.
 *
 * Now: try the original path with access(F_OK) first and return immediately if
 * it works. A hit costs one stat instead of 3~4 opendir+readdir. Only a path
 * that really does not exist takes the segment-by-segment route (which is when
 * case-insensitivity should show up at all).
 *
 * KNOWN ISSUE (2026-11-10): access(F_OK) lies on Android FUSE -- it returns
 * EACCES for files fopen() opens perfectly. Every other file switch in this
 * port was moved to fopen probing; this one was not. Treat a false negative
 * here as a performance regression (falls back to the slow path), not as a
 * correctness bug. */
static int path_exists(const char *p)
{
    return access(p, F_OK) == 0;
}

const char *host_path_resolve(const char *path)
{
    if (!path || !*path) return path;

    ci_ent *hit = ci_find(path);
    if (hit) return hit->resolved;

    if (path_exists(path)) {
        ci_store(path, path);      /* exact hit, cache it */
        return path;
    }

    size_t len = strlen(path);
    char *work = malloc(len + 1);
    char *acc  = malloc(len + 2);
    if (!work || !acc) { free(work); free(acc); return path; }
    memcpy(work, path, len + 1);
    acc[0] = 0;

    char *seg = work;
    if (path[0] == '/') { strcpy(acc, "/"); seg = work + 1; }

    int ok = 1;
    while (seg && *seg) {
        /* ★ 2026-11-03: this used to be `if (!*seg) break;` -- on "a//b" it
         * would zero *seg at the first '/' and treat that as end of path,
         * returning the directory "a" as resolved. The game really does build
         * paths like "./Documents/.../Repentance//inputconfigs.dat", so fopen
         * opened a directory, fread read nothing, and the buffer kept the
         * previous occupant's bytes -> rapidxml reports "expected <" -> throws
         * -> abort. Consecutive slashes must be skipped, not treated as end. */
        if (*seg == '/') { seg++; continue; }
        char *slash = strchr(seg, '/');
        if (slash) *slash = 0;
        if (!*seg) break;

        char *match;
        if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) {
            match = strdup(seg);
        } else {
            match = ci_lookup(acc[0] ? acc : ".", seg);
        }
        if (!match) { ok = 0; break; }

        size_t al = strlen(acc);
        if (al && acc[al - 1] != '/') acc[al++] = '/';
        strcpy(acc + al, match);
        free(match);

        if (!slash) { seg = NULL; break; }
        seg = slash + 1;
    }

    if (ok) {
        ci_ent *stored = ci_store(path, acc);
        free(work);
        free(acc);
        return stored ? stored->resolved : path;
    }
    free(work);
    free(acc);

    /* ====================================================================
     * Prefix fallback: packed/x.a -> resources/packed/x.a
     *
     * 2026-11-04: the game's DLC archive paths are written as
     * "packed/<name>.a", but the resources actually live in
     * "resources/packed/". Both prefixes coexist in the PE constant pool --
     * the base game uses resources/packed/<name>.a, AB+ and Repentance use
     * packed/<name>.a -- that is not a typo.
     *
     * The Termux dev environment got away with a symlink chain
     *     packed -> rt/packed -> resources/packed
     * but that link **never makes it into the APK**: Android's
     * /storage/emulated/0 is FUSE and does not support symlinks. On device,
     * fopen("packed/afterbirthp.a") necessarily returns ENOENT, so no DLC
     * opens at all: the log goes back to 19 "Failed to open" lines,
     * backdrops.xml reads the old version out of afterbirth.a, gridgfxroot
     * returns NULL, and it crashes at 0x48d604.
     *
     * So the fallback lives in the host layer instead, depending on no
     * filesystem feature, and holds in both environments. Only one level deep:
     * targets already starting with "resources/" are not recursed, and
     * absolute paths are excluded, so resources/resources/... and special
     * files are left alone.
     * ==================================================================== */
    if (path[0] != '/' && strncmp(path, "resources/", 10) != 0) {
        char alt[512];
        int n = snprintf(alt, sizeof alt, "resources/%s", path);
        if (n > 0 && n < (int)sizeof alt) {
            const char *r = host_path_resolve(alt);
            /* When resolution fails host_path_resolve returns the input itself
             * (the local alt), so pointer equality distinguishes the cases --
             * only a genuinely resolved result is cached as a fallback. */
            if (r && r != alt) {
                ci_store(path, strdup(r));
                return r;
            }
        }
    }
    ci_store(path, NULL);
    return path;
}
