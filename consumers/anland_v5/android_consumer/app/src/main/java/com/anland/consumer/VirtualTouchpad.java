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

    // Two-finger classification, decided once per two-finger phase so a gesture
    // cannot oscillate between scrolling and being declined.
    private static final int TWO_FINGER_UNDECIDED = 0;
    private static final int TWO_FINGER_SCROLL = 1;
    private static final int TWO_FINGER_NOT_SCROLL = 2;
    private int twoFingerMode = TWO_FINGER_UNDECIDED;
    // Both fingers' positions when the two-finger phase began. The scroll test
    // measures displacement from here rather than frame to frame, matching AOSP.
    private float twoFingerStartX1, twoFingerStartY1;
    private float twoFingerStartX2, twoFingerStartY2;

    // Latched for the rest of the gesture once this class declines it, so the host
    // sees the whole remaining stream (including the final UP) and can release the
    // touches it forwarded.
    private boolean gestureUnhandled = false;

    // AOSP's equivalents are 1.5mm and 7.0mm ("Two Finger Scroll/Move Distance
    // Thresh" in libgestures). Coordinates here are view pixels rather than
    // millimetres, so the physical values do not carry over; these are multiples of
    // touchSlop instead, and both multipliers are exposed in Settings because the
    // right values depend on the pad. Defaults keep AOSP's ~4.7:1 ratio.
    static final float DEFAULT_SCROLL_THRESHOLD_FACTOR = 0.5f;
    static final float DEFAULT_MOVE_THRESHOLD_FACTOR = 2.35f;
    private float scrollDistanceThreshold;
    private float moveDistanceThreshold;

    // Which axis a two-finger scroll is reported on: an axis wins when it exceeds
    // this fraction of the other, so a diagonal drag can report both.
    private static final float AXIS_DOMINANCE_RATIO = 0.5f;
    /** Scroll distance per pixel of finger travel. Configurable in Settings. */
    static final float DEFAULT_SCROLL_SPEED = 0.5f;
    private float scrollSpeed = DEFAULT_SCROLL_SPEED;
    private boolean scrollReversed = false;

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
        setGestureThresholds(DEFAULT_SCROLL_THRESHOLD_FACTOR,
                DEFAULT_MOVE_THRESHOLD_FACTOR);
        updateScreenSize();
        mouseX = screenWidth / 2f;
        mouseY = screenHeight / 2f;
    }

    /** Set the acceleration strength (clamped to 0.5 ~ 10.0). */
    void setAccelStrength(float strength) {
        mouseAccelStrength = Math.max(0.5f, Math.min(10.0f, strength));
    }

    /** Set the two-finger scroll distance per pixel of travel (0.05 ~ 5.0). */
    void setScrollSpeed(float speed) {
        scrollSpeed = Math.max(0.05f, Math.min(5.0f, speed));
    }

    /** Invert both scroll axes ("natural" scrolling). */
    void setScrollReversed(boolean reversed) {
        scrollReversed = reversed;
    }

    /**
     * Set the two-finger classification thresholds, as multiples of touchSlop.
     *
     * scrollFactor is how far a finger must travel before it counts as moving at all;
     * lower it if a pinch is being mistaken for a scroll. moveFactor is how far the
     * leading finger must travel before an ambiguous phase is resolved; lower it if
     * scrolling feels slow to engage.
     */
    void setGestureThresholds(float scrollFactor, float moveFactor) {
        scrollDistanceThreshold = touchSlop * Math.max(0.05f, Math.min(5.0f, scrollFactor));
        moveDistanceThreshold = touchSlop * Math.max(0.1f, Math.min(10.0f, moveFactor));
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

    /** Larger of the two by magnitude, keeping its sign (libgestures MaxMag). */
    private static float maxMag(float a, float b) {
        return Math.abs(a) > Math.abs(b) ? a : b;
    }

    /** Smaller of the two by magnitude, keeping its sign (libgestures MinMag). */
    private static float minMag(float a, float b) {
        return Math.abs(a) < Math.abs(b) ? a : b;
    }

    /**
     * Classify a two-finger phase, following the rule AOSP's touchpad gesture
     * library uses (libgestures ImmediateInterpreter::GetTwoFingerGestureType):
     * take each finger's displacement since the phase began, pick whichever axis
     * dominates, and require both fingers to have travelled the same way along it.
     *
     * A pinch moves the fingers in opposite directions, so the sign product is
     * negative and it is not a scroll. One finger resting while the other travels
     * zeroes the small term, which also fails the test — AOSP calls that a cursor
     * move; here everything that is not a scroll is left to the host.
     */
    private int classifyTwoFinger(float x1, float y1, float x2, float y2) {
        float dx1 = x1 - twoFingerStartX1;
        float dy1 = y1 - twoFingerStartY1;
        float dx2 = x2 - twoFingerStartX2;
        float dy2 = y2 - twoFingerStartY2;

        float largeDx = maxMag(dx1, dx2);
        float largeDy = maxMag(dy1, dy2);
        float large;
        float small;
        if (Math.abs(largeDx) > Math.abs(largeDy)) {
            large = largeDx;
            small = minMag(dx1, dx2);
        } else {
            large = largeDy;
            small = minMag(dy1, dy2);
        }
        if (Math.abs(small) < scrollDistanceThreshold)
            small = 0f;

        if (large * small <= 0f) {
            // Not the same direction: a pinch, or one finger resting while the other
            // travels. Stay undecided until one finger has clearly committed, so a
            // two-finger tap is still allowed to become a click.
            return Math.abs(large) < moveDistanceThreshold
                    ? TWO_FINGER_UNDECIDED : TWO_FINGER_NOT_SCROLL;
        }
        return Math.abs(large) < scrollDistanceThreshold
                ? TWO_FINGER_UNDECIDED : TWO_FINGER_SCROLL;
    }

    /**
     * Give up on the current gesture: release anything in progress and latch the
     * unhandled flag so every remaining event reports unhandled, including the final
     * UP. The host needs that whole tail to release the touch points it forwarded.
     */
    private boolean declineGesture() {
        if (isDraggingActive)
            sendButton(0x110, false);
        isDraggingActive = false;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isLongPressPossible = false;
        gestureUnhandled = true;
        resetSmoothing();
        return false;
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
    /**
     * Interpret one touchpad event.
     *
     * Only three gestures are implemented: one finger moves the cursor, two fingers
     * travelling together scroll, and a quick two-finger tap is a right click.
     * Anything else — a pinch, three or more fingers — is declined.
     *
     * @return true when this class consumed the event, false when the gesture is not
     *         one it implements. A false result latches for the remainder of the
     *         gesture, so the caller sees the whole stream through the final UP and
     *         can forward it as touch instead.
     */
    boolean onTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();

        // Once declined, stay declined: the caller is mid-way through forwarding this
        // gesture as touch and still needs the pointer-up events to release it.
        if (gestureUnhandled) {
            if (action == MotionEvent.ACTION_UP
                    || action == MotionEvent.ACTION_CANCEL) {
                resetTouchpadState();
                resetSmoothing();
            }
            return false;
        }

        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                float x = event.getX();
                float y = event.getY();
                startX1 = lastX1 = x;
                startY1 = lastY1 = y;
                downTime1 = event.getEventTime();
                hasLongPressed = false;
                isLongPressPossible = true;
                isSingleTapCandidate = true;
                isTwoFingerTapCandidate = false;
                isDraggingActive = false;
                isMultiFinger = false;
                currentState = STATE_ONE_FINGER;
                twoFingerMode = TWO_FINGER_UNDECIDED;
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                isMultiFinger = true;
                isSingleTapCandidate = false;
                isLongPressPossible = false;
                if (currentState == STATE_DRAGGING) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                }
                if (pointerCount == 2) {
                    currentState = STATE_TWO_FINGER;
                    isTwoFingerTapCandidate = true;
                    lastX1 = twoFingerStartX1 = event.getX(0);
                    lastY1 = twoFingerStartY1 = event.getY(0);
                    lastX2 = twoFingerStartX2 = event.getX(1);
                    lastY2 = twoFingerStartY2 = event.getY(1);
                    twoFingerMode = TWO_FINGER_UNDECIDED;
                } else if (pointerCount >= 3) {
                    // Three or more fingers is never one of ours.
                    return declineGesture();
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (pointerCount == 1 && !isMultiFinger) {
                    float x = event.getX();
                    float y = event.getY();
                    float rawDx = x - lastX1;
                    float rawDy = y - lastY1;
                    float dist = (float) Math.hypot(x - startX1, y - startY1);

                    if (dist > touchSlop) {
                        isLongPressPossible = false;
                        isSingleTapCandidate = false;
                        // The original state machine intentionally preserves
                        // this flag through POINTER_UP. In capture mode, clear
                        // it once the remaining finger actually moves so a
                        // scroll/drag cannot finish as a right-click.
                        if (output != null)
                            isTwoFingerTapCandidate = false;
                    }

                    if (isLongPressPossible && !hasLongPressed &&
                            (event.getEventTime() - downTime1) >= TOUCH_LONG_PRESS_TIMEOUT) {
                        hasLongPressed = true;
                        currentState = STATE_DRAGGING;
                        isDraggingActive = true;
                        sendButton(0x110, true);
                        mouseX = clamp(mouseX, 0, screenWidth);
                        mouseY = clamp(mouseY, 0, screenHeight);
                        if (output != null)
                            output.onMotion(0f, 0f);
                        else if (mNative != null)
                            mNative.sendMouseMotion(mouseX, mouseY, 0f, 0f);
                        resetSmoothing();
                        break;
                    }

                    float[] smoothed = applySmoothing(rawDx, rawDy);
                    float smoothDx = smoothed[0];
                    float smoothDy = smoothed[1];

                    if (smoothDx != 0f || smoothDy != 0f) {
                        // 计算移动距离（平滑后的欧式距离）
                        float distance = (float) Math.hypot(smoothDx, smoothDy);

                        // 改进的加速度曲线：以 10px 为参考阈值，使小位移也能获得明显加速
                        float speedFactor = distance / 10.0f;
                        // 使用 sigmoid-like 曲线：scale = 1 + (strength - 1) * (speed / (1 + speed))
                        float dynamicScale = 1.0f + (mouseAccelStrength - 1.0f) * (speedFactor / (1.0f + speedFactor));
                        // 限制范围，防止失控（最大不超过 10 倍）
                        dynamicScale = Math.max(0.3f, Math.min(10.0f, dynamicScale));

                        float moveX = smoothDx * dynamicScale;
                        float moveY = smoothDy * dynamicScale;
                        sendMotion(moveX, moveY);
                    }

                    lastX1 = x;
                    lastY1 = y;

                } else if (pointerCount == 2) {
                    if (currentState == STATE_TWO_FINGER) {
                        float x1 = event.getX(0);
                        float y1 = event.getY(0);
                        float x2 = event.getX(1);
                        float y2 = event.getY(1);

                        if (twoFingerMode == TWO_FINGER_UNDECIDED) {
                            twoFingerMode = classifyTwoFinger(x1, y1, x2, y2);
                            if (twoFingerMode == TWO_FINGER_NOT_SCROLL) {
                                // A pinch, or one finger travelling against a resting
                                // one. Hand the gesture back to the caller.
                                return declineGesture();
                            }
                        }

                        if (twoFingerMode == TWO_FINGER_SCROLL) {
                            float avgDx = ((x1 - lastX1) + (x2 - lastX2)) / 2;
                            float avgDy = ((y1 - lastY1) + (y2 - lastY2)) / 2;

                            if (Math.abs(avgDx) > 1 || Math.abs(avgDy) > 1) {
                                isTwoFingerTapCandidate = false;
                                float scale = scrollReversed
                                        ? -scrollSpeed : scrollSpeed;
                                if (Math.abs(avgDy)
                                        > Math.abs(avgDx) * AXIS_DOMINANCE_RATIO) {
                                    sendScroll(0, -avgDy * scale);
                                }
                                if (Math.abs(avgDx)
                                        > Math.abs(avgDy) * AXIS_DOMINANCE_RATIO) {
                                    sendScroll(1, avgDx * scale);
                                }
                                lastX1 = x1;
                                lastY1 = y1;
                                lastX2 = x2;
                                lastY2 = y2;
                            }
                        } else {
                            // Still ambiguous, so emit nothing yet, but keep the
                            // anchors current: the first scroll delta after the
                            // decision must not be the whole accumulated drift.
                            lastX1 = x1;
                            lastY1 = y1;
                            lastX2 = x2;
                            lastY2 = y2;
                        }
                    }
                }
                break;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                int remaining = pointerCount - 1;
                if (remaining == 1) {
                    isMultiFinger = false;
                    isSingleTapCandidate = false;
                    isLongPressPossible = false;
                    int idx = (event.getActionIndex() == 0) ? 1 : 0;
                    lastX1 = event.getX(idx);
                    lastY1 = event.getY(idx);
                    startX1 = lastX1;
                    startY1 = lastY1;
                    downTime1 = event.getEventTime();
                    hasLongPressed = false;
                    currentState = STATE_ONE_FINGER;
                    resetSmoothing();
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                long duration = event.getEventTime() - downTime1;
                boolean isQuickTap = duration < 300;

                if (isDraggingActive) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (isTwoFingerTapCandidate && isQuickTap) {
                    sendButton(0x111, true);
                    sendButton(0x111, false);
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (currentState == STATE_ONE_FINGER && isSingleTapCandidate && isQuickTap) {
                    long gap = event.getEventTime() - lastTapTime;
                    float dist = (float) Math.hypot(lastX1 - lastTapX, lastY1 - lastTapY);
                    if (synthesizeDoubleTap && gap < 300 && dist < touchSlop
                            && !isDoubleTapPending) {
                        isDoubleTapPending = true;
                        sendButton(0x110, true);
                        sendButton(0x110, false);
                        sendButton(0x110, true);
                        sendButton(0x110, false);
                        isDoubleTapPending = false;
                        lastTapTime = 0;
                    } else {
                        sendButton(0x110, true);
                        sendButton(0x110, false);
                        lastTapTime = event.getEventTime();
                        lastTapX = lastX1;
                        lastTapY = lastY1;
                        isDoubleTapPending = false;
                    }
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                if (isDraggingActive) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
        }
        return true;
    }

    private void resetTouchpadState() {
        currentState = STATE_IDLE;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isDoubleTapPending = false;
        hasLongPressed = false;
        isDraggingActive = false;
        isLongPressPossible = false;
        isMultiFinger = false;
        twoFingerMode = TWO_FINGER_UNDECIDED;
        // Cleared here rather than in declineGesture: the latch has to outlive the
        // events that follow it and only lifts when the gesture itself ends.
        gestureUnhandled = false;
    }

    private void resetSmoothing() {
        smoothedDx = 0f;
        smoothedDy = 0f;
        accumulatedX = 0f;
        accumulatedY = 0f;
        smoothInitialized = false;
    }

    private float[] applySmoothing(float rawDx, float rawDy) {
        float deadDx = Math.abs(rawDx) < DEAD_ZONE ? 0f : rawDx;
        float deadDy = Math.abs(rawDy) < DEAD_ZONE ? 0f : rawDy;

        if (deadDx == 0f && deadDy == 0f) {
            return new float[]{0f, 0f};
        }

        if (!smoothInitialized) {
            smoothedDx = deadDx;
            smoothedDy = deadDy;
            smoothInitialized = true;
        } else {
            smoothedDx = SMOOTHING_FACTOR * deadDx + (1 - SMOOTHING_FACTOR) * smoothedDx;
            smoothedDy = SMOOTHING_FACTOR * deadDy + (1 - SMOOTHING_FACTOR) * smoothedDy;
        }

        // 累积阈值大幅降低，让移动更加连续
        accumulatedX += smoothedDx;
        accumulatedY += smoothedDy;

        float outX = 0f;
        float outY = 0f;
        if (Math.abs(accumulatedX) >= ACCUMULATED_THRESHOLD) {
            outX = accumulatedX;
            accumulatedX = 0f;
        }
        if (Math.abs(accumulatedY) >= ACCUMULATED_THRESHOLD) {
            outY = accumulatedY;
            accumulatedY = 0f;
        }
        return new float[]{outX, outY};
    }

    private float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }

    private void updateScreenSize() {
        Point size = new Point();
        WindowManager wm = context.getSystemService(WindowManager.class);
        if (wm != null) {
            wm.getDefaultDisplay().getSize(size);
            screenWidth = size.x;
            screenHeight = size.y;
        }
    }
}
