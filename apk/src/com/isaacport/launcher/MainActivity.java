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
    static final int FRAME_MAGIC   = 0x4D524649;  /* 'IFRM' RGBA8888 (old) */
    /* * 2026-11-07: the native side switched to RGB565 to halve the streaming bandwidth,
     * distinguished by the 'IFR5' magic. Both must be accepted, or upgrading native yields a black screen. */
    static final int FRAME_MAGIC_565 = 0x35524649; /* 'IFR5' RGB565 */

    private TextView out;
    private SurfaceView surface;
    private volatile boolean surfaceReady;
    /** The latest frame (RGBA, already flipped top-down by native). Drawn by the frame-receiver thread. */
    private Bitmap frameBitmap;
    private volatile boolean logVisible;
    private VirtualPad pad;
    private int sendKeyN;
    private TextView status;
    /** Pad connection indicator in the top-right corner (tells you whether input works without reading logs). */
    private TextView padHud;
    private volatile long framesGot;
    private StringBuilder buf = new StringBuilder();
    private volatile Process child;

    /* ---------------------------------------------------------------
     * Input bridge: connects to 127.0.0.1:7137 and pushes key state to the host process, one line at a time.
     * The host's GetAsyncKeyState consults that table (see host_user32.c).
     * --------------------------------------------------------------- */
    static final int IN_PORT = 7137;
    private volatile Socket inSock;
    private volatile BufferedWriter inOut;

    /* PC default bindings (VK codes), used by the keyboard fallback path */
    static final int VK_SPACE = 0x20, VK_E = 0x45, VK_ESC = 0x1B, VK_TAB = 0x09;
    static final int VK_W = 0x57, VK_A = 0x41, VK_S = 0x53, VK_D = 0x44;
    static final int VK_UP = 0x26, VK_DOWN = 0x28, VK_LEFT = 0x25, VK_RIGHT = 0x27;

    /* XInput button bits (kept in sync with VirtualPad / host_xinput.c) */
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

    /* Pad state: one set from the virtual pad (VirtualPad), one from a physical pad; merged when sending */
    private int vpButtons, vpLx, vpLy, vpRx, vpRy, vpLt, vpRt;
    private int hwButtons, hwLx, hwLy, hwRx, hwRy, hwLt, hwRt;
    private String lastPadLine;

    /* ==========================================================================
     * Send queue (** 2026-11-10, the key fix)
     *
     * Decisive evidence from the device log:
     *     !! pad write failed(1): android.os.NetworkOnMainThreadException
     *
     * The virtual pad's touch callbacks run on the **UI main thread**, and sendPadNow wrote to
     * the socket directly inside the callback. Since targetSdk 9, Android forbids network I/O on
     * the main thread and throws NetworkOnMainThreadException outright.
     *
     * The chain of consequences:
     *   press A -> main-thread write fails -> inOutLost -> reconnect (background thread)
     *        -> the resend after reconnecting succeeds -> the game sees btn=1000
     *   press again -> the main thread fails again -> the game sees btn=0000
     * So holding a button becomes a high-frequency flicker, experienced as "sometimes it works".
     * That is also where the 161 reconnects came from.
     *
     * Fix: the main thread only enqueues; a dedicated thread does the writing. Duplicate state
     * lines are also merged (keep only the newest) to cut pointless writes.
     * ========================================================================== */
    private final Object txLock = new Object();
    private String txPad;                       /* pending pad line, newest only */
    private final java.util.ArrayDeque<String> txKeys =
            new java.util.ArrayDeque<String>();
    private boolean txStarted;

    /** Start the send thread (called once from onCreate, idempotent). */
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
                    /* Key events go first (order preserved); pad state goes after (newest only).
                     * That way "press A while pushing the stick" does not lose the button. */
                    if (keys != null) {
                        for (String k : keys) writeRaw(k);
                    }
                    if (pad != null) writeRaw(pad);
                }
            }
        }, "isaac-pad-tx").start();
    }

    /** Enqueue one pad state (overwrites the previous unsent one). Callable from any thread. */
    private void queuePad(String line) {
        synchronized (txLock) {
            txPad = line;
            txLock.notifyAll();
        }
    }

    /** Enqueue one key event (order preserved). Callable from any thread. */
    private void queueKey(String line) {
        synchronized (txLock) {
            /* Cap of 256: a backlog that large means the connection is dead anyway;
             * storing more is pointless, and dropping old entries beats unbounded growth. */
            while (txKeys.size() >= 256) txKeys.pollFirst();
            txKeys.addLast(line);
            txLock.notifyAll();
        }
    }

    /** The actual write. **Only allowed on the tx thread** -- that is the core of the fix. */
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
                    log("!! tx write failed(" + sendFailStreak + "): " + e);
                }
                /* Two consecutive failures before declaring the connection dead: a single failure may be transient,
                 * while reconnecting costs a whole close+bind+accept cycle. */
                if (sendFailStreak >= 2) {
                    sendFailStreak = 0;
                    inOutLost();
                }
            }
        }
    }
    /** Native diagnostic log path (passed to start). */
    private String jniLogPath;
    private int padLogN;
    private int padHudN;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        /* The image comes from native's 7138 frame stream (rendered offscreen, then pushed over),
         * so this is a full-bleed SurfaceView with a thin log strip overlaid at the bottom. */
        /* 2026-11-05: keep the screen on.
         *
         * The device showed what looked like "frozen after reaching the main menu": the user stared at the
         * screen for over 200 seconds, it timed out and went dark -> the Activity entered pause/stop ->
         * onDestroy's p.destroy() killed the child process -> the image froze on the last frame.
         * It looked like a freeze but was the power-saving mechanism.
         *
         * One line fixes it: while this Activity is in the foreground the screen stays on. */
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        /* * 2026-11-06: full screen (immersive). The user asked for "full-screen resolution, smaller buttons".
         *
         * Three things must be done together; miss one and the status/navigation bar remains:
         *   1. FLAG_FULLSCREEN -- hide the status bar
         *   2. IMMERSIVE_STICKY -- hide the navigation bar; an edge swipe reveals it only temporarily
         *   3. LAYOUT_*_FULLSCREEN -- let content extend into the occupied area (otherwise
         *      the image is pushed down and a black band appears)
         *
         * Use IMMERSIVE_STICKY rather than IMMERSIVE: with the latter, once the user swipes an edge,
         * the system bars come back permanently; STICKY hides them again automatically. */
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

        /* ** 2026-11-07: keep the SurfaceView at the game's aspect ratio (16:9).
         *
         * Background: in-process mode now renders at the options.ini size (960x540) and lets
         * SurfaceFlinger scale the buffer up to the View. That fixed
         * "the image occupies only the lower left" and "terrible frame rate" (pixel count down to 1/6.5).
         *
         * The cost: the screen is 2665x1272 (aspect 2.10) while the game is 16:9
         * (1.78). Filling the screen outright stretches the image 18% horizontally, visibly distorting a 2D pixel game.
         *
         * So the SurfaceView keeps 16:9 centred with black bars on both sides.
         * The bars are drawn by the Android compositor at essentially zero cost. */
        root.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            public void onLayoutChange(View v, int l, int t, int r, int b,
                                       int ol, int ot, int or_, int ob) {
                int W = r - l, H = b - t;
                if (W <= 0 || H <= 0) return;
                int w = W, h = W * 9 / 16;
                if (h > H) { h = H; w = H * 16 / 9; }
                FrameLayout.LayoutParams cur =
                        (FrameLayout.LayoutParams) surface.getLayoutParams();
                /* Skip it if the size is unchanged -- setLayoutParams triggers another layout pass,
                 * and without this guard it loops forever. */
                if (cur != null && cur.width == w && cur.height == h) return;
                FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(w, h);
                lp.gravity = android.view.Gravity.CENTER;
                surface.setLayoutParams(lp);
                log(">> SurfaceView fitted to 16:9: " + w + "x" + h
                        + " (container " + W + "x" + H + ")");
            }
        });

        /* ---------------------------------------------------------------
         * Control bar + log panel
         *
         * 2026-11-04 correction: the previous version made the log area WRAP_CONTENT + weight=1,
         * so it filled most of the screen and opened as a wall of text. The log is for debugging and
         * should not cover the game.
         *
         * Now: two small buttons pinned to the top-left, the log panel hidden by default (GONE),
         * expanded only by tapping "log", and capped at 35% of the screen height.
         * So the default view is pure game.
         * --------------------------------------------------------------- */

        /* Status line: whether the frame stream is up, visible at a glance.
         * Previously you had to guess from the log, which is a poor experience. */
        status = new TextView(this);
        status.setTextSize(11f);
        status.setTextColor(0xFFFFFF00);
        status.setBackgroundColor(0x99000000);
        status.setPadding(10, 6, 10, 6);
        status.setText("frame stream: waiting for connection...");

        /* * 2026-11-06: pad connection indicator.
         *
         * The biggest frustration on device was "pressing does nothing, but is it not sent or not received?"
         * This small label shows the input bridge state and the last thing sent:
         *   🎮 connecting... / 🎮 not connected / 🎮 0x1000
         * With it there is no need to tail the log. */
        padHud = new TextView(this);
        padHud.setTextSize(10f);
        padHud.setTextColor(0xFF00E5FF);
        padHud.setBackgroundColor(0x99000000);
        padHud.setPadding(8, 4, 8, 4);
        padHud.setText("🎮 connecting...");

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
        scroll.setVisibility(View.GONE);      /* hidden by default */

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        /* 2026-11-05: give the control bar a dark background.
         * On a white screen Android's default light Button is nearly invisible against pure white. */
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
                if (!show) pad.releaseAll();       /* release keys before hiding, so nothing sticks */
                pad.setVisibility(show ? View.VISIBLE : View.GONE);
                padBtn.setText(show ? "nopad" : "pad");
            }
        });

        /* * 2026-11-06: smaller buttons, packed to the left.
         *
         * Feedback was "the top button is in the way". Previously three buttons with weight=1 filled
         * the whole row at WRAP_CONTENT height (Android buttons have a ~48dp minimum),
         * taking a full strip off the top of a full-screen game.
         *
         * Now: fixed small size (~68x34 dp), pinned top-left, the whole row 34dp tall.
         * It occupies a small patch in the top-left corner and never covers the centre. */
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

        /* Virtual pad: above the game, below the log panel.
         * * 2026-11-06: produces XInput pad state directly (no longer sends VK codes).
         * The game's primary path is XInput; see host_xinput.c. */
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
        /* bar is no longer MATCH_PARENT: it takes only the content width, pinned top-left. */
        panel.addView(bar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        /* The log takes at most 35% of the screen height and scrolls beyond that */
        int logMax = (int)(getResources().getDisplayMetrics().heightPixels * 0.35f);
        panel.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, logMax));

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        lp.gravity = android.view.Gravity.TOP;   /* pinned top-left so it does not cover the centre */
        panel.setLayoutParams(lp);
        root.addView(panel);

        setContentView(root);

        /* Make the root layout focusable so pad events reach the Activity's onKeyDown. */
        root.setFocusable(true);
        root.setFocusableInTouchMode(true);

        /* ** 2026-11-10: performance HUD -- fps / CPU / memory / threads.
         *
         * Why it is needed: users report "low frame rate" but can only describe the feeling, while the [perf] log
         * only prints after native accumulates 60 frames. Without numbers on screen you cannot
         * tell "a steady 5fps" from "occasional stutter", nor compare before and after an optimisation.
         *
         * Data sources (all from /proc, no extra process, no system permission):
         *   . fps        -> difference of NativeBridge.frameCount()
         *   . CPU%       -> difference of /proc/self/stat utime+stime over wall clock
         *   . memory     -> VmRSS from /proc/self/status
         *                   (the old version used resident from /proc/self/statm)
         *   . threads    -> Threads from /proc/self/status
         *
         * Refreshed once per second. Monospace, small, top-right -- out of the way. */
        final TextView perfHud = new TextView(this);
        perfHud.setTextSize(9f);
        perfHud.setTextColor(0xFF00FFB0);
        perfHud.setBackgroundColor(0xB0000000);
        perfHud.setPadding(8, 3, 8, 3);
        perfHud.setTypeface(android.graphics.Typeface.MONOSPACE);
        perfHud.setText("perf: waiting...");
        FrameLayout.LayoutParams perfLp = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT);
        perfLp.gravity = android.view.Gravity.TOP | android.view.Gravity.END;
        perfLp.topMargin = (int)(getResources().getDisplayMetrics().density * 40);
        perfLp.rightMargin = (int)(getResources().getDisplayMetrics().density * 8);
        root.addView(perfHud, perfLp);

        startPerfHud(perfHud);

        final File externalDir = resolveGameDir();

        /* The Java-side log always goes to the **external directory** (FUSE), not the ext4 copy --
         * so Termux and file managers can read it directly without adb. */
        try {
            appLog = new OutputStreamWriter(
                    new FileOutputStream(new File(externalDir, "app.log")), "UTF-8");
            log(">> app.log started");
        } catch (Exception e) {
            Log.w(TAG, "app.log unavailable: " + e);
        }

        /* * stageToInternal copies 3.5 GB and must never run on the UI thread (ANR).
         * It is the first step of the launch thread. */
        /* Startup timer: the status line ticks up every second.
         * Archive loading takes over 3 minutes with almost no output; without this the user assumes a hang. */
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
                                status.setText("starting... " + sec + " s"
                                        + " (archive loading takes about 3 minutes, please wait)");
                        }
                    });
                    try { Thread.sleep(1000); } catch (InterruptedException ie) { return; }
                }
            }
        }, "isaac-uptime").start();

        new Thread(new Runnable() {
            public void run() {
                /* * 2026-11-07: dump our own memory layout first.
                 *
                 * The root cause of in-process failure was that guest image base 0x30000000 was occupied.
                 * The generated code is full of hard-coded absolute addresses (like 0x307aa3b4),
                 * so the image can only land there. To find out who occupies it and where else would work,
                 * you must look at the real memory map.
                 *
                 * Java reading its own /proc/self/maps needs no permission --
                 * it is the app process's own map, including the ART heap and library load addresses.
                 * Saved to the external directory so Termux can just cat it. */
                dumpMaps(new File(externalDir, "app_maps.txt"));

                File gameDir = stageToInternal(externalDir,
                        new File(getFilesDir(), "game"));
                launch(gameDir, externalDir);
            }
        }, "isaac-launch").start();

        /* ** 2026-11-10: start the send thread before opening the connection.
         * It is the sole executor of every socket write -- touch and pad callbacks only enqueue.
         * Without starting it, sendPadNow's enqueued items would never be consumed. */
        startTxThread();

        new Thread(new Runnable() {
            public void run() { connectInputBridge(); }
        }, "isaac-input").start();
        new Thread(new Runnable() {
            public void run() { runFrameReceiver(); }
        }, "isaac-frames").start();
    }

    /** Save /proc/self/maps to the external directory.
     *
     * Purpose: diagnose "who occupies guest image base 0x30000000".
     * Reads only its own process map; needs no permission. */
    private void dumpMaps(File out) {
        java.io.InputStream in = null;
        java.io.FileOutputStream w = null;
        try {
            in = new java.io.FileInputStream("/proc/self/maps");
            w = new java.io.FileOutputStream(out);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) w.write(buf, 0, n);
            log(">> maps saved: " + out);
        } catch (Exception e) {
            log("!! dumpMaps: " + e);
        } finally {
            if (in != null) try { in.close(); } catch (Exception ignored) { }
            if (w != null) try { w.close(); } catch (Exception ignored) { }
        }
    }

    // ------------------------------------------------------------ in-process

    /** Decide between the in-process .so and a child process.
     *
     * Priority:
     *   1. <game>/exec exists        -> force child process (for debugging)
     *   2. <game>/libisaac.so exists -> in-process (external .so, fast iteration)
     *   3. libisaac.so inside the APK -> in-process
     *   4. none of the above         -> child process (old ELF fallback)
     *
     * Why an external .so must be copied into the private directory: /storage/emulated is noexec,
     * so System.load() from there fails. The private directory (ext4) works. */
    private boolean useInProcess(File externalDir) {
        try {
            if (new File(externalDir, "exec").exists()) {
                log(">> mode: child process (found the exec switch file)");
                return false;
            }
            if (resolveSoPath(externalDir) != null) {
                log(">> mode: in-process .so (direct Surface)");
                return true;
            }
        } catch (Exception e) {
            log("!! useInProcess: " + e);
        }
        log(">> mode: child process (libisaac.so not found)");
        return false;
    }

    /** Locate the .so: external first (synced into the private directory), else the one in the APK.
     * Returns an absolute path suitable for System.load, or null if neither exists. */
    private String resolveSoPath(File externalDir) throws IOException {
        File dir = new File(getFilesDir(), "lib");
        if (!dir.isDirectory() && !dir.mkdirs()) return null;
        File local = new File(dir, "libisaac.so");

        /* * 2026-11-07: put libopenal.so in the same directory.
         *
         * host_openal.c first calls dlopen("libopenal.so") so the linker searches
         * nativeLibraryDir; failing that it derives its own directory via dladdr
         * (which is this dir). With both ready, whichever path dlopen takes succeeds.
         * The OpenAL layer used to be a bookkeeping stub that never made a sound; this step is what gives the game audio. */
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
            log(">> libopenal sync skipped: " + t);
        }

        /* ---- 1) external .so ---- */
        File ext = new File(externalDir, "libisaac.so");
        if (ext.isFile() && ext.length() > 0) {
            long extLen = ext.length();
            long extTime = ext.lastModified();
            File stamp = new File(dir, "libisaac.so.stamp");

            /* ** 2026-11-10: fingerprint changed from length:mtime to length:mtime:head-tail-hash.
             *
             * Why it had to change: the external .so is the only iteration channel (no APK reinstall),
             * yet the old fingerprint looked only at length + mtime. Two problems:
             *   . changing a line or two can leave the length identical -> same fingerprint -> the old copy is used
             *   . a file manager or cloud sync may reset mtime when writing back
             * The consequence is that **you think you tested the new library but ran the old one** -- a wasted device run.
             *
             * New fingerprint = length + mtime + hash of the first 64KB and last 64KB.
             * Reading only 128KB costs under 10ms (negligible versus seconds for a full 70MB hash).
             * The ELF header carries build differences and the tail carries symbol-table differences; together they distinguish builds. */
            String want = extLen + ":" + extTime + ":" + quickHash(ext);
            if (local.isFile() && local.length() == extLen
                    && want.equals(readSmall(stamp))) {
                log(">> so: external unchanged, reusing the internal copy");
            } else {
                log(">> so: syncing external " + ext + " (" + (extLen / (1024 * 1024))
                        + " MB) into the private directory");
                copyFile(ext, local);
                /* Verify by reading back: never leave a "looks right" stamp when the copy was truncated or failed */
                if (local.length() != extLen) {
                    log("!! so: length mismatch after copy (" + local.length() + " vs "
                            + extLen + "); will retry on next start");
                } else {
                    writeSmall(stamp, want);
                }
            }
            return local.getAbsolutePath();
        }

        /* ---- 2) the one inside the APK ---- */
        String libDir = getApplicationInfo().nativeLibraryDir;
        File packed = new File(libDir == null ? "" : libDir, PAYLOAD);
        if (packed.isFile() && packed.length() > 0) {
            log(">> so: using the one in the APK " + packed);
            return packed.getAbsolutePath();
        }
        return null;
    }

    /** Start in-process: load the .so, wait for the Surface, hand over to JNI. */
    private void launchInProcess(final File gameDir, final File externalDir) {
        try {
            final String soPath = resolveSoPath(externalDir);
            if (soPath == null) throw new IOException("libisaac.so not found");

            if (!NativeBridge.load(soPath)) {
                log("!! System.load failed: " + NativeBridge.loadError);
                log("!! falling back to child-process mode");
                launchInProcessFallback(gameDir, externalDir);
                return;
            }
            log(">> so loaded: " + soPath);

            /* * Confirm which .so version is running first.
             * An old library lacks the new symbols -> calling diag()/stage() throws
             * UnsatisfiedLinkError, so it must be probed right here. */
            try {
                long be = NativeBridge.buildEpoch() & 0xFFFFFFFFL;
                log(">> so buildEpoch=" + be + "（"
                        + new java.text.SimpleDateFormat("MM-dd HH:mm:ss")
                            .format(new java.util.Date(be * 1000L)) + "）");
                if (be == 0) log("!! .so has no BUILD_EPOCH -- almost certainly an old library");
            } catch (Throwable t) {
                log("!! .so lacks buildEpoch -- an old library; all new diagnostics are invalid: " + t);
            }

            /* * 2026-11-07, fourth attempt: a **real file fd**, not a pipe.
             *
             * The first three attempts all failed, each for a different reason:
             *   v1  dup2(fd,1/2)   -> disturbs ART, ANR
             *   v2  native fopen    -> never created the file once on device (errno unknown)
             *   v3  pipe + Java read -> on a crash the process dies too, the reader is gone,
             *                           so the crash data in the buffer is never read
             *
             * The lesson from v3 is the key one: **a log channel must work on its own when the process dies**.
             * A pipe depends on a live reader, which is fundamentally wrong.
             *
             * v4: Java opens the file with FileOutputStream (already proven reliable --
             * app.log in the same directory has always written fine), calls detachFd() for a raw fd
             * and hands it to native. Native calls write(2) directly, touching no paths or permissions,
             * and the crash handler writes the same fd -- when the process dies the data is already in the page cache.
             *
             * This also sidesteps SIGPIPE: a file fd has no such notion. */
            final File nativeLog = new File(externalDir, "native.log");
            jniLogPath = new File(externalDir, "inproc.log").getAbsolutePath();
            try {
                int wfd = -1;
                try {
                    /* * Use ParcelFileDescriptor.open rather than FileOutputStream:
                     * FileDescriptor.detachFd() is a hidden API and will not compile;
                     * ParcelFileDescriptor.detachFd() is public (API 12+).
                     *
                     * Do not use TRUNCATE: if something fails later, the previous contents remain for comparison. */
                    android.os.ParcelFileDescriptor pfd =
                            android.os.ParcelFileDescriptor.open(nativeLog,
                                    android.os.ParcelFileDescriptor.MODE_WRITE_ONLY
                                    | android.os.ParcelFileDescriptor.MODE_CREATE
                                    | android.os.ParcelFileDescriptor.MODE_TRUNCATE);
                    wfd = pfd.detachFd();
                    log(">> native.log fd=" + wfd + " ready (Java confirmed it opens)");
                } catch (Throwable t) {
                    log("!! failed to open native.log: " + t);
                }

                log(">> about to call init(path, fd=" + wfd + ")");

                /* * 2026-11-07: stage-tracking thread.
                 *
                 * If init crashes, the process simply vanishes -- Java gets no chance to
                 * write anything. But stage is an int in native memory and can be read once set.
                 * This thread reads it every 50ms and writes to app.log **only on change**.
                 * If init crashes, the last line is the last known stage.
                 *
                 * Track for only 15 seconds: init itself takes milliseconds, so exceeding that means it is fine. */
                new Thread(new Runnable() {
                    public void run() {
                        int last = -999;
                        for (int i = 0; i < 300; i++) {
                            try { Thread.sleep(50); }
                            catch (InterruptedException e) { return; }
                            int st;
                            try { st = NativeBridge.stage(); }
                            catch (Throwable t) { return; }   /* an old library lacks this symbol */
                            if (st != last) {
                                last = st;
                                log(">> [trace] stage=" + st);
                            }
                            if (st >= 30) return;   /* already in main(); it manages itself from here */
                        }
                    }
                }, "isaac-stage-track").start();

                int rc = NativeBridge.init(jniLogPath, wfd);
                log(">> native log channel: fd=" + wfd + "  init rc=" + rc
                        + "  diag=0x" + Integer.toHexString(NativeBridge.diag())
                        + "  (bit0=fd bit1=primary file bit2=backup file bit3=path failed)");
            } catch (Throwable t) {
                log("!! failed to establish the log channel: " + t);
                jniLogPath = new File(externalDir, "inproc.log").getAbsolutePath();
                try {
                    int rc = NativeBridge.init(jniLogPath, -1);
                    log(">> native log channel: file only  init rc=" + rc);
                } catch (Throwable t2) {
                    log("!! NativeBridge.init also failed: " + t2);
                }
            }

            if (!gameDir.isDirectory()) {
                log("!! game dir missing: " + gameDir);
            }

            /* Wait for the Surface (surfaceCreated is asynchronous and may take a few frames). */
            new Thread(new Runnable() {
                public void run() {
                    for (int i = 0; i < 600; i++) {
                        if (surfaceReady && surface != null) break;
                        try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                    }
                    if (!surfaceReady) {
                        log("!! Surface not ready within 60 seconds");
                        return;
                    }
                    android.view.SurfaceHolder hd = surface.getHolder();

                    /* * 2026-11-07: must wait until the Surface is genuinely usable.
                     * ANativeWindow_fromSurface returns NULL for an invalid Surface,
                     * which forces a fallback to pbuffer (no full-screen direct path).
                     * Only isValid() == true means the underlying buffer has been allocated. */
                    int waited = 0;
                    while (!hd.getSurface().isValid() && waited < 100) {
                        try { Thread.sleep(100); } catch (InterruptedException ie) { return; }
                        waited++;
                    }
                    log(">> Surface isValid=" + hd.getSurface().isValid()
                            + " (waited " + (waited * 100) + " ms)");

                    android.graphics.Rect fr = hd.getSurfaceFrame();
                    int w = (fr != null && fr.width() > 0) ? fr.width() : 960;
                    int h = (fr != null && fr.height() > 0) ? fr.height() : 540;

                    /* Render size matches the Surface; inside, the game then scales to its own
                     * virtual resolution according to options.ini logic. */
                    log(">> starting in-process render " + w + "x" + h + "  cwd=" + gameDir);
                    runOnUiThread(new Runnable() {
                        public void run() {
                            if (status != null) {
                                status.setVisibility(View.VISIBLE);
                                status.setText("starting in-process... (loading takes about 20 s)");
                            }
                        }
                    });
                    NativeBridge.start(surface.getHolder().getSurface(),
                            w, h, gameDir.getAbsolutePath());
                    log(">> NativeBridge.start called");

                    /* * 2026-11-07: liveness check + stage heartbeat.
                     *
                     * The previous version only checked whether inproc.log existed, but the log channel itself
                     * can fail. Now it reports two things alternately:
                     *   . whether native.log has content (does the channel work)
                     *   . how far stage() has got (depends on no IO, always readable) */
                    new Thread(new Runnable() {
                        public void run() {
                            File lf = new File(externalDir, "native.log");
                            for (int i = 0; i < 12; i++) {
                                try { Thread.sleep(500); }
                                catch (InterruptedException e) { return; }
                                if (lf.isFile() && lf.length() > 0) {
                                    log(">> native.log alive: " + lf.length() + " bytes");
                                    return;
                                }
                            }
                            log("!! still no native.log after 6 s: the channel is not working"
                                    + "（stage=" + NativeBridge.stage()
                                    + " diag=0x"
                                    + Integer.toHexString(NativeBridge.diag()) + "）");
                        }
                    }, "isaac-log-watch").start();

                    /* * 2026-11-07: dump maps again after the .so is loaded.
                     * The startup dump (line 2 of app.log) happens before System.load, so it contains
                     * neither libisaac.so nor the 0x3C000000 guest image, and cannot tell
                     * whether the image and executable memory were actually mapped. */
                    new Thread(new Runnable() {
                        public void run() {
                            try { Thread.sleep(3000); }
                            catch (InterruptedException e) { return; }
                            dumpMaps(new File(externalDir, "inproc_maps.txt"));
                        }
                    }, "isaac-maps").start();

                    /* * 2026-11-06: supervisor thread.
                     *
                     * When in-process mode fails (guest mmap collides with ART, PE load fails,
                     * EGL cannot be created), the symptom is a permanent black screen. The user cannot tell
                     * "still loading" from "already dead".
                     *
                     * Criteria:
                     *   . hasExited()  -> main returned; declare failure immediately
                     *   . frameCount() still 0 after 45 seconds -> declare failure
                     * On failure, fall back to child-process mode automatically (it renders, just at low fps).
                     */
                    final long tStart = System.currentTimeMillis();
                    new Thread(new Runnable() {
                        public void run() {
                            for (int i = 0; i < 300; i++) {
                                try { Thread.sleep(500); }
                                catch (InterruptedException e) { return; }
                                int fc = NativeBridge.frameCount();
                                boolean exited = NativeBridge.hasExited();
                                /* * 2026-11-07: leave a heartbeat every 10 seconds.
                                 * It used to log only when fc>0 or exited, so
                                 * the state "alive but not a single frame" was
                                 * completely silent in app.log -- you could not tell loading from dead. */
                                if (i > 0 && (i % 20) == 0) {
                                    log(">> inproc " + (i / 2) + "s: frames=" + fc
                                            + " exited=" + (exited ? 1 : 0)
                                            + " stage=" + NativeBridge.stage()
                                            + " diag=0x"
                                            + Integer.toHexString(NativeBridge.diag()));
                                }
                                if (fc > 0) {
                                    log(">> in-process render started (first frame in "
                                            + ((System.currentTimeMillis() - tStart) / 1000)
                                            + " s)");
                                    return;
                                }
                                if (exited) {
                                    log("!! in-process startup failed: main returned, switching back to child process");
                                    runOnUiThread(new Runnable() {
                                        public void run() {
                                            if (status != null) {
                                                status.setVisibility(View.VISIBLE);
                                                status.setText("in-process startup failed, switching to child-process mode...");
                                            }
                                        }
                                    });
                                    launchInProcessFallback(gameDir, externalDir);
                                    return;
                                }
                            }
                            log("!! in-process produced no frame for 150 s, switching back to child process");
                            launchInProcessFallback(gameDir, externalDir);
                        }
                    }, "isaac-inproc-watch").start();
                }
            }, "isaac-inproc-start").start();

            /* The input bridge still uses TCP: VirtualPad -> sendPadNow -> 7137.
             * native's u_bridge_thread starts a listening thread inside the .so the same way,
             * and this chain is already proven on device; do not reinvent it. */
            new Thread(new Runnable() {
                public void run() { connectInputBridge(); }
            }, "isaac-input").start();

            /* * 2026-11-07: in-process mode also needs the frame-receiver thread.
             *
             * Reason: if eglCreateWindowSurface fails (or the window is judged unusable at
             * the JNI layer), native falls back to pbuffer mode and still pushes frames to
             * TCP:7138. Without the receiver that is a pure black screen -- the user has no idea
             * what happened and cannot play.
             *
             * When window mode succeeds, 7138 is never listening, so this thread's connect fails and
             * it retries silently until timeout, affecting nothing. */
            new Thread(new Runnable() {
                public void run() { runFrameReceiver(); }
            }, "isaac-frames").start();
        } catch (Exception e) {
            log("!! launchInProcess failed: " + e);
            launchInProcessFallback(gameDir, externalDir);
        }
    }

    /** Fallback when in-process startup fails.
     *
     * * You **must not** simply call startSubprocess here:
     *   the .so is already running in this process and its u_bridge_thread holds TCP:7137
     *   in a listen loop. Starting a child process would make bind(7137) fail there,
     *   killing the input bridge outright -- and two "games" would run at once, polluting each other's logs.
     *
     * The right approach: write a <game>/exec switch file and ask the user to reopen the app.
     * On the next start useInProcess() sees exec and goes straight to child process, leaving the process clean. */
    private void launchInProcessFallback(File gameDir, File externalDir) {
        try {
            File flag = new File(externalDir, "exec");
            if (!flag.exists()) {
                //noinspection ResultOfMethodCallIgnored
                flag.createNewFile();
            }
            log(">> wrote " + flag + "; the next start will use child-process mode");
            runOnUiThread(new Runnable() {
                public void run() {
                    if (status != null) {
                        status.setVisibility(View.VISIBLE);
                        status.setText("in-process mode failed; switched to child-process mode."
                                + "Please swipe this app away and reopen it.");
                    }
                }
            });
        } catch (Exception e) {
            log("!! fallback failed: " + e);
        }
    }

    // ---------------------------------------------------------------- paths

    /** Staged game tree: /sdcard/Android/media/<pkg>/files/game unless overridden. */
    /* ------------------------------------------------------------------
     * Moving resources onto ext4
     *
     * 2026-11-05: on device everything worked but very slowly; one round measured 23029 fopens,
     * and /storage/emulated/0 is FUSE -- an order of magnitude slower than ext4.
     * Archive loading (9 .a files, about 1 GB) takes 150-200 s on Termux's ext4,
     * and only longer on device.
     *
     * Approach: on first start copy the whole game tree into the app private directory
     * (/data/user/0/<pkg>/files/game, ext4) and point cwd there afterwards.
     * A few seconds of copying buys a much faster start every time after.
     *
     * A .stage_ok marker plus the source directory mtime decides whether to re-copy;
     * restaging resources with stage_runtime.sh on the Termux side triggers a re-copy automatically.
     * ------------------------------------------------------------------ */
    private File stageToInternal(File src, File dst) {
        File marker = new File(dst, ".stage_ok");
        /* * 2026-11-05: the predicate must look only at the **resources themselves**, not the src directory as a whole.
         *
         * It used to use src.lastModified(), but push_elf.sh writes isaac (a 70 MB ELF) into src,
         * so any change to the directory mtime declared the resources stale
         * and every native rebuild triggered a 3.5 GB re-copy -- which should never happen.
         *
         * It now takes the mtime of the resources directory (what stage_runtime.sh lays down)
         * plus the total resource byte count. Neither is affected by pushing the ELF. */
        long srcStamp = 0;
        try {
            File res = new File(src, "resources");
            long t = res.isDirectory() ? res.lastModified() : src.lastModified();
            long bytes = dirSize(new File(dst, "resources"));
            srcStamp = t * 1000003L + bytes;   /* a simple mix so the two cannot coincidentally match */
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
                if (status != null) status.setText("frame stream: starting...");
            }
        });
        return dst;
    }

    /** Recursively total a directory's bytes. Used by the staging predicate. */
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

    /** Recursive copy. Symlinks are not followed (stage_runtime.sh already expanded them to real files). */
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
                /* Skip logs (constantly changing) and executables (managed separately by resolveExe,
                 * and copying 70 MB in is pure waste -- it has to land in bin/ to run anyway). */
                /* * Also skip libisaac.so / exec / fast:
                 *   the first two are not game resources (.so is 73 MB; copying it in is pure waste
                 *   and it is loaded directly from the external path via System.load);
                 *   exec / fast are our own switch files and do not belong in the game directory. */
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
                    final String msg = "first start: moving resources to internal storage "
                            + (bytes[0] / (1024 * 1024)) + " MB / " + files[0] + " files";
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
        /* symlinks / device files: skip */
    }

    private File resolveGameDir() {
        String override = getIntent() != null ? getIntent().getStringExtra("gamedir") : null;
        if (override != null && override.length() > 0) return new File(override);
        File ext = Environment.getExternalStorageDirectory();
        return new File(ext, "Android/media/" + PKG + "/files/game");
    }

    /* ------------------------------------------------------------------
     * Where the executable comes from (2026-11-05: external, no reinstall)
     *
     * Purpose: after changing native code (host_*.c etc.), drop the new ELF into
     * the external directory and restart the app to test, with no APK reinstall.
     *
     * Why "place externally + run internally" takes two steps:
     *   /storage/emulated/0 is FUSE mounted noexec -- measured that even after
     *   chmod +x it still reports Permission denied. So the ELF must land back in
     *   the app private directory (ext4) to be executable.
     *
     * Lookup order:
     *   1) <game>/isaac            <- external, preferred (native changes go this way)
     *   2) lib/arm64-v8a/libisaac.so inside the APK  <- fallback
     *
     * Sync predicate: length + lastModified. Any change in either triggers a re-copy.
     * Far faster than hashing (a 70 MB hash takes hundreds of milliseconds), and code changes
     * always alter length or timestamp, so it suffices in practice.
     * ------------------------------------------------------------------ */
    private File resolveExe(File externalDir) throws IOException {
        File dir = new File(getFilesDir(), "bin");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("cannot create " + dir);
        }
        File exe = new File(dir, EXE_NAME);

        /* ---- 1) external ELF preferred ---- */
        File ext = new File(externalDir, "isaac");
        if (ext.isFile() && ext.length() > 0) {
            long extLen = ext.length();
            long extTime = ext.lastModified();
            File stamp = new File(dir, EXE_NAME + ".stamp");
            String want = extLen + ":" + extTime;

            if (exe.isFile() && exe.length() == extLen &&
                    want.equals(readSmall(stamp))) {
                log(">> exe: external ELF unchanged, reusing the internal copy (no reinstall)");
            } else {
                log(">> exe: found external ELF " + ext
                        + " (" + (extLen / (1024 * 1024)) + " MB), syncing internally");
                copyFile(ext, exe);
                writeSmall(stamp, want);
            }
            if (!exe.canExecute()) {
                exe.setExecutable(true, false);
                exe.setReadable(true, false);
            }
            if (!exe.canExecute()) throw new IOException("not executable: " + exe);
            log(">> exe   = " + exe + " (" + exe.length() + " bytes, source=external)");
            return exe;
        }

        /* ---- 2) fall back to the payload inside the APK ---- */
        String libDir = getApplicationInfo().nativeLibraryDir;
        File packed = new File(libDir == null ? "" : libDir, PAYLOAD);
        long packedLen = packed.length();

        if (!exe.exists() || exe.length() != packedLen) {
            log(">> exe: no external ELF, extracting from the APK " + packed + " -> " + exe);
            copyFile(packed, exe);
            if (!exe.setExecutable(true, false)) log(">> WARN setExecutable(false)");
            exe.setReadable(true, false);
        }
        if (!exe.canExecute()) throw new IOException("not executable: " + exe);
        log(">> exe   = " + exe + " (" + exe.length() + " bytes, source=APK)");
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

    /** Fast fingerprint for the external .so: read the first and last 64KB, compute a 64-bit hash.
     *
     * See resolveSoPath: decides whether the external file really changed.
     * A full 70 MB hash is out (seconds added to every start), while the ELF header
     * (program headers / architecture / entry) and tail (section table) almost always distinguish builds.
     *
     * Returns "0" on failure, degrading the fingerprint to length+mtime (never blocks startup). */
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
        /* * 2026-11-06: prefer in-process (.so).
         *
         * The gain is a full-screen direct Surface (zero copies, 60fps, no streaming overhead).
         * The cost is that a guest crash takes the whole app with it.
         *
         * Fallback: place an empty <game>/exec file to force child-process mode.
         * In child-process mode the app survives crashes and run.log is complete -- for debugging. */
        if (useInProcess(externalDir)) {
            launchInProcess(gameDir, externalDir);
            return;
        }
        try {
            startSubprocess(resolveExe(externalDir), gameDir, externalDir);
        } catch (Exception e) {
            log("!! launch failed: " + e);
            Log.e(TAG, "launch failed", e);
        }
    }

    /** The old child-process mode: separate process + pbuffer + TCP streaming.
     * Kept as a fallback -- a crash does not take the app down and run.log is complete, ideal for debugging. */
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
            /* Have native push each RGBA frame to 7138 (for SurfaceView display).
             * Pushing one in 2 frames is about 30fps: 960x540x4 = 2 MB/frame,
             * which at 60fps is 120 MB/s; TCP loopback plus Java drawing cannot sustain it. */
            /* * 2026-11-06: two performance switches, adjustable without rebuilding native.
             *
             * FRAME_STREAM: push one frame to the screen every N frames. Originally 4 (~15fps),
             *   960x540 RGBA = 2 MB/frame, and pushing plus Java drawing consumed a lot of time.
             *   At 8 the bandwidth halves, the game itself runs full speed, the image is 7-8fps
             *   but input response feels noticeably tighter.
             *
             * The user wanted "playable", so frame rate beats visual smoothness.
             * To restore the old behaviour, set the system property for this app or change it back to "4". */
            String fs = System.getProperty("isaac.frame_stream");
            env.put("FRAME_STREAM", fs != null ? fs : "8");

            /* TIME_SCALE: game clock multiplier.
             *
             * The white-screen section is the intro cutscene (144 seconds, 30fps).
             * So that an out-of-the-box install does not mean waiting two and a half minutes,
             * this uses 3x -- it finishes in 48 seconds, and since the image is white anyway
             * the speed-up costs nothing visually.
             *
             * For normal speed set isaac.time_scale=1 in the system property. */
            /* * 2026-11-06: default changed to 1 (normal speed, playable).
             *
             * It used to default to 8 to skip loading and cutscenes, but that ran the game at 8x
             * and was impossible to control. Play requires 1.
             *
             * To skip long loading and the intro cutscene, drop an empty file named
             *   <game>/fast
             * fast (literally that name) there and the next start uses 8x.
             * Delete it to return to normal speed. No reinstall and no adb. */
            int scale = 1;
            try {
                if (new File(externalDir, "fast").exists()) scale = 8;
            } catch (Exception ignored) { }
            /* Property override: a parse failure must not kill startup; fall back to the file-based result */
            try {
                String ts = System.getProperty("isaac.time_scale");
                if (ts != null) {
                    int v = Integer.parseInt(ts.trim());
                    if (v >= 1 && v <= 16) scale = v;
                }
            } catch (Exception ignored) { }
            env.put("TIME_SCALE", Integer.toString(scale));
            log(">> TIME_SCALE=" + scale + (scale == 1 ? " (normal speed)" : " (accelerated, skipping cutscenes)"));
            env.put("ANDROID_ROOT", "/system");
            env.put("ANDROID_DATA", "/data");

            /* * 2026-11-07: the OpenAL path in child-process mode.
             * The executable is files/bin/isaac, and dladdr derives files/bin/,
             * while libopenal.so is in the game directory -- they do not match.
             * So tell host_openal.c explicitly where to look. */
            {
                File oal = new File(externalDir, "libopenal.so");
                if (oal.isFile()) env.put("ISAAC_OPENAL", oal.getAbsolutePath());
            }

            long t0 = System.currentTimeMillis();
            child = pb.start();
            log(">> started, streaming output");

            /* Dual logging:
             *   1. the app private directory (unaffected by anything external, long-term archive)
             *   2. game/run.log -- a path Termux and file managers can read directly,
             *      so full stdout+stderr is available without adb when something breaks.
             * 2026-11-05: previously only 1 was written and Termux could not read it (different uid),
             * leaving screenshots as the only option, which is far too slow. */
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
                        /* * 2026-11-06 startup speed-up: it used to flush every line.
                         * Each flush is a syscall to /storage/emulated (FUSE); thousands of lines during
                         * loading means thousands of synchronous writes.
                         * Now it flushes every 4 lines, or after 400ms, or on an obviously
                         * important line (* / SEGV / exit / error).
                         * A hang loses at most 3 lines / 0.4 s, which is enough to localise it. */
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
                    /* * 2026-11-05: beyond 400 lines it used to call Log.i() only and **stop writing the file**,
                     * so app.log only ever held the first 400 lines -- yet the crash/exit scene
                     * is exactly at the end. The file is now always complete; only the UI is capped. */
                    if (lines <= 400) {
                        log(line);
                    } else {
                        if (lines == 401) log("... (UI shows only the first 400 lines; the file is still complete)");
                        Log.i(TAG, line);
                        if (sharedLog != null) { sharedLog.write(line); sharedLog.write('\n'); sharedLog.flush(); }
                        if (fileLog != null) { fileLog.write(line); fileLog.write('\n'); }
                    }
                }
                int rc = child.waitFor();
                long ms = System.currentTimeMillis() - t0;

                /* The exit code is the most important clue:
                 *   137 = 128+9  -> SIGKILL, almost certainly killed by the system OOM
                 *   139 = 128+11 -> SIGSEGV, the game crashed itself
                 *   134 = 128+6  → SIGABRT
                 *   0            -> normal exit
                 * This used to log a single line, and the 400-line cap made it invisible anyway. */
                final String why;
                if (rc == 0)        why = "normal exit";
                else if (rc == 137) why = "★ SIGKILL -- most likely killed by the system OOM";
                else if (rc == 139) why = "★ SIGSEGV -- the game crashed";
                else if (rc == 134) why = "★ SIGABRT -- an explicit abort";
                else if (rc == 143) why = "SIGTERM (asked to terminate)";
                else why = "abnormal exit";
                log(">> child exited rc=" + rc + " (" + why + ") after "
                        + (ms / 1000) + "s, " + lines + " lines");
                runOnUiThread(new Runnable() {
                    public void run() {
                        if (status != null) {
                            status.setVisibility(View.VISIBLE);
                            status.setText("game exited: " + why
                                    + " (see the end of game/run.log for details)");
                        }
                    }
                });
            } finally {
                if (fileLog != null) try { fileLog.close(); } catch (IOException ignored) { }
                if (sharedLog != null) try { sharedLog.close(); } catch (IOException ignored) { }
            }
        } catch (Exception e) {
            log("!! child-process start failed: " + e);
            Log.e(TAG, "subprocess start failed", e);
        }
    }

    /** Little-endian 32-bit integer. */
    private static int le32(byte[] b, int off) {
        return (b[off] & 0xFF)
                | ((b[off + 1] & 0xFF) << 8)
                | ((b[off + 2] & 0xFF) << 16)
                | ((b[off + 3] & 0xFF) << 24);
    }

    // ---------------------------------------------------------------- input

    /** Retry connecting to the host input bridge (it listens once the child process is up).
     *
     * * 2026-11-06: this is the real reason "the pad does not respond at all".
     *
     * It used to retry 120 times x 500ms = **60 seconds**.
     * But the game must first load 3.5 GB of resources (over 100 seconds even for the -O2 build)
     * before it listens on 7137; by then Java had long given up -- inOut stayed null
     * and every sendKey logged "bridge not connected, dropping".
     *
     * Now:
     *   . the retry cap is raised to 30 minutes (3600 attempts), covering the slowest cold start
     *   . once connected, a send failure reconnects automatically instead of losing it forever
     *   . the connection time is recorded, which indicates how slow loading is */
    /* * 2026-11-06: fix two Java-side bugs behind "the pad does not work"
     *
     * bug 1: reconnecting did not close the old socket.
     *   Every success created a new socket overwriting inSock/inOut; the old one was neither closed nor reclaimed.
     *   Native was meanwhile blocked reading from the first connection, so:
     *     data went into socket N while native read socket 1 -> never received.
     *   Measured on device: native accepted 7 connections yet received 0 pad states.
     *
     * bug 2: concurrent reconnects.
     *   sendPadNow / sendKey / onPause could all trigger inOutLost(),
     *   each spawning a Thread to connect, so several threads connected at once and overwrote inOut.
     *   A lock now ensures only one connection thread at a time, plus a generation counter:
     *   a newer connection makes the older loop exit on its own. */
    private final Object inLock = new Object();
    /* * 2026-11-07: write lock. BufferedWriter is not thread-safe.
     * The touch thread (VirtualPad), physical-pad callbacks and onPause's releaseAll
     * all call sendPadNow; concurrent writes to one writer corrupt its internal state,
     * throwing IOException -> inOutLost -> reconnect -> corrupt again -> an endless loop.
     * Measured on device: 124 connections and 0 bytes received by native. */
    private final Object writeLock = new Object();
    private volatile int inGen;          /* connection generation: +1 on each new one */
    private volatile boolean inConnecting;

    private void connectInputBridge() {
        final int myGen;
        synchronized (inLock) {
            myGen = ++inGen;
            inConnecting = true;
        }
        final long t0 = System.currentTimeMillis();
        for (int i = 0; i < 3600; i++) {
            if (myGen != inGen) {         /* superseded by a newer connection */
                synchronized (inLock) { if (myGen == inGen) inConnecting = false; }
                return;
            }
            try {
                Socket sk = new Socket();
                sk.connect(new InetSocketAddress("127.0.0.1", IN_PORT), 500);
                sk.setTcpNoDelay(true);

                Socket old;
                synchronized (inLock) {
                    if (myGen != inGen) {   /* someone else won during connect; close this one */
                        try { sk.close(); } catch (Exception ignored) { }
                        return;
                    }
                    old = inSock;
                    inSock = sk;
                    inOut = new BufferedWriter(new OutputStreamWriter(
                            sk.getOutputStream(), "UTF-8"));
                    inConnecting = false;
                }
                /* * Swap in the new one before closing the old: "inOut pointing at a closed socket" must never happen. */
                if (old != null) {
                    try { old.close(); } catch (Exception ignored) { }
                }
                log(">> input bridge connected (waited "
                        + ((System.currentTimeMillis() - t0) / 1000) + " s, connection "
                        + myGen + ")");
                setPadHud("🎮 connected", 0xFF00E5FF);
                /* * Resend the current state immediately after a (re)connection succeeds.
                 * Otherwise, if the user did not move during the reconnect, lastPadLine is unchanged and
                 * the new connection never receives a first frame -- the game sees all zeros.
                 * sendPadNow forces a send when lastPadLine is null. */
                lastPadLine = null;
                sendPadNow();
                return;
            } catch (Exception e) {
                try { Thread.sleep(500); } catch (InterruptedException ie) { return; }
            }
        }
        synchronized (inLock) { if (myGen == inGen) inConnecting = false; }
        log("!! input bridge: not connected within 30 minutes (the game may not have started)");
    }

    /* ---------------------------------------------------------------
     * Virtual pad -> keyboard mirror (* 2026-11-10)
     *
     * Device symptom: entering the game pops up "Unrecognized Gamepad! Please setup a key config"
     * and the pad does nothing. native.log shows btn=1000 (A), btn=2100 (A+B) and
     * btn=0010 (START) all being taken by the game -- the data chain is fully working.
     *
     * Conclusion: **that screen reads the keyboard only; it does not poll the pad**. So pad presses go nowhere.
     *
     * Countermeasure: every virtual pad button also sends the matching VK alongside its XInput line.
     * Then whichever input path the game is using (pad or keyboard), it receives it.
     *
     * The mapping follows Isaac's default keyboard config so it matches native keyboard play:
     *   A->SPACE (use item)  B->E (bomb)  X->Q (card)  Y->TAB (map)
     *   START→Enter  BACK→ESC  LB→Shift  RB→C
     *   left stick->WASD (move)  right stick->arrow keys (shoot)
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

    private int kbMirrorButtons;      /* button bits from the last mirror */
    private int kbMirrorMove;         /* movement direction from the last mirror (WASD bits) */
    private int kbMirrorShoot;        /* shoot direction from the last mirror (arrow-key bits) */

    /** Dead zone: below this magnitude no direction is reported, so stick drift does not cause constant movement */
    private static final int STICK_DEADZONE = 8000;

    /** Turn one stick component into the "up/down" or "left/right" pair of keys. */
    private static int axisKeys(int v, int negVk, int posVk) {
        if (v <= -STICK_DEADZONE) return negVk;
        if (v >=  STICK_DEADZONE) return posVk;
        return 0;
    }

    /** Dispatch VKs by bit difference: newly set bits send "press", cleared bits send "release". */
    private void mirrorKeys(int nowMask, int prevMask) {
        int changed = nowMask ^ prevMask;
        if (changed == 0) return;
        for (int[] m : BTN_TO_VK) {
            int bit = m[0];
            if ((changed & bit) == 0) continue;
            sendKey(m[1], (nowMask & bit) != 0);
        }
    }

    /** Mirror the pad state to keyboard events when it changes.
     *
     * Only **changes** are sent, never repeats -- the keyboard is level-based, and repeats make the game
     * see "press/press/press" rather than "held", which some input handlers drop. */
    private synchronized void mirrorPadAsKeyboard(int buttons, int lx, int ly, int rx, int ry) {
        mirrorKeys(buttons, kbMirrorButtons);
        kbMirrorButtons = buttons;

        /* Left stick -> WASD (move). First combine both components into one direction mask,
         * then compare whole masks -- otherwise releasing W and pressing A overwrite each other. */
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

        /* Right stick -> arrow keys (shoot) */
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
        if (sendKeyN++ < 30) log(">> sendKey vk=" + vk + (down ? " press" : " release"));
        /* * 2026-11-10: also goes through the queue -- sendKey may be called from the UI main thread
         * (virtual pad mirror) or from a background callback; the tx thread writes it all. */
        queueKey(Integer.toString(vk) + (down ? " 1" : " 0"));
    }

    /* ---------------------------------------------------------------
     * Pad state: virtual pad + physical pad merged into one XInput state line.
     *
     * Protocol (parsed by host_user32.c):
     *     G <buttons-hex> <lx> <ly> <rx> <ry> <lt> <rt>\n
     * Axes -32768..32767 (Y up is positive), triggers 0..255.
     *
     * Merge rule: buttons are unioned; axes/triggers take the non-zero one (the virtual pad wins,
     * since that is what the user is operating). Unchanged content is not resent.
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
        /* 2026-11-06: debug log. On device we saw both "the bridge received it but the game ignored it" and
         * "the bridge received nothing" (touch landed on blank space); the two must be distinguishable -- so this
         * logs the first 30, showing at a glance whether anything was sent. */
        if (padLogN++ < 30) log(">> pad " + line
                + (inOut == null ? "  [bridge not connected]" : ""));
        if (padHudN++ < 8 || (b & 0xFFFF) != 0)
            setPadHud(String.format("🎮 %04X", b & 0xFFFF), 0xFF00E5FF);

        /* * 2026-11-10: mirror a keyboard event at the same time.
         *
         * This covers the Isaac screens (including the startup prompt) that read the keyboard only and never poll the pad.
         * See the comment on mirrorPadAsKeyboard. */
        mirrorPadAsKeyboard(b & 0xFFFF, lx, ly, rx, ry);

        /* ** 2026-11-10: enqueue only, **never write the socket on the calling thread**.
         *
         * This method is called from VirtualPad's onTouchEvent -- the UI main thread.
         * Writing the socket directly throws NetworkOnMainThreadException (confirmed by the device log),
         * so every touch failed once and triggered a reconnect, experienced as "sometimes it works".
         * See the long comment above startTxThread. */
        if (line.equals(lastPadLine)) return;   /* unchanged content is not sent */
        lastPadLine = line;
        queuePad(line);
    }

    /* Consecutive write failure count (see writeRaw) */
    private int sendFailStreak;

    /** The input bridge broke: clear it and reconnect in the background, or all later input is silently lost. */
    /** Connection lost: clear inOut and retry in **exactly one reconnect thread**.
     *
     * * 2026-11-06: every write failure used to spawn a new thread, so three call sites
     * (sendPadNow / sendKey / sendReset) landing together meant three concurrent connections
     * overwriting inOut -- native could no longer keep track.
     * inConnecting now guards it: if a reconnect is running, no new one starts. */
    /** Update the top-right pad indicator (main thread). */
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
            if (inConnecting) return;      /* a thread is already reconnecting */
            inConnecting = true;
        }
        /* ** 2026-11-07: reconnect throttling.
         *
         * On device there were 728 reconnects, each doing: close the old socket -> bind/accept
         * -> write a line. That entire cycle runs alongside the guest main loop and directly costs frames.
         * At least 250ms between failures turns the "storm" into a usable retry cadence. */
        long since = System.currentTimeMillis() - lastRetryMs;
        if (since < 250L) {
            try { Thread.sleep(250L - since); } catch (InterruptedException e) { return; }
        }
        lastRetryMs = System.currentTimeMillis();
        setPadHud("🎮 reconnecting...", 0xFFFF5555);
        new Thread(new Runnable() {
            public void run() { connectInputBridge(); }
        }, "isaac-input-reconnect").start();
    }

    private volatile long lastRetryMs;

    /** Even when the bridge is not up yet, someone must keep trying.
     * sendReset used to just set inOut to null on an exception and return,
     * after which nobody reconnected and nobody reported it: the pad went permanently silent. */
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
        /* * 2026-11-10: also goes through the queue. This means "release all keys",
         * so it must keep its order relative to later key events (hence the keys queue rather than merging). */
        queueKey("R");
        lastPadLine = null;      /* the next sendPadNow will force a resend */
    }

    /** Android KeyEvent keyCode -> PC virtual key code; -1 means no mapping. */
    private static int vkFromKeyCode(int kc) {
        switch (kc) {
            case KeyEvent.KEYCODE_BUTTON_A:      return VK_SPACE;  /* use item */
            case KeyEvent.KEYCODE_BUTTON_X:      return VK_E;      /* drop bomb */
            case KeyEvent.KEYCODE_BUTTON_START:  return VK_ESC;    /* pause */
            case KeyEvent.KEYCODE_BUTTON_SELECT: return VK_TAB;    /* map */
            case KeyEvent.KEYCODE_BUTTON_B:      return VK_TAB;
            case KeyEvent.KEYCODE_BUTTON_Y:      return VK_E;
            case KeyEvent.KEYCODE_DPAD_UP:       return VK_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:     return VK_DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT:     return VK_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:    return VK_RIGHT;
            /* With a physical keyboard, use it directly */
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

    /* Android physical pad keyCode -> XInput button bits; 0 means no mapping. */
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

    /** Whether the event comes from a physical pad or stick. */
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

    /* Physical pad stick -> XInput axes. Android AXIS_Y is negative upwards; XInput is positive upwards. */
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

            /* Triggers: different pads use different axis names; take the maximum for compatibility */
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
        /* Clear physical pad state too, or a pressed key sticks when switching back */
        hwButtons = hwLx = hwLy = hwRx = hwRy = hwLt = hwRt = 0;
        sendReset();          /* release all input when the Activity is not visible, so nothing sticks */
        super.onPause();
    }

    // ---------------------------------------------------------------- display

    /** Connect to native's 7138, read the fixed header + RGBA, and draw it into the SurfaceView. */
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
                        if (status != null) status.setText("frame stream: connected, waiting for the first frame...");
                    }
                });
                DataInputStream in = new DataInputStream(sk.getInputStream());
                byte[] payload = null;
                byte[] hdr = new byte[16];

                while (true) {
                    /* * 2026-11-05: native sends **little-endian** (a uint32_t array sent directly),
                     * while DataInputStream.readInt() reads **big-endian** -- they do not agree,
                     * so the magic was always wrong (seen as "!! bad frame magic" and endless reconnects).
                     * Decode little-endian by hand; do not rely on readInt(). */
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
                        /* A format change requires rebuilding the Bitmap -- copyPixelsFromBuffer
                         * requires the byte count to match the Bitmap's pixel format exactly. */
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
                                    status.setText("frame stream: " + fg + " frames received  " + fw + "x" + fh);
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
                        /* The Surface may vanish on rotation or backgrounding; ignore this frame */
                    } finally {
                        if (cv != null) {
                            try { hd.unlockCanvasAndPost(cv); } catch (Exception ignored) { }
                        }
                    }
                }
            } catch (Exception e) {
                /* native is not up yet; wait a moment and retry */
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
        /* No need to refresh the UI per line while the log panel is hidden -- text still accumulates in buf,
         * and can be posted in one go when expanded. Saves one runOnUiThread per line. */
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
     * Performance HUD (* 2026-11-10)
     *
     * Read once per second:
     *   fps    <- difference of NativeBridge.frameCount()
     *   CPU%   <- /proc/self/stat utime+stime difference over wall clock (can exceed 100%, multi-core)
     *   memory <- VmRSS from /proc/self/status
     *   threads <- Threads from the same file
     *
     * Everything comes from /proc/self; no system permission needed.
     * Poll on a background thread and hand only the string to the UI thread -- never read files on the UI thread.
     * ========================================================================== */
    private void startPerfHud(final TextView tv) {
        new Thread(new Runnable() {
            public void run() {
                long prevCpuTicks = -1;
                long prevWall = 0;
                int  prevFrames = -1;
                int  hz = 100;             /* USER_HZ, fixed at 100 on Android/Linux */
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
                     * Fields 14/15 are utime/stime (units = USER_HZ).
                     * Note the comm field may contain spaces and parentheses, so **split after the last ')' from the right**. */
                    try {
                        java.io.BufferedReader r = new java.io.BufferedReader(
                                new java.io.FileReader("/proc/self/stat"), 256);
                        String line = r.readLine();
                        r.close();
                        if (line != null) {
                            int p = line.lastIndexOf(')');
                            if (p > 0) {
                                String[] f = line.substring(p + 2).split(" ");
                                /* After stripping "pid (comm) ", f[0] is the third field (state),
                                 * so utime = f[11] and stime = f[12]. */
                                if (f.length > 12) {
                                    cpuTicks = Long.parseLong(f[11]) + Long.parseLong(f[12]);
                                }
                            }
                        }
                    } catch (Throwable ignored) { }

                    /* --- memory / threads: /proc/self/status --- */
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

                    /* --- compute differences --- */
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
                            "FPS %s  CPU %s  mem %s  threads %s  frames %s",
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

    /** Extract the number from a line like "VmRSS:\t  123456 kB". */
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

    /* 2026-11-05: onDestroy no longer kills the child process.
     *
     * The p.destroy() here used to be one of the direct causes of the "freeze":
     * when the Activity was destroyed because the screen went dark, the system reclaimed it, or the
     * user accidentally switched away, the child process was killed too and the image froze.
     *
     * Now only two paths end the game:
     *   1. the user taps the stop button
     *   2. the game crashes or exits on its own
     * On Activity destruction the child process keeps running, and reopening reconnects the frame stream and input bridge.
     *
     * The cost is a genuine lingering process. But there is only one game, and Termux can pkill it,
     * so the choice here is "rather linger than kill by mistake".
     */
    @Override
    protected void onDestroy() {
        log(">> activity destroyed (child keeps running)");
        super.onDestroy();
    }
}
