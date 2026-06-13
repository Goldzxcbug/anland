package com.virtual_drm.consumer;

import android.app.Activity;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;


public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "VirtualDRM";

    private SurfaceView surfaceView;
    private boolean surfaceReady = false;

    static {
        System.loadLibrary("virtual_drm_consumer");
    }

    private native void nativeStart(Surface surface);
    private native void nativeStop();
    private native void nativeSendTouch(int action, float x, float y, int pointerId);
    private native void nativeSendTouchFrame();
    private native void nativeSendKey(int action, int keycode);
    private native void nativeSendMouseMotion(float x, float y);
    private native void nativeSendMouseButton(int button, boolean pressed);
    private native void nativeSendMouseScroll(int axis, float value);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);

        surfaceView = new SurfaceView(this);
        setContentView(surfaceView);
        surfaceView.getHolder().addCallback(this);

        setupFullscreen();
        setupCursorHiding();
        enterLockTask();
    }

    private void setupFullscreen() {
        WindowInsetsController ctrl = getWindow().getInsetsController();
        if (ctrl != null) {
            ctrl.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
            ctrl.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        getWindow().getAttributes().layoutInDisplayCutoutMode =
            WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
    }

    private void enterLockTask() {
        try {
            startLockTask();
        } catch (Exception e) {
            Log.w(TAG, "lockTask failed", e);
        }
    }

    private void setupCursorHiding() {
        surfaceView.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL));
    }

    @Override
    protected void onResume() {
        super.onResume();
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        setupFullscreen();
        if (surfaceReady) {
            nativeStop();
            nativeStart(surfaceView.getHolder().getSurface());
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        nativeStop();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
        surfaceReady = true;
        nativeStop();
        nativeStart(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        nativeStop();
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (isMouseEvent(event))
            return handleMouseEvent(event);
        return handleTouchEvent(event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (isMouseEvent(event)) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_HOVER_MOVE) {
                nativeSendMouseMotion(event.getX(), event.getY());
                return true;
            }
            if (action == MotionEvent.ACTION_SCROLL) {
                float vScroll = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
                float hScroll = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
                if (vScroll != 0)
                    nativeSendMouseScroll(0, -vScroll * 10);
                if (hScroll != 0)
                    nativeSendMouseScroll(1, hScroll * 10);
                return true;
            }
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (event.getRepeatCount() > 0)
            return true;
        int scanCode = event.getScanCode();
        if (scanCode != 0) {
            nativeSendKey(0, scanCode);
            return true;
        }
        return true;
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        int scanCode = event.getScanCode();
        if (scanCode != 0) {
            nativeSendKey(1, scanCode);
            return true;
        }
        return true;
    }

    private boolean isMouseEvent(MotionEvent event) {
        return event.getSource() == InputDevice.SOURCE_MOUSE ||
               (event.getSource() & InputDevice.SOURCE_MOUSE) != 0;
    }

    private boolean handleMouseEvent(MotionEvent event) {
        int action = event.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_BUTTON_PRESS:
                nativeSendMouseMotion(event.getX(), event.getY());
                nativeSendMouseButton(linuxButton(event.getActionButton()), true);
                return true;
            case MotionEvent.ACTION_BUTTON_RELEASE:
                nativeSendMouseMotion(event.getX(), event.getY());
                nativeSendMouseButton(linuxButton(event.getActionButton()), false);
                return true;
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_MOVE:
                nativeSendMouseMotion(event.getX(), event.getY());
                return true;
        }
        return false;
    }

    private boolean handleTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerIdx = event.getActionIndex();
        int pointerId = event.getPointerId(pointerIdx);

        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                nativeSendTouch(0, event.getX(pointerIdx), event.getY(pointerIdx), pointerId);
                nativeSendTouchFrame();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                nativeSendTouch(1, event.getX(pointerIdx), event.getY(pointerIdx), pointerId);
                nativeSendTouchFrame();
                return true;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    nativeSendTouch(2, event.getX(i), event.getY(i), event.getPointerId(i));
                }
                nativeSendTouchFrame();
                return true;
            case MotionEvent.ACTION_CANCEL:
                for (int i = 0; i < event.getPointerCount(); i++) {
                    nativeSendTouch(1, event.getX(i), event.getY(i), event.getPointerId(i));
                }
                nativeSendTouchFrame();
                return true;
        }
        return false;
    }

    private int linuxButton(int androidButton) {
        switch (androidButton) {
            case MotionEvent.BUTTON_PRIMARY:   return 0x110; // BTN_LEFT
            case MotionEvent.BUTTON_SECONDARY: return 0x111; // BTN_RIGHT
            case MotionEvent.BUTTON_TERTIARY:  return 0x112; // BTN_MIDDLE
            case MotionEvent.BUTTON_BACK:      return 0x113; // BTN_SIDE
            case MotionEvent.BUTTON_FORWARD:   return 0x114; // BTN_EXTRA
            default:                           return 0x110;
        }
    }
}
