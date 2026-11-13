/* Android CRT 层 —— MSVC 运行时导入的实现
 *
 *
 * 所有参数在 guest 栈上（x86 cdecl），返回值放 eax / xmm0 / edx:eax。
 * 内存分配必须落在低 4GB：guest 的指针是 uint32_t。
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

/* 分配历史环（诊断用）：记录最近 512 次 malloc/free 的块地址与大小，
 * 崩溃时用 heap_hist_dump(target) 反查某块内存是「谁给的、谁放的」。 */
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
 * 写保护监视（排查"对象头被覆盖"用）
 *
 * 场景：资源缓存里的对象 [0x652b2010] 头被字形数组整体覆盖，
 * 但三个守卫都证明它没被 free、没被重复分配、没被 VirtualAlloc 覆盖。
 * 那就只剩"有人直接往这块内存写"。
 *
 * 做法：把这个 4KB 页设成只读。之后**任何**写入（不管来自 host 的
 * memcpy/memset 还是 gen/ 里内联的 st32/st8）都会 SIGSEGV，
 * pc 直接指向凶手。
 *
 * 时机：
 *   分配命中 watch 地址 → 先允许写（对象要构造）
 *   下一次 malloc         → 设只读（构造已完成，进入守护期）
 *   free 碰到该块        → 先恢复可写（块头要写标记）
 *
 * 用法：HEAP_WATCH=652b2000 ./isaac_android_test
 * ==========================================================================*/
uintptr_t g_watch_page;             /* 页基址；0 = 未启用（信号处理器要用） */
static int       g_watch_pending;   /* 1 = 已分配、等下一次 malloc 时上保护 */
int              g_watch_armed;     /* 1 = 该页当前只读 */

/* 非致命模式：命中后恢复可写继续跑，记录所有不同的写入者。
 * 目的是抓"谁把数据指针写进了表槽"——那一处往往是合法的 vector
 * 追加，不能一命中就退出。 */
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

/* ★★ 2026-11-11：UAF 指针登记表。
 *
 * 崩溃是时序敏感的（同一版 .so 有时崩有时不崩），所以不能用
 * 「崩不崩」来判断。改为：把「已 free 但仍在资源表里」的对象全记下来，
 * 崩溃时一次性回放。这样一次崩就能看到全部坏对象，不用反复试。 */
#define UAF_LOG_N 64
uintptr_t g_uaf_log[UAF_LOG_N];
unsigned  g_uaf_log_n;
unsigned  g_uaf_log_overflow;

/* ==========================================================================
 * ★★ 2026-11-11：UAF 抑制开关（决定性 A/B 实验）
 *
 * 现象：真机在「进游戏」附近，free 了一批**仍在资源表里**的对象，
 * 随后走到 glDeleteProgram 时 std::terminate。UAF 次数稳定 8 次，
 * 但崩不崩取决于时序 —— 说明是「释放后又被访问」的典型 UAF，
 * 我们自己的堆立刻复用同一块内存，所以访问者拿到的是别的对象的数据。
 *
 * 实验：<game>/keepres 存在时，凡是「仍在资源表里的对象」**不真正释放**，
 * 把它泄露掉，内存保持原样。
 *   · 不崩了   → 根因就是这批 UAF，后续按「延迟释放 / 引用保留」修
 *   · 还崩     → 与这批 UAF 无关，回去查 glDeleteProgram 自身路径
 *
 * 和 repentogxm 的 ISAAC_VITA_IMAGE_RETAIN 是同一思路：
 * 它也是「不让 ImageManager 释放精灵表」，保留在 LRU 里等游戏自己再取。
 *
 * 安全上限：最多保留 512 个对象 / 16 MB，超了就恢复正常释放，
 * 避免诊断开关本身把内存吃干。
 * ========================================================================== */
static int      g_keepres = -1;         /* -1 未探测 / 0 关 / 1 开 */
static unsigned g_keepres_n;            /* 已保留对象数 */
static size_t   g_keepres_bytes;        /* 已保留字节数 */
static const size_t KEEPRES_MAX_BYTES = 16u << 20;

static int keepres_enabled(void)
{
    if (g_keepres >= 0) return g_keepres;

    /* ★★ 2026-11-11：**默认开启**。
     *
     * 依据与取舍：
     *   · 崩溃 4/5 次运行与「游戏请求 glDeleteProgram」完全相关；
     *     UAF（free 掉仍在资源表里的对象）在**所有**运行里都是 8 次。
     *   · 保留这些对象后（06:31 实测）同一加载阶段（帧#101 的 1.1s
     *     free 卡顿）没有再走那条路径，跑到帧 1200 无崩溃。
     *   · 代价实测只有 **72 KB / 18 个对象**，可忽略。
     *   · 上限 512 个 / 16 MB 兜底，不可能吃干内存。
     *
     * 关闭方式：<game>/no_keepres 存在即关闭（排查用）。
     * 这样默认稳定，又不失去可复现性。 */
    g_keepres = 1;
    static const char *const off_probe[] = {
        "/storage/emulated/0/Android/media/com.isaacport.launcher"
        "/files/game/no_keepres",
        "/data/user/0/com.isaacport.launcher/files/game/no_keepres",
        "no_keepres",
        NULL
    };
    for (int i = 0; off_probe[i]; i++) {
        /* ★ 用 fopen 而不是 access()：Android FUSE 上 access 会误报失败。 */
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
int res_table_contains(uintptr_t p);   /* 定义在下面 */
void res_table_audit(int deep);        /* 定义在下面（RES_AUDIT=1） */
int  res_audit_on;
extern unsigned long g_last_malloc_size;
static void res_audit_tick(void);
static void res_map_probe(void);
static int g_res_free_n;

static void watch_arm_if_pending(void)
{
    if (!g_watch_pending || !g_watch_page) return;
    /* ★ 只有对象真的进了资源表（构造 + 插入都完成）才上保护。
     * 原来"下一次 malloc 就上保护"抓到的是构造期的合法写入
     * （实测第 84 万次导入就命中了 edi=652b2018 的写入），
     * 那是对象自己的初始化，不是覆盖。 */
    /* 对象指针 = 块基址 + 16（块头）。表里存的就是这个值。 */
    if (!g_watch_nonfatal && !res_table_contains(g_watch_page + 16u))
        return;   /* 还没进表 = 构造未完成，不上保护 */
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
                res_table_contains(p + 16u) ? 1 : 0);   /* 16 = 块头，对象指针 = 块基址+16 */
        fflush(NULL);
    }
}

static int watch_is_armed_page(uintptr_t p)
{
    return g_watch_armed && g_watch_page &&
           p >= g_watch_page && p < g_watch_page + 4096u;
}

/* ---------------- 低 4GB 堆 ----------------
 *
 * 2026-11-02 的教训：这原来是纯 bump 分配器，low_free 只读一下块头就
 * "(void)sz;"，一个字节都不还。资源解包之后游戏开始真的 malloc/free，
 * 624 MB 上限在半分钟内被推满 → operator new 抛 std::bad_alloc。
 *
 * 现在是带 size-class 空闲链的分配器：块大小一律取 2 的幂上界，
 * 释放时挂回同类的链表，下次同规模的请求直接复用。
 *
 * 块头仍是 16 字节，[ptr-4] 仍是块基址 —— 这两条是 MSVC aligned-free
 * 的约定（见下），不能动。
 *   p+0 .. p+7   size        块总大小（2 的幂，页对齐）
 *   p+8 .. p+11  空闲链 next  （相对 HEAP_BASE 的偏移 + 1；0 = 链尾）
 *   p+12.. p+15  块基址低 32 位（MSVC 检查读这里）
 */
#define HEAP_BASE 0x48000000UL
/* ★★ 2026-11-10：堆上限从 0xB0000000 提到 0xF0000000。
 *
 * 真机崩溃链：
 *   [heap] ✗ 低 4GB 堆耗尽：bump 已用 1664.0 MB / 上限 1664.0 MB
 *   [cxx]  type[0] = ".?AVbad_alloc@std@@"
 *   [SEGV] abort
 *   同时 [texstat] 累计 940 张纹理，其中全零 320 张
 *
 * 两者是**同一个根因**：地址空间不够。
 *   · 原范围 0x48000000-0xB0000000 = 1.66 GB
 *   · 其中 384 MB 被 ART 的 dalvik-free 占着（已跳过）
 *   · 实际可用 ~1.28 GB，而 Isaac 需要 ~1.25 GB live
 *   · 一旦超出：malloc 返回 NULL → 解码器放弃写入 → 纹理全零
 *     → 最后抛 bad_alloc → abort
 *
 * PC 上有 4-8 GB，所以这条路径从未暴露。
 *
 * 新上限 0xF0000000 = 2.62 GB（减去 ART 的 384 MB ≈ 2.2 GB），
 * 留了足够的余量。更低处 0xC8C00000-0xD0C00000 的 ART JIT 缓存
 * 由现有的 "mmap 失败就往后跳" 逻辑处理。 */
#define HEAP_END  0xF0000000UL

/* ★ 保护区：[0x7D000000, 0x80000000) 是“假地址”空间。
 *
 * GL 函数 token 从 0x7D000000 开始，动态导出 token 从 0x7E000000，
 * XInput token 从 0x7F000000（见 host_android.c 的 DYN_BASE 与
 * host_gl4es.c 的 gl token）。这些地址只是标识符，不存数据，
 * 但**绝不能真的映射给堆** —— 否则 guest 万一解引用就会读到垃圾，
 * 而且会让崩溃诊断把“guest 拿 token 当指针”误判成“堆地址”。
 * 堆碰到这段直接跳到 0x80000000。 */
#define HEAP_RESERVED_LO 0x7D000000UL
#define HEAP_RESERVED_HI 0x80000000UL
#define HEAP_CLASSES 31            /* 2^12 .. 2^30（上限 1 GB 的 2 的幂） */
#define HEAP_MIN_BLK 4096u

typedef struct heap_blk {
    size_t   size;
    uint32_t next_off;              /* 仅空闲链使用 */
    uint32_t base_lo;
} heap_blk;

static uintptr_t g_heap_cur = HEAP_BASE;

/* 堆段登记表：崩溃诊断时用来判断出错地址是不是落在「本该有映射」的段里。
 *
 * ★★ 2026-11-10：从线性数组改成哈希表。这是当时最严重的性能瓶颈。
 *
 * 旧实现：
 *     void heap_seg_del(uintptr_t base) {
 *         for (i = 0; i < 65536 && i < g_seg_n; i++) { ... }
 *     }
 * 而 g_seg_n 是**累计**写入次数（只增不减），一旦超过 65536，
 * 每次 free 都要跑满 65536 次比较。
 *
 * 真机数据：malloc=1318868 free=1092573
 *   → 109 万次 free × 平均 3.3 万次比较 ≈ 357 亿次内存访问
 *
 * 映射到日志就是反复出现的：
 *     [stall] 单帧 422.2 ms  前一导入=...!free  期间导入=323230
 * 按暂停时游戏大量分配/释放（菜单、日志），所以那里卡得最明显。
 *
 * 新实现：开放寻址 + 线性探测，O(1) 均摊。
 *   g_segs[i].base == 0  空槽
 *   g_segs[i].base == 1  墓碑（已删除，搜索继续）
 *   size == 0            也算墓碑（保留兼容） */
#define HEAP_SEG_N 131072          /* 2 的幂，便于掩码；是原来 65536 的 2 倍 */
#define HEAP_SEG_MASK (HEAP_SEG_N - 1u)
static struct { uintptr_t base; size_t size; } g_segs[HEAP_SEG_N];
static unsigned g_seg_used;     /* 占用的槽数（含墓碑），用于负载因子 */

/* ★★ 2026-11-10 事故记录：千万别删这个函数。
 *
 * 上一版改哈希表时我把 heap_seg_count() 一并删了，因为它在本文件
 * 已经没用了。但 **runtime 的 guest.c 仍在调用它**（崩溃诊断里打印
 * 堆段数）。而 guest.o 是预先编译好的，不会因头文件改动而重编。
 * 链接器对 -shared 的未定义符号不报错，于是：
 *   编译过 → 链接过 → 真机 dlopen 时才报
 *   UnsatisfiedLinkError: cannot locate symbol "heap_seg_count"
 * 游戏直接加载失败。
 *
 * 教训：删任何非 static 函数前，必须搜**所有**目录（含 runtime/
 * 和预编译 .o），不能只搜当前目录的 *.c。
 *
 * 语义：返回**当前活跃**的堆段数（不是累计写入次数 —— 那个数字
 * 在旧的环形实现里会一直增长，对诊断没意义）。 */
static unsigned g_seg_live;     /* 当前活跃段数 */

unsigned heap_seg_count(void) { return g_seg_live; }

static inline unsigned seg_hash(uintptr_t base)
{
    /* base 都是页对齐的，低 12 位恒为 0，所以先右移再混合。
     * Knuth 乘法散列，对 2 的幂容量表现很好。 */
    uint64_t h = (uint64_t)(base >> 12) * 2654435761ull;
    return (unsigned)(h >> 20) & HEAP_SEG_MASK;
}

void heap_seg_add(uintptr_t base, size_t size)
{
    if (!base) return;
    unsigned i = seg_hash(base);
    for (unsigned n = 0; n < HEAP_SEG_N; n++) {
        uintptr_t b = g_segs[i].base;
        if (b == base) {                 /* 重复 add：只更新大小 */
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
    /* 表满：丢弃。（诊断用，不应发生） */
}

void heap_seg_del(uintptr_t base)
{
    if (!base) return;
    unsigned i = seg_hash(base);
    for (unsigned n = 0; n < HEAP_SEG_N; n++) {
        uintptr_t b = g_segs[i].base;
        if (b == 0) return;              /* 遇到空槽：不存在 */
        if (b == base) {
            g_segs[i].base = 1;          /* 墓碑，搜索继续 */
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
        if (b == 0) return 0;            /* 空槽：不可能在后面 */
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
/* 审计用：bump 游标之前的内存一定已经 mmap（分配器每次分配都会 map）。 */
uintptr_t heap_bump_cursor(void) { return g_heap_cur; }
uint32_t g_free_caller;
static int g_double_free_reported;
static uint32_t  g_free_head[HEAP_CLASSES];
static size_t    g_heap_live;       /* 当前在外面的字节数 */
static size_t    g_heap_peak;

/* 返回满足 2^c >= sz 的 class；连最大 class 都不够时返回 -1。
 *
 * ★ 2026-11-03 的根因：这里原来没有"不足"的出口，上限又是
 *   HEAP_CLASSES-1 = 23（8 MB）。一次 fread 需要 8,561,852 字节，
 *   分配器只给 8,388,608 字节的块，fread 照样写满 8,561,836 字节，
 *   越界 173 KB，把相邻的两个 4 KB 资源对象块整片覆盖成字体数据 ——
 *   表现为"资源缓存里 2/488 个元素的 obj 指向字形数组"。
 *   宁可返回 NULL 让调用者失败，也绝不能返回比请求小的块。 */
static int heap_class(size_t sz)
{
    int c = 12;
    while (c < HEAP_CLASSES - 1 && ((size_t)1 << c) < sz) c++;
    if (((size_t)1 << c) < sz) return -1;
    return c;
}

/* 空闲链坏节点连续跳过计数：见下面两处 "连续 64 个坏节点" 的注释。
 * 必须是静态的 —— 低层的 `return low_malloc(n)` 会被编译器做成尾递归
 * 优化（跳回函数开头），局部变量每次都会被重新初始化。 */
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
    /* 大请求探针：bad_alloc 时要知道请求有多大。 */
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
    if (bsz < need) return NULL;        /* 双保险：绝不返回不足的块 */

    uint32_t off = g_free_head[c];
    if (off) {
        uintptr_t p = HEAP_BASE + (size_t)(off - 1u);
        heap_blk *b = (heap_blk *)p;
        /* ★ 空闲链上的块必须带"已释放"标记（bit0）。
         * 没有标记却挂在链上 = 链被污染：同一个块被挂了两次，
         * 或者块被别的东西写坏。后果就是同一个 4KB 分给两个对象，
         * 各自往里面写 —— 表现成"资源对象的 vtable 变成字形数据"。 */
        if (!(b->size & 1)) {
            if (g_chain_corrupt < 8) {
                g_chain_corrupt++;
                fprintf(stderr,
                    "[heap] ★★ free-list corruption: class=%d off=%08x base=%08lx "
                    "size=%zu(no freed marker) request=%zu\n",
                    c, off, (unsigned long)p, b->size, n);
                fflush(NULL);
            }
            /* 跳过这个坏节点，继续找下一个，避免直接双分配。
             *
             * ★ 2026-11-06：跳过必须**有上限**。
             * 实测空闲链会构成自环（坏块的 next_off 指回自己或成环），
             * 原来这里无条件下一个 → 无限递归/循环。
             * 采样画像：99.4% 的 CPU 卡在 low_malloc 里，游戏完全不动，
             * 日志上只有 8 行 "空闲链污染"（因为打印有上限）。
             *
             * 现在：连续跳过 64 次就判定整条链不可信，清空它，
             * 直接走后面的 mmap 新块。坏块会泄漏，但游戏能继续跑。 */
            if (++g_skip_streak > 64u) {
                fprintf(stderr,
                    "[heap] ★ free-list class=%d had 64 consecutive bad nodes; clearing the list (avoiding an infinite loop)\n", c);
                g_free_head[c] = 0;
                g_skip_streak = 0;
                /* 不 return，往下走到 mmap 分配新块 */
            } else {
                g_free_head[c] = b->next_off;
                return low_malloc(n);
            }
        }
        /* ★ 空闲链上的块，其 size 必须**恰好等于**本 class 的 2^c。
         * 不等说明块头被覆盖，或者块被挂错了链 —— 后果是同一块
         * 被分给两个对象，互相覆盖（历史上表现为"链表指针变成垃圾"）。 */
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
                g_free_head[c] = b->next_off;   /* 跳过这个坏块 */
                return low_malloc(n);
            }
        }
        g_skip_streak = 0;              /* 找到好块，重置跳过计数 */
        b->size &= ~(size_t)1;          /* 清掉"已释放"标记 */
        hist_rec(p, b->size, 0);
        g_free_head[c] = b->next_off;
        b->base_lo = (uint32_t)p;
        g_heap_live += b->size;
        if (g_heap_live > g_heap_peak) g_heap_peak = g_heap_live;
        watch_note_alloc(p);
        return (void *)(p + sizeof(heap_blk));
    }

    /* ★★ 2026-11-10：跳过假地址保护区（0x7D000000-0x80000000）。
     * 见 HEAP_RESERVED_LO 的注释。放在耗尽检查之前，否则一旦
     * cur 恰好停在保护区入口就会被误判为本轮用尽。 */
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
    /* ★★ 2026-11-07：mmap 失败时**跳过**占用区，而不是原地卡死。
     *
     * 真机崩溃链（native.log）：
     *     [heap] ✗ mmap(0x88900000, 4096) 失败，堆指针停在原地
     *     [cxx] _CxxThrowException  type[0] = ".?AVbad_alloc@std@@"
     *     [SEGV] sig=6  abort
     *
     * 原因是地址空间里 ART 已经占了块：
     *     08900000-38900000  [anon:dalvik-main]   768 MB
     *     88900000-a0900000  [anon:dalvik-free]   384 MB  ← 0x88900000 在这
     * 而 guest 堆范围写死 0x48000000-0xB0000000（1.7 GB），直接撞上。
     *
     * 原注释担心"推进会留洞"。但要注意区别：
     *   · 留洞 = 记了一笔分配却没映射（之后拿到该地址的对象会 SIGSEGV）
     *   · 跳过 = 这片区域**不属于我们**，也就不可能有对象指向它
     * 被 ART 占的页里不会出现我们发的指针，所以跳过去是安全的。
     *
     * 之前 bump 分配器（host_android.c）已经因为同样的原因改过一次，
     * 这里是同一类修法：失败就往后挪 1 MB 重试，总共试 512 MB。 */
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
    /* ★ MSVC 的 aligned-free 会在 [ptr-4] 里找"分配块基址"，并在释放前
     * 校验 (ptr - base - 4) <= 0x1f。stringbuf 的析构（RVA 0x40f0a0）
     * 就是照这个约定做的：
     *     0xf0da  mov ecx,[eax-4]      ; ecx = base
     *     0xf0e0  sub eax,ecx
     *     0xf0e2  add eax,-4
     *     0xf0e5  cmp eax,0x1f
     *     0xf0e8  ja  _invalid_parameter_noinfo_noreturn
     * 我们的块头是 16 字节，所以把基址写在 [r-4] 处，(r-p-4)=12 通过检查。
     * 不写就会读到 0，校验失败 → 游戏 abort。 */
    b->base_lo = (uint32_t)(uintptr_t)p;
    /* 未初始化内存探针（HEAP_POISON=1）：
     * 每个新块填 0xCD。如果崩溃处的"野指针"变成 0xCDCDCDCD，
     * 说明是读取了未初始化字段；如果仍是某个具体值，
     * 说明那个值是被真实写入的（来自文件/计算）。
     * 只在调试时开 —— 每次分配都 memset 整块，开销很大。 */
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

/* 诊断：某地址所在块的当前状态。0=不在堆内 1=在用小写 2=空闲 */
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

/* 检查某个指针是否是全局资源表（0x3c7e7d00）里某个元素的 obj。
 * 游戏用它做缓存，元素应该只在摘除后才释放 —— 如果还在表里就 free，
 * 后面再访问就是 UAF。返回 1 = 在表里。 */
int res_table_contains(uintptr_t p)
{
    /* ★★ 2026-11-11：两个致命问题都在这里。
     *
     * 1. **性能**：这个函数在**每次 free** 都被调用（见 heap_free），
     *    而它对 32 个 bucket 各遍历一遍。真机 [stall] 实测
     *    「前一导入=free」的单帧开销达到 **4994 ms / 10858 ms** ——
     *    free 卡十几秒，直接把加载和帧率拖死。
     *    如果某个 bucket 的 beg/end 是垃圾值（大范围），一次 free
     *    就要遍历上亿项。
     *
     * 2. **越界读**：beg/end 直接来自 guest 内存，没有上限校验。
     *    一旦读到垃圾，就会扫过大量无映射地址 → SEGV。
     *    本地日志里 `[SEGV] sig=11 fault_addr=0x307e7d00` 就是
     *    这类访问（0x307e7d00 = 0x3c7e7d00 - 0x0c000000）造成的。
     *
     * 修法：给每个 bucket 加 1 MB 上限（131072 项），并限制总扫描项数。
     * 资源表的 bucket 不可能有 1 MB —— 超过就说明这个 bucket 的结构
     * 不合法，跳过它比扫垃圾安全得多。 */
    uint32_t tbl = ld32(0x3c7e7d00u);
    if (tbl < 0x10004u || tbl + 32u * 12u >= 0xC0000000u) return 0;

    enum { BUCKET_MAX_BYTES = 0x100000u, SCAN_BUDGET = 262144u };
    uint32_t budget = SCAN_BUDGET;

    for (unsigned b = 0; b < 32u; b++) {
        uint32_t beg = ld32(tbl + b * 12u);
        uint32_t end = ld32(tbl + b * 12u + 4u);
        if (beg == end || end < beg || end >= 0xC0000000u) continue;
        if (end - beg > (uint32_t)BUCKET_MAX_BYTES) continue;   /* ★ 垃圾 bucket */
        for (uint32_t q = beg; q + 8u <= end; q += 8u) {
            if ((uintptr_t)ld32(q) == p) return 1;
            if (--budget == 0u) return 0;                        /* ★ 总预算用尽 */
        }
    }
    return 0;
}

/* 遍历 AnmCache 的 map（MSVC std::map / 红黑树）并打印所有 key。
 *
 * 节点布局（MSVC 32 位）：
 *   +0x00 _Left   +0x04 _Parent   +0x08 _Right
 *   +0x0c _Color  +0x0d _Isnil
 *   +0x10 _Myval  = pair<const string, T>，string 是 0x18 字节 SSO
 * 容器布局：map+0 = _Myhead，map+4 = _Mysize。
 * 反汇编 0x90ad 的 `cmp eax,[esi+0x10]` 证实 key 在 +0x10。 */
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
    /* 先 dump 第一个节点的原始字节，用来定位 key 的真实偏移。
     * 不猜：MSVC 的 _Tree_node 在 32 位下可能带对齐填充。 */
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
        /* 中序后继 */
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
 * 资源表一致性审计（RES_AUDIT=1）
 *
 * 要回答的问题：那 2 个坏元素到底是"插入时就坏"，还是"插入后变的"。
 * 判据：obj 必须可读，且 [obj+0] 必须是 vtable（0x30xxxxxx 段）。
 * 正常元素 486/488 满足；坏的两个 [obj+0] 是 int16 字形数据。
 * 触发：表元素总数变化时（精确到每次 push_back 之后），另加每 2048 次
 * malloc 的深度扫描。
 * ==========================================================================*/
static unsigned g_audit_last_count = 0xffffffffu;
static unsigned g_audit_tick;
static unsigned long g_audit_last_ok_malloc;   /* 最后一次全表无坏元素的 malloc# */
unsigned long g_audit_new_bad;                 /* 审计新发现坏元素的次数 */
unsigned long g_last_malloc_size;              /* 当前 malloc 请求大小 */

static int audit_readable(uintptr_t p, size_t n)
{
    /* 镜像/只读段。★ 2026-11-07：原来写的是 `p + n <= 0x38000000u`，
     * 而上一个条件已经要求 p >= 0x3c000000 —— 0x3c000000 > 0x38000000，
     * 所以这个判断**永远为假**，审计从来没把镜像当作可读。
     * 按 probe 用的上界改成 0x3c900000。 */
    if (p >= 0x3c000000u && p + n <= 0x3c900000u) return 1;   /* 镜像/只读段 */
    if (p >= 0x40000000u && p + n <= 0x40400000u) return 1;   /* guest 栈 */
    /* 堆：bump 游标之前的内存一定已经 mmap，不必走 65536 条的段表 */
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
        if (g_audit_seen[i] == slot) return;      /* 同一槽只报一次 */
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
                /* [obj]==0 是"对象刚 malloc、vtable 还没写"的正常瞬态，
                 * 不算坏；真正的坏值是字形数据（非零且不在 vtable 段）。 */
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

/* 每个 malloc 调一次。RES_AUDIT=1 时每次都深度扫（窗口 = 一次 malloc）；
 * RES_AUDIT=2 时保留轻量模式（只在表大小变化或每 2048 次时扫）。 */
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

/* 外部写入审计：fread / memcpy 之类往 guest 内存写大块数据时调用。
 * 如果写入区间覆盖了资源表里某个元素的 obj 对象，就是"加载器踩到缓存对象"
 * 的确凿证据 —— 这条比事后审计精确一个数量级。 */
extern unsigned long g_read_reports;
unsigned long g_read_reports;
void res_write_audit(uint32_t beg, uint32_t len, uint32_t caller, const char *who)
{
    if (!len) return;
    uint32_t end = beg + len;
    if (end < beg) return;                    /* 溢出 */
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
 * AnmCache 容器探针（RES_MAP_PROBE=1）
 *
 * RVA 0x8fa0 是 AnmCache 的 "add reference"：它遍历 0xbfeda4..0xbfeda8
 * 这个 4 字节元素容器，对每个元素在 map(0xbfec30) 里 find。
 * find 返回 end() 就打印 "cannot add reference"（已从反汇编确认：
 * 0x90ba cmp esi,ecx / 0x90bc jne 成功 / 否则走到 0x90e6 打印）。
 *
 * 所以只要看 map 的 size 会不会增长，就能判断"加载到底有没有插入"。
 * 运行时地址 = 0x3c000000 + (RVA - 0x400000)：
 *   map 对象   0x3c7fec30   [+0]=_Myhead  [+4]=_Mysize
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

/* 包一层：分配**返回之后**才考虑上保护。
 * 原来在 low_malloc 入口处 arm，结果紧随其后的块头写入正好落在
 * 刚设成只读的页上 —— 抓到的"写入者"其实是分配器自己（
 * low_malloc+0x160），白跑一轮。 */
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
    /* ★★ 2026-11-11：这里原来是**每次 free** 都调一次 res_table_contains，
     * 而下面 UAF 那段又对同一个 ptr 再调一次 —— 同一张 guest 表扫两遍。
     * 真机实测「前一导入=free」单帧卡 4994 ms / 10858 ms，这是主因之一。
     *
     * 现在：合并成一次查询，且**诊断额度用完后不再扫表**。
     * 这两个报告都只打前 10 / 8 条，额度满了继续扫纯粹是浪费。 */
    {
        uintptr_t up = (uintptr_t)ptr;
        int kp = keepres_enabled();
        int want_res = (g_res_free_n < 10);
        int want_uaf = (g_uaf_reports < 8) &&
                       ((up & 0xfff00000UL) >= 0x60000000UL);
        /* ★ keepres 必须**独立于诊断额度**，否则额度一用完就失效
         * （原来它嵌在 want_res||want_uaf 里，最多只能保留 10 个对象）。 */
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
            /* ★ A/B 实验：仍在资源表里就不真正释放（见 keepres_enabled 注释）。 */
            if (in_res && want_keep) {
                size_t sz = 0;
                heap_block_state((uintptr_t)ptr, &sz);  /* 块大小 */
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
                    return;          /* ★ 不 free：内存与内容原样保留 */
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
        /* ★ 重复释放。旧实现（free 是空操作）把这个问题完全掩盖了；
         * 一旦真的归还到空闲链，同一块会被分给两个对象，
         * 数据互相覆盖 —— 现场表现为一个容器的字段变成字符串内容。 */
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
    if (c < 0) { g_n_free_bad++; return; }   /* 块大小不合法，不挂链 */
    hist_rec(p, raw, 1);
    /* deadbeef 探针（默认关闭）：把释放的块填成 0xA5，用于判定 UAF。
     * 每次 free 都 memset 整块，开销很大；排查 UAF 时才打开。
     * 本轮已经用它排除了 UAF，留着但默认关。 */
#if defined(HEAP_DEADBEEF)
    memset((void *)(p + sizeof(heap_blk)), 0xA5, raw - sizeof(heap_blk));
#endif
    b->size = raw | 1;                  /* 标"已释放" */
    b->next_off = g_free_head[c];
    g_free_head[c] = (uint32_t)(p - HEAP_BASE) + 1u;
    if (g_heap_live >= raw) g_heap_live -= raw;
}

/* ---------------- 内存 / 字符串 ---------------- */
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
    /* 巨块 memset 几乎总是错误的长度位（实测 0x0c000000 = 201 MB，
     * 从 0x802bc010 起写必然冲进未映射页）。打调用者，方便回查。 */
    if (n >= 0x01000000u) {
        fprintf(stderr, "[memset] ★ huge fill dst=%08x val=%u n=%u(0x%x) caller=%08x\n",
                d, v, n, n, ld32(c->esp));
        /* 现场反推：这个 memset 来自哈希表 rehash(RVA 0x565800)，
         * 该函数把 this 存在 [ebp-4]，把原始桶数存在 [ebp-0x28]。
         * 把容器对象和帧变量都挖出来，就能看出是哪个字段坏了。 */
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
            /* 沿 ebp 链往上走，把调用者栈帧里的返回地址全打出来。
             * 翻译出来的代码里 ebp 是标准帧指针，链条一直有效。 */
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
                        ret -= 0x3c000000u;         /* 去掉 guest 基址 */
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
    /* anm2 的 spritesheet 路径拼接走这里（n=260，见 RVA 0x259d84）。
     * PNC_TRACE=1 时打印源和目标，看拼出来的路径对不对。 */
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
                /* 顺带 dump 0x2599c0 的两个 vector，看切分结果 */
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

/* ---------------- 堆 ---------------- */
/* malloc 记下调用者，供 UAF 反查用 */
#define MALLOC_TRACE_N 65536
static struct { uint32_t ptr; uint32_t caller; uint32_t size; } g_mt[MALLOC_TRACE_N];
static unsigned g_mt_n;
extern uint32_t g_last_free_caller;

uint32_t g_malloc_caller;

static void h_malloc(CPU *c)
{
    uint32_t n = A(c, 0);
    g_malloc_caller = ld32(c->esp);      /* 本次 malloc 的返回地址 */
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

/* 反查某个地址最近一次是被哪个调用者 malloc 出来的 */
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
    /* ★ calloc 必须显式清零。
     *
     * 原来这里写着「mmap 已是 0」——那只在 bump 分配器下成立。
     * 改成 size-class 复用之后，同一块会被反复分配，里面是上一个
     * 使用者留下的数据。实测：一个 calloc(1,28) 的哈希表对象拿到了
     * 上一个 std::string 的缓冲区，[+0] 是 0x746e6f66（"font"），
     * 被当成桶数送进 rehash → 素数表越界 → 805 MB 的 memset。
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
        /* 用户可用大小 = 块总大小 - 块头，不能拿块大小直接当数据长度 */
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

/* ---------------- 转换 ---------------- */
/* ---------------- qsort ----------------
 * 比较器是 guest 函数指针（cdecl: int cmp(const void*, const void*)）。
 * 这是真实现，不是桩：每次比较都经 guest_call 回调 guest 自己的比较器。
 * 用堆排序 —— 原地、无递归、最坏 O(n log n)；不用宿主 qsort_r，因为
 * 它的签名在 glibc 和 bionic 之间不一致，跨平台移植时是雷。 */
static int qsort_guest_cmp(CPU *__restrict c, uint32_t fn,
                           uint32_t a, uint32_t b)
{
    uint32_t saved = c->esp;
    gpush(c, b);
    gpush(c, a);
    gpush(c, 0xFFF1A11EU);
    guest_call(c, fn);
    c->esp = saved;                 /* cdecl：调用方清栈 */
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
    /* 参数合理性：坏参数不该把 guest 内存扫穿 */
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
/* 下面这些在文件后面定义，先声明 */
static void   setd(CPU *c, double v);
static void   setf(CPU *c, float v);
static double argd(CPU *c, int i);

static void h_atof(CPU *c) {
    uint32_t s = A(c,0);
    double v = s ? atof(G(s)) : 0.0;
    setd(c, v);                           /* MSVC x86: double 返回在 ST(0) */
    cdecl_ret(c);
}

/* ---------------- 数学 ---------------- */
/* float 参数在 guest 栈上（cdecl），返回 xmm0 */
static float argf(CPU *c, int i) { uint32_t b = A(c, i); float v; memcpy(&v,&b,4); return v; }

/* ★ double 参数在 x86 栈上占 8 字节（低 4 字节在前） */
static double argd(CPU *c, int i)
{
    uint64_t lo = A(c, i), hi = A(c, i + 1);
    uint64_t raw = lo | (hi << 32);
    double v; memcpy(&v, &raw, 8);
    return v;
}

/* ★ MSVC x86 的 float/double 返回值在 **x87 ST(0)**，不是 xmm0。
 *   把结果写进 xmm0 会让 guest 的 `fstp` 读到 ST 栈顶的旧值 ——
 *   实测症状：modf 返回后 fault_addr = 0x3ff00000，正好是 double 1.0
 *   的高 32 位，即「把 double 位模式当指针用」。 */
static void setd(CPU *c, double v) { fpush(c, v); }
static void setf(CPU *c, float v)  { fpush(c, (double)v); }

/* SSE2 变体（_libm_sse2_*）：参数和返回值都在 xmm0/xmm1 */
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
    uint32_t b = A(c, 2);              /* (double x, int exp)：x 占 2 槽 */
    setd(c, ldexp(argd(c, 0), (int)b)); cdecl_ret(c);
}
static void h_modf(CPU *c) {
    /* double modf(double x, double *iptr) —— x 占 2 槽，iptr 在第 3 槽 */
    uint32_t ip = A(c, 2);
    double f, i;
    f = modf(argd(c, 0), &i);
    if (ip) { uint64_t b; memcpy(&b, &i, 8);
              st32(ip, (uint32_t)b); st32(ip + 4, (uint32_t)(b >> 32)); }
    setd(c, f); cdecl_ret(c);
}
static void h__fdclass(CPU *c) { c->eax = 0; cdecl_ret(c); }
static void h___setusermatherr(CPU *c) { c->eax = 0; cdecl_ret(c); }

/* ---------------- 运行时杂项 ---------------- */
static uint32_t g_fmode = 0;
static void h__set_fmode(CPU *c) { g_fmode = A(c,0); c->eax = 0; cdecl_ret(c); }

/* ★ 这些函数把「指向进程状态的指针」交给 guest。指针必须是低 4GB 的
 * guest 地址，绝不能是宿主静态变量的地址 —— 后者在 64 位 PIE 里位于
 * 0x5xxxxxxxxx，被截断成 uint32_t 后是垃圾，guest 一访问就崩。
 * 这是本次调试里同一类错误的第三次出现（TIB、TLS 块、这里）。 */
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
    uint32_t *p = status_slot(3);       /* 指向 argv 指针的指针 */
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
/* abort() **不会返回**。原来这里是 cdecl_ret()，于是 guest 继续执行
 * 调用点之后的字节 —— 那种「假装返回」会把崩溃推到很远的地方。
 * 实测：DINPUT8 加载失败后游戏调 abort，我们假装返回，接着它就 call 了
 * 一个 NULL 函数指针。 */
static void h_abort(CPU *c) { (void)c; fprintf(stderr,"[host] guest abort()\n"); fflush(NULL); abort(); }
/* exit 家族**不会返回**。原来这里用 cdecl_ret() 弹栈返回，等于让
 * 「永不返回」的函数假装返回了 —— guest 于是继续执行调用点之后的字节，
 * 本次实测就是跑出一个到 0x26 的野间接调用。宿主的 exit() 才是对的语义。 */
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
/* _cexit：刷新并关流，但**返回**（这就是它与 exit 的区别） */
static void h__cexit(CPU *c) { fflush(NULL); cdecl_ret(c); }
/* _c_exit：只清 CRT 状态，不跑 atexit，也不返回 */
static void h__c_exit(CPU *c) { exit(0); }
static void h_terminate(CPU *c)
{
    fprintf(stderr, "[host] std::terminate - uncaught C++ exception\n");
    fflush(NULL);
    /* ★★ 2026-11-11：崩溃取证（纯只读，不调任何 GL）。
     *
     * 之前只知道「terminate 发生在 glDeleteProgram 附近」，但
     * [glreq] 那条日志没有 fflush，abort 前顺序不可靠。这里补两样：
     *   1. 最近 64 次**真正分发到**的 GL 入口
     *   2. guest 栈上的返回地址链（定位是哪个函数调的 terminate）
     */
    {
        extern void host_gl_dump_ring(const char *);
        host_gl_dump_ring("crashed in terminate");
    }
    fprintf(stderr, "[host]   guest esp=%08x ebp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n",
            c->esp, c->ebp, c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi);
    /* 栈上的返回地址（esp 起 10 个）。
     * ★★ 2026-11-11：**必须先校验 esp**。
     * 本地 TERMINATE_SELFTEST 用 esp=0 触发时，这里直接 ld32(0) → SIGSEGV，
     * 取证代码自己变成二次崩溃源。这正是上次推真机崩掉的那类错误。 */
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
    /* ebp 链（有 FPO 时不可靠，仅作参考） */
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

    /* ★★ 2026-11-11：**全栈扫描**找返回地址。
     *
     * ebp 链在 MSVC /O2 下常因帧指针省略而断掉。改成扫整个 guest 栈，
     * 凡是落在 PE 代码段（0x3c000000..0x3d000000，镜像 0x889000 字节）
     * 且**紧跟在一个 call 之后**的值，都是候选返回地址。
     * 这比 ebp 链可靠得多，代价是几百次内存读，崩溃时无所谓。 */
    {
        uint32_t lo = c->stack_floor, hi = c->stack_ceiling;
        uint32_t begin = c->esp;
        /* ★ 地址合理性保护：esp 落在进程低地址或超出栈顶时**不扫**。
         * 否则 ld32(0) 之类会直接 SIGSEGV，把 terminate 取证变成二次崩溃。 */
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
    /* ★ 回放「已 free 但仍在资源表」的对象 —— 生命周期 bug 的直接证据。 */
    {
        extern uintptr_t g_uaf_log[64];
        extern unsigned  g_uaf_log_n, g_uaf_log_overflow;
        extern int res_table_contains(uintptr_t p);
        fprintf(stderr, "[host]   %u UAF records (+%u overflowed):\n",
                g_uaf_log_n, g_uaf_log_overflow);
        for (unsigned i = 0; i < g_uaf_log_n; i++) {
            uintptr_t q = g_uaf_log[i];
            /* 现在还在表里吗？还在 = 一直没摘，典型泄漏型 UAF */
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
/* _access：真的去问文件系统。以前恒返 -1，于是 KAGE 的
 *   ContentManager 把所有资源都当成“不存在”，连 fopen 都不尝试，
 *   日志里就是一片 "Failed to open resources/packed/*.a"。 */
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

/* _CxxThrowException：诊断版。
 *
 * 没有 SEH 展开表，抛出的异常没人接 —— 唯一的坐实信息在
 * _ThrowInfo 里：它指向 CatchableTypeArray，再指向 TypeDescriptor，
 * 类型名（mangled）在 descriptor+8。先把名字打出来再炸，
 * 否则只看到一句 "FAULT at ...!.."，等于没有信息。 */
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
    /* rapidxml::parse_error 的 MSVC 32 位布局：
     *   +0 vptr  +4 what(char*)  +8 where(char*)
     * 把 what 和出错位置附近的内容打出来，才知道是哪个字符不合语法。 */
    if (pObj >= 0x10000u && pObj < 0xC0000000u) {
        fprintf(stderr, "[cxx]   obj dwords:");
        for (int i = 0; i < 5; i++) fprintf(stderr, " %08x", ld32(pObj + (uint32_t)i * 4u));
        fprintf(stderr, "\n");
        /* MSVC 布局：std::exception 基类在前（vptr,_Mywhat,_Mydofree 共 0xc），
         * rapidxml::parse_error 自己的 m_what 在 +0xc、m_where 在 +0x10。 */
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
 * __RTDynamicCast —— MSVC 32 位 RTTI 的 dynamic_cast 实现
 * （2026-11-06 新增）
 *
 * 为什么必须有它
 * --------------
 * 它原本不在分发表里，所以走到 guest_fault() → longjmp，
 * 直接把游戏主循环打断。真机日志：
 *     最后导入 = VCRUNTIME140.dll!__RTDynamicCast
 *     [host] std::terminate —— 未捕获的 C++ 异常
 *     [SEGV] sig=6 ... abort
 * 也就是说：**只要游戏在某处做一次 dynamic_cast，就崩**。
 *
 * 签名（cdecl，VCRUNTIME 的 C 导出）：
 *     void *__RTDynamicCast(void *pObject, long VfDelta, void *Sink,
 *                           void *TargetType, int isReference);
 *
 * MSVC 32 位 RTTI 的四个结构（字段都是 RVA，要加镜像基址）：
 *
 *   RTTICompleteObjectLocator
 *     +0  signature        +4  offset（vfptr 相对完整对象的偏移）
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
 * 算法：
 *   1. vtbl   = *(void**)(pObject + VfDelta)
 *   2. col    = *(void**)(vtbl - 4)
 *   3. full   = pObject + VfDelta - col->offset     ← 完整对象起点
 *   4. 遍历 BaseClassArray，找 pTypeDescriptor == TargetType
 *   5. 命中 → full + mdisp
 *
 * 虚基类（pdisp/vdisp != -1）的指针调整需要读 vbtable，这里先只做
 * mdisp；以撒里出问题的是普通继承链，够用。命中率不足时日志会显示。
 * ========================================================================== */
static uint32_t rt_rva(uint32_t v)
{
    /* 镜像被重定位到 0x3c000000，RVA 最大约 0x85f000。
     * 小值当 RVA，大值当已经算好的绝对地址。 */
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

/* ★★ 2026-11-11：terminate 取证的**本地自测入口**。
 *
 * 上一次真机崩溃就是因为我推了没在本地跑过的代码。
 * 这里提供一个能在本地触发的入口，用来确认 terminate 里的
 * 全栈扫描 / UAF 回放不会二次崩溃。
 * 只在 TERMINATE_SELFTEST=1 时被 host_gl4es.c 调用。 */
void host_terminate_selftest(void)
{
    CPU dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.esp = 0;              /* 故意给一个不合理的 esp，验证保护分支 */
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

/* 信号处理器调用：非致命模式下恢复可写并返回，让程序继续 */
int watch_handle_hit(uintptr_t pc, uint32_t val)
{
    for (unsigned i = 0; i < g_watch_seen_n; i++)
        if (g_watch_seen_pc[i] == pc) return 0;      /* 已记录过，静默放行 */
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
    g_watch_pending = 0;      /* 不再重新上保护，避免死循环 */
    return 1;
}

/* ============================================================================
 * vtable 护栏：抓"对象头被覆盖"
 *
 * 资源表里每个元素的 obj 指针指向一个 0x90 字节对象，[0] 是 vtable。
 * 一旦有人往 [0] 写非 vtable 值（0x30xxxxxx 之外），就是覆盖现场。
 * 这比写保护精确得多：逐字段写（+0x48 之类）完全不受影响。
 * ==========================================================================*/
/* ★ 2026-11-06：改成全局 + 构造器初始化，让 guest.h 的 st32 能内联判断。
 * 原来是"函数内静态变量 + 惰性 init"，因为 guest_vtable_guard_enabled()
 * 是外部函数、每次 st32 都要调用，采样显示占 3.7% CPU。 */
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
            if (ld32(q) == a) return 1;      /* a 是某元素的 obj 基址 */
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
