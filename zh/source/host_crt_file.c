/* Android CRT 层 —— stdio 文件族
 *
 * 游戏在启动早期就要建存档目录、读 config、打开资源文件，所以这一层
 * 是硬需求，不是可选项。
 *
 * 核心约束：guest 的 FILE* 是 uint32_t，必须指向低 4GB 内存。所以这里
 * 不返回宿主 FILE*，而是返回一块低 4GB 的 guest_file 小结构，里面存
 * 宿主 FILE* 和 fd。所有 stdio 导入都经它中转。
 *
 * 路径也是 guest 指针（ANSI，相对当前工作目录）。游戏用 "./Documents/..."
 * 这类相对路径，所以运行时 cwd 必须正确 —— 见 continue_android_build.sh。
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

/* ---- 低 4GB 的 guest FILE 表 ---- */
#define GF_BASE 0x3A100000u
#define GF_MAX  512u
#define GF_SIZE 0x10000u

typedef struct {
    uint32_t host_fp;      /* 宿主 FILE* 截断成 32 位会丢，所以索引回表 */
    int32_t  hindex;       /* 指向 g_hostfp[]；-1 表示已关闭 */
    int32_t  fd;
    int32_t  eof;
    int32_t  err;
    int32_t  in_use;
    int32_t  xml_mode;    /* 1 = 这个流读的是 XML，需要引号替换（见 h_fread） */
    int32_t  cache_slot;  /* >=0 表示这个句柄指向常驻缓存槽；-1 表示私有 FILE* */
} gfile;

/* ★ 2026-11-05：句柄表必须加锁。
 *
 * 游戏有 2 个 worker 线程，fopen/fclose 会从不同线程进来。
 * 原来 gf_alloc 用 `for (...) if (!in_use) { in_use = 1; return; }`
 * 无锁分配 —— 两个线程可能同时看到同一个空槽，拿到**相同的 guest
 * FILE 地址**，然后互相覆盖 host_fp / hindex，最终在某个 FILE* 上
 * 交错 fseek/read，表现为数据错乱乃至死锁。
 *
 * 这把锁只包住"分配/回收槽位"这一小段，不包 I/O，代价可以忽略。 */
static pthread_mutex_t g_gf_mu = PTHREAD_MUTEX_INITIALIZER;

static gfile   *g_gtab;                /* guest 侧（低 4GB） */
static FILE   **g_hostfp;              /* 宿主侧，按 hindex 索引 */
static unsigned g_hfp_n;               /* 已用过的 hindex 上界 */
static unsigned *g_free_hidx;          /* 回收的 hindex 栈 */
static unsigned  g_free_n;
static int      g_tab_ready;

/* ==========================================================================
 * 常驻文件缓存（2026-11-05）
 *
 * 现象：真机/本地日志里，归档文件的打开次数与条目数**一对一**
 *       afterbirthp.a  5512 条目 → 5513 次 fopen
 *       repentance.a   4180 条目 → 4181 次 fopen
 *       一轮下来 15573 次 fopen/fclose。
 *
 * 这是游戏 KAGE 的 Archive::extract 本身的行为：每取一个条目就
 * 打开一次归档文件、定位到索引、读记录、定位到数据、解压、关闭。
 * 在 Windows 上靠系统文件缓存几乎免费，在 Android 的 FUSE 上很贵。
 *
 * 做法：保留最近用过的 N 个 FILE*，fclose 时不真关，下次 open 同路径
 * 直接 rewind 复用。语义与 open/close 完全一致（从头开始读），
 * 只在"同一路径同时被打开两次"时退化 —— 那种情况会分配独立 FILE*，
 * 因为槽位带 refs 计数，被占用就不会复用。
 *
 * 边界：槽位固定 16 个、路径 256 字节。归档只有 9 个，够用。
 * ========================================================================== */
#define FCACHE_N 16
#define FCACHE_PATH 256

typedef struct {
    char  path[FCACHE_PATH];
    FILE *fp;
    int   refs;      /* >0 表示有 guest 句柄正在用 */
} fcache_ent;

static fcache_ent g_fcache[FCACHE_N];
static unsigned   g_fcache_next;

/* 找一个可用的槽：同路径且空闲优先；否则找空槽 */
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
    /* 全满：淘汰一个空闲的（refs==0） */
    for (int k = 0; k < FCACHE_N; k++) {
        int i = (int)((g_fcache_next + (unsigned)k) % FCACHE_N);
        if (g_fcache[i].refs == 0) {
            if (g_fcache[i].fp) fclose(g_fcache[i].fp);
            g_fcache[i].fp = NULL;
            g_fcache[i].path[0] = 0;
            return i;
        }
    }
    return -1;      /* 全部在用，放弃缓存 */
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

/* 分配一个 guest FILE，返回 guest 地址（0 = 失败） */
static uint32_t gf_alloc(FILE *hf, int fd)
{
    if (gf_init() != 0) return 0;
    /* hindex 必须可回收：原来它只增不减，累计打开 512 个文件之后
     * 分配就永久失败，而且失败分支不打印 —— 表现成「第 512 次打开
     * 之后所有资源都读不到」，极难看出是句柄表的问题。 */
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
    /* 有 hidx 却没槽位：不可能，但把 hidx 还回去别泄漏 */
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
 * ★★ 2026-11-12：大小写不敏感的 fopen 回退 —— **字体丢失的根因修复**
 *
 * 证据链：
 *   1. 12 个 .fnt 里有 **9 个**引用的 PNG 文件名和实际文件大小写不符：
 *        teammeatfont16.fnt 里写的是 "TeamMeatFont16_0.png"
 *        实际文件名是            "teammeatfont16_0.png"
 *      （luamini / pftempesta / upheaval / teammeat* 全是这个情况）
 *   2. 字体图集上传时 **RGB 和 alpha 同时为 0** —— 不是 premultiply 清的，
 *      是 ImagePng 压根没读到像素。
 *   3. 移植版（Windows / iOS）没问题 —— NTFS 和 APFS 都**不区分大小写**，
 *      所以同一个名字能找到文件。**Android 的 ext4/F2FS 区分大小写。**
 *
 * 表现：字体图集是空图 → shader 采样 alpha=0 → 所有字符取到同一处
 *       → HUD 数字全渲染成同一个 µ；主菜单列表整片空白。
 *
 * 修法：fopen 失败后，按路径逐级目录用 readdir 做不区分大小写的匹配。
 * 只在**原样打开失败**时才走这条路，正常路径零成本。
 * ========================================================================== */
static FILE *ci_fopen(const char *path, const char *mode)
{
    if (!path || !*path) return NULL;
    if (!strchr(mode, 'r')) return NULL;   /* 只对读模式做回退 */

    /* 最多 16 级目录；路径长度上限 512 */
    char cur[512];
    if (strlen(path) >= sizeof cur) return NULL;

    /* 绝对路径从 "/" 开始，相对路径从 "." 开始 */
    const char *p = path;
    if (p[0] == '/') { snprintf(cur, sizeof cur, ""); }
    else             { snprintf(cur, sizeof cur, "."); }

    int levels = 0;
    while (*p && levels < 16) {
        /* 跳过重复的 '/' */
        while (*p == '/') p++;
        if (!*p) break;

        /* 取出这一级组件 */
        char comp[256];
        size_t n = 0;
        while (*p && *p != '/' && n + 1 < sizeof comp) comp[n++] = *p++;
        comp[n] = 0;
        if (n == 0) break;

        char next[512];
        snprintf(next, sizeof next, "%s/%s", cur, comp);

        /* 精确命中就用精确的 */
        struct stat st;
        if (stat(next, &st) == 0) {
            snprintf(cur, sizeof cur, "%s", next);
            levels++;
            continue;
        }

        /* 否则在这一级目录里找一个不区分大小写的匹配 */
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

/* ---- 打开 / 关闭 ---- */
static void h_fopen(CPU *c)
{
    uint32_t pathp = A(c, 0), modep = A(c, 1);
    if (!pathp || !modep) { c->eax = 0; cdecl_ret(c); return; }
    /* Windows 语义：大小写不敏感。解析失败再原样试一次，
     * 这样绝对路径 / 特殊文件（/dev/null 之类）不会被破坏。 */
    const char *resolved = host_path_resolve(G(pathp));
    const char *use = resolved ? resolved : G(pathp);
    /* 目录不是文件：Linux 的 fopen(dir, "rb") 会成功，随后 fread 失败，
     * 调用方拿到的是没被写过的缓冲区 —— 那种失败比 ENOENT 难查得多。 */
    {
        struct stat st;
        if (stat(use, &st) == 0 && S_ISDIR(st.st_mode)) {
            fprintf(stderr, "[crt] fopen(\"%s\") resolved to directory \"%s\"; refusing\n",
                    G(pathp), use);
            c->eax = 0; cdecl_ret(c); return;
        }
    }
    /* 先看常驻缓存：同一路径反复开关时直接复用 FILE*。
     * 只对只读模式生效 —— 写模式语义不同，不能共用流。 */
    {
        const char *mode = G(modep);
        int readonly = mode && (mode[0] == 'r') && strchr(mode, '+') == NULL
                       && strchr(mode, 'w') == NULL && strchr(mode, 'a') == NULL;
        /* ★ 2026-11-05：禁用常驻句柄缓存。
         *
         * 真机症状：进主界面后彻底卡死（draw 停在 #16800，fread 和帧流
         * 都静止，但进程还活着 —— 死锁）。
         *
         * 原因：游戏有 2 个 worker 线程（日志里 `线程启动` 出现 2 次），
         * 而这个缓存**没有任何锁**。两个线程同时 fopen 同一归档时：
         *   · fcache_acquire 可能返回同一个槽
         *   · 两个线程共用同一个 FILE*，fflush/fseek/read 交错
         *   · FILE* 内部锁互相阻塞 → 死锁
         *
         * 而且实测它并没有带来收益：fopen 从 15573 降到 2，
         * 但加载时间一点没变（第二十二节已记录）。零收益 + 高风险的
         * 组合，直接关掉，回归已知可用的行为。
         *
         * 将来若要重启用，前提是：给 g_fcache 加 pthread_mutex，
         * 并且 refs 只能是"独占打开"语义（同一路径同时两次打开时
         * 必须各给独立 FILE*，不能共用）。 */
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
                    /* 复用：回到文件开头。这正是 open/close 的语义。 */
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
                /* 缓存路径失败就落到下面的普通 fopen */
            }
        }
    }

    FILE *hf = fopen(use, G(modep));
    /* ★★ 2026-11-12：记录所有 fopen 的**真实路径**（前 200 条）。
     * 大小写回退 0 次命中，说明字体不是走 fopen 读散文件的 ——
     * 必须看清游戏到底请求了哪些路径。
     * 上限 200 条，避免把加载期刷爆。 */
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
        /* ★ 2026-11-12：大小写不敏感回退（见 ci_fopen 的注释）。
         * 字体 .fnt 里引用 "TeamMeatFont16_0.png"，实际文件是小写 ——
         * Android 区分大小写，Windows/iOS 不区分，这就是字体丢失的根因。 */
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
    /* ★ 2026-11-05：默认静默。
     *
     * 原来每次成功 fopen 都打一行。实测真机一轮要 fopen 23029 次
     * （9 个归档 × 每个条目一次），就是 23029 行 stderr；
     * 而 stderr 被 Java 转发写进 run.log，那个文件在 /storage/emulated/0
     * 上（FUSE，比 ext4 慢一个数量级）。等于每打开一个归档条目
     * 就附带一次慢速写盘 —— 这本身就是加载阶段的巨大负担。
     *
     * 排查路径问题时用 FOPEN_TRACE=1 打开。 */
    {
        static int on = -1;
        if (on < 0) on = getenv("FOPEN_TRACE") != NULL;
        static unsigned long n_ok;
        n_ok++;
        /* 每 5000 次打一条进度。
         * 目的不是排查路径，而是让"归档加载阶段"在屏幕上看得见 ——
         * 这个阶段本来要 2~3 分钟且一行输出都没有，看起来像卡死。 */
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
        /* 缓存槽：保留 FILE* 不关，只递减引用计数。
         * 下次 fopen 同路径时 rewind 复用 —— 省掉一次 open/close。 */
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

/* ---- fread 范围记录（供 GL 层判定纹理数据来源）---- */
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

/* ptr 是否落在最近某次 fread 的目标范围内。
 * 返回 1 表示“在范围内”（说明这块内存被文件读取覆盖过），
 * 0 表示“不在”（PE 内置数据，或从未 fread 过）。
 * 只查最近 512 次，避免长循环。 */
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

/* ---- 读写 ---- */
static void h_fread(CPU *c)
{
    uint32_t dst = A(c, 0), sz = A(c, 1), n = A(c, 2), fp = A(c, 3);
    gfile *g = gf_get(fp);
    FILE *hf = gf_file(g);
    if (!hf || !dst || !sz || !n) { c->eax = 0; cdecl_ret(c); return; }
    /* 计时探针：看时间到底花在 I/O 还是别处。每 2000 次报一条。 */
    static unsigned long n_read, n_bytes_note;
    static unsigned long long us_total;
    static unsigned long n_png, n_zero;   /* PNG 签名块 / 全零大块 计数 */

    /* ★★ 2026-11-10：启动提速 —— 重探针改成默认关闭。
     *
     * 旧代码每次 fread 都做：
     *   · 2 次 clock_gettime(CLOCK_MONOTONIC)     ~50 ns
     *   · PNG 签名检查（4 字节比较）
     *   · bytes>=1024 时扫前 32 字节判全零      ~20 ns
     * 而 Isaac 加载一轮要调 **2400 万次** fread：
     *   2400万 × 70ns ≈ 1.7 秒，纯属诊断开销。
     *
     * 这些探针已经完成使命（已确认 PNG 签名块 16 次、
     * 全零大块 80 次，数据层没有问题）。现在默认关闭，
     * 需要时用 IO_PROBE=1 开启。
     *
     * 保留的只有字节累计计数（几次加法，可忽略），
     * 因为“读了多少 MB”对判断资源加载是否完成有用。 */
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
    /* ★★ 2026-11-10：记录 fread 目标范围，供 GL 层判定“纹理数据从哪来”。
     *
     * 问题：103 张纹理全零（21 张 512x512、7 张 1024x512），
     * 而同样 64x64 的岩浆纹理（id=41）完全正确 —— 说明解码管道通，
     * 但大纹理数据是零。需要知道：那些零是**从文件读出来的**，
     * 还是**解压/解码写出来的**。两者方向完全不同：
     *   · 文件读出来就是零 → 归档读取/定位有问题
     *   · 文件有数据但解码后为零 → 解压器或某个导入桩有 bug
     *
     * 做法：环形记录最近 4096 次 fread 的 (目标, 大小)。GL 层遇到
     * 全零纹理时反查 ptr 是否落在这些范围里。
     * 开销：每次 fread 两次赋值，2400 万次约 50ms，可接受。 */
    fread_record(dst, (uint32_t)(got * (size_t)sz));
    {
        if (++n_read % 200000UL == 0UL) {
            fprintf(stderr, "[io] fread total %lu calls, %lu MB, host time %llu ms "
                    "| PNG signature blocks=%lu all-zero blocks=%lu\n",
                    n_read, n_bytes_note / (1024UL * 1024UL), us_total / 1000ull,
                    n_png, n_zero);
            fflush(NULL);
        }
        /* ★ 2026-11-06：贴图全零的第二层探针。
         *
         * 已知：HEAP_POISON=1 下 glTexImage2D 的像素仍是 0x00（不是 0xCD），
         * 说明解码器确实写入了零 —— 那就得看它是**吃到了零**还是**吐出了零**。
         *
         * 这里在每次 fread 后看 4 字节：
         *   · PNG 签名块  = 读到 89 50 4E 47 的次数
         *   · 全零大块    = 单次读入 >= 1KB 且前 32 字节全零的次数
         * 两者比例直接回答“输入是零，还是解码出错”。
         * 只读 4~32 字节，开销可忽略。 */
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
     * XML 引号替换（workaround）
     *
     * 第十三节查明：解析器在"属性值以 / 结尾"时，把值末尾的 '/' 误判成
     * 自闭合标记 "/>"，直接跳到元素末尾，吞掉后面所有属性。
     * <backdrops gfxroot="gfx/backdrop/" gridgfxroot="gfx/grid/"> → 只剩第一个。
     *
     * 症状根源是值边界的 off-by-one（起始没跳过分隔符、长度按跳过算）。
     * 换成单引号后，值边界字符不同，可以绕开这条路径 —— XML 标准里
     * ' 与 " 完全等价，语义不变。
     *
     * 只对"看起来是 XML"的流生效：第一次读到以 '<' 开头的块就锁定该流。
     * 必须在**每个** 4 字节小块里替换 —— 这些 XML 是逐块流式读的，
     * 引号是单字节，不会跨块。
     * ==================================================================== */
    if (0) {   /* 临时禁用排查 */
        static int xml_quirk = -1;
        /* 默认关闭：2026-11-03 实测它确实让解析走得更远（不再崩在属性查找），
         * 但引出下一个 bug —— 某处 malloc(1879 MB) → bad_alloc → abort。
         * 说明"值边界 off-by-one"的方向成立，但换引号只是绕过边界字符，
         * 没有真正修掉指针偏移。用 XML_QUOTE_FIX=1 可复现这条路径。 */
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
    /* XML 探针：解压/读取出来的内容若以 '<' 开头，检查首尾是否完整。
     * 截断的 XML 会让 rapidxml 构建出畸形节点树，后果是后续遍历
     * 读到野指针 —— 但报错点离真正原因很远。 */
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
            /* 首 48 字节 + 末 48 字节（判断是否以 '>' 收尾） */
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

/* ---- 定位 ---- */
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
    c->edx = (uint32_t)((uint64_t)(int64_t)p >> 32);   /* 64 位返回值 */
    cdecl_ret(c);
}

static void h_fflush(CPU *c)
{
    uint32_t fp = A(c, 0);
    if (!fp) {
        /* fflush(NULL) = 全部刷新 */
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

/* ---- 供 __stdio_common_vfprintf 使用的低层写 ---- */
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
