package com.isaacport.launcher;

import android.view.Surface;

/**
 * JNI entry points for running the port in-process.
 *
 * Why this class exists
 * ---------------------
 * The game used to be a standalone executable (launched via ProcessBuilder),
 * which meant no access to Android's Surface -- a Surface is an in-process
 * object with no clean public cross-process API. The only option was
 * pbuffer render + glReadPixels + TCP stream to Java + drawBitmap:
 * 2 MB per frame copied four times, yielding 7 fps.
 *
 * Now the port is built as libisaac.so, dlopen'd into the app process, handing
 * the SurfaceView's Surface straight to EGL:
 *
 *     eglCreateWindowSurface(ANativeWindow_fromSurface(surface))
 *     -> the game renders -> eglSwapBuffers -> SurfaceFlinger
 *
 * Zero copies. This is what the iOS port does (GL bound to a UIView's CALayer).
 *
 * The cost: if guest code crashes, the whole app crashes. So MainActivity keeps
 * child-process mode as a switchable fallback (see useInProcess() there).
 */
public final class NativeBridge {

    private NativeBridge() { }

    /** Reason for a load failure (shown in the UI). */
    public static volatile String loadError;

    /**
     * dlopen the port.
     *
     * @param soPath  absolute path to libisaac.so (must be in the app's private
     *                directory; external storage is noexec)
     * @return true on success
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
     * Bind the log channel. **Must be called before start()**, exactly once.
     *
     * * 2026-11-07, third attempt: switched to a pipe fd.
     * The first two attempts (dup2 / native fopen) both failed on device: dup2
     * disturbed ART and caused an ANR, and fopen never managed to create the
     * file (while Java writing to the same directory worked perfectly).
     * Now Java creates the pipe and passes the raw write-end fd in; native just
     * calls write(2), touching no paths and no permissions.
     *
     * @param logPath fallback file path (used only when the fd is unusable)
     * @param logFd   pipe write-end fd; -1 means file only
     * @return bitmask bit0=fd usable bit1=primary ok bit2=backup ok bit3=path failed
     */
    public static native int init(String logPath, int logFd);

    /**
     * Native startup stage code. Depends on no file, socket or logging library,
     * so it can always be read.
     *
     *   0  not started    5 game thread entered   10 window extracted
     *  11 about to create thread   14 chdir failed  15 chdir done
     *  20 about to enter main()                    30 main() returned
     *  -1 load_android_lib failed  -2 surface is null
     *  -3 calloc failed            -4 pthread_create failed
     */
    public static native int stage();

    /**
     * Diagnostic bits. Also a bare int, depending on no IO whatsoever.
     *
     *   bit0      pipe fd usable
     *   bit1      primary log file fopen succeeded
     *   bit2      backup log file fopen succeeded
     *   bit3      jstring to char* failed
     *   bit8-15   errno of the primary file fopen
     *   bit16-23  errno of the backup file fopen
     *
     * errno reference: 2=ENOENT no such path, 13=EACCES permission denied,
     * 24=EMFILE out of fds, 30=EROFS read-only filesystem.
     */
    public static native int diag();

    /** Build timestamp of the .so (epoch seconds). Confirms you are not running a stale library. */
    public static native int buildEpoch();

    /**
     * Start the game main loop. Spawns a separate thread internally; this method
     * returns immediately.
     *
     * @param surface the SurfaceView's Surface
     * @param w,h     render size (normally the Surface's pixel size)
     * @param gameDir game root directory (contains isaac-ng.exe.unpacked and resources)
     */
    public static native void start(Surface surface, int w, int h, String gameDir);

    /** Whether it is running (main has not returned). Note: a PE load failure can still briefly report true. */
    public static native boolean isRunning();

    /** Frames rendered. The reliable signal that in-process startup succeeded. */
    public static native int frameCount();

    /** Whether main has returned (= startup failed; the game main loop never returns). */
    public static native boolean hasExited();
}
