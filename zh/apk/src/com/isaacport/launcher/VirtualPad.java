package com.isaacport.launcher;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;

/**
 * 虚拟手柄 —— 屏幕上叠加的触屏控件，直接映射成 **XInput 手柄状态**。
 *
 * 2026-11-06（第二轮）：整屏可操作
 * ---------------------------------
 * 旧版只在几个**固定圆形**里响应，结果用户按在空白处（实测 x=2431 y=734）
 * 什么都没发生，日志里一条状态都没有 —— 看起来就是"手柄没用"。
 *
 * 现在改成**分区兜底**，手指落在任何地方都有意义：
 *
 *   左半屏（x < 0.45w）
 *       任意位置按下 = 浮动左摇杆，按下点就是圆心，拖动即方向
 *       （浮动摇杆是手机 FPS 的通用做法，不需要低头找圆）
 *   右半屏
 *       先判固定按钮：A/B/X/Y 菱形、START、右摇杆圆
 *       都不命中时：
 *         · 落在屏幕下方 40% → 浮动右摇杆（射击方向）
 *         · 落在别处         → 短按抬起时补发一次 A 键（确认/使用）
 *
 * 固定按钮的"双发"不变：
 *   A → XI_A + bLeftTrigger=255（主动道具）
 *   B → XI_B + LB（炸弹）
 *   X → XI_X + RB（药丸/卡牌）
 *   Y → XI_Y + BACK（地图）
 *   START → START（暂停）
 *
 * 多点触控：每个 pointerId 单独记账，可以一边推摇杆一边按按钮。
 */
public class VirtualPad extends View {

    /** 状态回调：完整的一帧 XInput 手柄状态。 */
    public interface Sink {
        void pad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt);
    }

    /* XInput 按钮位 */
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

    /* 控件区号 */
    private static final int Z_NONE = 0;
    private static final int Z_MOVE = 1;      /* 左摇杆（固定或浮动） */
    private static final int Z_SHOOT = 2;     /* 右摇杆 */
    private static final int Z_A = 10, Z_B = 11, Z_X = 12, Z_Y = 13;
    private static final int Z_START = 20;

    private static final int MAX_PTR = 16;
    /** 短按判定：按下到抬起的毫秒数与位移阈值 */
    private static final long TAP_MS = 300;
    private static final float TAP_SLOP = 40f;

    private Sink sink;

    /* 固定布局（像素，onSizeChanged 里算） */
    private float moveCx, moveCy, moveR;          /* 左摇杆（视觉提示位置） */
    private float shootCx, shootCy, shootR;
    private float btnCx, btnCy, btnR, btnGap;
    private float startCx, startCy, startR;

    /* 浮动摇杆：按下时以落点为圆心 */
    private float floatMoveCx, floatMoveCy, floatMoveR;
    private float floatShootCx, floatShootCy, floatShootR;
    private boolean moveFloating, shootFloating;

    private int viewW, viewH;

    /* 状态 */
    private final int[] ptrZone = new int[MAX_PTR];
    private final long[] ptrDownMs = new long[MAX_PTR];
    private final float[] ptrDownX = new float[MAX_PTR];
    private final float[] ptrDownY = new float[MAX_PTR];
    private final boolean[] ptrTapCandidate = new boolean[MAX_PTR];

    private float moveDx, moveDy;      /* 摇杆头偏移，仅用于绘制 */
    private float shootDx, shootDy;

    private int buttons;
    private int lx, ly, rx, ry;
    private int lt, rt;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textHalo = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dim = new Paint(Paint.ANTI_ALIAS_FLAG);

    public VirtualPad(Context ctx) {
        super(ctx);
        for (int i = 0; i < MAX_PTR; i++) ptrZone[i] = Z_NONE;

        fill.setStyle(Paint.Style.FILL);
        fill.setColor(0x66000000);            /* 半透明黑，白底也看得见 */

        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(4f);
        stroke.setColor(0xFF00E5FF);          /* 亮青色 */

        text.setColor(0xFFFFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);

        textHalo.setColor(0xFF000000);
        textHalo.setTextAlign(Paint.Align.CENTER);
        textHalo.setFakeBoldText(true);
        textHalo.setStyle(Paint.Style.STROKE);
        textHalo.setStrokeWidth(6f);

        /* 浮动摇杆的淡描边：提示"这里也能按" */
        dim.setStyle(Paint.Style.STROKE);
        dim.setStrokeWidth(2f);
        dim.setColor(0x4400E5FF);

        setFocusable(false);
    }

    public void setSink(Sink s) { sink = s; }

    public interface Trace { void put(String s); }
    private Trace tracer;
    public void setTrace(Trace t) { tracer = t; }

    private void trace(String s) {
        Trace t = tracer;
        if (t != null) t.put(s);
    }

    /** 把当前状态推给宿主。 */
    private void push() {
        int outLt = lt;
        int outButtons = buttons;
        if (zoneActive(Z_A))     { outButtons |= XI_A; outLt = 255; }
        if (zoneActive(Z_B))     { outButtons |= XI_B | XI_LEFT_SHOULDER; }
        if (zoneActive(Z_X))     { outButtons |= XI_X | XI_RIGHT_SHOULDER; }
        if (zoneActive(Z_Y))     { outButtons |= XI_Y | XI_BACK; }
        if (zoneActive(Z_START)) { outButtons |= XI_START; }

        Sink s = sink;
        if (s != null) s.pad(outButtons, lx, ly, rx, ry, outLt, rt);
    }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        viewW = w; viewH = h;
        float unit = Math.min(w, h);

        moveR  = unit * 0.115f;
        moveCx = w * 0.16f;
        moveCy = h * 0.74f;

        shootR  = unit * 0.095f;
        shootCx = w * 0.885f;
        shootCy = h * 0.855f;

        btnR   = unit * 0.052f;
        btnGap = unit * 0.082f;
        btnCx  = w * 0.775f;
        btnCy  = h * 0.80f;

        startR  = unit * 0.042f;
        startCx = w * 0.90f;
        startCy = h * 0.10f;

        text.setTextSize(unit * 0.036f);
        textHalo.setTextSize(unit * 0.036f);
    }

    /* ------------------------------------------------------------ 绘制 */

    @Override
    protected void onDraw(Canvas cv) {
        if (moveR <= 0) return;

        /* 分区提示线：让用户知道整屏都能按 */
        cv.drawLine(viewW * 0.45f, 0, viewW * 0.45f, viewH, dim);

        /* 左摇杆：浮动时画在手下方，否则画固定位置 */
        float mcx = moveFloating ? floatMoveCx : moveCx;
        float mcy = moveFloating ? floatMoveCy : moveCy;
        float mr  = moveFloating ? floatMoveR  : moveR;
        cv.drawCircle(mcx, mcy, mr, fill);
        cv.drawCircle(mcx, mcy, mr, stroke);
        fill.setColor(0xCC00E5FF);
        cv.drawCircle(mcx + moveDx, mcy + moveDy, mr * 0.42f, fill);
        fill.setColor(0x66000000);
        cv.drawCircle(mcx + moveDx, mcy + moveDy, mr * 0.42f, stroke);

        /* 四键菱形 */
        drawBtn(cv, btnCx, btnCy - btnGap, "Y", Z_Y);
        drawBtn(cv, btnCx - btnGap, btnCy, "X", Z_X);
        drawBtn(cv, btnCx + btnGap, btnCy, "B", Z_B);
        drawBtn(cv, btnCx, btnCy + btnGap, "A", Z_A);

        /* 右摇杆 */
        float scx = shootFloating ? floatShootCx : shootCx;
        float scy = shootFloating ? floatShootCy : shootCy;
        float sr  = shootFloating ? floatShootR  : shootR;
        cv.drawCircle(scx, scy, sr, fill);
        cv.drawCircle(scx, scy, sr, stroke);
        fill.setColor(0xCC00E5FF);
        cv.drawCircle(scx + shootDx, scy + shootDy, sr * 0.42f, fill);
        fill.setColor(0x66000000);
        cv.drawCircle(scx + shootDx, scy + shootDy, sr * 0.42f, stroke);

        /* START */
        cv.drawCircle(startCx, startCy, startR, fill);
        cv.drawCircle(startCx, startCy, startR, stroke);
        float startTy = startCy + text.getTextSize() * 0.35f;
        cv.drawText("II", startCx, startTy, textHalo);
        cv.drawText("II", startCx, startTy, text);
    }

    private void drawBtn(Canvas cv, float cx, float cy, String label, int zone) {
        boolean on = zoneActive(zone);
        fill.setColor(on ? 0x9900E5FF : 0x66000000);
        cv.drawCircle(cx, cy, btnR, fill);
        cv.drawCircle(cx, cy, btnR, stroke);
        fill.setColor(0x66000000);
        float ty = cy + text.getTextSize() * 0.35f;
        cv.drawText(label, cx, ty, textHalo);
        cv.drawText(label, cx, ty, text);
    }

    private boolean zoneActive(int zone) {
        for (int i = 0; i < MAX_PTR; i++) if (ptrZone[i] == zone) return true;
        return false;
    }

    /* ------------------------------------------------------------ 触摸 */

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        if (moveR <= 0) return false;
        int action = ev.getActionMasked();
        int idx = ev.getActionIndex();

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            int id = ev.getPointerId(idx);
            if (id >= 0 && id < MAX_PTR)
                assign(id, ev.getX(idx), ev.getY(idx));
            return true;
        }
        if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < ev.getPointerCount(); i++) {
                int id = ev.getPointerId(i);
                if (id < 0 || id >= MAX_PTR) continue;
                int z = ptrZone[id];
                if (z == Z_MOVE || z == Z_SHOOT) {
                    /* 移动超出短按阈值就不再算 tap */
                    float dx = ev.getX(i) - ptrDownX[id];
                    float dy = ev.getY(i) - ptrDownY[id];
                    if (dx * dx + dy * dy > TAP_SLOP * TAP_SLOP)
                        ptrTapCandidate[id] = false;
                    updateStick(id, z, ev.getX(i), ev.getY(i));
                }
            }
            return true;
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP
                || action == MotionEvent.ACTION_CANCEL) {
            if (action == MotionEvent.ACTION_CANCEL) {
                releaseAll();
            } else {
                int id = ev.getPointerId(idx);
                if (id >= 0 && id < MAX_PTR) release(id);
            }
            return true;
        }
        return true;
    }

    /** 按下：先判固定按钮；都没命中就按屏幕分区兜底。 */
    private void assign(int id, float x, float y) {
        release(id);

        ptrDownMs[id] = System.currentTimeMillis();
        ptrDownX[id] = x;
        ptrDownY[id] = y;
        ptrTapCandidate[id] = false;

        int z = hitFixed(x, y);
        if (z != Z_NONE) {
            ptrZone[id] = z;
            if (z == Z_MOVE) {
                moveFloating = false;
                updateStick(id, Z_MOVE, x, y);
            } else if (z == Z_SHOOT) {
                shootFloating = false;
                updateStick(id, Z_SHOOT, x, y);
            } else {
                push();
            }
            trace("down z=" + z + " @" + (int) x + "," + (int) y);
            invalidate();
            return;
        }

        /* ---- 兜底：整屏都能按 ---- */
        if (x < viewW * 0.45f) {
            /* 左半屏 → 浮动左摇杆，落点为圆心 */
            moveFloating = true;
            floatMoveCx = x;
            floatMoveCy = y;
            floatMoveR = Math.min(viewW, viewH) * 0.13f;
            ptrZone[id] = Z_MOVE;
            moveDx = moveDy = 0;
            updateStick(id, Z_MOVE, x, y);
            trace("down 浮动左摇杆 @" + (int) x + "," + (int) y);
        } else if (y > viewH * 0.6f) {
            /* 右半屏下半 → 浮动右摇杆（射击） */
            shootFloating = true;
            floatShootCx = x;
            floatShootCy = y;
            floatShootR = Math.min(viewW, viewH) * 0.11f;
            ptrZone[id] = Z_SHOOT;
            shootDx = shootDy = 0;
            updateStick(id, Z_SHOOT, x, y);
            trace("down 浮动右摇杆 @" + (int) x + "," + (int) y);
        } else {
            /* 右半屏上半的空白 → 短按补发 A（确认/使用） */
            ptrZone[id] = Z_A;
            ptrTapCandidate[id] = true;
            push();
            trace("down 空白=A @" + (int) x + "," + (int) y);
        }
        invalidate();
    }

    /** 固定控件命中测试。 */
    private int hitFixed(float x, float y) {
        if (hit(x, y, startCx, startCy, startR * 1.6f)) return Z_START;
        if (hit(x, y, btnCx, btnCy - btnGap, btnR * 1.5f)) return Z_Y;
        if (hit(x, y, btnCx - btnGap, btnCy, btnR * 1.5f)) return Z_X;
        if (hit(x, y, btnCx + btnGap, btnCy, btnR * 1.5f)) return Z_B;
        if (hit(x, y, btnCx, btnCy + btnGap, btnR * 1.5f)) return Z_A;
        if (hit(x, y, shootCx, shootCy, shootR * 1.55f)) return Z_SHOOT;
        if (hit(x, y, moveCx, moveCy, moveR * 1.45f)) return Z_MOVE;
        return Z_NONE;
    }

    private static boolean hit(float x, float y, float cx, float cy, float r) {
        float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    }

    /** 摇杆移动：算模拟量（XInput 语义：右/上为正，满量程 32767）。 */
    private void updateStick(int id, int zone, float x, float y) {
        float cx = (zone == Z_MOVE) ? (moveFloating ? floatMoveCx : moveCx)
                                    : (shootFloating ? floatShootCx : shootCx);
        float cy = (zone == Z_MOVE) ? (moveFloating ? floatMoveCy : moveCy)
                                    : (shootFloating ? floatShootCy : shootCy);
        float r  = (zone == Z_MOVE) ? (moveFloating ? floatMoveR : moveR)
                                    : (shootFloating ? floatShootR : shootR);

        float dx = x - cx, dy = y - cy;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > r) { dx = dx / len * r; dy = dy / len * r; }

        if (zone == Z_MOVE) { moveDx = dx; moveDy = dy; }
        else                { shootDx = dx; shootDy = dy; }

        int ax = Math.round(dx / r * 32767f);
        int ay = Math.round(-dy / r * 32767f);   /* 屏幕 Y 向下，XInput Y 向上 */
        if (ax < -32768) ax = -32768;
        if (ax >  32767) ax =  32767;
        if (ay < -32768) ay = -32768;
        if (ay >  32767) ay =  32767;

        if (zone == Z_MOVE) { lx = ax; ly = ay; }
        else                { rx = ax; ry = ay; }

        push();
        invalidate();
    }

    /** 松开某个 pointer。 */
    private void release(int id) {
        int z = ptrZone[id];
        if (z == Z_NONE) return;
        ptrZone[id] = Z_NONE;

        boolean tap = ptrTapCandidate[id];
        ptrTapCandidate[id] = false;

        switch (z) {
            case Z_MOVE:
                lx = ly = 0;
                moveDx = moveDy = 0;
                moveFloating = false;
                break;
            case Z_SHOOT:
                rx = ry = 0;
                shootDx = shootDy = 0;
                shootFloating = false;
                break;
            default:
                break;      /* 按钮位由 push() 根据 ptrZone 现算 */
        }

        /* 空白区短按 → 抬起时补发一次 A 脉冲，保证"随便点一下"也有反应 */
        if (tap && z == Z_A && System.currentTimeMillis() - ptrDownMs[id] <= TAP_MS) {
            push();                            /* 当前已是按下态 */
            ptrZone[id] = Z_NONE;
            push();                            /* 再发一次释放态 */
            trace("tap=A 脉冲");
            invalidate();
            return;
        }

        push();
        invalidate();
    }

    /** 全部松开（Activity 失焦、切后台时调用）。 */
    public void releaseAll() {
        for (int i = 0; i < MAX_PTR; i++) {
            ptrZone[i] = Z_NONE;
            ptrTapCandidate[i] = false;
        }
        lx = ly = rx = ry = 0;
        buttons = 0;
        lt = rt = 0;
        moveDx = moveDy = shootDx = shootDy = 0;
        moveFloating = shootFloating = false;
        push();
        invalidate();
    }
}
