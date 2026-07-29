package com.anland.consumer;

import android.content.Context;
import android.graphics.Point;
import android.view.MotionEvent;
import android.view.ViewConfiguration;
import android.view.WindowManager;

/**
 * Laptop-style virtual touchpad: interprets finger gestures on the surface as
 * relative mouse motion, taps/clicks, long-press drag and two-finger scroll,
 * forwarding them to the remote through {@link Native}.
 *
 * Self-contained state machine — the host routes non-mouse touches here (see
 * MainActivity.onTouchEvent) when touchpad mode is on, pushes the acceleration
 * preference via {@link #setAccelStrength}, and calls {@link #onSurfaceChanged}
 * when the surface is resized.
 */
public final class VirtualTouchpad {

    /**
     * Optional output used by the pointer-capture adapter.  The original
     * touchpad path keeps writing directly to Native; a capture instance can
     * reuse the same gesture state machine while supplying its own movement
     * and coordinate backend.
     */
    interface Output {
        void onMotion(float dx, float dy);
        void onScroll(int axis, float value);
        void onButton(int button, boolean pressed);
    }

    // 状态机
    private static final int STATE_IDLE = 0;
    private static final int STATE_ONE_FINGER = 1;
    private static final int STATE_TWO_FINGER = 2;
    private static final int STATE_DRAGGING = 3;
    private int currentState = STATE_IDLE;

    private float lastX1, lastY1;
    private float startX1, startY1;
    private float lastX2, lastY2;
    private long downTime1;
    private final float touchSlop;

    private boolean isSingleTapCandidate = false;
    private boolean isTwoFingerTapCandidate = false;
    private boolean isDraggingActive = false;

    private long lastTapTime = 0;
    private float lastTapX, lastTapY;
    private boolean isDoubleTapPending = false;

    private static final long TOUCH_LONG_PRESS_TIMEOUT = 500;
    private boolean hasLongPressed = false;
    private boolean isLongPressPossible = false;
    private boolean isMultiFinger = false;

    // 鼠标位置（相对模式）
    private float mouseX = 0;
    private float mouseY = 0;
    private int screenWidth = 1920;
    private int screenHeight = 1080;

    private float mouseAccelStrength = 1.0f; // 加速度强度，0.5 ~ 10.0

    // ===== 调整后的平滑/抗抖动参数（更灵敏、更连续） =====
    private static final float DEAD_ZONE = 0.3f;          // 死区从 0.5 降到 0.3
    private static final float SMOOTHING_FACTOR = 0.45f;   // 提高响应速度
    private static final float ACCUMULATED_THRESHOLD = 0.1f; // 从 0.8 大幅降低，让移动更连续

    private float smoothedDx = 0f;
    private float smoothedDy = 0f;
    private float accumulatedX = 0f;
    private float accumulatedY = 0f;
    private boolean smoothInitialized = false;

    private final Context context;
    private final Native mNative;
    private final Output output;
    // The original on-screen touchpad emits an explicit double-click sequence.
    // Captured hardware taps already arrive as separate clicks, so emitting a
    // second synthetic pair would turn two taps into three clicks.
    private final boolean synthesizeDoubleTap;

    VirtualTouchpad(Context context, Native n) {
        this(context, n, null);
    }

    VirtualTouchpad(Context context, Native n, Output output) {
        this.context = context;
        this.mNative = n;
        this.output = output;
        this.synthesizeDoubleTap = output == null;
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        updateScreenSize();
        mouseX = screenWidth / 2f;
        mouseY = screenHeight / 2f;
    }

    /** Set the acceleration strength (clamped to 0.5 ~ 10.0). */
    void setAccelStrength(float strength) {
        mouseAccelStrength = Math.max(0.5f, Math.min(10.0f, strength));
    }

    /** Re-read screen size and re-anchor the cursor after a surface resize. */
    void onSurfaceChanged() {
        updateScreenSize();
        mouseX = clamp(mouseX, 0, screenWidth);
        mouseY = clamp(mouseY, 0, screenHeight);
        resetSmoothing();
    }

    /** Cancel an in-progress gesture when the capture window loses focus. */
    void cancel() {
        if (isDraggingActive)
            sendButton(0x110, false);
        resetTouchpadState();
        resetSmoothing();
        lastTapTime = 0L;
        lastTapX = 0f;
        lastTapY = 0f;
    }

    /**
     * Re-enter two-finger tracking with fresh anchors, used by the pointer-capture
     * adapter when a gesture drops back to two fingers.
     *
     * A third finger cancels the gesture, and the pointer-count transition that
     * follows never reaches onTouch (the host drops events while three or more
     * fingers are down), so the state machine would otherwise stay idle and stop
     * reporting scroll until every finger lifts. Anchoring on the current positions
     * keeps the first delta after the transition from jumping.
     *
     * Deliberately does not arm the tap candidates: the fingers have already been
     * down long enough to be a third-finger gesture, not a two-finger tap.
     */
    void rearmTwoFinger(float x1, float y1, float x2, float y2) {
        currentState = STATE_TWO_FINGER;
        lastX1 = x1;
        lastY1 = y1;
        lastX2 = x2;
        lastY2 = y2;
        isMultiFinger = true;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isLongPressPossible = false;
        hasLongPressed = false;
        resetSmoothing();
    }

    private void sendButton(int button, boolean pressed) {
        if (output != null)
            output.onButton(button, pressed);
        else if (mNative != null)
            mNative.sendMouseButton(button, pressed);
    }

    private void sendScroll(int axis, float value) {
        if (output != null)
            output.onScroll(axis, value);
        else if (mNative != null)
            mNative.sendMouseScroll(axis, value);
    }

    /**
     * Send a relative movement to an adapter, or preserve the original
     * absolute-cursor behavior for the normal virtual touchpad.
     */
    private void sendMotion(float dx, float dy) {
        if (output != null) {
            output.onMotion(dx, dy);
            return;
        }
        mouseX = clamp(mouseX + dx, 0, screenWidth);
        mouseY = clamp(mouseY + dy, 0, screenHeight);
        if (mNative != null)
            mNative.sendMouseMotion(mouseX, mouseY, 0f, 0f);
    }

    // ==================== 触摸板手势及辅助方法 ====================
    boolean onTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();

        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                float x = event.getX();
                float y = event.getY();
                startX1 = lastX1 = x;
                startY1 = lastY1 = y;
                downTime1 = event.getEventTime();
                hasLongPressed = false;
                isLongPressPossible = true;
               
