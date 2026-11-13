package com.isaacport.launcher;

import android.app.Activity;
import android.os.Bundle;
import android.os.Environment;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.io.OutputStream;
import java.io.BufferedWriter;
import java.io.OutputStreamWriter;
import java.net.InetSocketAddress;
import java.net.Socket;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.widget.FrameLayout;
import java.io.DataInputStream;
import java.nio.ByteBuffer;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.Map;

/**
 * Minimal launcher for the arm64-native TBOI:R port.
 *
 * What it does (and deliberately does NOT do):
 *   1. Copies lib/arm64-v8a/libisaac.so out of the APK's native library dir
 *      into the app's own files dir and marks it 0755.  The APK lib dir is
 *      where Android lets us ship a native payload; the copy keeps everything
 *      we exec() inside the app data dir (same place Termux runs its own
 *      binaries from, so it stays inside the untrusted_app_27 exec rules).
 *   2. Starts it with ProcessBuilder, cwd = staged game tree, stdout+stderr
 *      merged and mirrored to logcat (tag IsaacPort) and to the screen.
 *   3. Sets no display surface: the current host_gl4es.c renders into an EGL
 *      pbuffer, so the window stays black on purpose.  See SETUP.md section 5.
 *
 * The child is expected to run until it hits the current known fault; this
 * launcher reports the exit status but does not try to fix anything.
 */
public class MainActivity extends Activity {

    static final String TAG = "IsaacPort";
    static final String PKG = "com.isaacport.launcher";
    static final String PAYLOAD = "libisaac.so";
    static final String EXE_NAME = "isaac";
    static final String PE_NAME = "isaac-ng.exe.unpacked";

    static final int FRAME_PORT = 7138;
    static final int FRAME_MAGIC   = 0x4D524649;  /* 'IFRM' RGBA8888（旧） */
    /* ★ 2026-11-07：native 侧为了把推流带宽减半，改成了 RGB565，
     * magic 用 'IFR5' 区分。两种都要认，否则升级 native 后画面全黑。 */
    static final int FRAME_MAGIC_565 = 0x35524649; /* 'IFR5' RGB565 */

    private TextView out;
    private SurfaceView surface;
    private volatile boolean surfaceReady;
    /** 最近一帧（RGBA，已由 native 翻成从上到下）。供收帧线程绘制。 */
    private Bitmap frameBitmap;
    private volatile boolean logVisible;
    private VirtualPad pad;
    private int sendKeyN;
    private TextView status;
    /** 屏幕右上角的手柄连接指示器（不用翻日志就能判断输入通不通）。 */
    private TextView padHud;
    private volatile long framesGot;
    private StringBuilder buf = new StringBuilder();
    private volatile Process child;

    /* ---------------------------------------------------------------
     * 输入桥：连 127.0.0.1:7137，把按键状态按行推给宿主进程。
     * 宿主侧的 GetAsyncKeyState 查这张表（见 host_user32.c）。
     * --------------------------------------------------------------- */
    static final int IN_PORT = 7137;
    private volatile Socket inSock;
    private volatile BufferedWriter inOut;

    /* PC 版默认键位（VK 码），键盘回退路径用 */
    static final int VK_SPACE = 0x20, VK_E = 0x45, VK_ESC = 0x1B, VK_TAB = 0x09;
    static final int VK_W = 0x57, VK_A = 0x41, VK_S = 0x53, VK_D = 0x44;
    static final int VK_UP = 0x26, VK_DOWN = 0x28, VK_LEFT = 0x25, VK_RIGHT = 0x27;

    /* XInput 按钮位（和 VirtualPad / host_xinput.c 保持一致） */
    static final int XI_DPAD_UP        = 0x0001;
    static final int XI_DPAD_DOWN      = 0x0002;
    static final int XI_DPAD_LEFT      = 0x0004;
    static final int XI_DPAD_RIGHT     = 0x0008;
    static final int XI_START          = 0x0010;
    static final int XI_BACK           = 0x0020;
    static final int XI_LEFT_THUMB     = 0x0040;
    static final int XI_RIGHT_THUMB    = 0x0080;
    static final int XI_LEFT_SHOULDER  = 0x0100;
    static final int XI_RIGHT_SHOULDER = 0x0200;
    static final int XI_A              = 0x1000;
    static final int XI_B              = 0x2000;
    static final int XI_X              = 0x4000;
    static final int XI_Y              = 0x8000;

    /* 手柄状态：虚拟手柄（VirtualPad）一份，实体手柄一份，发送时合成 */
    private int vpButtons, vpLx, vpLy, vpRx, vpRy, vpLt, vpRt;
    private int hwButtons, hwLx, hwLy, hwRx, hwRy, hwLt, hwRt;
    private String lastPadLine;

    /* ==========================================================================
     * 发送队列（★★ 2026-11-10 关键修复）
     *
     * 真机日志里的决定性证据：
     *     !! pad 写失败(1): android.os.NetworkOnMainThreadException
     *
     * 虚拟手柄的触摸回调跑在 **UI 主线程**，而 sendPadNow 直接在
     * 回调里写 socket。Android 从 targetSdk 9 起禁止主线程网络 I/O，
     * 直接抛 NetworkOnMainThreadException。
     *
     * 后果链：
     *   按 A → 主线程写失败 → inOutLost → 重连（后台线程）
     *        → 重连后补发成功 → 游戏看到 btn=1000
     *   再按 → 主线程又失败 → 游戏看到 btn=0000
     * 于是"按住不放"变成高频抖动，表现为"有时候有效"。
     * 161 次重连也是这么来的。
     *
     * 修法：主线程只入队，专用线程负责写。顺带把重复的状态行合并
     * （只保留最新一条），减少无意义的写。
     * ========================================================================== */
    private final Object txLock = new Object();
    private String txPad;                       /* 待发 pad 行，只留最新 */
    private final java.util.ArrayDeque<String> txKeys =
            new java.util.ArrayDeque<String>();
    private boolean txStarted;

    /** 启动发送线程（ onCreate 里调一次，幂等）。 */
    private void startTxThread() {
        synchronized (txLock) {
            if (txStarted) return;
            txStarted = true;
        }
        new Thread(new Runnable() {
            public void run() {
                for (;;) {
                    String pad = null;
                    String[] keys = null;
                    synchronized (txLock) {
                        while (txPad == null && txKeys.isEmpty()) {
                            try { txLock.wait(); }
                            catch (InterruptedException e) { return; }
                        }
                        pad = txPad; txPad = null;
                        if (!txKeys.isEmpty()) {
                            keys = txKeys.toArray(new String[0]);
                            txKeys.clear();
                        }
                    }
                    /* 键盘事件先发（保顺序），pad 状态后发（只留最新）。
                     * 这样"按下 A 同时推摇杆"不会丢按键。 */
                    if (keys != null) {
                        for (String k : keys) writeRaw(k);
                    }
                    if (pad != null) writeRaw(pad);
                }
            }
        }, "isaac-pad-tx").start();
    }

    /** 入队一条 pad 状态（覆盖前一条未发的）。可在任意线程调用。 */
    private void queuePad(String line) {
        synchronized (txLock) {
            txPad = line;
            txLock.notifyAll();
        }
    }

    /** 入队一个键盘事件（保顺序）。可在任意线程调用。 */
    private void queueKey(String line) {
        synchronized (txLock) {
            /* 上限 256：真积压到这个量说明连接已经死了，
             * 再存也没意义，丢掉旧的比无限增长好。 */
            while (txKeys.size() >= 256) txKeys.pollFirst();
            txKeys.addLast(line);
            txLock.notifyAll();
        }
    }

    /** 真正的写。**只允许在 tx 线程里跑** —— 这就是修复的核心。 */
    private void writeRaw(String line) {
        synchronized (writeLock) {
            BufferedWriter w = inOut;
            if (w == null) { ensureBridge(); return; }
            try {
                w.write(line);
                w.write('\n');
                w.flush();
                sendFailStreak = 0;
            } catch (Exception e) {
                sendFailStreak++;
                if (sendFailStreak <= 3) {
                    log("!! tx 写失败(" + sendFailStreak + "): " + e);
                }
                /* 连续 2 次才判定连接已死：一次失败可能是瞬时，
                 * 而重连要 close+bind+accept 一整套，代价大得多。 */
                if (sendFailStreak >= 2) {
                    sendFailStreak = 0;
                    inOutLost();
                }
            }
        }
    }
    /** native 侧诊断日志路径（传给 start）。 */
    private String jniLogPath;
    private int padLogN;
    private int padHudN;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        /* 画面来自 native 的 7138 帧流（离线渲染再推过来），
         * 所以这里是 SurfaceView 铺满 + 底部叠一小条日志。 */
        /* 2026-11-05：保持屏幕常亮。
         *
         * 真机上出现过"进主界面后卡死"：实际是用户盯着屏幕等 200 多秒，
         * 屏幕超时熄灭 → Activity 进入 pause/stop → onDestroy 里
         * p.destroy() 把子进程杀了 → 画面冻在最后一帧。
         * 看起来像卡死，其实是省电机制。
         *
         * 单行修掉：只要这个 Activity 在前台，屏幕就不熄灭。 */
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        /* ★ 2026-11-06：全屏（沉浸式）。用户要求「分辨率为全屏，按钮放小一点」。
         *
         * 三件事必须一起做，少一个都会留下状态栏/导航栏：
         *   1. FLAG_FULLSCREEN —— 隐藏状态栏
         *   2. IMMERSIVE_STICKY —— 隐藏导航栏，且从边缘滑动只临时显示
         *   3. LAYOUT_*_FULLSCREEN —— 让内容延伸到被占用的区域（否则
         *      画面会被顶下去一截，出现黑边）
         *
         * 用 IMMERSIVE_STICKY 而不是 IMMERSIVE：后者一旦用户划过边缘，
         * 系统栏会永久回来；STICKY 会自动再隐藏。 */
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_FULLSCREEN);
        {
            View dv = getWindow().getDecorView();
            dv.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                  | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }

        FrameLayout root = new FrameLayout(this);

        surface = new SurfaceView(this);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            public void surfaceCreated(SurfaceHolder h)  { surfaceReady = true; }
            public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) { surfaceReady = true; }
            public void surfaceDestroyed(SurfaceHolder h) { surfaceReady = false; }
        });
        root.addView(surface, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        /* ★★ 2026-11-07：SurfaceView 保持游戏的宽高比（16:9）。
         *
         * 背景：进程内模式改为按 options.ini 的尺寸（960x540）渲染，
         * 由 SurfaceFlinger 把 buffer 放大到 View。这修好了
         * "画面只占左下角"和"帧率超低"（像素量降到 1/6.5）。
         *
         * 但代价是：屏幕是 2665x1272（宽高比 2.10），而游戏是 16:9
         * （1.78）。直接铺满会把画面横向拉宽 18%，2D 像素游戏会明显变形。
         *
         * 所以让 SurfaceView 自己保持 16:9 居中，两侧留黑边。
         * 黑边由 Android 合成器绘制，代价几乎为零。 */
        root.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            public void onLayoutChange(View v, int l, int t, int r, int b,
                                       int ol, int ot, int or_, int ob) {
                int W = r - l, H = b - t;
                if (W <= 0 || H <= 0) return;
                int w = W, h = W * 9 / 16;
                if (h > H) { h = H; w = H * 16 / 9; }
                FrameLayout.LayoutParams cur =
                        (FrameLayout.LayoutParams) surface.getLayoutParams();
                /* 尺寸没变就别再设 —— setLayoutParams 会再触发一次布局，
                 * 不加这个保护就是无限循环。 */
                if (cur != null && cur.width == w && cur.height == h) return;
                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h);
                lp.gravity = android.view.Gravity.CENTER;
                surface.setLayoutParams(lp);
                log(">> SurfaceView 按 16:9 适配: " + w + "x" + h
                        + "（容器 " + W + "x" + H + "）");
            }
        });

        /* ---------------------------------------------------------------
         * 控制条 + 日志面板
         *
         * 2026-11-04 修正：上一版把日志区做成 WRAP_CONTENT + weight=1，
         * 结果它撑满了大半个屏幕，一打开全是字。日志是调试用的，
         * 不该盖住游戏画面。
         *
         * 现在：两个小按钮固定在左上角，日志面板默认隐藏（GONE），
         * 点 "log" 才展开，且高度锁在屏幕的 35% 以内。
         * 这样默认进去就是纯画面。
         * --------------------------------------------------------------- */

        /* 状态条：帧流到底通没通，一眼可见。
         * 之前只能靠日志猜，用户体验太差。 */
        status = new TextView(this);
        status.setTextSize(11f);
        status.setTextColor(0xFFFFFF00);
        status.setBackgroundColor(0x99000000);
        status.setPadding(10, 6, 10, 6);
        status.setText("帧流: 等待连接…");

        /* ★ 2026-11-06：手柄连接指示器。
         *
         * 真机排查时最大的困扰是「按了没反应，但不知道是没发还是没收到」。
         * 这个小标签直接显示输入桥的状态和最后一次发送的内容：
         *   🎮 连接中… / 🎮 未连接 / 🎮 0x1000
         * 有它就不必再 tail 日志。 */
        padHud = new TextView(this);
        padHud.setTextSize(10f);
        padHud.setTextColor(0xFF00E5FF);
        padHud.setBackgroundColor(0x99000000);
        padHud.setPadding(8, 4, 8, 4);
        padHud.setText("🎮 连接中…");

        FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        slp.gravity = android.view.Gravity.CENTER_HORIZONTAL | android.view.Gravity.TOP;
        slp.topMargin = (int)(getResources().getDisplayMetrics().heightPixels * 0.16f);

        out = new TextView(this);
        out.setTextSize(8f);
        out.setTextColor(Color.WHITE);
        out.setBackgroundColor(0xCC000000);
        out.setPadding(8, 4, 8, 4);

        final ScrollView scroll = new ScrollView(this);
        scroll.addView(out);
        scroll.setVisibility(View.GONE);      /* 默认隐藏 */

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        /* 2026-11-05：控制栏加深色底。
         * 白屏时 Android 默认的浅色 Button 压在纯白画面上几乎看不见。 */
        bar.setBackgroundColor(0xCC000000);

        Button stop = new Button(this);
        stop.setText("stop");
        stop.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Process p = child;
                if (p != null) { p.destroy(); log(">> destroy()"); }
            }
        });

        final Button toggle = new Button(this);
        toggle.setText("log");
        toggle.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                boolean show = scroll.getVisibility() != View.VISIBLE;
                logVisible = show;
                scroll.setVisibility(show ? View.VISIBLE : View.GONE);
                toggle.setText(show ? "hide" : "log");
                if (show) renderLog();
            }
        });

        final Button padBtn = new Button(this);
        padBtn.setText("pad");
        padBtn.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                boolean show = pad.getVisibility() != View.VISIBLE;
                if (!show) pad.releaseAll();       /* 藏起来先松键，别卡住 */
                pad.setVisibility(show ? View.VISIBLE : View.GONE);
                padBtn.setText(show ? "nopad" : "pad");
            }
        });

        /* ★ 2026-11-06：按钮缩小 + 靠左紧凑排列。
         *
         * 用户反馈「上面那个按钮有点挡位置」。原来三个按钮 weight=1 铺满
         * 整行、高度用 WRAP_CONTENT（Android 默认 Button 有 ~48dp 最小高度），
         * 在全屏游戏上占掉顶部一整条。
         *
         * 现在：固定小尺寸（约 68x34 dp）、靠左上、整条高度压到 34dp。
         * 只占屏幕左上角一小块，不压画面中心。 */
        int bw = (int)(getResources().getDisplayMetrics().density * 56);
        int bh = (int)(getResources().getDisplayMetrics().density * 26);
        Button[] smallBtns = new Button[]{stop, toggle, padBtn};
        for (Button sb : smallBtns) {
            sb.setTextSize(10f);
            sb.setPadding(4, 0, 4, 0);
            sb.setMinWidth(0);
            sb.setMinimumWidth(0);
            sb.setMinHeight(0);
            sb.setMinimumHeight(0);
        }
        LinearLayout.LayoutParams btnLp = new LinearLayout.LayoutParams(
                bw, bh);
        btnLp.rightMargin = (int)(getResources().getDisplayMetrics().density * 2);
        bar.addView(stop, btnLp);
        bar.addView(toggle, btnLp);
        bar.addView(padBtn, btnLp);

        /* 虚拟手柄：叠在画面之上、日志面板之下。
         * ★ 2026-11-06：直接产生 XInput 手柄状态（不再发 VK 码）。
         * 游戏主路径是 XInput，见 host_xinput.c。 */
        pad = new VirtualPad(this);
        pad.setSink(new VirtualPad.Sink() {
            public void pad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt) {
                vpButtons = buttons;
                vpLx = lx; vpLy = ly; vpRx = rx; vpRy = ry; vpLt = lt; vpRt = rt;
                sendPadNow();
            }
        });
        pad.setTrace(new VirtualPad.Trace() {
            public void put(String s) { log(">> " + s); }
        });
        root.addView(pad, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        root.addView(status, slp);

        FrameLayout.LayoutParams hlp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        hlp.gravity = android.view.Gravity.END | android.view.Gravity.TOP;
        hlp.topMargin = (int)(getResources().getDisplayMetrics().heightPixels * 0.02f);
        hlp.rightMargin = 8;
        root.addView(padHud, hlp);

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        /* bar 不再 MATCH_PARENT：只占内容宽度，靠左上角。 */
        panel.addView(bar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        /* 日志最多占屏幕高度的 35%，超出滚动 */
        int logMax = (int)(getResources().getDisplayMetrics().heightPixels * 0.35f);
        panel.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, logMax));

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = android.view.Gravity.TOP;   /* 贴左上，不压画面中心 */
        panel.setLayoutParams(lp);
        root.addView(panel);

        setContentView(root);

        /* 让根布局可获取焦点，手柄事件才会走到 Activity 的 onKeyDown。 */
        root.setFocusable(true);
        root.setFocusableInTouchMode(true);

        /* ★★ 2026-11-10：性能 HUD —— fps / CPU / 内存 / 线程。
         *
         * 为什么需要：用户报告“帧率低”但只能描述感受，而 [perf] 日志
         * 只在 native 脏攒满 60 帧后才输出。屏幕上看不到数字就没法
         * 判断“是持续 5fps 还是偶发卡顿”，也无法对比优化前后的变化。
         *
         * 数据来源（全部读 /proc，无额外进程、无系统权限）：
         *   · fps        → NativeBridge.frameCount() 的差分
         *   · CPU%       → /proc/self/stat 的 utime+stime 差分 / 墙钟
         *   · 内存       → /proc/self/status 的 VmRSS
         *                   （对应旧版 /proc/self/statm 的 resident）
         *   · 线程数     → /proc/self/status 的 Threads
         *
         * 每秒刷新一次。用等宽字体、小字号、右上角 —— 不挡操作。 */
        final TextView perfHud = new TextView(this);
        perfHud.setTextSize(9f);
        perfHud.setTextColor(0xFF00FFB0);
        perfHud.setBackgroundColor(0xB0000000);
        perfHud.setPadding(8, 3, 8, 3);
        perfHud.setTypeface(android.graphics.Typeface.MONOSPACE);
        perfHud.setText("perf: 等待…");
        FrameLayout.LayoutParams perfLp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        perfLp.gravity = android.view.Gravity.TOP | android.view.Gravity.END;
        perfLp.topMargin = (int)(getResources().getDisplayMetrics().density * 40);
        perfLp.rightMargin = (int)(getResources().getDisplayMetrics().density * 8);
        root.addView(perfHud, perfLp);

        startPerfHud(perfHud);

        final File externalDir = resolveGameDir();

        /* Java 侧日志固定写到**外部目录**（FUSE），不用 ext4 那份 ——
         * 目的是让 Termux / 文件管理器能直接读到，排查时不用 adb。 */
        try {
            appLog = new OutputStreamWriter(
                    new FileOutputStream(new File(externalDir, "app.log")), "UTF-8");
            log(">> app.log started");
        } catch (Exception e) {
            Log.w(TAG, "app.log unavailable: " + e);
        }

        /* ★ stageToInternal 会拷 3.5 GB，绝不能在 UI 线程做（会 ANR）。
         * 放进 launch 线程的第一步。 */
        /* 启动计时器：状态条每秒 +1。
         * 归档加载要 3 分钟以上且几乎没有输出，没有这个用户会以为卡死。 */
        final long t0 = System.currentTimeMillis();
        new Thread(new Runnable() {
            public void run() {
                for (int i = 0; i < 1200; i++) {
                    if (framesGot > 0) return;
                    final int sec = (int)((System.currentTimeMillis() - t0) / 1000);
                    runOnUiThread(new Runnable() {
                        public void run() {
                            if (status != null && framesGot == 0
                                    && status.getVisibility() == View.VISIBLE)
                                status.setText("启动中… " + sec + " 秒"
                                        + "（加载归档约需 3 分钟，请稍候）");
                        }
                    });
                    try { Thread.sleep(1000); } catch (InterruptedException ie) { return; }
                }
            }
        }, "isaac-uptime").start();

        new Thread(new Runnable() {
            public void run() {
                /* ★ 2026-11-07：先把自己的内存布局存下来。
                 *
                 * 进程内模式失败的根因是 guest 镜像基址 0x30000000 被占。
                 * gen 代码里全是硬编码的绝对地址（0x307aa3b4 这类），
                 * 镜像只能落在那里。要判断是谁占的、换到哪里可行，
                 * 必须看真实的内存映射。
                 *
                 * Java 读自己的 /proc/self/maps 不需要任何权限 ——
                 * 这是 app 进程自己的地图，包含 ART 堆、lib 加载位置。
                 * 存到外部目录，Termux 直接 cat 就能看。 */
                dumpMaps(new File(externalDir, "app_maps.txt"));

                File gameDir = stageToInternal(externalDir,
                        new File(getFilesDir(), "game"));
                launch(gameDir, externalDir);
            }
        }, "isaac-launch").start();

        /* ★★ 2026-11-10：先启动发送线程，再建连接。
         * 它是所有 socket 写入的唯一执行者 —— 触摸/手柄回调只入队。
         * 不启动的话 sendPadNow 的入队会永远没人消费。 */
        startTxThread();

        new Thread(new Runnable() {
            public void run() { connectInputBridge(); }
        }, "isaac-input").start();
        new Thread(new Runnable() {
            public void run() { runFrameReceiver(); }
        }, "isaac-frames").start();
    }

    /** 把 /proc/self/maps 存到外部目录。
     *
     * 用途：诊断「guest 镜像基址 0x30000000 被谁占用」。
     * 只读自己的进程地图，不需要任何权限。 */
    private void dumpMaps(File out) {
        java.io.InputStream in = null;
        java.io.FileOutputStream w = null;
        try {
            in = new java.io.FileInputStream("/proc/self/maps");
            w = new java.io.FileOutputStream(out);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) w.write(buf, 0, n);
            log(">> maps 已存: " + out);
        } catch (Exception e) {
            log("!! dumpMaps: " + e);
        } finally {
            if (in != null) try { in.close(); } catch (Exception ignored) { }
            if (w != null) try { w.close(); } catch (Exception ignored) { }
        }
    }

    // ------------------------------------------------------------ in-process

    /** 决定用进程内 .so 还是子进程。
     *
     * 优先级：
     *   1. <game>/exec 存在        → 强制子进程（排查用）
     *   2. <game>/libisaac.so 存在  → in-process（外置 .so，快速迭代）
     *   3. APK 内 libisaac.so 存在  → in-process
     *   4. 都没有                   → 子进程（老 ELF 兜底）
     *
     * 为什么外置 .so 要拷到私有目录：/storage/emulated 是 noexec，
     * System.load() 从那里会失败。私有目录（ext4）可以。 */
    private boolean useInProcess(File externalDir) {
        try {
            if (new File(externalDir, "exec").exists()) {
                log(">> 模式: 子进程（发现 exec 开关文件）");
                return false;
            }
            if (resolveSoPath(externalDir) != null) {
                log(">> 模式: 进程内 .so（直连 Surface）");
                return true;
            }
        } catch (Exception e) {
            log("!! useInProcess: " + e);
        }
        log(">> 模式: 子进程（没有找到 libisaac.so）");
        return false;
    }

    /** 找 .so：外置优先（同步到私有目录），否则用 APK 内的。
     * 返回可 System.load 的绝对路径；都没有返回 null。 */
    private String resolveSoPath(File externalDir) throws IOException {
        File dir = new File(getFilesDir(), "lib");
        if (!dir.isDirectory() && !dir.mkdirs()) return null;
        File local = new File(dir, "libisaac.so");

        /* ★ 2026-11-07：把 libopenal.so 也放到同一目录。
         *
         * host_openal.c 先用 dlopen("libopenal.so") 让链接器在
         * nativeLibraryDir 里找；找不到时再用 dladdr 推自己的目录
         * （就是这个 dir）。两边都备好，无论 dlopen 走哪条都能命中。
         * 原来 OpenAL 层是记账桩，从没真实发声；这一步才让游戏有声音。 */
        try {
            String nd = getApplicationInfo().nativeLibraryDir;
            if (nd != null) {
                File src = new File(nd, "libopenal.so");
                if (src.isFile()) {
                    File dst = new File(dir, "libopenal.so");
                    if (!dst.isFile() || dst.length() != src.length())
                        copyFile(src, dst);
                }
            }
        } catch (Throwable t) {
            log(">> libopenal 同步跳过: " + t);
        }

        /* ---- 1) 外置 .so ---- */
        File ext = new File(externalDir, "libisaac.so");
        if (ext.isFile() && ext.length() > 0) {
            long extLen = ext.length();
            long extTime = ext.lastModified();
            File stamp = new File(dir, "libisaac.so.stamp");

            /* ★★ 2026-11-10：指纹从「长度:时间」改成「长度:时间:首尾哈希」。
             *
             * 为什么必须改：外置 .so 是唯一的迭代通道（不用重装 APK），
             * 而旧的指纹只看长度+时间。两个问题：
             *   · 只改一两行代码时长度可能恰好相同 → 指纹相同 → 用旧副本
             *   · 文件管理器/云盘回写时 mtime 可能被重置
             * 后果是**你以为测了新库，实际跑的是旧的** —— 白浪费一轮真机测试。
             *
             * 新指纹 = 长度 + mtime + 前 64KB/后 64KB 的哈希。
             * 只读 128KB，成本 < 10ms（相对于 70MB 全量哈希的几秒可忽略）。
             * ELF 头部含构建差异，尾部含符号表差异，两者一起足以区分构建。 */
            String want = extLen + ":" + extTime + ":" + quickHash(ext);
            if (local.isFile() && local.length() == extLen
                    && want.equals(readSmall(stamp))) {
                log(">> so: 外置未变，复用内部副本");
            } else {
                log(">> so: 同步外置 " + ext + "（" + (extLen / (1024 * 1024))
                        + " MB）到私有目录");
                copyFile(ext, local);
                /* 写完回读校验：拷贝被截断/失败时不要留下"看起来对"的 stamp */
                if (local.length() != extLen) {
                    log("!! so: 拷贝后长度不符（" + local.length() + " vs "
                            + extLen + "），下次启动会重试");
                } else {
                    writeSmall(stamp, want);
                }
            }
            return local.getAbsolutePath();
        }

        /* ---- 2) APK 内的 ---- */
        String libDir = getApplicationInfo().nativeLibraryDir;
        File packed = new File(libDir == null ? "" : libDir, PAYLOAD);
        if (packed.isFile() && packed.length() > 0) {
            log(">> so: 用 APK 内的 " + packed);
            return packed.getAbsolutePath();
        }
        return null;
    }

    /** 进程内启动：加载 .so，等 Surface 就绪，交给 JNI。 */
    private void launchInProcess(final File gameDir, final File externalDir) {
        try {
            final String soPath = resolveSoPath(externalDir);
            if (soPath == null) throw new IOException("找不到 libisaac.so");

            if (!NativeBridge.load(soPath)) {
                log("!! System.load 失败: " + NativeBridge.loadError);
                log("!! 退回子进程模式");
                launchInProcessFallback(gameDir, externalDir);
                return;
            }
            log(">> so 已加载: " + soPath);

            /* ★ 先确认跑的是哪一版 .so。
             * 旧库没有新加的符号 → 调 diag()/stage() 会抛
             * UnsatisfiedLinkError，所以这里就必须先探一下。 */
            try {
                long be = NativeBridge.buildEpoch() & 0xFFFFFFFFL;
                log(">> so buildEpoch=" + be + "（"
                        + new java.text.SimpleDateFormat("MM-dd HH:mm:ss")
                            .format(new java.util.Date(be * 1000L)) + "）");
                if (be == 0) log("!! .so 未带 BUILD_EPOCH —— 几乎肯定是旧库");
            } catch (Throwable t) {
                log("!! .so 缺 buildEpoch —— 是旧库，新诊断全部无效: " + t);
            }

            /* ★ 2026-11-07 第四版：**真实文件 fd**，不用 pipe。
             *
             * 前三版全失败，原因各不相同：
             *   v1  dup2(fd,1/2)   → 冲击 ART，ANR
             *   v2  native fopen    → 真机上一次都没创建出文件（errno 未知）
             *   v3  pipe + Java 读 → 崩溃时进程一起死，读端没了，
             *                          缓冲区里的崩溃信息永远读不到
             *
             * v3 的教训最关键：**日志通道必须能在进程死亡时独立生效**。
             * pipe 依赖一个活着的读者，本质上是错的。
             *
             * v4：Java 用 FileOutputStream 打开文件（已被证明可靠——
             * 同一个目录的 app.log 一直写得正常），detachFd() 拿到裸 fd
             * 交给 native。native 直接 write(2)，不碰路径权限，
             * crash handler 也写同一个 fd —— 进程死了数据已在页缓存里。
             *
             * 顺带解决 SIGPIPE：文件 fd 没有 SIGPIPE 这个概念。 */
            final File nativeLog = new File(externalDir, "native.log");
            jniLogPath = new File(externalDir, "inproc.log").getAbsolutePath();
            try {
                int wfd = -1;
                try {
                    /* ★ 用 ParcelFileDescriptor.open 而不是 FileOutputStream：
                     * FileDescriptor.detachFd() 是隐藏 API，编译不过；
                     * ParcelFileDescriptor.detachFd() 是公开的（API 12+）。
                     *
                     * 不用 TRUNCATE：万一后面失败，上次的内容还能留作对比。 */
                    android.os.ParcelFileDescriptor pfd =
                            android.os.ParcelFileDescriptor.open(nativeLog,
                                    android.os.ParcelFileDescriptor.MODE_WRITE_ONLY
                                    | android.os.ParcelFileDescriptor.MODE_CREATE
                                    | android.os.ParcelFileDescriptor.MODE_TRUNCATE);
                    wfd = pfd.detachFd();
                    log(">> native.log fd=" + wfd + " 已就绪（Java 侧已确认可打开）");
                } catch (Throwable t) {
                    log("!! 打开 native.log 失败: " + t);
                }

                log(">> 即将调用 init(path, fd=" + wfd + ")");

                /* ★ 2026-11-07：stage 追踪线程。
                 *
                 * init 里崩溃的话，进程直接没了 —— Java 这边什么都来不及
                 * 写。但 stage 是原生内存里的一个 int，一旦设好就能读。
                 * 这个线程每 50ms 读一次，**只在变化时**写 app.log。
                 * 万一 init 崩了，最后一行就是最后已知阶段。
                 *
                 * 只追 15 秒：init 本身是毫秒级的，超过就说明正常了。 */
                new Thread(new Runnable() {
                    public void run() {
                        int last = -999;
                        for (int i = 0; i < 300; i++) {
                            try { Thread.sleep(50); }
                            catch (InterruptedException e) { return; }
                            int st;
                            try { st = NativeBridge.stage(); }
                            catch (Throwable t) { return; }   /* 旧库没这符号 */
                            if (st != last) {
                                last = st;
                                log(">> [追踪] stage=" + st);
                            }
                            if (st >= 30) return;   /* 已进 main()，后续靠自己 */
                        }
                    }
                }, "isaac-stage-track").start();

                int rc = NativeBridge.init(jniLogPath, wfd);
                log(">> native 日志通道：fd=" + wfd + "  init rc=" + rc
                        + "  diag=0x" + Integer.toHexString(NativeBridge.diag())
                        + "  (bit0=fd bit1=文件 bit2=备文件 bit3=路径失败)");
            } catch (Throwable t) {
                log("!! 日志通道建立失败: " + t);
                jniLogPath = new File(externalDir, "inproc.log").getAbsolutePath();
                try {
                    int rc = NativeBridge.init(jniLogPath, -1);
                    log(">> native 日志通道：仅文件  init rc=" + rc);
                } catch (Throwable t2) {
                    log("!! NativeBridge.init 也失败了: " + t2);
                }
            }

            if (!gameDir.isDirectory()) {
                log("!! game dir missing: " + gameDir);
            }

            /* 等 Surface 就绪（surfaceCreated 是异步的，可能要几帧）。 */
            new Thread(new Runnable() {
                public void run() {
                    for (int i = 0; i < 600; i++) {
                        if (surfaceReady && surface != null) break;
                        try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                    }
                    if (!surfaceReady) {
                        log("!! Surface 60 秒内没就绪");
                        return;
                    }
                    android.view.SurfaceHolder hd = surface.getHolder();

                    /* ★ 2026-11-07：必须等到 Surface 真正可用。
                     * ANativeWindow_fromSurface 对无效 Surface 返回 NULL，
                     * 那样就只能退回 pbuffer（没有全屏直连）。
                     * isValid() 为 true 才说明底层 buffer 已分配。 */
                    int waited = 0;
                    while (!hd.getSurface().isValid() && waited < 100) {
                        try { Thread.sleep(100); } catch (InterruptedException ie) { return; }
                        waited++;
                    }
                    log(">> Surface isValid=" + hd.getSurface().isValid()
                            + "（等了 " + (waited * 100) + " ms）");

                    android.graphics.Rect fr = hd.getSurfaceFrame();
                    int w = (fr != null && fr.width() > 0) ? fr.width() : 960;
                    int h = (fr != null && fr.height() > 0) ? fr.height() : 540;

                    /* 渲染尺寸跟 Surface 一致；游戏内部再按 options.ini
                     * 的逻辑缩放到自己的虚拟分辨率。 */
                    log(">> 启动进程内渲染 " + w + "x" + h + "  cwd=" + gameDir);
                    runOnUiThread(new Runnable() {
                        public void run() {
                            if (status != null) {
                                status.setVisibility(View.VISIBLE);
                                status.setText("进程内启动中…（加载约 20 秒）");
                            }
                        }
                    });
                    NativeBridge.start(surface.getHolder().getSurface(),
                            w, h, gameDir.getAbsolutePath());
                    log(">> NativeBridge.start 已调用");

                    /* ★ 2026-11-07：存活检查 + stage 心跳。
                     *
                     * 上一版只看 inproc.log 存不存在，但日志通道本身
                     * 可能失败。现在交替报告两件事：
                     *   · native.log 有没有内容（pipe 通不通）
                     *   · stage() 跑到哪一步（不依赖任何 IO，永远可读） */
                    new Thread(new Runnable() {
                        public void run() {
                            File lf = new File(externalDir, "native.log");
                            for (int i = 0; i < 12; i++) {
                                try { Thread.sleep(500); }
                                catch (InterruptedException e) { return; }
                                if (lf.isFile() && lf.length() > 0) {
                                    log(">> native.log 存活：" + lf.length() + " 字节");
                                    return;
                                }
                            }
                            log("!! 6 秒仍无 native.log：pipe 没通"
                                    + "（stage=" + NativeBridge.stage()
                                    + " diag=0x"
                                    + Integer.toHexString(NativeBridge.diag()) + "）");
                        }
                    }, "isaac-log-watch").start();

                    /* ★ 2026-11-07：.so 加载之后再 dump 一份 maps。
                     * 启动时那份（app.log 第 2 行）在 System.load 之前，
                     * 里面既没有 libisaac.so 也没有 0x3C000000 的 guest 镜像，
                     * 判断不了镜像和可执行内存到底映射成功没有。 */
                    new Thread(new Runnable() {
                        public void run() {
                            try { Thread.sleep(3000); }
                            catch (InterruptedException e) { return; }
                            dumpMaps(new File(externalDir, "inproc_maps.txt"));
                        }
                    }, "isaac-maps").start();

                    /* ★ 2026-11-06：监督线程。
                     *
                     * 进程内模式如果失败（guest mmap 撞上 ART、PE 加载失败、
                     * EGL 建不起来），表现是「一直黑屏」。用户分不清是
                     * 还在加载还是已经废了。
                     *
                     * 判据：
                     *   · hasExited()  → main 返回了，立刻判定失败
                     *   · 45 秒后 frameCount() 仍为 0 → 判定失败
                     * 失败就自动切回子进程模式（有画面，只是 fps 低）。
                     */
                    final long tStart = System.currentTimeMillis();
                    new Thread(new Runnable() {
                        public void run() {
                            for (int i = 0; i < 300; i++) {
                                try { Thread.sleep(500); }
                                catch (InterruptedException e) { return; }
                                int fc = NativeBridge.frameCount();
                                boolean exited = NativeBridge.hasExited();
                                /* ★ 2026-11-07：每 10 秒留一条心跳。
                                 * 原来只在 fc>0 或 exited 时写日志，于是
                                 * "还活着但一帧都没有" 这个状态在 app.log 里
                                 * 是完全静默的 —— 分不清是在加载还是死了。 */
                                if (i > 0 && (i % 20) == 0) {
                                    log(">> inproc " + (i / 2) + "s: frames=" + fc
                                            + " exited=" + (exited ? 1 : 0)
                                            + " stage=" + NativeBridge.stage()
                                            + " diag=0x"
                                            + Integer.toHexString(NativeBridge.diag()));
                                }
                                if (fc > 0) {
                                    log(">> 进程内渲染已启动（首帧在 "
                                            + ((System.currentTimeMillis() - tStart) / 1000)
                                            + " 秒）");
                                    return;
                                }
                                if (exited) {
                                    log("!! 进程内启动失败：main 已返回，切回子进程");
                                    runOnUiThread(new Runnable() {
                                        public void run() {
                                            if (status != null) {
                                                status.setVisibility(View.VISIBLE);
                                                status.setText("进程内启动失败，切回子进程模式…");
                                            }
                                        }
                                    });
                                    launchInProcessFallback(gameDir, externalDir);
                                    return;
                                }
                            }
                            log("!! 进程内 150 秒无帧，切回子进程");
                            launchInProcessFallback(gameDir, externalDir);
                        }
                    }, "isaac-inproc-watch").start();
                }
            }, "isaac-inproc-start").start();

            /* 输入桥仍然走 TCP：VirtualPad → sendPadNow → 7137。
             * native 侧的 u_bridge_thread 在 .so 里同样会起监听线程，
             * 这条链已经在真机上验证过，不重复造。 */
            new Thread(new Runnable() {
                public void run() { connectInputBridge(); }
            }, "isaac-input").start();

            /* ★ 2026-11-07：进程内模式也要起收帧线程。
             *
             * 理由：如果 eglCreateWindowSurface 失败（或在 JNI 层就被判定
             * 窗口不可信），native 会退回 pbuffer 模式，此时它仍然会把帧
             * 推到 TCP:7138。没有收帧线程就是纯黑屏 —— 用户完全不知道
             * 发生了什么，也玩不了。
             *
             * 窗口模式成功时 7138 根本不会监听，这个线程的 connect 会失败、
             * 静默重试到超时，不影响任何东西。 */
            new Thread(new Runnable() {
                public void run() { runFrameReceiver(); }
            }, "isaac-frames").start();
        } catch (Exception e) {
            log("!! launchInProcess 失败: " + e);
            launchInProcessFallback(gameDir, externalDir);
        }
    }

    /** 进程内启动失败时的兜底。
     *
     * ★ 这里**不能**直接 startSubprocess：
     *   .so 已经在当前进程里跑起来了，它的 u_bridge_thread 占着 TCP:7137
     *   死循环监听。再起一个子进程，子进程 bind(7137) 会失败，
     *   输入桥直接废掉 —— 而且两个"游戏"同时在跑，日志互相污染。
     *
     * 正确做法：写一个 <game>/exec 开关文件，然后提示用户重开 app。
     * 下次启动 useInProcess() 看到 exec 就直接走子进程，整个进程是干净的。 */
    private void launchInProcessFallback(File gameDir, File externalDir) {
        try {
            File flag = new File(externalDir, "exec");
            if (!flag.exists()) {
                //noinspection ResultOfMethodCallIgnored
                flag.createNewFile();
            }
            log(">> 已写入 " + flag + "，下次启动将使用子进程模式");
            runOnUiThread(new Runnable() {
                public void run() {
                    if (status != null) {
                        status.setVisibility(View.VISIBLE);
                        status.setText("进程内模式失败，已切换到子进程模式。"
                                + "请划掉本应用重开。");
                    }
                }
            });
        } catch (Exception e) {
            log("!! 兜底失败: " + e);
        }
    }

    // ---------------------------------------------------------------- paths

    /** Staged game tree: /sdcard/Android/media/<pkg>/files/game unless overridden. */
    /* ------------------------------------------------------------------
     * 资源搬到 ext4
     *
     * 2026-11-05：真机上一切正常但极慢，实测一轮要 fopen 23029 次，
     * 而 /storage/emulated/0 是 FUSE —— 比 ext4 慢一个数量级。
     * 归档加载（9 个 .a 共约 1GB）在 Termux 的 ext4 上要 150~200 秒，
     * 真机只会更久。
     *
     * 做法：首次启动把 game 树整份拷进 app 私有目录
     * （/data/user/0/<pkg>/files/game，ext4），之后 cwd 指向那里。
     * 拷贝多少秒，换来的是之后每次启动都快得多。
     *
     * 用 .stage_ok 标记 + 源目录 mtime 判断是否需要重拷；
     * Termux 那边 stage_runtime.sh 重铺资源后会自动触发一次重拷。
     * ------------------------------------------------------------------ */
    private File stageToInternal(File src, File dst) {
        File marker = new File(dst, ".stage_ok");
        /* ★ 2026-11-05：判据必须只看**资源本身**，不能看 src 目录整体。
         *
         * 原来用 src.lastModified()，而 push_elf.sh 会往 src 里写 isaac
         * （70MB 的 ELF），目录 mtime 一变就判定"资源过期"，
         * 于是每次改 native 都触发一次 3.5GB 的重拷 —— 完全不该发生。
         *
         * 现在取 resources 目录的 mtime（那是 stage_runtime.sh 铺设的产物），
         * 再拼上资源总字节数。两者都不受 ELF 推送影响。 */
        long srcStamp = 0;
        try {
            File res = new File(src, "resources");
            long t = res.isDirectory() ? res.lastModified() : src.lastModified();
            long bytes = dirSize(new File(dst, "resources"));
            srcStamp = t * 1000003L + bytes;   /* 简单混合，避免两者巧合相等 */
        } catch (Exception ignored) { }

        if (marker.isFile()) {
            try {
                String was = readSmall(marker);
                if (was != null && was.trim().equals(Long.toString(srcStamp))) {
                    log(">> internal stage up-to-date");
                    return dst;
                }
            } catch (Exception ignored) { }
            log(">> internal stage stale, re-copying");
        }

        log(">> staging to internal storage (ext4), this takes a few minutes once…");
        long t0 = System.currentTimeMillis();
        long[] copied = new long[1];
        int[] files = new int[1];
        copyTree(src, dst, copied, files);

        try {
            java.io.FileWriter w = new java.io.FileWriter(marker);
            w.write(Long.toString(srcStamp));
            w.close();
        } catch (Exception e) {
            log("!! cannot write stage marker: " + e);
        }
        long ms = System.currentTimeMillis() - t0;
        log(">> staged " + files[0] + " files / "
                + (copied[0] / (1024 * 1024)) + " MB in " + (ms / 1000) + "s");
        runOnUiThread(new Runnable() {
            public void run() {
                if (status != null) status.setText("帧流: 启动中…");
            }
        });
        return dst;
    }

    /** 递归统计目录总字节数。用于 staging 判据。 */
    private static long dirSize(File d) {
        if (d == null) return 0;
        if (d.isFile()) return d.length();
        File[] kids = d.listFiles();
        if (kids == null) return 0;
        long sum = 0;
        for (File k : kids) sum += dirSize(k);
        return sum;
    }

    private static String readSmall(File f) {
        try {
            java.io.FileInputStream in = new java.io.FileInputStream(f);
            byte[] b = new byte[64];
            int n = in.read(b);
            in.close();
            return n > 0 ? new String(b, 0, n, "UTF-8") : null;
        } catch (Exception e) {
            return null;
        }
    }

    /** 递归拷贝。符号链接不跟随（源侧已由 stage_runtime.sh 展开成实文件）。 */
    private void copyTree(File src, File dst, long[] bytes, int[] files) {
        if (src.isDirectory()) {
            if (!dst.isDirectory() && !dst.mkdirs()) {
                log("!! mkdir failed: " + dst);
                return;
            }
            File[] kids = src.listFiles();
            if (kids == null) return;
            for (File k : kids) {
                String n = k.getName();
                /* 跳过日志（持续变化）和可执行文件（由 resolveExe 单独管理，
                 * 且 70MB 拷进去纯属浪费——它本来就要落进 bin/ 里跑）。 */
                /* ★ 加 libisaac.so / exec / fast：
                 *   前两个不是游戏资源（.so 有 73MB，拷进内部纯属浪费，
                 *   而且它会通过 System.load 从外部路径直接用）；
                 *   exec / fast 是我们自己的开关文件，不该进游戏目录。 */
                if (n.equals("run.log") || n.equals("app.log")
                        || n.equals("isaac") || n.equals(".stage_ok")
                        || n.equals("libisaac.so") || n.equals("libisaac.so.stamp")
                        || n.equals("exec") || n.equals("fast")) continue;
                copyTree(k, new File(dst, n), bytes, files);
            }
        } else if (src.isFile()) {
            File parent = dst.getParentFile();
            if (parent != null && !parent.isDirectory()) parent.mkdirs();
            java.io.FileInputStream in = null;
            java.io.FileOutputStream out = null;
            try {
                in = new java.io.FileInputStream(src);
                out = new java.io.FileOutputStream(dst);
                byte[] buf = new byte[1 << 20];
                int n;
                while ((n = in.read(buf)) > 0) { out.write(buf, 0, n); bytes[0] += n; }
                out.flush();
                files[0]++;
                if ((files[0] % 1000) == 0) {
                    final String msg = "首次启动：正在搬资源到内部存储 "
                            + (bytes[0] / (1024 * 1024)) + " MB / " + files[0] + " 个文件";
                    log("   … " + msg);
                    runOnUiThread(new Runnable() {
                        public void run() {
                            if (status != null) status.setText(msg);
                        }
                    });
                }
            } catch (Exception e) {
                log("!! copy failed " + src + ": " + e);
            } finally {
                if (in != null) try { in.close(); } catch (Exception ignored) { }
                if (out != null) try { out.close(); } catch (Exception ignored) { }
            }
        }
        /* 符号链接 / 设备文件：跳过 */
    }

    private File resolveGameDir() {
        String override = getIntent() != null ? getIntent().getStringExtra("gamedir") : null;
        if (override != null && override.length() > 0) return new File(override);
        File ext = Environment.getExternalStorageDirectory();
        return new File(ext, "Android/media/" + PKG + "/files/game");
    }

    /* ------------------------------------------------------------------
     * 可执行文件的来源（2026-11-05：外置，免重装）
     *
     * 目的：改了 native 代码（host_*.c 等）之后，只把新的 ELF 丢到
     * 外部目录、重启 app 就能测，不必重装 APK。
     *
     * 为什么要"外部放 + 内部跑"两步：
     *   /storage/emulated/0 是 FUSE，挂载时带 noexec —— 实测在那里
     *   chmod +x 之后仍然 Permission denied。所以 ELF 必须落回
     *   app 私有目录（ext4）才能 exec。
     *
     * 查找顺序：
     *   1) <game>/isaac            ← 外置，优先（改 native 走这条）
     *   2) APK 内 lib/arm64-v8a/libisaac.so  ← 兜底
     *
     * 同步判据：length + lastModified。两者任一变化就重拷。
     * 比算 hash 快得多（70MB 算一次要几百毫秒），而且改代码必然变长度
     * 或时间戳，实践中够用。
     * ------------------------------------------------------------------ */
    private File resolveExe(File externalDir) throws IOException {
        File dir = new File(getFilesDir(), "bin");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("cannot create " + dir);
        }
        File exe = new File(dir, EXE_NAME);

        /* ---- 1) 外置 ELF 优先 ---- */
        File ext = new File(externalDir, "isaac");
        if (ext.isFile() && ext.length() > 0) {
            long extLen = ext.length();
            long extTime = ext.lastModified();
            File stamp = new File(dir, EXE_NAME + ".stamp");
            String want = extLen + ":" + extTime;

            if (exe.isFile() && exe.length() == extLen &&
                    want.equals(readSmall(stamp))) {
                log(">> exe: 外置 ELF 未变，复用内部副本（免重装）");
            } else {
                log(">> exe: 发现外置 ELF " + ext
                        + "（" + (extLen / (1024 * 1024)) + " MB），同步到内部");
                copyFile(ext, exe);
                writeSmall(stamp, want);
            }
            if (!exe.canExecute()) {
                exe.setExecutable(true, false);
                exe.setReadable(true, false);
            }
            if (!exe.canExecute()) throw new IOException("not executable: " + exe);
            log(">> exe   = " + exe + " (" + exe.length() + " bytes, 来源=外置)");
            return exe;
        }

        /* ---- 2) 回退到 APK 内的 payload ---- */
        String libDir = getApplicationInfo().nativeLibraryDir;
        File packed = new File(libDir == null ? "" : libDir, PAYLOAD);
        long packedLen = packed.length();

        if (!exe.exists() || exe.length() != packedLen) {
            log(">> exe: 无外置 ELF，从 APK 解出 " + packed + " -> " + exe);
            copyFile(packed, exe);
            if (!exe.setExecutable(true, false)) log(">> WARN setExecutable(false)");
            exe.setReadable(true, false);
        }
        if (!exe.canExecute()) throw new IOException("not executable: " + exe);
        log(">> exe   = " + exe + " (" + exe.length() + " bytes, 来源=APK)");
        return exe;
    }

    private static void copyFile(File src, File dst) throws IOException {
        InputStream in = new FileInputStream(src);
        try {
            FileOutputStream fos = new FileOutputStream(dst);
            try {
                byte[] chunk = new byte[1 << 20];
                int n;
                long total = 0;
                while ((n = in.read(chunk)) > 0) { fos.write(chunk, 0, n); total += n; }
                fos.flush();
            } finally { fos.close(); }
        } finally { in.close(); }
    }

    /** 外置 .so 的快速指纹：读首尾各 64KB，算一个 64 位哈希。
     *
     * 用途见 resolveSoPath：判断"外置文件到底变没变"。
     * 不能对 70MB 做全量哈希（每次启动多几秒），而 ELF 的头部
     * （程序头/架构/入口）与尾部（节表）几乎总能区分不同构建。
     *
     * 失败返回 "0"，此时指纹退化成只靠长度+mtime（不阻塞启动）。 */
    private static String quickHash(File f) {
        java.io.RandomAccessFile raf = null;
        try {
            raf = new java.io.RandomAccessFile(f, "r");
            long len = raf.length();
            byte[] head = new byte[(int)Math.min(65536L, len)];
            raf.readFully(head);
            byte[] tail = new byte[0];
            if (len > 65536L) {
                long tlen = Math.min(65536L, len - 65536L);
                tail = new byte[(int)tlen];
                raf.seek(len - tlen);
                raf.readFully(tail);
            }
            long h = 0xcbf29ce484222325L;          /* FNV-1a 64 */
            for (byte b : head) { h ^= (b & 0xFF); h *= 0x100000001b3L; }
            for (byte b : tail) { h ^= (b & 0xFF); h *= 0x100000001b3L; }
            return Long.toHexString(h);
        } catch (Throwable t) {
            return "0";
        } finally {
            if (raf != null) try { raf.close(); } catch (Exception ignored) { }
        }
    }

    private static void writeSmall(File f, String s) {
        try {
            java.io.FileWriter w = new java.io.FileWriter(f);
            w.write(s);
            w.close();
        } catch (Exception ignored) { }
    }

    // --------------------------------------------------------------- launch

    private void launch(File gameDir, File externalDir) {
        /* ★ 2026-11-06：优先走进程内（.so 化）。
         *
         * in-process 的收益是全屏直连 Surface（零拷贝、60fps、没有推流开销）。
         * 代价是 guest 崩溃会带走整个 app。
         *
         * 退路：在 <game>/exec 放一个空文件，就强制回到子进程模式。
         * 子进程模式下 app 不会跟着崩，run.log 也完整 —— 排查用。 */
        if (useInProcess(externalDir)) {
            launchInProcess(gameDir, externalDir);
            return;
        }
        try {
            startSubprocess(resolveExe(externalDir), gameDir, externalDir);
        } catch (Exception e) {
            log("!! launch 失败: " + e);
            Log.e(TAG, "launch failed", e);
        }
    }

    /** 老的子进程模式：独立进程 + pbuffer + TCP 推流。
     * 保留作为兜底 —— 崩了不会带走 app，run.log 完整，适合排查。 */
    private void startSubprocess(File exe, File gameDir, File externalDir) {
        try {
            log(">> cwd   = " + gameDir + " (exists=" + gameDir.isDirectory() + ")");
            log(">> pe    = " + new File(gameDir, PE_NAME) + " (exists="
                    + new File(gameDir, PE_NAME).isFile() + ")");
            if (!gameDir.isDirectory()) {
                log("!! game dir missing. Run work/apk/stage_runtime.sh first "
                        + "(see SETUP.md section 4).");
            }

            ProcessBuilder pb = new ProcessBuilder(exe.getAbsolutePath());
            pb.directory(gameDir);
            pb.redirectErrorStream(true);
            Map<String, String> env = pb.environment();
            env.put("HOME", gameDir.getAbsolutePath());
            env.put("PWD", gameDir.getAbsolutePath());
            env.put("TMPDIR", new File(gameDir, "tmp").getAbsolutePath());
            env.put("LD_LIBRARY_PATH", "/system/lib64:/system/vendor/lib64");
            /* 让 native 把每帧 RGBA 推到 7138（SurfaceView 显示用）。
             * 每 2 帧推一张 ≈ 30fps：960x540x4 = 2MB/帧，
             * 60fps 就是 120MB/s，TCP loopback + Java 绘制撑不住。 */
            /* ★ 2026-11-06：两个性能开关可调，不用重编 native。
             *
             * FRAME_STREAM：每 N 帧推一张到屏幕。原来 4（≈15fps 画面），
             *   960x540 RGBA = 2MB/帧，推送+Java 绘制吃掉大量时间。
             *   改成 8 后带宽减半，游戏本身能跑满，画面 7~8fps
             *   但操作响应会明显跟手。
             *
             * 用户想要"能玩"，帧率优先于画面流畅度。
             * 想恢复原样就在系统设置里给本应用写属性，或改回 "4"。 */
            String fs = System.getProperty("isaac.frame_stream");
            env.put("FRAME_STREAM", fs != null ? fs : "8");

            /* TIME_SCALE：游戏时钟倍率。
             *
             * 白屏那段是 intro cutscene（144 秒，30fps）。
             * 为了让"安装即可用"的体验不至于让人干等两分半，
             * 这里给 3 倍速 —— 48 秒播完，而且画面本来就是白的，
             * 加速不损失任何观感。
             *
             * 想要原速就在系统属性里设 isaac.time_scale=1。 */
            /* ★ 2026-11-06：默认改成 1（正常速度，可游玩）。
             *
             * 之前为了方便跳加载/过场，默认给 8 —— 但那让游戏以 8 倍速
             * 运行，根本没法操作。游玩就必须是 1。
             *
             * 想跳过漫长的加载和 intro cutscene 时，在
             *   <game>/fast
             * 放一个空文件（文件名就叫 fast），下次启动就用 8 倍速。
             * 删掉它即恢复正常速度。这样不用重装、不用 adb。 */
            int scale = 1;
            try {
                if (new File(externalDir, "fast").exists()) scale = 8;
            } catch (Exception ignored) { }
            /* 属性覆盖：解析失败不能把启动带死，回落到文件判定结果 */
            try {
                String ts = System.getProperty("isaac.time_scale");
                if (ts != null) {
                    int v = Integer.parseInt(ts.trim());
                    if (v >= 1 && v <= 16) scale = v;
                }
            } catch (Exception ignored) { }
            env.put("TIME_SCALE", Integer.toString(scale));
            log(">> TIME_SCALE=" + scale + (scale == 1 ? "（正常速度）" : "（加速，跳过过场）"));
            env.put("ANDROID_ROOT", "/system");
            env.put("ANDROID_DATA", "/data");

            /* ★ 2026-11-07：子进程模式下 OpenAL 的路径。
             * 可执行文件在 files/bin/isaac，dladdr 推出来的是
             * files/bin/，而 libopenal.so 在 game 目录 —— 对不上。
             * 这里显式告诉 host_openal.c 去哪找。 */
            {
                File oal = new File(externalDir, "libopenal.so");
                if (oal.isFile()) env.put("ISAAC_OPENAL", oal.getAbsolutePath());
            }

            long t0 = System.currentTimeMillis();
            child = pb.start();
            log(">> started, streaming output");

            /* 日志双写：
             *   ① app 私有目录（不受外部影响，长期留档）
             *   ② game/run.log —— 这个路径 Termux / 文件管理器能直接读，
             *      出问题时不用 adb 就能拿到完整 stdout+stderr。
             * 2026-11-05：之前只写 ①，Termux 读不到（不同 uid），
             * 排查只能靠截图，效率太低。 */
            Writer fileLog = null;
            Writer sharedLog = null;
            try {
                File lf = new File(getFilesDir(), "run-"
                        + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date())
                        + ".log");
                fileLog = new OutputStreamWriter(new FileOutputStream(lf), "UTF-8");
                log(">> child log file: " + lf.getAbsolutePath());

                try {
                    File sl = new File(externalDir, "run.log");
                    sharedLog = new OutputStreamWriter(new FileOutputStream(sl), "UTF-8");
                    log(">> shared log: " + sl.getAbsolutePath());
                } catch (Exception e) {
                    log(">> shared log unavailable: " + e);
                }

                BufferedReader r = new BufferedReader(new InputStreamReader(
                        child.getInputStream(), "UTF-8"));
                String line;
                int lines = 0;
                int flushN = 0;
                long lastFlush = System.currentTimeMillis();
                while ((line = r.readLine()) != null) {
                    lines++;
                    if (fileLog != null) { fileLog.write(line); fileLog.write('\n'); }
                    if (sharedLog != null) {
                        sharedLog.write(line); sharedLog.write('\n');
                        /* ★ 2026-11-06 启动提速：原来每行都 flush。
                         * 每 flush 一次就是对 /storage/emulated（FUSE）的
                         * 一次 syscall，加载期几千行 = 几千次同步写。
                         * 现在：每 4 行、或距上次超过 400ms、或遇到明显
                         * 重要的行（★ / SEGV / 退出 / 错误）才 flush。
                         * 卡住时最多丢 3 行 / 0.4 秒，够定位。 */
                        flushN++;
                        boolean important = line.indexOf('★') >= 0
                                || line.indexOf("SEGV") >= 0
                                || line.indexOf("terminate") >= 0
                                || line.indexOf("[input]") >= 0
                                || line.indexOf("[xinput]") >= 0
                                || line.indexOf("[xprobe]") >= 0;
                        long now = System.currentTimeMillis();
                        if (flushN >= 4 || important || now - lastFlush > 400) {
                            sharedLog.flush();
                            flushN = 0;
                            lastFlush = now;
                        }
                    }
                    /* ★ 2026-11-05：原来 >400 行只调 Log.i()，**不再写文件**，
                     * 于是 app.log 永远只有前 400 行 —— 而崩溃/退出的现场
                     * 恰恰在最后。现在文件始终全量，只限制 UI 显示。 */
                    if (lines <= 400) {
                        log(line);
                    } else {
                        if (lines == 401) log("... (UI 只显示前 400 行，文件仍是全量)");
                        Log.i(TAG, line);
                        if (sharedLog != null) { sharedLog.write(line); sharedLog.write('\n'); sharedLog.flush(); }
                        if (fileLog != null) { fileLog.write(line); fileLog.write('\n'); }
                    }
                }
                int rc = child.waitFor();
                long ms = System.currentTimeMillis() - t0;

                /* 退出码是最重要的线索：
                 *   137 = 128+9  → SIGKILL，几乎一定是 OOM 被系统杀
                 *   139 = 128+11 → SIGSEGV，游戏自己崩了
                 *   134 = 128+6  → SIGABRT
                 *   0            → 正常退出
                 * 之前这里只打一行日志，而且因为 400 行截断根本看不到。 */
                final String why;
                if (rc == 0)        why = "正常退出";
                else if (rc == 137) why = "★ SIGKILL —— 很可能被系统 OOM 杀了";
                else if (rc == 139) why = "★ SIGSEGV —— 游戏崩溃";
                else if (rc == 134) why = "★ SIGABRT —— 主动 abort";
                else if (rc == 143) why = "SIGTERM（被要求结束）";
                else why = "异常退出";
                log(">> child exited rc=" + rc + " (" + why + ") after "
                        + (ms / 1000) + "s, " + lines + " lines");
                runOnUiThread(new Runnable() {
                    public void run() {
                        if (status != null) {
                            status.setVisibility(View.VISIBLE);
                            status.setText("游戏已退出：" + why
                                    + "（详情见 game/run.log 末尾）");
                        }
                    }
                });
            } finally {
                if (fileLog != null) try { fileLog.close(); } catch (IOException ignored) { }
                if (sharedLog != null) try { sharedLog.close(); } catch (IOException ignored) { }
            }
        } catch (Exception e) {
            log("!! 子进程启动失败: " + e);
            Log.e(TAG, "subprocess start failed", e);
        }
    }

    /** 小端 32 位整数。 */
    private static int le32(byte[] b, int off) {
        return (b[off] & 0xFF)
                | ((b[off + 1] & 0xFF) << 8)
                | ((b[off + 2] & 0xFF) << 16)
                | ((b[off + 3] & 0xFF) << 24);
    }

    // ---------------------------------------------------------------- input

    /** 重试连接宿主输入桥（子进程起来后会 listen）。
     *
     * ★ 2026-11-06：这是"手柄完全没反应"的真正原因。
     *
     * 原来只重试 120 次 × 500ms = **60 秒**。
     * 但游戏要先加载 3.5GB 资源（-O2 版也要 100 秒以上）才会在 7137
     * 监听，等它准备好时 Java 早就放弃了 —— inOut 永远是 null，
     * 每次 sendKey 都打 "桥未连接，丢弃"。
     *
     * 现在：
     *   · 重试上限放宽到 30 分钟（3600 次），覆盖最慢的冷启动
     *   · 连上后若发送失败会自动重连，不会一断就永久失联
     *   · 连上时记录耗时，便于判断加载有多慢 */
    /* ★ 2026-11-06：修「手柄按不了」的两个 Java 侧 bug
     *
     * bug 1：重连时不关旧 socket。
     *   每次成功就 new 一个 socket 覆盖 inSock/inOut，旧的既不断开也不回收。
     *   native 侧原来又卡在第一个连接的 read 上，于是：
     *     数据写进第 N 个 socket，native 在读第 1 个 → 永远收不到。
     *   实测真机日志：native accept 了 7 次连接，但「手柄状态」收到 0 条。
     *
     * bug 2：并发重连。
     *   sendPadNow / sendKey / onPause 三处都可能触发 inOutLost()，
     *   各自 new Thread 去连，结果是多个线程同时建连、互相覆盖 inOut。
     *   现在用一把锁保证同一时刻只有一个连接线程，并加 generation 计数：
     *   后建立的连接会让先前的循环自动退出。 */
    private final Object inLock = new Object();
    /* ★ 2026-11-07：写锁。BufferedWriter 不是线程安全的。
     * 触摸线程（VirtualPad）、实体手柄回调、onPause 的 releaseAll
     * 都会调 sendPadNow，并发写同一个 writer 会让内部 state 错乱，
     * 抛 IOException → inOutLost → 重连 → 又写坏 → 死循环。
     * 真机实测：124 次连接、native 收到 0 条数据。 */
    private final Object writeLock = new Object();
    private volatile int inGen;          /* 连接代号：每次新建 +1 */
    private volatile boolean inConnecting;

    private void connectInputBridge() {
        final int myGen;
        synchronized (inLock) {
            myGen = ++inGen;
            inConnecting = true;
        }
        final long t0 = System.currentTimeMillis();
        for (int i = 0; i < 3600; i++) {
            if (myGen != inGen) {         /* 已被更新的连接取代 */
                synchronized (inLock) { if (myGen == inGen) inConnecting = false; }
                return;
            }
            try {
                Socket sk = new Socket();
                sk.connect(new InetSocketAddress("127.0.0.1", IN_PORT), 500);
                sk.setTcpNoDelay(true);

                Socket old;
                synchronized (inLock) {
                    if (myGen != inGen) {   /* 建连期间被别人抢了，关掉这个 */
                        try { sk.close(); } catch (Exception ignored) { }
                        return;
                    }
                    old = inSock;
                    inSock = sk;
                    inOut = new BufferedWriter(new OutputStreamWriter(
                            sk.getOutputStream(), "UTF-8"));
                    inConnecting = false;
                }
                /* ★ 先换新，再关旧：绝不能出现「inOut 指向已关的 socket」。 */
                if (old != null) {
                    try { old.close(); } catch (Exception ignored) { }
                }
                log(">> input bridge connected（等了 "
                        + ((System.currentTimeMillis() - t0) / 1000) + " 秒，第 "
                        + myGen + " 次连接）");
                setPadHud("🎮 已连接", 0xFF00E5FF);
                /* ★ 连接（重连）成功后立刻补发一次当前状态。
                 * 否则重连期间用户没动过手指，lastPadLine 没变，
                 * 新连接上永远收不到第一帧 —— 游戏看到的是全零。
                 * sendPadNow 里 lastPadLine 为 null 时会强制发送。 */
                lastPadLine = null;
                sendPadNow();
                return;
            } catch (Exception e) {
                try { Thread.sleep(500); } catch (InterruptedException ie) { return; }
            }
        }
        synchronized (inLock) { if (myGen == inGen) inConnecting = false; }
        log("!! input bridge: 30 分钟内都没连上（游戏可能没起来）");
    }

    /* ---------------------------------------------------------------
     * 虚拟手柄 → 键盘镜像（★ 2026-11-10）
     *
     * 真机现象：进游戏弹"Unrecognized Gamepad! Please setup a key config"，
     * 按手柄毫无反应。native.log 里能看到 btn=1000（A）、btn=2100（A+B）、
     * btn=0010（START）都被游戏取走了 —— 数据链完全通。
     *
     * 结论：**那个界面只读键盘，不轮询手柄**。所以手柄按下去石沉大海。
     *
     * 对策：虚拟手柄的每个按钮除了发 XInput 行，同时帮一份 VK。
     * 这样无论游戏当前走哪条输入路径（手柄或键盘），都能收到。
     *
     * 映射取 Isaac 的默认键盘配置，保证能和原生键盘操作对上：
     *   A→SPACE（使用道具）  B→E（炸弹）  X→Q（卡片）  Y→TAB（地图）
     *   START→Enter  BACK→ESC  LB→Shift  RB→C
     *   左摇杆→WASD（移动）  右摇杆→方向键（射击）
     * --------------------------------------------------------------- */
    private static final int[][] BTN_TO_VK = {
            {0x1000, 0x20},   /* A     -> SPACE  */
            {0x2000, 0x45},   /* B     -> E      */
            {0x4000, 0x51},   /* X     -> Q      */
            {0x8000, 0x09},   /* Y     -> TAB    */
            {0x0010, 0x0D},   /* START -> ENTER  */
            {0x0020, 0x1B},   /* BACK  -> ESC    */
            {0x0100, 0x10},   /* LB    -> SHIFT  */
            {0x0200, 0x43},   /* RB    -> C      */
            {0x0001, 0x26},   /* DPAD_U-> UP     */
            {0x0002, 0x28},   /* DPAD_D-> DOWN   */
            {0x0004, 0x25},   /* DPAD_L-> LEFT   */
            {0x0008, 0x27},   /* DPAD_R-> RIGHT  */
    };

    private int kbMirrorButtons;      /* 上一次镜像出去的按钮位 */
    private int kbMirrorMove;         /* 上一次镜像出去的移动方向（WASD 位）*/
    private int kbMirrorShoot;        /* 上一次镜像出去的射击方向（方向键位）*/

    /** 死区：低于这个量不当作方向，避免摇杆漂移导致一直走 */
    private static final int STICK_DEADZONE = 8000;

    /** 把摇杆的一个分量转成「上/下」或「左/右」两个键。 */
    private static int axisKeys(int v, int negVk, int posVk) {
        if (v <= -STICK_DEADZONE) return negVk;
        if (v >=  STICK_DEADZONE) return posVk;
        return 0;
    }

    /** 按位差分发 VK：新置位的发「按下」，清掉的发「释放」。 */
    private void mirrorKeys(int nowMask, int prevMask) {
        int changed = nowMask ^ prevMask;
        if (changed == 0) return;
        for (int[] m : BTN_TO_VK) {
            int bit = m[0];
            if ((changed & bit) == 0) continue;
            sendKey(m[1], (nowMask & bit) != 0);
        }
    }

    /** 手柄状态变化时同步一份键盘事件。
     *
     * 只发**变化**，不重复发 —— 键盘是电平语义，重复发会让游戏
     * 看到“按下/按下/按下”而不是“按住”，某些输入处理会丢掉。 */
    private synchronized void mirrorPadAsKeyboard(int buttons, int lx, int ly, int rx, int ry) {
        mirrorKeys(buttons, kbMirrorButtons);
        kbMirrorButtons = buttons;

        /* 左摇杆 → WASD（移动）。先把两个分量合成一个"方向掩码"，
         * 再整张比对 —— 否则 W 释放和 A 按下会互相覆盖。 */
        int move = 0;
        int mvUp = axisKeys(ly, 0, 0x57);      /* W */
        int mvDn = axisKeys(ly, 0x53, 0);      /* S */
        int mvLf = axisKeys(lx, 0x41, 0);      /* A */
        int mvRt = axisKeys(lx, 0, 0x44);      /* D */
        int[] mvKeys = { mvUp, mvDn, mvLf, mvRt };
        int[] mvBits = { 0x1, 0x2, 0x4, 0x8 };
        for (int i = 0; i < 4; i++) if (mvKeys[i] != 0) move |= mvBits[i];
        if (move != kbMirrorMove) {
            int diff = move ^ kbMirrorMove;
            for (int i = 0; i < 4; i++) {
                if ((diff & mvBits[i]) == 0) continue;
                int vk = (mvBits[i] == 0x1) ? 0x57 : (mvBits[i] == 0x2) ? 0x53
                       : (mvBits[i] == 0x4) ? 0x41 : 0x44;
                sendKey(vk, (move & mvBits[i]) != 0);
            }
            kbMirrorMove = move;
        }

        /* 右摇杆 → 方向键（射击） */
        int shoot = 0;
        int[] shKeys = { axisKeys(ry, 0, 0x26), axisKeys(ry, 0x28, 0),
                         axisKeys(rx, 0x25, 0), axisKeys(rx, 0, 0x27) };
        for (int i = 0; i < 4; i++) if (shKeys[i] != 0) shoot |= mvBits[i];
        if (shoot != kbMirrorShoot) {
            int diff = shoot ^ kbMirrorShoot;
            for (int i = 0; i < 4; i++) {
                if ((diff & mvBits[i]) == 0) continue;
                int vk = (mvBits[i] == 0x1) ? 0x26 : (mvBits[i] == 0x2) ? 0x28
                       : (mvBits[i] == 0x4) ? 0x25 : 0x27;
                sendKey(vk, (shoot & mvBits[i]) != 0);
            }
            kbMirrorShoot = shoot;
        }
    }

    private void sendKey(int vk, boolean down) {
        if (sendKeyN++ < 30) log(">> sendKey vk=" + vk + (down ? " 按下" : " 释放"));
        /* ★ 2026-11-10：同样走队列 —— sendKey 可能从 UI 主线程调用
         * （虚拟手柄镜像），也可能从后台回调，统一交给 tx 线程写。 */
        queueKey(Integer.toString(vk) + (down ? " 1" : " 0"));
    }

    /* ---------------------------------------------------------------
     * 手柄状态：虚拟手柄 + 实体手柄合成一条 XInput 状态行。
     *
     * 协议（host_user32.c 解析）：
     *     G <buttons十六进制> <lx> <ly> <rx> <ry> <lt> <rt>\n
     * 轴 -32768..32767（Y 向上为正），扳机 0..255。
     *
     * 合成规则：按钮位取并集；轴/扳机取"非零者"（虚拟手柄优先，
     * 因为它是用户当前正在操作的）。内容没变就不重复发。
     * --------------------------------------------------------------- */
    private void sendPadNow() {
        int b = vpButtons | hwButtons;
        int lx = vpLx != 0 ? vpLx : hwLx;
        int ly = vpLy != 0 ? vpLy : hwLy;
        int rx = vpRx != 0 ? vpRx : hwRx;
        int ry = vpRy != 0 ? vpRy : hwRy;
        int lt = vpLt != 0 ? vpLt : hwLt;
        int rt = vpRt != 0 ? vpRt : hwRt;

        String line = "G " + Integer.toHexString(b & 0xFFFF)
                + " " + lx + " " + ly + " " + rx + " " + ry
                + " " + lt + " " + rt;

        BufferedWriter w = inOut;
        /* 2026-11-06：调试日志。真机上出现过"桥收到了但游戏没反应"和
         * "桥一条都没收到"（触摸落在空白区），两者必须能区分 —— 这里打
         * 前 30 条，一眼看出到底发没发。 */
        if (padLogN++ < 30) log(">> pad " + line
                + (inOut == null ? "  [桥未连接]" : ""));
        if (padHudN++ < 8 || (b & 0xFFFF) != 0)
            setPadHud(String.format("🎮 %04X", b & 0xFFFF), 0xFF00E5FF);

        /* ★ 2026-11-10：同时镜像一份键盘事件。
         *
         * 补上 Isaac 某些界面（含启动提示）只读键盘、不轮询手柄的情况。
         * 详见 mirrorPadAsKeyboard 的注释。 */
        mirrorPadAsKeyboard(b & 0xFFFF, lx, ly, rx, ry);

        /* ★★ 2026-11-10：只入队，**绝不在调用线程写 socket**。
         *
         * 这个方法会被 VirtualPad 的 onTouchEvent 调用 —— 那是 UI 主线程。
         * 直接写 socket 会抛 NetworkOnMainThreadException（真机日志已证实），
         * 于是每次触摸都失败一次、触发重连，表现为"有时候有效"。
         * 详见 startTxThread 上方的长注释。 */
        if (line.equals(lastPadLine)) return;   /* 内容没变不发 */
        lastPadLine = line;
        queuePad(line);
    }

    /* 连续写失败计数（见 writeRaw） */
    private int sendFailStreak;

    /** 输入桥断了：清空并后台重连，否则之后所有输入静默丢失。 */
    /** 连接断了：清 inOut 并**只在一个重连线程**里重试。
     *
     * ★ 2026-11-06：原来每次写失败都新起一个线程，三个调用点
     * （sendPadNow / sendKey / sendReset）一起来就是三路并发连接，
     * 互相覆盖 inOut —— native 那边就再也对不上号。
     * 现在用 inConnecting 兜住：已经在连就不再起新的。 */
    /** 更新右上角手柄指示器（主线程）。 */
    private void setPadHud(final String text, final int color) {
        final TextView v = padHud;
        if (v == null) return;
        runOnUiThread(new Runnable() {
            public void run() {
                v.setText(text);
                v.setTextColor(color);
                v.setVisibility(View.VISIBLE);
            }
        });
    }

    private void inOutLost() {
        synchronized (inLock) {
            inOut = null;
            if (inConnecting) return;      /* 已经有线程在重连 */
            inConnecting = true;
        }
        /* ★★ 2026-11-07：重连节流。
         *
         * 真机上出现过 728 次重连，每次都：close 旧 socket → bind/accept
         * → 写一行。这一整套在 guest 主循环旁边跑，直接拖帧。
         * 两个失败之间至少隔 250ms，把"风暴"压成可用的重试节奏。 */
        long since = System.currentTimeMillis() - lastRetryMs;
        if (since < 250L) {
            try { Thread.sleep(250L - since); } catch (InterruptedException e) { return; }
        }
        lastRetryMs = System.currentTimeMillis();
        setPadHud("🎮 重连中…", 0xFFFF5555);
        new Thread(new Runnable() {
            public void run() { connectInputBridge(); }
        }, "isaac-input-reconnect").start();
    }

    private volatile long lastRetryMs;

    /** 桥还没连上时也要保证有人去连。
     * 原来 sendReset 遇到异常只把 inOut 置 null 就结束，
     * 之后既没人重连也没人报错，手柄永久静默。 */
    private void ensureBridge() {
        if (inOut != null) return;
        synchronized (inLock) {
            if (inConnecting) return;
            inConnecting = true;
        }
        new Thread(new Runnable() {
            public void run() { connectInputBridge(); }
        }, "isaac-input-ensure").start();
    }

    private void sendReset() {
        /* ★ 2026-11-10：也走队列。这是"松开所有键"的语义，
         * 必须和后续的按键事件保持先后（所以走 keys 队列而不合并）。 */
        queueKey("R");
        lastPadLine = null;      /* 下一次 sendPadNow 强制重发 */
    }

    /** Android KeyEvent 的 keyCode → PC 虚拟键码；返回 -1 表示不映射。 */
    private static int vkFromKeyCode(int kc) {
        switch (kc) {
            case KeyEvent.KEYCODE_BUTTON_A:      return VK_SPACE;  /* 使用道具 */
            case KeyEvent.KEYCODE_BUTTON_X:      return VK_E;      /* 放炸弹 */
            case KeyEvent.KEYCODE_BUTTON_START:  return VK_ESC;    /* 暂停 */
            case KeyEvent.KEYCODE_BUTTON_SELECT: return VK_TAB;    /* 地图 */
            case KeyEvent.KEYCODE_BUTTON_B:      return VK_TAB;
            case KeyEvent.KEYCODE_BUTTON_Y:      return VK_E;
            case KeyEvent.KEYCODE_DPAD_UP:       return VK_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:     return VK_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:     return VK_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:    return VK_RIGHT;
            /* 有实体键盘时直接用 */
            case KeyEvent.KEYCODE_W: return VK_W;
            case KeyEvent.KEYCODE_A: return VK_A;
            case KeyEvent.KEYCODE_S: return VK_S;
            case KeyEvent.KEYCODE_D: return VK_D;
            case KeyEvent.KEYCODE_SPACE: return VK_SPACE;
            case KeyEvent.KEYCODE_E: return VK_E;
            case KeyEvent.KEYCODE_ESCAPE: return VK_ESC;
            case KeyEvent.KEYCODE_TAB: return VK_TAB;
            case KeyEvent.KEYCODE_ENTER: return 0x0D;
            default: return -1;
        }
    }

    /* Android 实体手柄 keyCode → XInput 按钮位；0 表示不映射。 */
    private static int padBitFromKeyCode(int kc) {
        switch (kc) {
            case KeyEvent.KEYCODE_BUTTON_A:      return XI_A;
            case KeyEvent.KEYCODE_BUTTON_B:      return XI_B;
            case KeyEvent.KEYCODE_BUTTON_X:      return XI_X;
            case KeyEvent.KEYCODE_BUTTON_Y:      return XI_Y;
            case KeyEvent.KEYCODE_BUTTON_L1:     return XI_LEFT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_R1:     return XI_RIGHT_SHOULDER;
            case KeyEvent.KEYCODE_BUTTON_START:  return XI_START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return XI_BACK;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return XI_LEFT_THUMB;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return XI_RIGHT_THUMB;
            case KeyEvent.KEYCODE_DPAD_UP:        return XI_DPAD_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:      return XI_DPAD_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:      return XI_DPAD_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:     return XI_DPAD_RIGHT;
            default: return 0;
        }
    }

    /** 事件是否来自实体手柄 / 摇杆。 */
    private static boolean fromGamepad(KeyEvent ev) {
        int src = ev.getSource();
        return (src & android.view.InputDevice.SOURCE_GAMEPAD) != 0
                || (src & android.view.InputDevice.SOURCE_JOYSTICK) != 0;
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent ev) {
        if (fromGamepad(ev)) {
            int bit = padBitFromKeyCode(keyCode);
            if (bit != 0) { hwButtons |= bit; sendPadNow(); return true; }
        }
        int vk = vkFromKeyCode(keyCode);
        if (vk >= 0) { sendKey(vk, true); return true; }
        return super.onKeyDown(keyCode, ev);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent ev) {
        if (fromGamepad(ev)) {
            int bit = padBitFromKeyCode(keyCode);
            if (bit != 0) { hwButtons &= ~bit; sendPadNow(); return true; }
        }
        int vk = vkFromKeyCode(keyCode);
        if (vk >= 0) { sendKey(vk, false); return true; }
        return super.onKeyUp(keyCode, ev);
    }

    /* 实体手柄摇杆 → XInput 轴。Android 的 AXIS_Y 向上为负，XInput 向上为正。 */
    @SuppressWarnings("deprecated")
    private static int axisToXinput(float v, boolean invert) {
        if (invert) v = -v;
        float f = v * 32767f;
        if (f < -32768f) f = -32768f;
        if (f > 32767f) f = 32767f;
        return Math.round(f);
    }

    private static int triggerTo255(float v) {
        if (v <= 0f) return 0;
        if (v >= 1f) return 255;
        return Math.round(v * 255f);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent ev) {
        int src = ev.getSource();
        if ((src & android.view.InputDevice.SOURCE_JOYSTICK) != 0
                || (src & android.view.InputDevice.SOURCE_GAMEPAD) != 0) {
            hwLx = axisToXinput(ev.getAxisValue(MotionEvent.AXIS_X), false);
            hwLy = axisToXinput(ev.getAxisValue(MotionEvent.AXIS_Y), true);
            hwRx = axisToXinput(ev.getAxisValue(MotionEvent.AXIS_Z), false);
            hwRy = axisToXinput(ev.getAxisValue(MotionEvent.AXIS_RZ), true);

            /* 扳机：不同手柄用不同轴名，取最大值兼容 */
            float lt = Math.max(ev.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                                ev.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rt = Math.max(ev.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                                ev.getAxisValue(MotionEvent.AXIS_GAS));
            hwLt = triggerTo255(lt);
            hwRt = triggerTo255(rt);
            sendPadNow();
            return true;
        }
        return super.onGenericMotionEvent(ev);
    }

    @Override
    protected void onPause() {
        if (pad != null) pad.releaseAll();
        /* 实体手柄状态也清掉，否则切回来会卡住一个按下的键 */
        hwButtons = hwLx = hwLy = hwRx = hwRy = hwLt = hwRt = 0;
        sendReset();          /* Activity 不可见时松开所有输入，防止卡住 */
        super.onPause();
    }

    // ---------------------------------------------------------------- display

    /** 连 native 的 7138，收定长头 + RGBA，贴到 SurfaceView。 */
    private void runFrameReceiver() {
        for (int attempt = 0; attempt < 240; attempt++) {
            Socket sk = null;
            try {
                sk = new Socket();
                sk.connect(new InetSocketAddress("127.0.0.1", FRAME_PORT), 500);
                sk.setTcpNoDelay(true);
                sk.setReceiveBufferSize(1 << 20);
                log(">> frame stream connected");
                runOnUiThread(new Runnable() {
                    public void run() {
                        if (status != null) status.setText("帧流: 已连接，等第一帧…");
                    }
                });
                DataInputStream in = new DataInputStream(sk.getInputStream());
                byte[] payload = null;
                byte[] hdr = new byte[16];

                while (true) {
                    /* ★ 2026-11-05：native 按**小端**发（uint32_t 数组直接 send），
                     * 而 DataInputStream.readInt() 读的是**大端** —— 两者对不上，
                     * magic 永远是错的（表现为 "!! bad frame magic" 反复重连）。
                     * 手工按小端解，别再依赖 readInt()。 */
                    in.readFully(hdr);
                    int magic = le32(hdr, 0);
                    int w     = le32(hdr, 4);
                    int h     = le32(hdr, 8);
                    int bytes = le32(hdr, 12);
                    final boolean is565 = (magic == FRAME_MAGIC_565);
                    if (magic != FRAME_MAGIC && !is565) {
                        log("!! bad frame magic: " + Integer.toHexString(magic));
                        break;
                    }
                    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) break;
                    int bpp  = is565 ? 2 : 4;
                    int need = w * h * bpp;
                    if (bytes != need) break;
                    if (payload == null || payload.length != need) {
                        payload = new byte[need];
                        frameBitmap = Bitmap.createBitmap(w, h,
                                is565 ? Bitmap.Config.RGB_565
                                      : Bitmap.Config.ARGB_8888);
                        /* 换格式必须重建 Bitmap —— copyPixelsFromBuffer
                         * 要求字节数和 Bitmap 的像素格式严格匹配。 */
                    }
                    in.readFully(payload);
                    framesGot++;
                    if (framesGot == 1) {
                        runOnUiThread(new Runnable() {
                            public void run() {
                                if (status != null) status.setVisibility(View.GONE);
                            }
                        });
                    }
                    if ((framesGot % 30) == 1) {
                        final long fg = framesGot;
                        final int fw = w, fh = h;
                        runOnUiThread(new Runnable() {
                            public void run() {
                                if (status != null)
                                    status.setText("帧流: 已收 " + fg + " 帧  " + fw + "x" + fh);
                            }
                        });
                    }

                    SurfaceHolder hd = surface == null ? null : surface.getHolder();
                    if (hd == null || !surfaceReady) continue;
                    Canvas cv = null;
                    try {
                        cv = hd.lockCanvas();
                        if (cv != null && frameBitmap != null) {
                            frameBitmap.copyPixelsFromBuffer(ByteBuffer.wrap(payload));
                            cv.drawBitmap(frameBitmap, null,
                                    new android.graphics.Rect(0, 0,
                                            cv.getWidth(), cv.getHeight()), null);
                        }
                    } catch (Exception e) {
                        /* Surface 可能在旋转/切后台时消失，忽略这一帧 */
                    } finally {
                        if (cv != null) {
                            try { hd.unlockCanvasAndPost(cv); } catch (Exception ignored) { }
                        }
                    }
                }
            } catch (Exception e) {
                /* native 还没起好，等一会儿再试 */
            } finally {
                if (sk != null) try { sk.close(); } catch (Exception ignored) { }
            }
            try { Thread.sleep(500); } catch (InterruptedException ie) { return; }
        }
    }

    private Writer appLog;
    private synchronized void writeAppLog(String s) {
        Writer w = appLog;
        if (w == null) return;
        try { w.write(s); w.write('\n'); w.flush(); } catch (Exception e) { appLog = null; }
    }

    private void log(final String s) {
        Log.i(TAG, s);
        writeAppLog(s);
        /* 日志面板隐藏时不必逐行刷 UI —— 文本仍累积在 buf 里，
         * 展开时一次性贴上去即可。省掉每行一次 runOnUiThread。 */
        if (logVisible) {
            runOnUiThread(new Runnable() {
                public void run() { renderLog(); }
            });
        } else {
            buf.append(s).append('\n');
            if (buf.length() > 24000) buf.delete(0, buf.length() - 24000);
        }
    }

    private void renderLog() {
        out.setText(buf);
    }

    /* ==========================================================================
     * 性能 HUD（★ 2026-11-10）
     *
     * 每秒读一次：
     *   fps    ← NativeBridge.frameCount() 差分
     *   CPU%   ← /proc/self/stat 的 utime+stime 差分 ÷ 墙钟（可超 100%，多核）
     *   内存   ← /proc/self/status 的 VmRSS
     *   线程   ← 同文件的 Threads
     *
     * 全读 /proc/self，不需要任何系统权限。
     * 在后台线程轮询，只把字符串交给 UI 线程 —— 不要在 UI 线程读文件。
     * ========================================================================== */
    private void startPerfHud(final TextView tv) {
        new Thread(new Runnable() {
            public void run() {
                long prevCpuTicks = -1;
                long prevWall = 0;
                int  prevFrames = -1;
                int  hz = 100;             /* USER_HZ，Android/Linux 固定 100 */
                try { hz = (int)android.system.Os.sysconf(android.system.OsConstants._SC_CLK_TCK); }
                catch (Throwable ignored) { }
                if (hz <= 0) hz = 100;

                for (int i = 0; i < 100000; i++) {
                    try { Thread.sleep(1000); }
                    catch (InterruptedException e) { return; }

                    long wall = System.currentTimeMillis();
                    long cpuTicks = -1;
                    long rssKb = -1;
                    int  threads = -1;
                    int  frames = -1;

                    /* --- CPU：/proc/self/stat ---
                     * 字段 14/15 是 utime/stime（单位 = USER_HZ）。
                     * 注意 comm 字段可能含空格和括号，所以**从右边最后一个 ')' 往后切**。 */
                    try {
                        java.io.BufferedReader r = new java.io.BufferedReader(
                                new java.io.FileReader("/proc/self/stat"), 256);
                        String line = r.readLine();
                        r.close();
                        if (line != null) {
                            int p = line.lastIndexOf(')');
                            if (p > 0) {
                                String[] f = line.substring(p + 2).split(" ");
                                /* 切掉 "pid (comm) " 之后，f[0] 是第 3 个字段 (state)，
                                 * 于是 utime = f[11]、stime = f[12]。 */
                                if (f.length > 12) {
                                    cpuTicks = Long.parseLong(f[11]) + Long.parseLong(f[12]);
                                }
                            }
                        }
                    } catch (Throwable ignored) { }

                    /* --- 内存 / 线程：/proc/self/status --- */
                    try {
                        java.io.BufferedReader r = new java.io.BufferedReader(
                                new java.io.FileReader("/proc/self/status"), 512);
                        String l;
                        while ((l = r.readLine()) != null) {
                            if (l.startsWith("VmRSS:")) {
                                rssKb = parseKb(l);
                            } else if (l.startsWith("Threads:")) {
                                threads = (int)parseKb(l);
                            }
                        }
                        r.close();
                    } catch (Throwable ignored) { }

                    try { frames = NativeBridge.frameCount(); } catch (Throwable ignored) { }

                    /* --- 算差分 --- */
                    double cpuPct = -1;
                    if (prevCpuTicks >= 0 && cpuTicks >= 0 && wall > prevWall) {
                        long dTick = cpuTicks - prevCpuTicks;
                        long dMs = wall - prevWall;
                        cpuPct = 100.0 * (double)dTick / ((double)hz * (double)dMs / 1000.0);
                    }
                    double fps = -1;
                    if (prevFrames >= 0 && frames >= 0 && wall > prevWall) {
                        fps = (double)(frames - prevFrames) * 1000.0 / (double)(wall - prevWall);
                    }

                    prevCpuTicks = cpuTicks;
                    prevWall = wall;
                    prevFrames = frames;

                    final String s = String.format(java.util.Locale.US,
                            "FPS %s  CPU %s  内存 %s  线程 %s  帧 %s",
                            fps < 0 ? "--" : String.format(java.util.Locale.US, "%.1f", fps),
                            cpuPct < 0 ? "--" : String.format(java.util.Locale.US, "%.0f%%", cpuPct),
                            rssKb < 0 ? "--" : (rssKb / 1024) + "MB",
                            threads < 0 ? "--" : String.valueOf(threads),
                            frames < 0 ? "--" : String.valueOf(frames));

                    runOnUiThread(new Runnable() {
                        public void run() { tv.setText(s); }
                    });
                }
            }
        }, "isaac-perf-hud").start();
    }

    /** 从 "VmRSS:\t  123456 kB" 这类行里取出数字。 */
    private static long parseKb(String line) {
        try {
            StringBuilder d = new StringBuilder();
            for (int i = 0; i < line.length(); i++) {
                char ch = line.charAt(i);
                if (ch >= '0' && ch <= '9') d.append(ch);
                else if (d.length() > 0) break;
            }
            return d.length() == 0 ? -1 : Long.parseLong(d.toString());
        } catch (Exception e) { return -1; }
    }

    /* 2026-11-05：onDestroy 不再杀子进程。
     *
     * 原来这里的 p.destroy() 是"卡死"的直接原因之一：
     * Activity 因为屏幕熄灭、系统回收、或用户误触切走而被销毁时，
     * 子进程跟着被杀，画面冻住。
     *
     * 现在只有两条路径会结束游戏：
     *   ① 用户点 stop 按钮
     *   ② 游戏自己崩溃/退出
     * Activity 销毁时子进程继续跑，下次打开直接重连帧流/输入桥。
     *
     * 代价：真正的进程残留。但游戏只有一个，而且 Termux 侧能 pkill，
     * 这里选择"宁可残留也别误杀"。
     */
    @Override
    protected void onDestroy() {
        log(">> activity destroyed (child 保持运行)");
        super.onDestroy();
    }
}
