/* jni_bridge.c -- turns the port from a "child process" into an in-process .so
 *
 * ==========================================================================
 * 1. Why this layer exists
 *
 * Originally the game was a standalone executable started via ProcessBuilder.
 * The cost was losing Android's Surface (a Surface is an in-process object), so
 * the only option was:
 *     pbuffer render -> glReadPixels -> convert to 565 -> TCP:7138 -> Java copy -> drawBitmap
 * That copied 2 MB per frame four times, plus a GPU sync, ending at 7 fps.
 *
 * Now: dlopen'd into the app process as a .so; JNI passes in the SurfaceView's
 * ANativeWindow, and we call eglCreateWindowSurface + eglSwapBuffers directly.
 * Zero copies. This is exactly what the iOS port does (GL bound to a UIView's
 * CALayer).
 *
 * ==========================================================================
 * 2. Why <jni.h> is not used
 *
 * Termux has android/native_window_jni.h but not jni.h (the NDK sysroot is not
 * installed in full). This layer needs only two JNI capabilities:
 *   1. Turn a jstring into a const char* (to get the game directory)
 *   2. Turn a jobject (Surface) into an ANativeWindow*
 * The second is done by libandroid.so's ANativeWindow_fromSurface, which only
 * needs a JNIEnv* and a jobject and assumes nothing about the types.
 *
 * So the minimal declarations are written by hand here: JNIEnv is a pointer to a
 * function table, and entry 169 of that table is GetStringUTFChars (a fixed
 * layout in jni.h, unchanged since JDK 1.1). That is more reliable than trying
 * to assemble a jni.h header.
 *
 * ==========================================================================
 * 3. Boundaries
 *
 * If guest code crashes, **the whole app crashes**. That is the inherent cost of
 * being a .so. The Java side keeps a "child process mode" switch: flip back when
 * things go wrong and you get a complete run.log.
 * See MainActivity.useInProcess().
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

/* ---- minimal JNI declarations ---- */
typedef void *jobject;
typedef void *jclass;
typedef void *jstring;
typedef int32_t jint;
typedef void *JNIEnv_p;

/* index of GetStringUTFChars in JNINativeInterface (fixed layout in jni.h) */
#define JNI_GET_STRING_UTF_CHARS  169
#define JNI_RELEASE_STRING_UTF    170

static const char *jstring_to_c(void *env, void *str)
{
    void **tbl;

    /* ★★ 2026-11-07: root-cause fix.
     *
     * Old code: typedef const char *(*fn_t)(void *, void *);
     *           only two parameters.
     *
     * But the real signature of GetStringUTFChars takes three:
     *     const char* GetStringUTFChars(JNIEnv*, jstring, jboolean *isCopy);
     *
     * Under AArch64 AAPCS the third argument travels in x2. Declaring only two
     * leaves x2 holding **whatever the previous call left there**, and ART's
     * implementation does:
     *     if (isCopy != NULL) *isCopy = ...;
     * so it writes to a garbage address -> SIGSEGV.
     *
     * Device evidence (18:47 native.log):
     *     *** crash sig=0b code=01 addr=000000000023 stage=41 ***
     * addr=0x23 is exactly that leftover value. stage=41 is "about to call
     * jstring_to_c".
     *
     * This also explains the historical "unexplainable segfaults" (including
     * round 36, where libEGL was suspected) -- it was undefined behaviour all
     * along; sometimes x2 happened to be 0 or a writable address and it "did not
     * crash"; otherwise it died on the spot.
     *
     * Fix: pass NULL explicitly as the third argument. The JNI spec allows isCopy
     * to be NULL ("If isCopy is not NULL, then *isCopy is set to..."), and ART's
     * implementation null-checks it. */
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

/* jni_bridge.c -- turns the port from a "child process" into an in-process .so
 *
 * ==========================================================================
 * 1. Why this layer exists
 *
 * Originally the game was a standalone executable started via ProcessBuilder.
 * The cost was losing Android's Surface (a Surface is an in-process object), so
 * the only option was:
 *     pbuffer render -> glReadPixels -> convert to 565 -> TCP:7138 -> Java copy -> drawBitmap
 * That copied 2 MB per frame four times, plus a GPU sync, ending at 7 fps.
 *
 * Now: dlopen'd into the app process as a .so; JNI passes in the SurfaceView's
 * ANativeWindow, and we call eglCreateWindowSurface + eglSwapBuffers directly.
 * Zero copies. This is exactly what the iOS port does (GL bound to a UIView's
 * CALayer).
 *
 * ==========================================================================
 * 2. Why <jni.h> is not used
 *
 * Termux has android/native_window_jni.h but not jni.h (the NDK sysroot is not
 * installed in full). This layer needs only two JNI capabilities:
 *   1. Turn a jstring into a const char* (to get the game directory)
 *   2. Turn a jobject (Surface) into an ANativeWindow*
 * The second is done by libandroid.so's ANativeWindow_fromSurface, which only
 * needs a JNIEnv* and a jobject and assumes nothing about the types.
 *
 * So the minimal declarations are written by hand here: JNIEnv is a pointer to a
 * function table, and entry 169 of that table is GetStringUTFChars (a fixed
 * layout in jni.h, unchanged since JDK 1.1). That is more reliable than trying
 * to assemble a jni.h header.
 *
 * ==========================================================================
 * 3. Boundaries
 *
 * If guest code crashes, **the whole app crashes**. That is the inherent cost of
 * being a .so. The Java side keeps a "child process mode" switch: flip back when
 * things go wrong and you get a complete run.log.
 * See MainActivity.useInProcess().
 * ========================================================================== */
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

/* ---- minimal JNI declarations ---- */
#define OAL_FALLBACK_LOG "/data/user/0/com.isaacport.launcher/files/inproc.log"

/* index of GetStringUTFChars in JNINativeInterface (fixed layout in jni.h) */
static FILE *g_log;
static FILE *g_log2;      /* private-directory backup (2026-11-07) */
static int   g_log_errno;    /* errno of the primary file fopen (0 = success) */
static int   g_log2_errno;   /* errno of the backup file fopen */
static int   g_log_tried;

    /* ★★ 2026-11-07: root-cause fix.
     *
     * Old code: typedef const char *(*fn_t)(void *, void *);
     *           only two parameters.
     *
     * But the real signature of GetStringUTFChars takes three:
     *     const char* GetStringUTFChars(JNIEnv*, jstring, jboolean *isCopy);
     *
     * Under AArch64 AAPCS the third argument travels in x2. Declaring only two
     * leaves x2 holding **whatever the previous call left there**, and ART's
     * implementation does:
     *     if (isCopy != NULL) *isCopy = ...;
     * so it writes to a garbage address -> SIGSEGV.
     *
     * Device evidence (18:47 native.log):
     *     *** crash sig=0b code=01 addr=000000000023 stage=41 ***
     * addr=0x23 is exactly that leftover value. stage=41 is "about to call
     * jstring_to_c".
     *
     * This also explains the historical "unexplainable segfaults" (including
     * round 36, where libEGL was suspected) -- it was undefined behaviour all
     * along; sometimes x2 happened to be 0 or a writable address and it "did not
     * crash"; otherwise it died on the spot.
     *
     * Fix: pass NULL explicitly as the third argument. The JNI spec allows isCopy
     * to be NULL ("If isCopy is not NULL, then *isCopy is set to..."), and ART's
     * implementation null-checks it. */
static int   g_logfd = -1;

/* buffered log (defined below; declared here for JLOG / crash_handler) */
static void log_flush(void);
static void log_append(const char *s, size_t n);

/* stage counter (defined early: jni_main_thread needs it). See NativeBridge_stage. */
static volatile int g_stage;

/* diagnostic bits (★ 2026-11-07)
 *
 * This is the **only channel guaranteed to be readable**: a volatile int that
 * Java reads directly. It does not depend on files, sockets, a logging library,
 * and cannot be lost in pipe buffering.
 *
 *   bit0  pipe fd usable
 *   bit1  primary log file fopen succeeded
 *   bit2  backup log file fopen succeeded
 *   bit3  jstring_to_c failed
 *   bit4  setvbuf failed
 *   bit8-15   errno of the primary file fopen (0 = success or not attempted)
 *   bit16-23  errno of the backup file fopen
 */
static volatile int g_diag;

/* -- crash capture (★ 2026-11-07)
 *
 * On device there is no root to read /data/tombstones, and Termux has no
 * READ_LOGS, so the crash address has to be captured here.
 *
 * Constraint: the handler must **never** use printf/malloc -- they are not
 * async-signal-safe, and re-entering them at a crash site loops forever. Only
 * write(2) is allowed. Hence the hand-written hex formatting.
 *
 * After capturing, _exit(66) rather than abort() (which would trigger another
 * core dump). */
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

/* Flush any buffered log first -- otherwise the pre-crash state is lost. */
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
    (void)!write(2, buf, (size_t)(p - buf));   /* keep a copy for logcat too */

    _exit(66);
}

static void crash_install(void)
{
    /* Not using SIGSTKSZ: newer bionic defines it as a sysconf() call, not a
     * compile-time constant, so using it as an array length fails to compile.
     * A fixed 64 KB is enough. */
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

/* jni_bridge.c -- turns the port from a "child process" into an in-process .so
 *
 * ==========================================================================
 * 1. Why this layer exists
 *
 * Originally the game was a standalone executable started via ProcessBuilder.
 * The cost was losing Android's Surface (a Surface is an in-process object), so
 * the only option was:
 *     pbuffer render -> glReadPixels -> convert to 565 -> TCP:7138 -> Java copy -> drawBitmap
 * That copied 2 MB per frame four times, plus a GPU sync, ending at 7 fps.
 *
 * Now: dlopen'd into the app process as a .so; JNI passes in the SurfaceView's
 * ANativeWindow, and we call eglCreateWindowSurface + eglSwapBuffers directly.
 * Zero copies. This is exactly what the iOS port does (GL bound to a UIView's
 * CALayer).
 *
 * ==========================================================================
 * 2. Why <jni.h> is not used
 *
 * Termux has android/native_window_jni.h but not jni.h (the NDK sysroot is not
 * installed in full). This layer needs only two JNI capabilities:
 *   1. Turn a jstring into a const char* (to get the game directory)
 *   2. Turn a jobject (Surface) into an ANativeWindow*
 * The second is done by libandroid.so's ANativeWindow_fromSurface, which only
 * needs a JNIEnv* and a jobject and assumes nothing about the types.
 *
 * So the minimal declarations are written by hand here: JNIEnv is a pointer to a
 * function table, and entry 169 of that table is GetStringUTFChars (a fixed
 * layout in jni.h, unchanged since JDK 1.1). That is more reliable than trying
 * to assemble a jni.h header.
 *
 * ==========================================================================
 * 3. Boundaries
 *
 * If guest code crashes, **the whole app crashes**. That is the inherent cost of
 * being a .so. The Java side keeps a "child process mode" switch: flip back when
 * things go wrong and you get a complete run.log.
 * See MainActivity.useInProcess().
 * ========================================================================== */
    (void)signal(SIGPIPE, SIG_IGN);
}

/* ---- minimal JNI declarations ---- */
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
                log_flush();   /* JLOG is for low-frequency key lines; flush immediately */            \
            }                                                            \
        }                                                                \
    } while (0)

/* index of GetStringUTFChars in JNINativeInterface (fixed layout in jni.h) */
#define LOG_BACKUP_1 "/data/user/0/com.isaacport.launcher/files/inproc.log"
#define LOG_BACKUP_2 "/data/user/0/com.isaacport.launcher/files/game/inproc.log"

static void jlog_open(const char *path)
{
    if (g_log_tried) return;

    /* ★★ 2026-11-07: root-cause fix.
     *
     * Old code: typedef const char *(*fn_t)(void *, void *);
     *           only two parameters.
     *
     * But the real signature of GetStringUTFChars takes three:
     *     const char* GetStringUTFChars(JNIEnv*, jstring, jboolean *isCopy);
     *
     * Under AArch64 AAPCS the third argument travels in x2. Declaring only two
     * leaves x2 holding **whatever the previous call left there**, and ART's
     * implementation does:
     *     if (isCopy != NULL) *isCopy = ...;
     * so it writes to a garbage address -> SIGSEGV.
     *
     * Device evidence (18:47 native.log):
     *     *** crash sig=0b code=01 addr=000000000023 stage=41 ***
     * addr=0x23 is exactly that leftover value. stage=41 is "about to call
     * jstring_to_c".
     *
     * This also explains the historical "unexplainable segfaults" (including
     * round 36, where libEGL was suspected) -- it was undefined behaviour all
     * along; sometimes x2 happened to be 0 or a writable address and it "did not
     * crash"; otherwise it died on the spot.
     *
     * Fix: pass NULL explicitly as the third argument. The JNI spec allows isCopy
     * to be NULL ("If isCopy is not NULL, then *isCopy is set to..."), and ART's
     * implementation null-checks it. */
    if (path && path[0] == '/') {
        errno = 0;
        g_log = fopen(path, "w");
        g_log_errno = g_log ? 0 : errno;
/* buffered log (defined below; declared here for JLOG / crash_handler) */
        if (g_log) setvbuf(g_log, NULL, _IOFBF, 65536);
    } else {
        g_log_errno = -1;          /* no path given */
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
    if (!g_log && !g_log2) return;   /* both failed; leave it for the next attempt */

    g_log_tried = 1;
    JLOG("=== inproc.log open: primary=%s(errno=%d) fallback=%s(errno=%d) ===\n",
         g_log  ? (path ? path : "(null)") : "failed", g_log_errno,
         g_log2 ? LOG_BACKUP_1 : "failed", g_log2_errno);
}

/* stage counter (defined early: jni_main_thread needs it). See NativeBridge_stage. */
static void jni_log_redirect(const char *path) { jlog_open(path); }

/* diagnostic bits (★ 2026-11-07)
 *
 * This is the **only channel guaranteed to be readable**: a volatile int that
 * Java reads directly. It does not depend on files, sockets, a logging library,
 * and cannot be lost in pipe buffering.
 *
 *   bit0  pipe fd usable
 *   bit1  primary log file fopen succeeded
 *   bit2  backup log file fopen succeeded
 *   bit3  jstring_to_c failed
 *   bit4  setvbuf failed
 *   bit8-15   errno of the primary file fopen (0 = success or not attempted)
 *   bit16-23  errno of the backup file fopen
 */
static long g_log_bytes;

/* -- crash capture (★ 2026-11-07)
 *
 * On device there is no root to read /data/tombstones, and Termux has no
 * READ_LOGS, so the crash address has to be captured here.
 *
 * Constraint: the handler must **never** use printf/malloc -- they are not
 * async-signal-safe, and re-entering them at a crash site loops forever. Only
 * write(2) is allowed. Hence the hand-written hex formatting.
 *
 * After capturing, _exit(66) rather than abort() (which would trigger another
 * core dump). */
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
/* Flush any buffered log first -- otherwise the pre-crash state is lost. */
            if (g_lbuf_n >= sizeof g_lbuf / 2) log_flush();
        }
    }
    return n;
}

    /* Not using SIGSTKSZ: newer bionic defines it as a sysconf() call, not a
     * compile-time constant, so using it as an array length fails to compile.
     * A fixed 64 KB is enough. */
extern int main(int argc, char **argv);
extern unsigned host_gl_swap_count(void);
extern void host_gl4es_set_window(void *win);
extern void host_gl4es_set_fb_size(int w, int h);

/* Already ran, so do not run again (an Activity rebuild can call this twice) */
static volatile int g_running;
static volatile int g_finished;

typedef struct {
    void     *win;
    int       w, h;
    char     *dir;      /* from strdup; the thread frees it itself */
} jni_job;

static void *jni_main_thread(void *arg)
{
    jni_job *job = (jni_job *)arg;
    char *argv0 = "isaac";

    g_stage = 5;
    JLOG("[jni] === in-process start ===\n");
    JLOG("[jni] Surface %dx%d  ANativeWindow=%p  gameDir=%s\n",
            job->w, job->h, job->win, job->dir ? job->dir : "(null)");

    /* ★ 2026-11-07: the log is already set up by init(fd); **do not fopen again**
     * here.
     *
     * An early version did dup2(fd,1/2) onto <game>/inproc.log inside this
     * function -- a process-level operation that replaces the whole app's
     * stdout/stderr and disturbs the ART runtime (ANR on device). It was then
     * changed to fopen, which **never succeeded once** on device, even though
     * Java writing to the same directory worked fine.
     *
     * It now goes through a pipe fd (see the comment on g_logfd). This function
     * only adds the piece of initialisation that needs job->dir; the rest relies
     * on the channel init already established. */
    if (!g_log_tried && job->dir && job->dir[0]) {
        char logpath[512];
        snprintf(logpath, sizeof logpath, "%s/inproc.log", job->dir);
        jlog_open(logpath);
    }
    g_stage = 10;
    JLOG("[jni] === in-process start === stage=10\n");
    fflush(NULL);

    /* ★ Change the working directory. The game's PE path was rewritten by
     * push_elf/build_apk to ./isaac-ng.exe.unpacked (a relative path), and all
     * resources are looked up relatively too. Without chdir you get
     * "PE load failed". */
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

    /* ★★ 2026-11-07: render size and Surface size are separated.
     *
     * Device symptom: the image occupied only a small patch in the lower left,
     * and the frame rate was very low.
     *
     * Root cause: options.ini has WindowWidth=960 WindowHeight=540 and the game
     * sets glViewport from it, while the Surface is 2665x1272. OpenGL's origin is
     * bottom-left -- so the 960x540 image lands exactly in the lower-left corner
     * of 2665x1272.
     *   960/2665 = 36%, 540/1272 = 42%, matching exactly what the user saw.
     *
     * Worse, 2665x1272 = 3.4M pixels while 960x540 is only 520k -- a **6.5x
     * waste**, and that is the direct cause of the terrible frame rate. gl4es is
     * an x86->ARM64 translation plus a GL translation layer, so its per-pixel
     * cost is far above native, and padding the pixel count doubles the price.
     *
     * Fix: make **buffer size = the render size the game believes in**, and let
     * SurfaceFlinger scale it up to the View size (hardware composition, nearly
     * free).
     *   1. Read WindowWidth/Height from options.ini (the same config the game
     *      reads)
     *   2. setBuffersGeometry(w, h) -- the ANativeWindow buffer is that size
     *   3. gl4es's fb size uses the same value
     * Then the game viewport, the EGL buffer and the gl4es fb all agree and no
     * longer skew.
     *
     * Letterboxing is left to Java: SurfaceView keeps the game aspect ratio
     * centred. */
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
/* Fall back to the Surface size if it cannot be read (at least the skew is smaller) */
        if (rw < 320 || rh < 200 || rw > 4096 || rh > 4096) {
            rw = job->w; rh = job->h;
            JLOG("[jni] ⚠ options.ini had no WindowWidth/Height; falling back to the Surface size\n");
        }

        JLOG("[jni] render size %dx%d (options.ini), Surface %dx%d\n",
             rw, rh, job->w, job->h);

        /* Change the buffer to the render size: SurfaceFlinger will scale it up to
         * the View. Note this step **must come before eglCreateWindowSurface**
         * (gl4es init runs inside main()), otherwise the buffer size is already
         * fixed. */
        if (job->win && p_ANativeWindow_setBuffersGeometry)
            p_ANativeWindow_setBuffersGeometry(job->win, rw, rh, 0);

        job->w = rw;
        job->h = rh;
    }

/* Hand the window to the GL layer: with it, use eglCreateWindowSurface; without it, pbuffer. */
    host_gl4es_set_window(job->win);
    host_gl4es_set_fb_size(job->w, job->h);
    JLOG("[jni] gl4es fb = %dx%d\n", job->w, job->h);

    fflush(NULL);
    g_stage = 20;                 /* about to enter main() */
    main(1, &argv0);

    g_stage = 30;                 /* main() returned */
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
 * Stage counter (★ 2026-11-07)
 *
 * On device inproc.log was never created; both the fopen and the fd fallback can
 * fail. So add one more channel that **cannot possibly fail**: a global int that
 * Java polls directly. It depends on no file, no socket, no logging library.
 *
 *     0   not started
 *     5   jni_main_thread entered
 *    10   ANativeWindow extraction done
 *    11   about to create the game thread
 *    14   chdir failed
 *    15   chdir done
 *    20   about to enter main()
 *    30   main() returned
 *    40   init entered
 *    41   init: fcntl/SIGPIPE done and one line written to the fd
 *    42   init: jstring_to_c done
 *    43   init: jlog_open done
 *    44   init: log written
 *    45   init done
 *   -1x  start() failed: -1=load_android_lib, -2=surface null,
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
 * Called after System.load and before start.
 * Returns a bitmask (Java writes it into app.log):
 *   bit0  fd channel usable
 *   bit1  primary log file fopen succeeded
 *   bit2  backup log file fopen succeeded
 *   bit3  jstring to char* failed (logPath unusable)
 * ========================================================================== */
__attribute__((visibility("default")))
jint Java_com_isaacport_launcher_NativeBridge_init(
        void *env, void *cls, jstring jlog, jint logfd)
{
    const char *p;
    int rc = 0;
    (void)cls;

    g_stage = 40;

    /* Install the crash handler again.
     *
     * It was originally installed in constructor(101) -- but during the 18:43
     * crash native.log contained not a single line about it, meaning the handler
     * was not in effect (or the constructor never ran).
     * crash_install is idempotent, so calling it twice has no side effects. */
    crash_install();

    if (logfd >= 0) {
        g_logfd = (int)logfd;

        /* ★ SIGPIPE must be ignored before any write.
         * crash_install above already set it; set it again here to guard against
         * construction-order issues. */
        (void)signal(SIGPIPE, SIG_IGN);

    /* ★★ 2026-11-07: root-cause fix.
     *
     * Old code: typedef const char *(*fn_t)(void *, void *);
     *           only two parameters.
     *
     * But the real signature of GetStringUTFChars takes three:
     *     const char* GetStringUTFChars(JNIEnv*, jstring, jboolean *isCopy);
     *
     * Under AArch64 AAPCS the third argument travels in x2. Declaring only two
     * leaves x2 holding **whatever the previous call left there**, and ART's
     * implementation does:
     *     if (isCopy != NULL) *isCopy = ...;
     * so it writes to a garbage address -> SIGSEGV.
     *
     * Device evidence (18:47 native.log):
     *     *** crash sig=0b code=01 addr=000000000023 stage=41 ***
     * addr=0x23 is exactly that leftover value. stage=41 is "about to call
     * jstring_to_c".
     *
     * This also explains the historical "unexplainable segfaults" (including
     * round 36, where libEGL was suspected) -- it was undefined behaviour all
     * along; sometimes x2 happened to be 0 or a writable address and it "did not
     * crash"; otherwise it died on the spot.
     *
     * Fix: pass NULL explicitly as the third argument. The JNI spec allows isCopy
     * to be NULL ("If isCopy is not NULL, then *isCopy is set to..."), and ART's
     * implementation null-checks it. */
        int fl = fcntl(g_logfd, F_GETFL, 0);
        if (fl >= 0) (void)fcntl(g_logfd, F_SETFL, fl | O_NONBLOCK);
        rc |= 1;
    }

/* buffered log (defined below; declared here for JLOG / crash_handler) */
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

/* stage counter (defined early: jni_main_thread needs it). See NativeBridge_stage. */
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

/* diagnostic bits (★ 2026-11-07)
 *
 * This is the **only channel guaranteed to be readable**: a volatile int that
 * Java reads directly. It does not depend on files, sockets, a logging library,
 * and cannot be lost in pipe buffering.
 *
 *   bit0  pipe fd usable
 *   bit1  primary log file fopen succeeded
 *   bit2  backup log file fopen succeeded
 *   bit3  jstring_to_c failed
 *   bit4  setvbuf failed
 *   bit8-15   errno of the primary file fopen (0 = success or not attempted)
 *   bit16-23  errno of the backup file fopen
 */
__attribute__((visibility("default")))
jint Java_com_isaacport_launcher_NativeBridge_diag(void *env, void *cls)
{
    (void)env; (void)cls;
    return (jint)g_diag;
}

/* -- crash capture (★ 2026-11-07)
 *
 * On device there is no root to read /data/tombstones, and Termux has no
 * READ_LOGS, so the crash address has to be captured here.
 *
 * Constraint: the handler must **never** use printf/malloc -- they are not
 * async-signal-safe, and re-entering them at a crash site loops forever. Only
 * write(2) is allowed. Hence the hand-written hex formatting.
 *
 * After capturing, _exit(66) rather than abort() (which would trigger another
 * core dump). */
#ifndef BUILD_EPOCH
#define BUILD_EPOCH 0
#endif

__attribute__((visibility("default")))
jint Java_com_isaacport_launcher_NativeBridge_buildEpoch(void *env, void *cls)
{
    (void)env; (void)cls;
    return (jint)BUILD_EPOCH;
}

/* Flush any buffered log first -- otherwise the pre-crash state is lost. */
__attribute__((visibility("default")))
void Java_com_isaacport_launcher_NativeBridge_start(
        void *env, void *cls, jobject surface, jint jw, jint jh, jstring jdir)
{
    jni_job *job;
    const char *dir;
    (void)cls;

    /* Not using SIGSTKSZ: newer bionic defines it as a sysconf() call, not a
     * compile-time constant, so using it as an array length fails to compile.
     * A fixed 64 KB is enough. */
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
/* Already ran, so do not run again (an Activity rebuild can call this twice) */
        job->win = NULL;
    }
    job->w = (int)jw;
    job->h = (int)jh;

    /* ★ 2026-11-07: the log is already set up by init(fd); **do not fopen again**
     * here.
     *
     * An early version did dup2(fd,1/2) onto <game>/inproc.log inside this
     * function -- a process-level operation that replaces the whole app's
     * stdout/stderr and disturbs the ART runtime (ANR on device). It was then
     * changed to fopen, which **never succeeded once** on device, even though
     * Java writing to the same directory worked fine.
     *
     * It now goes through a pipe fd (see the comment on g_logfd). This function
     * only adds the piece of initialisation that needs job->dir; the rest relies
     * on the channel init already established. */
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

/* Align the buffer size with the SurfaceView (0 = use the default format) */
    if (job->win && p_ANativeWindow_setBuffersGeometry && job->w > 0 && job->h > 0)
        p_ANativeWindow_setBuffersGeometry(job->win, job->w, job->h, 0);

    dir = jstring_to_c(env, jdir);
    job->dir = dir ? strdup(dir) : NULL;
    jstring_release(env, jdir, dir);
    JLOG("[jni] gameDir = %s\n", job->dir ? job->dir : "(NULL!)");

    g_running = 1;
    g_stage = 11;                 /* about to create the game thread */
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

/* Let Java query the state */
__attribute__((visibility("default")))
int Java_com_isaacport_launcher_NativeBridge_isRunning(void *env, void *cls)
{
    (void)env; (void)cls;
    return g_running;
}

/* Fall back to the Surface size if it cannot be read (at least the skew is smaller) */
__attribute__((visibility("default")))
int Java_com_isaacport_launcher_NativeBridge_frameCount(void *env, void *cls)
{
    (void)env; (void)cls;
    return (int)host_gl_swap_count();
}

        /* Change the buffer to the render size: SurfaceFlinger will scale it up to
         * the View. Note this step **must come before eglCreateWindowSurface**
         * (gl4es init runs inside main()), otherwise the buffer size is already
         * fixed. */
__attribute__((visibility("default")))
int Java_com_isaacport_launcher_NativeBridge_hasExited(void *env, void *cls)
{
    (void)env; (void)cls;
    return g_finished;
}
