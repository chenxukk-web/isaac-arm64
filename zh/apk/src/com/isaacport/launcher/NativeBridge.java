package com.isaacport.launcher;

import android.view.Surface;

/**
 * 进程内运行移植层的 JNI 入口。
 *
 * 为什么要有这个类
 * ----------------
 * 以前游戏是独立可执行文件（ProcessBuilder 启动），拿不到 Android 的
 * Surface —— Surface 是进程内对象，跨进程没有干净的公开 API。
 * 只能 pbuffer 渲染 + glReadPixels + TCP 推流给 Java 再 drawBitmap：
 * 每帧 2MB 数据被拷四遍，画面只有 7 fps。
 *
 * 现在把移植层编成 libisaac.so，dlopen 进 app 进程，
 * 把 SurfaceView 的 Surface 直接交给 EGL：
 *
 *     eglCreateWindowSurface(ANativeWindow_fromSurface(surface))
 *     → 游戏渲染 → eglSwapBuffers → SurfaceFlinger
 *
 * 零拷贝。这是 iOS 移植版的做法（GL 绑到 UIView 的 CALayer）。
 *
 * 代价：guest 代码崩了，整个 app 会崩。所以 MainActivity 保留了
 * 子进程模式作为可切换的退路（见该类的 useInProcess()）。
 */
public final class NativeBridge {

    private NativeBridge() { }

    /** 加载失败时的原因（供 UI 显示）。 */
    public static volatile String loadError;

    /**
     * dlopen 移植层。
     *
     * @param soPath  libisaac.so 的绝对路径（必须在 app 私有目录，外部存储是 noexec）
     * @return 成功返回 true
     */
    public static boolean load(String soPath) {
        try {
            System.load(soPath);
            loadError = null;
            return true;
        } catch (Throwable t) {
            loadError = String.valueOf(t);
            return false;
        }
    }

    /**
     * 日志通道绑定。**必须在 start() 之前调用**，且只调一次。
     *
     * ★ 2026-11-07 第三版：改为 pipe fd。
     * 前两版（dup2 / native fopen）在真机上都失败：dup2 冲击 ART 导致 ANR，
     * fopen 从未成功创建过文件（而 Java 写同一目录完全正常）。
     * 现在由 Java 建 pipe，把写端裸 fd 传进来，native 直接 write(2)，
     * 不碰任何路径与权限。
     *
     * @param logPath 兜底用的文件路径（fd 不可用时才用）
     * @param logFd   pipe 写端 fd；传 -1 表示只用文件
     * @return 位掩码 bit0=fd可用 bit1=主文件ok bit2=备文件ok bit3=路径失败
     */
    public static native int init(String logPath, int logFd);

    /**
     * native 启动阶段码。不依赖文件/socket/日志库，永远读得到。
     *
     *   0  未开始      5 游戏线程进入    10 窗口提取完成
     *  11 即将建线程   14 chdir 失败     15 chdir 完成
     *  20 即将进 main()              30 main() 返回
     *  -1 load_android_lib 失败  -2 surface 为 null
     *  -3 calloc 失败           -4 pthread_create 失败
     */
    public static native int stage();

    /**
     * 诊断位。同样是裸 int，不依赖任何 IO。
     *
     *   bit0      pipe fd 可用
     *   bit1      主日志文件 fopen 成功
     *   bit2      备份日志文件 fopen 成功
     *   bit3      jstring 转 char* 失败
     *   bit8-15   主文件 fopen 的 errno
     *   bit16-23  备份文件 fopen 的 errno
     *
     * errno 对照：2=ENOENT 路径不存在，13=EACCES 权限，
     * 24=EMFILE fd 耗尽，30=EROFS 只读。
     */
    public static native int diag();

    /** .so 的编译时间戳（epoch 秒）。用来确认跑的不是旧库。 */
    public static native int buildEpoch();

    /**
     * 启动游戏主循环。内部会起一个独立线程，本方法立即返回。
     *
     * @param surface SurfaceView 的 Surface
     * @param w,h     渲染尺寸（一般是 Surface 的像素尺寸）
     * @param gameDir 游戏根目录（含 isaac-ng.exe.unpacked 和资源）
     */
    public static native void start(Surface surface, int w, int h, String gameDir);

    /** 是否正在运行（main 还没返回）。注意：PE 加载失败也可能短暂为 true。 */
    public static native boolean isRunning();

    /** 已渲染帧数。判断「进程内启动成功」的可靠信号。 */
    public static native int frameCount();

    /** main 是否已经返回（= 启动失败，游戏主循环不返回）。 */
    public static native boolean hasExited();
}
