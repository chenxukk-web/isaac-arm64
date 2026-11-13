/* jni_bridge.c —— 把移植层从「子进程」变成「app 进程内的 .so」
 *
 * ==========================================================================
 * 一、为什么要有这一层
 *
 * 原来游戏是个独立可执行文件，用 ProcessBuilder 启动。代价是拿不到
 * Android 的 Surface（Surface 是进程内对象），只能：
 *     pbuffer 渲染 → glReadPixels → 转 565 → TCP:7138 → Java 拷贝 → drawBitmap
 * 每帧 2MB 数据被拷四遍，还要等 GPU 同步，最终画面只有 7 fps。
 *
 * 现在：作为 .so dlopen 进 app 进程，JNI 把 SurfaceView 的 ANativeWindow
 * 传进来，直接 eglCreateWindowSurface + eglSwapBuffers。零拷贝。
 * 这正是 iOS 移植版的做法（GL 绑定到 UIView 的 CALayer）。
 *
 * ==========================================================================
 * 二、为什么不用 <jni.h>
 *
 * Termux 里有 android/native_window_jni.h，但没有 jni.h（NDK 的 sysroot
 * 没装全）。而这一层只需要两个 JNI 能力：
 *   1. 把 jstring 转成 const char*（拿游戏目录）
 *   2. 把 jobject(Surface) 转成 ANativeWindow*
 * 第 2 个由 libandroid.so 的 ANativeWindow_fromSurface 完成，它只要
 * JNIEnv* 和 jobject，对类型不做任何假设。
 *
 * 所以这里手写最小声明：JNIEnv 是一个指向函数表的指针，函数表第 169 项
 * 是 GetStringUTFChars（jni.h 里的固定布局，从 JDK 1.1 起没变过）。
 * 比去凑一个 jni.h 头文件可靠。
 *
 * ==========================================================================
 * 三、边界
 *
 * guest 代码一旦崩，**整个 app 会崩**。这是 .so 化的固有代价。
 * Java 侧保留「子进程模式」开关：出问题切回去，能看完整 run.log。
 * 见 MainActivity 的 useInProcess()。
 * ========================================================================== */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <android/native_window_jni.h>

/* ---- 最小 JNI 声明 ---- */
typedef void *jobject;
typedef void *jclass;
typedef void *jstring;
typedef int32_t jint;
typedef void *JNIEnv_p;

/* JNINativeInterface 里 GetStringUTFChars 的索引（jni.h 固定布局） */
#define JNI_GET_STRING_UTF_CHARS  169
#define JNI_RELEASE_STRING_UTF    170

static const char *jstring_to_c(void *env, void *str)
{
    void **tbl;

    /* ★★ 2026-11-07：根因修复。
     *
     * 旧代码：typedef const char *(*fn_t)(void *, void *);
     *         只传两个参数。
     *
     * 但 GetStringUTFChars 的真实签名是三个：
     *     const char* GetStringUTFChars(JNIEnv*, jstring, jboolean *isCopy);
     *
     * 在 AArch64 的 AAPCS 下，第三个参数走 x2。只声明两个时 x2 保留
     * **上一个 call 的残留值**，而 ART 的实现会执行：
     *     if (isCopy != NULL) *isCopy = ...;
     * 于是往一个垃圾地址写 → SIGSEGV。
     *
     * 真机证据（18:47 native.log）：
     *     *** 崩溃 sig=0b code=01 addr=000000000023 stage=41 ***
     * addr=0x23 就是那个残留值。stage=41 正是"即将调 jstring_to_c"。
     *
     * 这也解释了历史上那些"无法解释的段错误"（包括怀疑过 libEGL 的
     * 第 36 轮）—— 它一直是 UB，有时候 x2 凑巧是 0 或可写地址，
     * 就"没崩"；否则当场死。
     *
     * 修复：显式传第三个参数 NULL。JNI 规范允许 isCopy 为 NULL
     * （"If isCopy is not NULL, then *isCopy is set to..."），
     * ART 实现里有判空。 */
    typedef const char *(*fn_t)(void *, void *, unsigned char *);
    if (!env || !str) return NULL;
    tbl = *(void ***)env;
    if (!tbl || !tbl[JNI_GET_STRING_UTF_CHARS]) return NULL;
    return ((fn_t)tbl[JNI_GET_STRING_UTF_CHARS])(env, str, NULL);
}

static void jstring_release(void *env, void *str, const char *c)
{
    void **tbl;
    typedef void (*fn_t)(void *, void *, const char *);
    if (!env || !str || !c) return;
    tbl = *(void ***)env;
    if (tbl && tbl[JNI_RELEASE_STRING_UTF])
        ((fn_t)tbl[JNI_RELEASE_STRING_UTF])(env, str, c);
}

/* ---- libandroid.so 的 ANativeWindow_fromSurface ---- */
static void *(*p_ANativeWindow_fromSurface)(void *env, jobject surface);
static void  (*p_ANativeWindow_release)(void *win);
static int32_t (*p_ANativeWindow_setBuffersGeometry)(void *win, int32_t w,
                                                     int32_t h, int32_t fmt);
static int32_t (*p_ANativeWindow_getWidth)(void *win);
static int32_t (*p_ANativeWindow_getHeight)(void *win);

static int load_android_lib(void)
{
    void *h = dlopen("libandroid.so", RTLD_NOW);
    if (!h) {
        fprintf(stderr, "[jni] ✗ cannot open libandroid.so: %s\n", dlerror());
        return 0;
    }
    p_ANativeWindow_fromSurface =
        dlsym(h, "ANativeWindow_fromSurface");
    p_ANativeWindow_release =
        dlsym(h, "ANativeWindow_release");
    p_ANativeWindow_setBuffersGeometry =
        dlsym(h, "ANativeWindow_setBuffersGeometry");
    p_ANativeWindow_getWidth  = dlsym(h, "ANativeWindow_getWidth");
    p_ANativeWindow_getHeight = dlsym(h, "ANativeWindow_getHeight");
    if (!p_ANativeWindow_fromSurface) {
        fprintf(stderr, "[jni] ✗ ANativeWindow_fromSurface not found\n");
        return 0;
    }
    return 1;
}

/* ==========================================================================
 * 日志重定向（2026-11-07 重做）
 *
 * 上一版把 dup2 放在 jni_main_thread 里，于是**之前所有失败路径的
 * fprintf 全进了 logcat**，Termux 没 READ_LOGS 权限读不到 ——
 * 真机上只看到 "NativeBridge.start 已调用"，然后什么都没有。
 *
 * 现在：
 *   1. Java 在 System.load 之后立刻调 init(logPath) → dup2
 *   2. start() 里再兜一次（幂等）
 *   3. jstring 转换失败时，退回编译期固定的应用私有路径
 *      （包名固定，所以这个路径是确定的）
 * 这样任何后续失败都能在文件里看到。
 * ========================================================================== */
#define OAL_FALLBACK_LOG "/data/user/0/com.isaacport.launcher/files/inproc.log"

/* ★ 2026-11-07：绝对不用 dup2。
 *
 * 上一版 dup2(fd,1/2) 把整个 app 进程的 stdout/stderr 换成文件，
 * 直接冲击 ART 运行时 —— 真机后果是**应用无响应（ANR）**，
 * 而且 inproc.log 根本没被创建（卡在 native 里）。
 *
 * 现在：单独 fopen 一个日志文件，只写它，不动 fd 1/2。
 * 代价是只能记录我们显式写的那几行，但足够定位问题。 */
static FILE *g_log;
static FILE *g_log2;      /* 私有目录备份（2026-11-07） */
static int   g_log_errno;    /* 主文件 fopen 的 errno（0 = 成功） */
static int   g_log2_errno;   /* 备份文件 fopen 的 errno */
static int   g_log_tried;

/* ★ 2026-11-07 第二版：fd 通道。
 *
 * 上面两个 fopen 在真机上**一次都没成功过** —— 18:21 那次运行里，
 * Java 写同一个外置目录 app.log 完全正常，native 的 fopen 却
 * 一个文件也没创建。路径/权限/存储域的雷太多，不再赌。
 *
 * 改成：Java 用 ParcelFileDescriptor.createPipe() 建一对 pipe，
 * 把写端的**裸 fd** 作为 int 传进来。native 直接 write(2)，
 * 完全不碰路径、不碰权限。
 *
 * fd 在同一进程里就是普通文件描述符，没有沙箱问题。 */
static int   g_logfd = -1;

/* 缓冲日志（定义在下方，这里先声明给 JLOG / crash_handler 用） */
static void log_flush(void);
static void log_append(const char *s, size_t n);

/* 阶段计数（定义提前：jni_main_thread 要用）。说明见 NativeBridge_stage 处。 */
static volatile int g_stage;

/* 诊断位（★ 2026-11-07）
 *
 * 这是**唯一保证能读到**的通道：一个 volatile int，Java 直接调用读取，
 * 不依赖文件、不依赖 socket、不依赖日志库、不会被 pipe 缓冲弄丢。
 *
 *   bit0  pipe fd 可用
 *   bit1  主日志文件 fopen 成功
 *   bit2  备份日志文件 fopen 成功
 *   bit3  jstring_to_c 失败
 *   bit4  setvbuf 失败
 *   bit8-15  主文件 fopen 的 errno（0 = 成功或未尝试）
 *   bit16-23 备份文件 fopen 的 errno
 */
static volatile int g_diag;

/* ── 崩溃捕获（★ 2026-11-07）
 *
 * 真机上没有 root 读不到 /data/tombstones，Termux 也没有 READ_LOGS，
 * 所以崩溃地址只能自己抓。
 *
 * 约束：handler 里**绝对不能**用 printf/malloc —— 它们不是
 * async-signal-safe，在崩溃现场重入会死循环。只允许 write(2)。
 * 所以十六进制格式化手写。
 *
 * 抓到之后 _exit(66)，不用 abort()（那会再触发一次核心转储）。 */
static void crash_puthex(char **p, unsigned long v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    char *s = *p;
    int i;
    for (i = digits - 1; i >= 0; i--) s[i] = hx[v & 0xF], v >>= 4;
    *p = s + digits;
}

static void crash_puts(char **p, const char *s)
{
    while (*s) *(*p)++ = *s++;
}

static void crash_handler(int sig, siginfo_t *si, void *uc)
{
    char buf[256];
    char *p = buf;
    (void)uc;

    /* 先把缓冲里还没落盘的日志刷出去 —— 否则崩溃前的现场会丢。 */
    log_flush();

    crash_puts(&p, "\n*** crash sig=");
    crash_puthex(&p, (unsigned long)sig, 2);
    crash_puts(&p, " code=");
    crash_puthex(&p, (unsigned long)(si ? si->si_code : 0), 2);
    crash_puts(&p, " addr=");
    crash_puthex(&p, (unsigned long)(si ? (unsigned long)si->si_addr : 0), 12);
    crash_puts(&p, " stage=");
    crash_puthex(&p, (unsigned long)g_stage, 2);
    crash_puts(&p, " diag=");
    crash_puthex(&p, (unsigned long)g_diag, 4);
    crash_puts(&p, " ***\n");

    if (g_logfd >= 0) (void)!write(g_logfd, buf, (size_t)(p - buf));
    if (g_log)       (void)!write(fileno(g_log), buf, (size_t)(p - buf));
    if (g_log2)      (void)!write(fileno(g_log2), buf, (size_t)(p - buf));
    (void)!write(2, buf, (size_t)(p - buf));   /* logcat 也留一份 */

    _exit(66);
}

static void crash_install(void)
{
    /* 不用 SIGSTKSZ：新版 bionic 把它定义成 sysconf() 调用，
     * 不是编译期常量，做数组长度会编译失败。固定 64 KB 足够。 */
    static char altstack[64 * 1024];
    stack_t ss;
    struct sigaction sa;

    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    (void)sigaltstack(&ss, NULL);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGSEGV, &sa, NULL);
    (void)sigaction(SIGBUS,  &sa, NULL);
    (void)sigaction(SIGABRT, &sa, NULL);
    (void)sigaction(SIGILL,  &sa, NULL);
    (void)sigaction(SIGFPE,  &sa, NULL);

    /* ★ SIGPIPE 直接忽略，不接管。
     *
     * 这是 18:30 那次「进去直接闪退」的头号嫌疑：native 现在往 pipe
     * 里 write，只要读端一断（Java 读线程退出 / fd 被回收），内核">
     * 就发 SIGPIPE，默认动作是**当场杀进程**。
     * 日志写不出去可以丢数据，绝不能拿进程陪葬。 */
    (void)signal(SIGPIPE, SIG_IGN);
}

/* ★ 构造期就装：SIGPIPE 必须在**任何 write 之前**被忽略，
 * 而宿主层的输入桥 __attribute__((constructor)) 里就可能开始干活。
 * 优先级 101 = 最早（glibc/bionic 约定 0-100 保留给系统）。 */
__attribute__((constructor(101)))
static void jni_early_init(void)
{
    crash_install();
}

#define JLOG(...) do {                                                   \
        fprintf(stderr, __VA_ARGS__);                                    \
        if (g_log)  { fprintf(g_log,  __VA_ARGS__); }                    \
        if (g_log2) { fprintf(g_log2, __VA_ARGS__); }                    \
        if (g_logfd >= 0) {                                              \
            char _lb[1024];                                              \
            int _ln = snprintf(_lb, sizeof _lb, __VA_ARGS__);             \
            if (_ln > 0) {                                               \
                size_t _w = (size_t)_ln < sizeof _lb ? (size_t)_ln        \
                                                    : sizeof _lb - 1;     \
                log_append(_lb, _w);                                      \
                log_flush();   /* JLOG 是低频关键行，立即落盘 */            \
            }                                                            \
        }                                                                \
    } while (0)

/* 私有目录备份路径。包名固定，所以可以硬编码（同 OAL_FALLBACK_LOG）。
 *
 * ★ 为什么需要备份：外置 /sdcard/Android/media/<pkg>/files/game 和
 * 私有 /data/user/0/<pkg>/files 是两个不同的存储域，一个失败另一个
 * 往往还能写。双写之后，只要用户能用 MT 管理器进到任一个，就有日志。 */
#define LOG_BACKUP_1 "/data/user/0/com.isaacport.launcher/files/inproc.log"
#define LOG_BACKUP_2 "/data/user/0/com.isaacport.launcher/files/game/inproc.log"

static void jlog_open(const char *path)
{
    if (g_log_tried) return;

    /* 只有 fopen 成功才置 tried：一旦第一次失败（路径不存在、
     * 权限没给），后续兜底要还能再试。 */
    if (path && path[0] == '/') {
        errno = 0;
        g_log = fopen(path, "w");
        g_log_errno = g_log ? 0 : errno;
        /* ★ 2026-11-07：改为全缓冲。原来 _IONBF 让每行都同步写 FUSE（毫秒级），
         * 主循环里有日志就是灾难。现在靠 log_flush() 批量写，
         * 崩溃现场由 crash_handler 直接 write 保底。 */
        if (g_log) setvbuf(g_log, NULL, _IOFBF, 65536);
    } else {
        g_log_errno = -1;          /* 没给路径 */
    }
    if (!g_log2 && !(path && strcmp(path, LOG_BACKUP_1) == 0)) {
        errno = 0;
        g_log2 = fopen(LOG_BACKUP_1, "w");
        g_log2_errno = g_log2 ? 0 : errno;
        if (!g_log2) {
            errno = 0;
            g_log2 = fopen(LOG_BACKUP_2, "w");
            if (g_log2) g_log2_errno = 0;
        }
        if (g_log2) setvbuf(g_log2, NULL, _IOFBF, 65536);
    }
    if (!g_log && !g_log2) return;   /* 两边都失败，留给下次再试 */

    g_log_tried = 1;
    JLOG("=== inproc.log open: primary=%s(errno=%d) fallback=%s(errno=%d) ===\n",
         g_log  ? (path ? path : "(null)") : "failed", g_log_errno,
         g_log2 ? LOG_BACKUP_1 : "failed", g_log2_errno);
}

/* 兼容旧名字 */
static void jni_log_redirect(const char *path) { jlog_open(path); }

/* ★ 2026-11-07：stderr 日志分流。
 *
 * 宿主层（host_main.c / host_gl4es.c / host_crt.c …）约 250 处
 * fprintf(stderr, ...)。进程内模式撤掉 dup2 之后，这些全部进了
 * logcat，而 Termux 没有 READ_LOGS —— main() 内部的加载进度、
 * EGL 结果、崩溃点一个都看不到，inproc.log 里只剩 [jni] 那几行，
 * 等于没日志。
 *
 * 做法：宿主层编译时加 -Dfprintf=host_fprintf（<stdio.h> 里的声明
 * 会被宏改写成 host_fprintf 的声明，签名正好对上）。这里实现分流：
 * 原样写回原 stream（logcat 照旧），当 stream == stderr 且日志文件
 * 已打开时再写一份到文件。
 *
 * 不碰 fd 1/2 —— 没有 dup2 那种进程级副作用。
 *
 * 上限：文件写满 8 MB 就停止分流，防止某个高频路径把私有分区写爆。
 * 原始 stderr 不受影响，logcat 里永远是全的。 */
static long g_log_bytes;

/* ★★ 2026-11-07：日志缓冲。
 *
 * 原来每行日志一次 write(2) 到 /sdcard（FUSE）。FUSE 写是同步的，
 * 单次毫秒级 —— 而主循环里每帧都有几条日志时，光日志就能拖垮帧率。
 * 真机现象：“卡很长时间才进行一次” 与此一致。
 *
 * 改成 4 KB 累积缓冲：满才写。代价是崩溃时可能丢掉最后不到 4 KB，
 * 但只要崩溃走 crash_handler（它直接 write 并带上 stage/diag），
 * 关键信息仍然在。
 *
 * 定时刷新：host_fprintf 在缓冲超过 2 KB 时也刷，避免长时间不落盘。 */
static char   g_lbuf[4096];
static size_t g_lbuf_n;

static void log_flush(void)
{
    if (g_lbuf_n == 0) return;
    if (g_logfd >= 0) (void)!write(g_logfd, g_lbuf, g_lbuf_n);
    if (g_log)  fwrite(g_lbuf, 1, g_lbuf_n, g_log);
    if (g_log2) fwrite(g_lbuf, 1, g_lbuf_n, g_log2);
    g_lbuf_n = 0;
}

static void log_append(const char *s, size_t n)
{
    while (n > 0) {
        size_t room = sizeof g_lbuf - g_lbuf_n;
        if (room == 0) { log_flush(); room = sizeof g_lbuf; }
        size_t take = n < room ? n : room;
        memcpy(g_lbuf + g_lbuf_n, s, take);
        g_lbuf_n += take;
        s += take;
        n -= take;
    }
}

int host_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(f, fmt, ap);
    va_end(ap);

    if (f == stderr && g_log_bytes < (8L << 20)) {
        char buf[1024];
        va_list ap2;
        va_start(ap2, fmt);
        int m = vsnprintf(buf, sizeof buf, fmt, ap2);
        va_end(ap2);
        if (m > 0) {
            size_t w = (size_t)m < sizeof buf ? (size_t)m : sizeof buf - 1;
            log_append(buf, w);
            g_log_bytes += (long)w;
            /* 半满就落盘：既少 syscall，又不会丢太多 */
            if (g_lbuf_n >= sizeof g_lbuf / 2) log_flush();
        }
    }
    return n;
}

/* ---- 移植层的入口 ---- */
extern int main(int argc, char **argv);
extern unsigned host_gl_swap_count(void);
extern void host_gl4es_set_window(void *win);
extern void host_gl4es_set_fb_size(int w, int h);

/* 已经跑过就不再跑（Activity 重建时可能被调两次） */
static volatile int g_running;
static volatile int g_finished;

typedef struct {
    void     *win;
    int       w, h;
    char     *dir;      /* strdup 出来的，线程自己释放 */
} jni_job;

static void *jni_main_thread(void *arg)
{
    jni_job *job = (jni_job *)arg;
    char *argv0 = "isaac";

    g_stage = 5;
    JLOG("[jni] === in-process start ===\n");
    JLOG("[jni] Surface %dx%d  ANativeWindow=%p  gameDir=%s\n",
            job->w, job->h, job->win, job->dir ? job->dir : "(null)");

    /* ★ 2026-11-07：日志已经由 init(fd) 建好，这里**不要再 fopen**。
     *
     * 早期版本在这个函数里 dup2(fd,1/2) 到 <game>/inproc.log ——
     * 那是进程级操作，会把整个 app 的 stdout/stderr 换掉、冲击 ART
     * 运行时（真机 ANR）；后来改成 fopen，但在真机上**一次都没成功**，
     * 而 Java 写同一个目录完全正常。
     *
     * 现在走 pipe fd（见 g_logfd 的注释），这个函数里只补一条
     * 在 job->dir 已知时的初始化，其余靠 init 已建立的通道。 */
    if (!g_log_tried && job->dir && job->dir[0]) {
        char logpath[512];
        snprintf(logpath, sizeof logpath, "%s/inproc.log", job->dir);
        jlog_open(logpath);
    }
    g_stage = 10;
    JLOG("[jni] === in-process start === stage=10\n");
    fflush(NULL);

    /* ★ 切工作目录。游戏的 PE 路径已被 push_elf/build_apk 改成
     * ./isaac-ng.exe.unpacked（相对路径），所有资源也按相对路径找。
     * 不 chdir 就会 "PE load failed"。 */
    if (job->dir && job->dir[0]) {
        if (chdir(job->dir) != 0) {
            JLOG("[jni] ⚠ chdir(%s) failed: %s\n",
                    job->dir, strerror(errno));
            g_stage = 14;
        } else {
            JLOG("[jni] cwd = %s\n", job->dir);
            g_stage = 15;
        }
    }

    /* ★★ 2026-11-07：渲染尺寸与 Surface 尺寸分离。
     *
     * 真机现象：画面只占左下角一小块，帧率极低。
     *
     * 根因：options.ini 里 WindowWidth=960 WindowHeight=540，游戏按这个
     * 尺寸设 glViewport；而 Surface 是 2665x1272。OpenGL 原点在左下 ——
     * 于是 960x540 的画面正好落在 2665x1272 的左下角。
     *   960/2665 = 36%，540/1272 = 42%，与用户看到的比例完全对得上。
     *
     * 而且 2665x1272 = 340 万像素，960x540 只有 52 万 —— **6.5 倍浪费**，
     * 这就是帧率超低的直接原因。gl4es 是 x86→ARM64 翻译 + GL 翻译层，
     * 每像素成本远高于原生，堆像素量代价加倍。
     *
     * 修法：让**buffer 尺寸 = 游戏认为的渲染尺寸**，由 SurfaceFlinger
     * 负责放大到 View 尺寸（硬件合成，几乎免费）。
     *   1. 读 options.ini 的 WindowWidth/Height（与游戏同一份配置）
     *   2. setBuffersGeometry(宽, 高) —— ANativeWindow 的 buffer 就是这么大
     *   3. gl4es 的 fb 尺寸也用这个
     * 这样游戏 viewport、EGL buffer、gl4es fb 三者一致，不再错位。
     *
     * 留黑边的问题交给 Java：SurfaceView 保持游戏宽高比居中。 */
    {
        int rw = 0, rh = 0;
        FILE *fi = fopen("./Documents/My Games/Binding of Isaac Repentance/options.ini", "rb");
        if (fi) {
            char line[256];
            while (fgets(line, sizeof line, fi)) {
                int v = 0;
                if (sscanf(line, "WindowWidth=%d", &v) == 1)  { rw = v; continue; }
                if (sscanf(line, "WindowHeight=%d", &v) == 1) { rh = v; continue; }
            }
            fclose(fi);
        }
        /* 读不到就退回 Surface 尺寸（至少不错位得那么厉害） */
        if (rw < 320 || rh < 200 || rw > 4096 || rh > 4096) {
            rw = job->w; rh = job->h;
            JLOG("[jni] ⚠ options.ini had no WindowWidth/Height; falling back to the Surface size\n");
        }

        JLOG("[jni] render size %dx%d (options.ini), Surface %dx%d\n",
             rw, rh, job->w, job->h);

        /* buffer 改成渲染尺寸：SurfaceFlinger 会把它放大到 View。
         * 注意这一步**必须在 eglCreateWindowSurface 之前**
         * （gl4es init 在 main() 里跑），否则 buffer 尺寸已经定死了。 */
        if (job->win && p_ANativeWindow_setBuffersGeometry)
            p_ANativeWindow_setBuffersGeometry(job->win, rw, rh, 0);

        job->w = rw;
        job->h = rh;
    }

    /* 窗口交给 GL 层：有它就走 eglCreateWindowSurface，没有就 pbuffer。 */
    host_gl4es_set_window(job->win);
    host_gl4es_set_fb_size(job->w, job->h);
    JLOG("[jni] gl4es fb = %dx%d\n", job->w, job->h);

    fflush(NULL);
    g_stage = 20;                 /* 即将进入 main() */
    main(1, &argv0);

    g_stage = 30;                 /* main 返回了 */
    JLOG("[jni] the game main loop returned\n");
    g_finished = 1;
    g_running = 0;
    if (job->win && p_ANativeWindow_release)
        p_ANativeWindow_release(job->win);
    free(job->dir);
    free(job);
    return NULL;
}

/* ==========================================================================
 * 阶段计数（★ 2026-11-07）
 *
 * 真机上 inproc.log 从来没被创建过，fopen / fd 两道保险都可能失效。
 * 所以再加一条**完全不可能失败**的通道：一个全局 int，Java 直接轮询。
 * 它不依赖文件、不依赖 socket、不依赖日志库。
 *
 *     0  未开始
 *     5  jni_main_thread 进入
 *    10  ANativeWindow 提取完成
 *    11  即将建游戏线程
 *    14  chdir 失败
 *    15  chdir 完成
 *    20  即将进入 main()
 *    30  main() 返回
 *    40  init 进入
 *    41  init: fcntl/SIGPIPE 完成并已往 fd 写一行
 *    42  init: jstring_to_c 完成
 *    43  init: jlog_open 完成
 *    44  init: 日志已写
 *    45  init 完成
 *   -1x  start() 失败：-1=load_android_lib, -2=surface null,
 *                        -3=calloc, -4=pthread_create
 * ========================================================================== */

__attribute__((visibility("default")))
jint Java_com_isaacport_launcher_NativeBridge_stage(void *env, void *cls)
{
    (void)env; (void)cls;
    return (jint)g_stage;
}

/* ==========================================================================
 * Java_com_isaacport_launcher_NativeBridge_init
 *
 *   public static native int init(String logPath, int logFd);
 *
 * 在 System.load 之后、start 之前调用。
 * 返回位掩码（Java 会写进 app.log）：
 *   bit0  fd 通道可用
 *   bit1  主日志文件 fopen 成功
 *   bit2  备份日志文件 fopen 成功
 *   bit3  jstring 转 char* 失败（logPath 不可用）
 * ========================================================================== */
__attribute__((visibility("default")))
jint Java_com_isaacport_launcher_NativeBridge_init(
        void *env, void *cls, jstring jlog, jint logfd)
{
    const char *p;
    int rc = 0;
    (void)cls;

    g_stage = 40;

    /* ★ 再装一次崩溃 handler。
     *
     * 它本来在 constructor(101) 里装 —— 但 18:43 那次闪退，
     * native.log 里连一行崩溃信息都没有，说明 handler 没生效
     * （或 constructor 没被执行到）。
     * crash_install 幂等，重复调用无副作用。 */
    crash_install();

    if (logfd >= 0) {
        g_logfd = (int)logfd;

        /* ★ SIGPIPE 必须在任何 write 之前忽略掉。
         * 上面 crash_install 已经设了，这里再设一次（防构造顺序问题）。 */
        (void)signal(SIGPIPE, SIG_IGN);

        /* ★ 设非阻塞。v4 用的是**真实文件 fd**（不再是 pipe），
         * 普通文件通常忽略 O_NONBLOCK，所以这纯属保险：
         * 万一 FUSE（/sdcard 就是 FUSE）卡住，宁可丢日志也不卡死游戏。 */
        int fl = fcntl(g_logfd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(g_logfd, F_SETFL, fl | O_NONBLOCK);
        rc |= 1;
    }

    /* ★ 第一件事：直接往 fd 写一行。
     * 不经过 fopen、不经过 jstring 转换、不经过任何可能失败的东西。
     * 如果 native.log 里连这行都没有，问题就在"fd 本身无效"，
     * 而不是后面任何一步。 */
    if (g_logfd >= 0) {
        const char *m = "# [jni] init entered, fd received\n";
        (void)!write(g_logfd, m, strlen(m));
    }
    g_stage = 41;

    p = jstring_to_c(env, jlog);
    g_stage = 42;
    if (p && p[0] == '/') jlog_open(p);
    else { jlog_open(OAL_FALLBACK_LOG); rc |= 8; }
    g_stage = 43;

    if (g_log)  rc |= 2;
    if (g_log2) rc |= 4;

    /* 把 fopen 的 errno 编进诊断位（bit8-15 主 / bit16-23 备）。
     * 这是为了回答一个关键问题：**为什么 Java 写同一个目录能成，
     * native 的 fopen 却一次都没成过。**
     *   EACCES(13) 权限   ENOENT(2) 目录不存在   EMFILE(24) fd 耗尽
     *   EROFS(30) 只读    ELOOP(40) 符号链接过多 */
    g_diag = (int)(rc
            | ((g_log  ? 0 : (g_log_errno  & 0xFF)) << 8)
            | ((g_log2 ? 0 : (g_log2_errno & 0xFF)) << 16));

    JLOG("[jni] init(fd=%d) rc=%d diag=0x%04x  primary=%s(/%d) fallback=%s(/%d) path=%s\n",
         (int)logfd, rc, (unsigned)g_diag,
         g_log ? "ok" : "failed", g_log_errno,
         g_log2 ? "ok" : "failed", g_log2_errno,
         p ? p : "(null)");
    g_stage = 44;
    JLOG("[jni] SIGPIPE ignored  crash handler installed stage=%d\n", g_stage);
    fflush(NULL);
    g_stage = 45;
    return (jint)rc;
}

/* 诊断位：Java 直接读，不依赖任何 IO。 */
__attribute__((visibility("default")))
jint Java_com_isaacport_launcher_NativeBridge_diag(void *env, void *cls)
{
    (void)env; (void)cls;
    return (jint)g_diag;
}

/* ★ 编译时间戳（epoch 秒）。
 *
 * 目的：确认那个 70 MB 的 .so 到底是哪一版。18:30 那次 app.log 说
 * 「外置未变，复用内部副本」，但同一天推过好几个不同长度的 .so ——
 * 得有个东西能在运行时直接证明版本，而不是靠捣长度。
 *
 * Java 侧把它跟 app.log 里的构建时间比对，就能确认有没有跑到旧库。 */
#ifndef BUILD_EPOCH
#define BUILD_EPOCH 0
#endif

__attribute__((visibility("default")))
jint Java_com_isaacport_launcher_NativeBridge_buildEpoch(void *env, void *cls)
{
    (void)env; (void)cls;
    return (jint)BUILD_EPOCH;
}

/* ==========================================================================
 * Java_com_isaacport_launcher_NativeBridge_start
 *
 *   public static native void start(Surface surface, int w, int h, String gameDir);
 *
 * 起一个线程跑游戏主循环（游戏永远不会返回，所以不能占用 JNI 调用线程）。
 * ========================================================================== */
__attribute__((visibility("default")))
void Java_com_isaacport_launcher_NativeBridge_start(
        void *env, void *cls, jobject surface, jint jw, jint jh, jstring jdir)
{
    jni_job *job;
    const char *dir;
    (void)cls;

    /* ── 第一件事：确保日志能落盘 ── */
    if (!g_log_tried) {
        const char *d = jstring_to_c(env, jdir);
        if (d && d[0] == '/') {
            char lp[512];
            snprintf(lp, sizeof lp, "%s/inproc.log", d);
            jlog_open(lp);
        }
        if (!g_log_tried) jlog_open(OAL_FALLBACK_LOG);
    }

    JLOG("\n[jni] start() entered  env=%p surface=%p %dx%d\n",
         env, surface, (int)jw, (int)jh);

    if (g_running) {
        JLOG("[jni] already running; ignoring duplicate start\n");
        return;
    }
    if (!load_android_lib()) {
        g_stage = -1;
        JLOG("[jni] ✗ load_android_lib failed - cannot obtain ANativeWindow\n");
        return;
    }
    JLOG("[jni] libandroid.so loaded\n");

    if (!surface) {
        g_stage = -2;
        JLOG("[jni] ✗ surface is null\n");
        return;
    }

    job = (jni_job *)calloc(1, sizeof *job);
    if (!job) { g_stage = -3; return; }

    job->win = p_ANativeWindow_fromSurface(env, surface);
    JLOG("[jni] ANativeWindow_fromSurface -> %p\n", job->win);
    if (!job->win) {
        fprintf(stderr, "[jni] ✗ ANativeWindow_fromSurface returned NULL\n"
                        "[jni]   -> invalid Surface (SurfaceView not attached yet, or destroyed)\n"
                        "[jni]   -> falling back to pbuffer mode and continuing\n");
        /* ★ 不 return。没有窗口也应该能把游戏跑起来（pbuffer + 推流），
         * 只是没有全屏直连。否则用户看到的是"什么都没发生"。 */
        job->win = NULL;
    }
    job->w = (int)jw;
    job->h = (int)jh;

    /* ★ 2026-11-07：用窗口自己的尺寸，并在交给 EGL 之前做健全性检查。
     *
     * 实测教训：给 eglCreateWindowSurface 传一个无效 ANativeWindow，
     * libEGL 会**直接段错误**（fault_addr=0x12c4），而不是返回 NULL。
     * 所以「创建失败就退回 pbuffer」这条兜底根本走不到 —— 进程先死了。
     *
     * 真实场景里 ANativeWindow_fromSurface 拿到的是有效句柄，但
     * Surface 被销毁后（切后台 / 旋转 / SurfaceView 重建）它就成了
     * 悬空指针。这里用 ANativeWindow_getWidth/Height 先探一下：
     * 能读出正的尺寸才认为可用。读不出来就说明句柄不可信，
     * 直接放弃窗口模式、退回 pbuffer + 推流（用户还能继续玩）。 */
    if (job->win && p_ANativeWindow_getWidth && p_ANativeWindow_getHeight) {
        int32_t ww = p_ANativeWindow_getWidth(job->win);
        int32_t hh = p_ANativeWindow_getHeight(job->win);
        JLOG("[jni] ANativeWindow reports %dx%d\n", (int)ww, (int)hh);
        if (ww <= 0 || hh <= 0) {
            JLOG("[jni] ⚠ abnormal window size; falling back to pbuffer mode\n");
            if (p_ANativeWindow_release) p_ANativeWindow_release(job->win);
            job->win = NULL;
        } else {
            job->w = ww;
            job->h = hh;
        }
    } else if (job->win) {
        JLOG("[jni] ⚠ cannot obtain ANativeWindow_getWidth/Height;"
                        "falling back to pbuffer to be safe\n");
        if (p_ANativeWindow_release) p_ANativeWindow_release(job->win);
        job->win = NULL;
    } else {
        JLOG("[jni] no window; starting in pbuffer mode\n");
    }

    /* 让缓冲区尺寸跟 SurfaceView 对齐（0 = 用默认格式） */
    if (job->win && p_ANativeWindow_setBuffersGeometry && job->w > 0 && job->h > 0)
        p_ANativeWindow_setBuffersGeometry(job->win, job->w, job->h, 0);

    dir = jstring_to_c(env, jdir);
    job->dir = dir ? strdup(dir) : NULL;
    jstring_release(env, jdir, dir);
    JLOG("[jni] gameDir = %s\n", job->dir ? job->dir : "(NULL!)");

    g_running = 1;
    g_stage = 11;                 /* 即将创建游戏线程 */
    JLOG("[jni] creating the game thread...\n");
    fflush(NULL);
    {
        pthread_t t;
        if (pthread_create(&t, NULL, jni_main_thread, job) == 0) {
            pthread_detach(t);
            JLOG("[jni] thread created\n");
            fflush(NULL);
        } else {
            g_stage = -4;
            JLOG("[jni] ✗ pthread_create failed\n");
            g_running = 0;
            if (p_ANativeWindow_release) p_ANativeWindow_release(job->win);
            free(job->dir);
            free(job);
        }
    }
}

/* 让 Java 能查询状态 */
__attribute__((visibility("default")))
int Java_com_isaacport_launcher_NativeBridge_isRunning(void *env, void *cls)
{
    (void)env; (void)cls;
    return g_running;
}

/* 已经渲染了多少帧。
 *
 * ★ 这是判断「进程内启动到底成没成」的唯一可靠信号：
 *   · 帧数在涨 → EGL + Surface 都通了，游戏在跑
 *   · 30 秒还是 0 → 失败（mmap 冲突 / PE 加载失败 / EGL 建不起来），
 *     Java 侧据此自动切回子进程模式，不至于让用户对着黑屏。
 *
 * 不能用 isRunning 判断：它在 main() 一进去就是 true，
 * 即使 guest_image_load 立刻失败也一样。 */
__attribute__((visibility("default")))
int Java_com_isaacport_launcher_NativeBridge_frameCount(void *env, void *cls)
{
    (void)env; (void)cls;
    return (int)host_gl_swap_count();
}

/* main() 返回过 = 启动失败（游戏主循环不返回）。
 * Java 据此立刻判定失败，不必等 30 秒。 */
__attribute__((visibility("default")))
int Java_com_isaacport_launcher_NativeBridge_hasExited(void *env, void *cls)
{
    (void)env; (void)cls;
    return g_finished;
}
