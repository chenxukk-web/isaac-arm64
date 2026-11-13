/* Windows 路径语义：大小写不敏感。
 *
 * 游戏的资源路径是 Windows 上写照的（`gfx/ui/ScoreMenu.anm2`），而
 * filelist / 包内文件名是小写（`gfx/ui/scoremenu.anm2`）。Windows 上
 * 两者等价，Android 的 ext4/f2fs 不是 —— 于是 fopen 直接 ENOENT，
 * 表现成一整片 "cannot add reference to ..."，而文件其实就在磁盘上。
 *
 * 这里做逐段的大小写不敏感解析：按 '/' 切开，每段先在父目录里找
 * 精确匹配，找不到就 readdir 一次做不敏感匹配。命中结果缓存
 * （包括负结果），同一个路径第二次就是一次查表。
 *
 * 返回的指针指向缓存内部，生命周期到该槽位被淘汰为止；
 * 调用方必须在下次调用前用完，或自己 strdup。
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
    char *key;          /* 原始请求路径 */
    char *resolved;     /* 实际存在的路径；NULL 表示解析失败（负缓存） */
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

/* 在 dir 里找一个与 want 不区分大小写相同的条目；精确匹配优先 */
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

/* ★ 2026-11-05：快速路径。
 *
 * 原来的流程对**每个**路径都做逐段解析：按 '/' 切段，每段
 * opendir + readdir 整个父目录做大小写不敏感匹配。
 *
 * 实测一轮要 fopen 23029 次，而绝大多数路径的大小写本来就是对的
 * （filelist 里怎么写的，包里就怎么存的）。为了那少数几个大小写
 * 不一致的文件，让全部 2 万次都去遍历目录，是巨大的浪费 ——
 * 这是真机上"加载比转译还慢"的主要原因。
 *
 * 现在：先用 access(F_OK) 试原始路径，能打开就直接返回。
 * 命中时开销是一次 stat，而不是 3~4 次 opendir+readdir。
 * 只有真的不存在时才走逐段解析（那才是大小写不敏感该出场的时候）。 */
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
        ci_store(path, path);      /* 原样命中，缓存下来 */
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
        /* ★ 2026-11-03：原来是 `if (!*seg) break;` —— 遇到 "a//b" 时
         * 会在第一个 '/' 处把 *seg 置 0 然后当成路径结束，返回的
         * resolved 是目录 "a"。游戏拼路径时确实会产出
         * "./Documents/.../Repentance//inputconfigs.dat"，于是
         * fopen 打开了目录，fread 读不出内容，缓冲区留着上一任的
         * 字节 → rapidxml 报 "expected <" → 抛异常 → abort。
         * 连续斜杠必须跳过，不能当成结束。 */
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
     * 前缀回落：packed/x.a -> resources/packed/x.a
     *
     * 2026-11-04：游戏的 DLC 归档路径写的就是 "packed/<name>.a"，而资源实际
     * 放在 "resources/packed/"。PE 常量表里两种前缀并存 —— 本体是
     * resources/packed/<name>.a，AB+ 和忏悔是 packed/<name>.a —— 这不是笔误。
     *
     * Termux 开发环境里靠一条符号链接链
     *     packed -> rt/packed -> resources/packed
     * 蒙过去了，但那条链接**不会进 APK**：Android 的 /storage/emulated/0
     * 是 FUSE，不支持符号链接。真机上 fopen("packed/afterbirthp.a")
     * 必然 ENOENT，于是 DLC 一个都打不开，日志回到 19 条 Failed to open、
     * backdrops.xml 又读到 afterbirth.a 里的旧版、gridgfxroot 返回 NULL、
     * 崩在 0x48d604。
     *
     * 所以在宿主层做这层回落，不依赖文件系统特性，两个环境都成立。
     * 只回落一层：目标以 "resources/" 开头时不再递归，
     * 且绝对路径不参与，避免 resources/resources/... 和特殊文件被打扰。
     * ==================================================================== */
    if (path[0] != '/' && strncmp(path, "resources/", 10) != 0) {
        char alt[512];
        int n = snprintf(alt, sizeof alt, "resources/%s", path);
        if (n > 0 && n < (int)sizeof alt) {
            const char *r = host_path_resolve(alt);
            /* 解析失败时 host_path_resolve 返回入参本身（局部 alt），
             * 用指针相等就能区分 —— 只有真解析成功才缓存回落结果。 */
            if (r && r != alt) {
                ci_store(path, strdup(r));
                return r;
            }
        }
    }
    ci_store(path, NULL);
    return path;
}
