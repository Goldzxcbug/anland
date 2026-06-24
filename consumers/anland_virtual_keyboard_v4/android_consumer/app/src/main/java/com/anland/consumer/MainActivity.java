package com.anland.consumer;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Point;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.nio.charset.StandardCharsets;

public class MainActivity extends Activity implements SurfaceHolder.Callback {

    private static final String TAG = "Anland";

    // ========== 主线原有成员 ==========
    private SurfaceView surfaceView;
    private boolean surfaceReady = false;
    private String mLastSentClip = null;
    private boolean mClipListening = false;
    private static final String PREFS_NAME = "anland_settings";
    private static final String KEY_BOUND_KEYCODE = "bound_keycode";
    private EditText hiddenInput;
    private InputMethodManager imm;
    private int mImeInset = -1;

    // evdev keycodes (主线原有)
    private static final int EVDEV_ESC = 1;
    private static final int EVDEV_BACKSPACE = 14;
    private static final int EVDEV_TAB = 15;
    private static final int EVDEV_ENTER = 28;
    private static final int EVDEV_UP = 103;
    private static final int EVDEV_LEFT = 105;
    private static final int EVDEV_RIGHT = 106;
    private static final int EVDEV_DOWN = 108;
    private static final int EVDEV_DELETE = 111;

    // ========== 新增：你的修改版功能 ==========
    // 灵敏度持久化
    private static final String PREFS_SENSITIVITY = "anland_prefs";
    private static final String KEY_SENSITIVITY = "sensitivity";
    private SharedPreferences sensitivityPrefs;
    private float sensitivity = 1.0f;

    // 虚拟键盘
    private VirtualKeyboardView keyboardView;
    private FrameLayout rootLayout;          // 改为成员变量，便于访问

    // 模式开关
    private boolean isTouchpadMode = true;
    private boolean isPortrait = true;
    private float mouseX = 0;
    private float mouseY = 0;
    private int screenWidth = 1920;
    private int screenHeight = 1080;

    // Surface 初始化防抖
    private final Handler surfaceInitHandler = new Handler(Looper.getMainLooper());
    private Runnable surfaceInitRunnable = null;
    private static final long SURFACE_INIT_DELAY = 120;

    // 音量键长按调节
    private long volumeUpLastAdjustTime = 0;
    private boolean volumeUpHasAdjusted = false;
    private long volumeDownLastAdjustTime = 0;
    private boolean volumeDownHasAdjusted = false;
    private static final long LONG_PRESS_THRESHOLD = 500;

    // 触摸板手势状态机
    private static final int STATE_IDLE = 0;
    private static final int STATE_ONE_FINGER = 1;
    private static final int STATE_TWO_FINGER = 2;
    private static final int STATE_DRAGGING = 3;
    private int currentState = STATE_IDLE;

    private float lastX1, lastY1;
    private float startX1, startY1;
    private float lastX2, lastY2;
    private long downTime1;
    private float touchSlop;

    private boolean isSingleTapCandidate = false;
    private boolean isTwoFingerTapCandidate = false;
    private boolean isThreeFingerTapCandidate = false;
    private boolean isDraggingActive = false;

    private long lastTapTime = 0;
    private float lastTapX, lastTapY;
    private boolean isDoubleTapPending = false;

    private static final long TOUCH_LONG_PRESS_TIMEOUT = 500;
    private boolean hasLongPressed = false;
    private boolean isLongPressPossible = false;
    private boolean isMultiFinger = false;

    // 加速度参数
    private static final float BASE_SCALE = 2.0f;
    private static final float SCALE_STEP = 0.12f;
    private static final float MAX_SCALE = 6.0f;

    // 鼠标按钮常量（与native一致）
    private static final int BTN_LEFT = 0x110;
    private static final int BTN_RIGHT = 0x111;
    private static final int BTN_MIDDLE = 0x112;

    // ========== Native 方法（主线原有） ==========
    static {
        System.loadLibrary("anland_consumer");
    }

    private native void nativeStart(Surface surface);
    private native void nativeStop();
    private native void nativeSendTouch(int action, float x, float y, int pointerId);
    private native void nativeSendTouchFrame();
    private native void nativeSendKey(int action, int keycode);
    private native void nativeSendMouseMotion(float x, float y, float dx, float dy);
    private native void nativeSendMouseButton(int button, boolean pressed);
    private native void nativeSendMouseScroll(int axis, float value);
    private native void nativeSetRefreshRate(float hz);
    private native void nativeSendClipboard(byte[] data);
    private native void nativeSendTextInput(byte[] data);

    // ========== 主线：供 native 调用的 Java 方法（剪贴板） ==========
    public void nativeSetClipboardText(String text) {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm != null) {
            mLastSentClip = text;
            cm.setPrimaryClip(ClipData.newPlainText("anland", text));
        }
    }

    public void nativeClipboardSync() {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence text = clip.getItemAt(0).getText();
            if (text != null) {
                mLastSentClip = text.toString();
                nativeSendClipboard(text.toString().getBytes(StandardCharsets.UTF_8));
            }
        }
    }

    public void nativeClipListening(boolean enable) {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm == null) return;
        if (enable) {
            if (mClipListening) return;
            cm.addPrimaryClipChangedListener(clipListener);
            mClipListening = true;
        } else {
            if (!mClipListening) return;
            cm.removePrimaryClipChangedListener(clipListener);
            mClipListening = false;
        }
    }

    private final ClipboardManager.OnPrimaryClipChangedListener clipListener =
            () -> pushClipboard();

    private void pushClipboard() {
        ClipboardManager cm = getSystemService(ClipboardManager.class);
        if (cm == null) return;
        ClipData clip = cm.getPrimaryClip();
        if (clip != null && clip.getItemCount() > 0) {
            CharSequence text = clip.getItemAt(0).getText();
            if (text != null) {
                String clipText = text.toString();
                if (!clipText.equals(mLastSentClip)) {
                    mLastSentClip = clipText;
                    nativeSendClipboard(clipText.getBytes(StandardCharsets.UTF_8));
                }
            }
        }
    }

    // ========== 主线：刷新率监听 ==========
    private final DisplayManager.DisplayListener displayListener =
            new DisplayManager.DisplayListener() {
                @Override public void onDisplayAdded(int displayId) {}
                @Override public void onDisplayRemoved(int displayId) {}
                @Override public void onDisplayChanged(int displayId) {
                    Display d = getDisplay();
                    if (d != null && d.getDisplayId() == displayId)
                        pushRefreshRate();
                }
            };

    private void pushRefreshRate() {
        Display d = getDisplay();
        if (d != null) {
            float rate = d.getRefreshRate();
            if (rate > 0) nativeSetRefreshRate(rate);
        }
    }

    // ========== 生命周期 ==========
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // ---- 主线原有初始化 ----
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);
        // 新增：允许布局延伸到系统栏（来自你的修改）
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS);

        // 新增：读取灵敏度
        sensitivityPrefs = getSharedPreferences(PREFS_SENSITIVITY, MODE_PRIVATE);
        sensitivity = sensitivityPrefs.getFloat(KEY_SENSITIVITY, 1.0f);
        sensitivity = Math.max(0.5f, Math.min(5.0f, sensitivity));

        // 新增：获取触摸slop
        touchSlop = ViewConfiguration.get(this).getScaledTouchSlop();
        updateScreenSize();
        isPortrait = (getResources().getConfiguration().orientation == Configuration.ORIENTATION_PORTRAIT);

        // ---- 主线：初始化视图 ----
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        surfaceView.setClickable(false);
        surfaceView.setFocusable(false);

        // 主线：hiddenInput
        initHiddenInput();

        // 新建根布局（改为 FrameLayout，以便容纳 surfaceView, hiddenInput, keyboardView）
        rootLayout = new FrameLayout(this);
        // 新增：允许子视图超出边界（虚拟键盘拖动）
        rootLayout.setClipChildren(false);
        rootLayout.setClipToPadding(false);
        rootLayout.setFitsSystemWindows(false);

        // 添加 surfaceView
        rootLayout.addView(surfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        // 添加 hiddenInput（主线原有，1x1）
        rootLayout.addView(hiddenInput, new FrameLayout.LayoutParams(1, 1));

        // 新增：添加虚拟键盘（默认隐藏）
        keyboardView = new VirtualKeyboardView(this);
        keyboardView.setVisibility(View.GONE);
        keyboardView.setOnKeyEventListener(new VirtualKeyboardView.OnKeyEventListener() {
            @Override
            public void onKeyDown(int scanCode) {
                nativeSendKey(0, scanCode);
            }
            @Override
            public void onKeyUp(int scanCode) {
                nativeSendKey(1, scanCode);
            }
        });
        rootLayout.addView(keyboardView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT));

        setContentView(rootLayout);

        // 主线：全屏 + 光标隐藏
        setupFullscreen();
        setupCursorHiding();

        // 新增：确保 DecorView 不裁剪
        View decorView = getWindow().getDecorView();
        if (decorView instanceof ViewGroup) {
            ((ViewGroup) decorView).setClipChildren(false);
            ((ViewGroup) decorView).setClipToPadding(false);
        }

        // 新增：键盘初始位置
        keyboardView.post(() -> {
            try {
                keyboardView.setInitialPosition();
            } catch (Exception e) {
                Log.e(TAG, "Initial position error", e);
            }
        });

        // 主线：窗口 inset 监听（用于 IME 调整 surface 大小）
        rootLayout.setOnApplyWindowInsetsListener((v, insets) -> {
            if (!insets.isVisible(WindowInsets.Type.ime()))
                releaseHiddenInput();
            applyImeInset(insets);
            return v.onApplyWindowInsets(insets);
        });

        // 新增：鼠标初始位置在屏幕中心
        mouseX = screenWidth / 2f;
        mouseY = screenHeight / 2f;
    }

    @Override
    protected void onResume() {
        super.onResume();
        setupFullscreen();
        updateScreenSize();

        DisplayManager dm = getSystemService(DisplayManager.class);
        if (dm != null)
            dm.registerDisplayListener(displayListener, null);

        // 主线：如果 surface 已就绪则启动
        if (surfaceReady) {
            nativeStop();
            nativeStart(surfaceView.getHolder().getSurface());
            pushRefreshRate();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        DisplayManager dm = getSystemService(DisplayManager.class);
        if (dm != null)
            dm.unregisterDisplayListener(displayListener);

        // 新增：取消待执行的 surface 初始化
        if (surfaceInitRunnable != null) {
            surfaceInitHandler.removeCallbacks(surfaceInitRunnable);
            surfaceInitRunnable = null;
        }
        nativeStop();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            pushClipboard();
        }
    }

    // ========== 横竖屏切换增强（新增） ==========
    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        isPortrait = (newConfig.orientation == Configuration.ORIENTATION_PORTRAIT);
        updateScreenSize();
        mouseX = clamp(mouseX, 0, screenWidth);
        mouseY = clamp(mouseY, 0, screenHeight);
        pushRefreshRate();

        if (keyboardView != null && keyboardView.getVisibility() == View.VISIBLE) {
            keyboardView.post(() -> {
                try {
                    keyboardView.setInitialPosition();
                } catch (Exception e) {
                    Log.e(TAG, "setInitialPosition error in onConfigurationChanged", e);
                }
            });
        }
        Log.d(TAG, "onConfigurationChanged: portrait=" + isPortrait + ", screen " + screenWidth + "x" + screenHeight);
        // 不主动重建 Surface，由 surfaceChanged 防抖处理
    }

    // ========== SurfaceHolder.Callback ==========
    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // 主线：空实现
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
        surfaceReady = true;
        updateScreenSize();

        // 新增：防抖，取消之前的延迟任务
        if (surfaceInitRunnable != null) {
            surfaceInitHandler.removeCallbacks(surfaceInitRunnable);
            surfaceInitRunnable = null;
        }

        // 新增：延迟初始化，避免旋转时多次重建
        surfaceInitRunnable = () -> {
            surfaceInitRunnable = null;
            Surface surface = holder.getSurface();
            if (surface != null && surface.isValid()) {
                nativeStop();
                nativeStart(surface);
                pushRefreshRate();
            } else {
                Log.w(TAG, "Surface invalid, retry in 100ms");
                surfaceInitHandler.postDelayed(() -> {
                    if (surfaceReady) {
                        nativeStop();
                        nativeStart(holder.getSurface());
                        pushRefreshRate();
                    }
                }, 100);
            }
        };
        surfaceInitHandler.postDelayed(surfaceInitRunnable, SURFACE_INIT_DELAY);

        // 新增：键盘位置更新
        if (keyboardView != null && keyboardView.getVisibility() == View.VISIBLE) {
            keyboardView.post(() -> {
                try {
                    keyboardView.setInitialPosition();
                } catch (Exception e) {
                    Log.e(TAG, "setInitialPosition error in surfaceChanged", e);
                }
            });
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        if (surfaceInitRunnable != null) {
            surfaceInitHandler.removeCallbacks(surfaceInitRunnable);
            surfaceInitRunnable = null;
        }
        nativeStop();
    }

    // ========== 辅助方法（主线 + 新增） ==========
    private void updateScreenSize() {
        Point size = new Point();
        getWindowManager().getDefaultDisplay().getSize(size);
        screenWidth = size.x;
        screenHeight = size.y;
    }

    private void setupFullscreen() {
        WindowInsetsController ctrl = getWindow().getInsetsController();
        if (ctrl != null) {
            ctrl.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
            ctrl.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        // 新增：兼容旧 API
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        );
        getWindow().getAttributes().layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
    }

    private void setupCursorHiding() {
        surfaceView.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL));
    }

    // ========== 主线：IME 相关 ==========
    private void initHiddenInput() {
        imm = getSystemService(InputMethodManager.class);

        hiddenInput = new EditText(this) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                super.onCreateInputConnection(outAttrs);
                return new ForwardingInputConnection(this);
            }
        };
        hiddenInput.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        hiddenInput.setCursorVisible(false);
        hiddenInput.setAlpha(0f);
        hiddenInput.setEnabled(false);
        hiddenInput.setFocusable(false);
        hiddenInput.setFocusableInTouchMode(false);
        hiddenInput.setClickable(false);
        hiddenInput.setLongClickable(false);
        hiddenInput.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN
                | EditorInfo.IME_FLAG_NO_ENTER_ACTION);
        hiddenInput.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | android.text.InputType.TYPE_TEXT_VARIATION_NORMAL);
    }

    private void sendText(String text) {
        if (text.isEmpty()) return;
        nativeSendTextInput(text.getBytes(StandardCharsets.UTF_8));
    }

    private void tapKey(int evdevCode) {
        nativeSendKey(0, evdevCode);
        nativeSendKey(1, evdevCode);
    }

    private static int toEvdevKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return EVDEV_ENTER;
            case KeyEvent.KEYCODE_DEL:          return EVDEV_BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL:  return EVDEV_DELETE;
            case KeyEvent.KEYCODE_TAB:          return EVDEV_TAB;
            case KeyEvent.KEYCODE_ESCAPE:       return EVDEV_ESC;
            case KeyEvent.KEYCODE_DPAD_LEFT:    return EVDEV_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:   return EVDEV_RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP:      return EVDEV_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:    return EVDEV_DOWN;
            default:                            return 0;
        }
    }

    private final class ForwardingInputConnection extends BaseInputConnection {
        private final StringBuilder composing = new StringBuilder();

        ForwardingInputConnection(View target) {
            super(target, false);
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            final String s = text == null ? "" : text.toString();
            if (composing.length() > 0 && composing.toString().equals(s)) {
                composing.setLength(0);
                return true;
            }
            eraseComposing();
            sendText(s);
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            replaceComposing(text == null ? "" : text.toString());
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing.setLength(0);
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            for (int i = 0; i < beforeLength; i++) tapKey(EVDEV_BACKSPACE);
            for (int i = 0; i < afterLength; i++) tapKey(EVDEV_DELETE);
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            final int evdev = toEvdevKey(event.getKeyCode());
            if (evdev == 0) return super.sendKeyEvent(event);
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                nativeSendKey(0, evdev);
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                nativeSendKey(1, evdev);
            }
            return true;
        }

        private void replaceComposing(String next) {
            final String prev = composing.toString();
            int prefix = 0;
            final int min = Math.min(prev.length(), next.length());
            while (prefix < min && prev.charAt(prefix) == next.charAt(prefix)) {
                prefix++;
            }
            if (prefix > 0 && Character.isHighSurrogate(prev.charAt(prefix - 1))) {
                prefix--;
            }
            final int erase = prev.codePointCount(prefix, prev.length());
            for (int i = 0; i < erase; i++) tapKey(EVDEV_BACKSPACE);
            if (prefix < next.length()) {
                sendText(next.substring(prefix));
            }
            composing.setLength(0);
            composing.append(next);
        }

        private void eraseComposing() {
            final int erase = composing.codePointCount(0, composing.length());
            for (int i = 0; i < erase; i++) tapKey(EVDEV_BACKSPACE);
            composing.setLength(0);
        }
    }

    private void applyImeInset(WindowInsets insets) {
        int imeBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
        if (imeBottom == mImeInset) return;
        mImeInset = imeBottom;
        FrameLayout.LayoutParams lp =
                (FrameLayout.LayoutParams) surfaceView.getLayoutParams();
        lp.bottomMargin = imeBottom;
        surfaceView.setLayoutParams(lp);
    }

    private boolean isImeVisible() {
        WindowInsets insets = getWindow().getDecorView().getRootWindowInsets();
        return insets != null && insets.isVisible(WindowInsets.Type.ime());
    }

    private void releaseHiddenInput() {
        if (!hiddenInput.isEnabled()) return;
        hiddenInput.clearFocus();
        hiddenInput.setFocusable(false);
        hiddenInput.setEnabled(false);
    }

    private void toggleKeyboard() {
        if (imm == null) imm = getSystemService(InputMethodManager.class);
        if (imm == null) return;
        if (isImeVisible()) {
            imm.hideSoftInputFromWindow(hiddenInput.getWindowToken(), 0);
            releaseHiddenInput();
        } else {
            hiddenInput.setEnabled(true);
            hiddenInput.setFocusable(true);
            hiddenInput.setFocusableInTouchMode(true);
            hiddenInput.requestFocus();
            imm.showSoftInput(hiddenInput, InputMethodManager.SHOW_IMPLICIT);
        }
    }

    // ========== 按键事件（主线 + 新增音量键逻辑） ==========
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        // ---- 新增：音量键处理 ----
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP) {
            if (event.getRepeatCount() == 0) {
                volumeUpLastAdjustTime = event.getEventTime();
                volumeUpHasAdjusted = false;
            } else {
                long now = event.getEventTime();
                if (now - volumeUpLastAdjustTime >= LONG_PRESS_THRESHOLD) {
                    volumeUpHasAdjusted = true;
                    sensitivity = Math.min(5.0f, sensitivity + 0.5f);
                    sensitivityPrefs.edit().putFloat(KEY_SENSITIVITY, sensitivity).apply();
                    volumeUpLastAdjustTime = now;
                }
            }
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            if (event.getRepeatCount() == 0) {
                volumeDownLastAdjustTime = event.getEventTime();
                volumeDownHasAdjusted = false;
            } else {
                long now = event.getEventTime();
                if (now - volumeDownLastAdjustTime >= LONG_PRESS_THRESHOLD) {
                    volumeDownHasAdjusted = true;
                    sensitivity = Math.max(0.5f, sensitivity - 0.5f);
                    sensitivityPrefs.edit().putFloat(KEY_SENSITIVITY, sensitivity).apply();
                    volumeDownLastAdjustTime = now;
                }
            }
            return true;
        }

        // ---- 主线：检查 SettingsActivity 绑定的热键（唤出系统 IME） ----
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        int boundKeycode = prefs.getInt(KEY_BOUND_KEYCODE, -1);
        if (boundKeycode != -1 && keyCode == boundKeycode) {
            toggleKeyboard();
            return true;
        }

        // ---- 主线：其他按键（物理键盘） ----
        if (event.getRepeatCount() == 0) {
            int scanCode = event.getScanCode();
            if (scanCode != 0) {
                nativeSendKey(0, scanCode);
                return true;
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        // ---- 新增：音量键抬起反馈 ----
        if (keyCode == KeyEvent.KEYCODE_VOLUME_UP) {
            if (volumeUpHasAdjusted) {
                Toast.makeText(this, "灵敏度: " + String.format("%.1f", sensitivity), Toast.LENGTH_SHORT).show();
            } else {
                isTouchpadMode = !isTouchpadMode;
                Toast.makeText(this, isTouchpadMode ? "触摸板模式（相对移动）" : "普通触摸模式（绝对定位）", Toast.LENGTH_SHORT).show();
                resetTouchpadState();
            }
            volumeUpHasAdjusted = false;
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            if (volumeDownHasAdjusted) {
                Toast.makeText(this, "灵敏度: " + String.format("%.1f", sensitivity), Toast.LENGTH_SHORT).show();
            } else {
                if (keyboardView != null) {
                    boolean show = keyboardView.getVisibility() == View.GONE;
                    keyboardView.setVisibility(show ? View.VISIBLE : View.GONE);
                    if (show) {
                        keyboardView.post(() -> {
                            try {
                                keyboardView.setInitialPosition();
                                keyboardView.bringToFront();
                            } catch (Exception e) {
                                Log.e(TAG, "Error showing keyboard", e);
                            }
                        });
                    }
                }
            }
            volumeDownHasAdjusted = false;
            return true;
        }

        // ---- 主线：物理键盘按键抬起 ----
        int scanCode = event.getScanCode();
        if (scanCode != 0) {
            nativeSendKey(1, scanCode);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    // ========== 触摸事件（主线 + 触摸板模式） ==========
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (isTouchpadMode) {
            return handleTouchpadGesture(event);
        } else {
            return handleTouchEventWithMouseFollow(event);
        }
    }

    // ----- 绝对定位模式（保留主线触摸转发，并额外更新鼠标位置） -----
    private boolean handleTouchEventWithMouseFollow(MotionEvent event) {
        int action = event.getActionMasked();

        // 横屏时移动鼠标跟随触摸点
        if (!isPortrait) {
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN ||
                    action == MotionEvent.ACTION_MOVE) {
                if (event.getPointerCount() > 0) {
                    float fx = event.getX(0);
                    float fy = event.getY(0);
                    mouseX = clamp(fx, 0, screenWidth);
                    mouseY = clamp(fy, 0, screenHeight);
                    nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
                }
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
            }
        } else {
            // 竖屏只记录位置，不发送移动
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN ||
                    action == MotionEvent.ACTION_MOVE) {
                if (event.getPointerCount() > 0) {
                    mouseX = clamp(event.getX(0), 0, screenWidth);
                    mouseY = clamp(event.getY(0), 0, screenHeight);
                }
            }
        }

        // 调用主线的触摸转发（绝对坐标）
        return handleTouchEvent(event);
    }

    // ---- 主线原有的触摸转发（绝对坐标） ----
    private boolean handleTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int idx = event.getActionIndex();
        int pid = event.getPointerId(idx);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                nativeSendTouch(0, event.getX(idx), event.getY(idx), pid);
                nativeSendTouchFrame();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                nativeSendTouch(1, event.getX(idx), event.getY(idx), pid);
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

    // ========================================================================
    // 新增：触摸板手势完整实现
    // ========================================================================
    private boolean handleTouchpadGesture(MotionEvent event) {
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
                isSingleTapCandidate = true;
                isTwoFingerTapCandidate = false;
                isThreeFingerTapCandidate = false;
                isDoubleTapPending = false;
                isMultiFinger = false;
                currentState = STATE_ONE_FINGER;
                break;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                isMultiFinger = true;
                isSingleTapCandidate = false;
                isDoubleTapPending = false;
                isLongPressPossible = false;
                if (currentState == STATE_DRAGGING) {
                    nativeSendMouseButton(BTN_LEFT, false);
                    isDraggingActive = false;
                }
                if (pointerCount == 2) {
                    currentState = STATE_TWO_FINGER;
                    isTwoFingerTapCandidate = true;
                    isThreeFingerTapCandidate = false;
                    lastX1 = event.getX(0);
                    lastY1 = event.getY(0);
                    lastX2 = event.getX(1);
                    lastY2 = event.getY(1);
                } else if (pointerCount == 3) {
                    currentState = STATE_IDLE;
                    isTwoFingerTapCandidate = false;
                    isThreeFingerTapCandidate = true;
                    lastX1 = event.getX(0);
                    lastY1 = event.getY(0);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (pointerCount == 1 && !isMultiFinger) {
                    float x = event.getX();
                    float y = event.getY();
                    float dx = x - lastX1;
                    float dy = y - lastY1;
                    float dist = (float) Math.hypot(x - startX1, y - startY1);
                    if (dist > touchSlop) {
                        isLongPressPossible = false;
                        isSingleTapCandidate = false;
                    }
                    if (isLongPressPossible && !hasLongPressed &&
                            (event.getEventTime() - downTime1) >= TOUCH_LONG_PRESS_TIMEOUT) {
                        hasLongPressed = true;
                        currentState = STATE_DRAGGING;
                        isDraggingActive = true;
                        nativeSendMouseButton(BTN_LEFT, true);
                        mouseX = clamp(mouseX, 0, screenWidth);
                        mouseY = clamp(mouseY, 0, screenHeight);
                        nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
                        break;
                    }
                    if (currentState != STATE_DRAGGING && (Math.abs(dx) > 1 || Math.abs(dy) > 1)) {
                        float scale = getAcceleration(dx, dy) * sensitivity;
                        mouseX = clamp(mouseX + dx * scale, 0, screenWidth);
                        mouseY = clamp(mouseY + dy * scale, 0, screenHeight);
                        nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
                        lastX1 = x;
                        lastY1 = y;
                    } else if (currentState == STATE_DRAGGING) {
                        if (Math.abs(dx) > 1 || Math.abs(dy) > 1) {
                            float scale = getAcceleration(dx, dy) * sensitivity;
                            mouseX = clamp(mouseX + dx * scale, 0, screenWidth);
                            mouseY = clamp(mouseY + dy * scale, 0, screenHeight);
                            nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
                            lastX1 = x;
                            lastY1 = y;
                        }
                    }
                } else if (pointerCount == 2) {
                    if (currentState == STATE_TWO_FINGER) {
                        float x1 = event.getX(0);
                        float y1 = event.getY(0);
                        float x2 = event.getX(1);
                        float y2 = event.getY(1);
                        float avgDx = ((x1 - lastX1) + (x2 - lastX2)) / 2;
                        float avgDy = ((y1 - lastY1) + (y2 - lastY2)) / 2;
                        if (Math.abs(avgDx) > 1 || Math.abs(avgDy) > 1) {
                            isTwoFingerTapCandidate = false;
                            if (Math.abs(avgDy) > Math.abs(avgDx) * 0.5) {
                                nativeSendMouseScroll(0, -avgDy);
                            }
                            if (Math.abs(avgDx) > Math.abs(avgDy) * 0.5) {
                                nativeSendMouseScroll(1, avgDx);
                            }
                            lastX1 = x1;
                            lastY1 = y1;
                            lastX2 = x2;
                            lastY2 = y2;
                        }
                    }
                } else if (pointerCount >= 3) {
                    if (isThreeFingerTapCandidate) {
                        float x1 = event.getX(0);
                        float y1 = event.getY(0);
                        float dist = (float) Math.hypot(x1 - lastX1, y1 - lastY1);
                        if (dist > touchSlop) {
                            isThreeFingerTapCandidate = false;
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
                    isDoubleTapPending = false;
                    isLongPressPossible = false;
                    int idx = (event.getActionIndex() == 0) ? 1 : 0;
                    lastX1 = event.getX(idx);
                    lastY1 = event.getY(idx);
                    startX1 = lastX1;
                    startY1 = lastY1;
                    downTime1 = event.getEventTime();
                    hasLongPressed = false;
                    currentState = STATE_ONE_FINGER;
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                long duration = event.getEventTime() - downTime1;
                boolean isQuickTap = duration < 300;

                if (isDraggingActive) {
                    nativeSendMouseButton(BTN_LEFT, false);
                    isDraggingActive = false;
                    resetTouchpadState();
                    return true;
                }
                if (isThreeFingerTapCandidate && isQuickTap) {
                    nativeSendMouseButton(BTN_MIDDLE, true);
                    nativeSendMouseButton(BTN_MIDDLE, false);
                    resetTouchpadState();
                    return true;
                }
                if (isTwoFingerTapCandidate && isQuickTap) {
                    nativeSendMouseButton(BTN_RIGHT, true);
                    nativeSendMouseButton(BTN_RIGHT, false);
                    resetTouchpadState();
                    return true;
                }
                if (currentState == STATE_ONE_FINGER && isSingleTapCandidate && isQuickTap) {
                    long gap = event.getEventTime() - lastTapTime;
                    float dist = (float) Math.hypot(lastX1 - lastTapX, lastY1 - lastTapY);
                    if (gap < 300 && dist < touchSlop && !isDoubleTapPending) {
                        isDoubleTapPending = true;
                        nativeSendMouseButton(BTN_LEFT, true);
                        nativeSendMouseButton(BTN_LEFT, false);
                        nativeSendMouseButton(BTN_LEFT, true);
                        nativeSendMouseButton(BTN_LEFT, false);
                        isDoubleTapPending = false;
                        lastTapTime = 0;
                    } else {
                        nativeSendMouseButton(BTN_LEFT, true);
                        nativeSendMouseButton(BTN_LEFT, false);
                        lastTapTime = event.getEventTime();
                        lastTapX = lastX1;
                        lastTapY = lastY1;
                        isDoubleTapPending = false;
                    }
                    resetTouchpadState();
                    return true;
                }
                resetTouchpadState();
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                if (isDraggingActive) {
                    nativeSendMouseButton(BTN_LEFT, false);
                    isDraggingActive = false;
                }
                resetTouchpadState();
                break;
            }
        }
        return true;
    }

    private float getAcceleration(float dx, float dy) {
        float distance = (float) Math.hypot(dx, dy);
        if (distance < 0.5f) return BASE_SCALE;
        float scale = BASE_SCALE + distance * SCALE_STEP;
        return Math.min(scale, MAX_SCALE);
    }

    private void resetTouchpadState() {
        currentState = STATE_IDLE;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isThreeFingerTapCandidate = false;
        isDoubleTapPending = false;
        hasLongPressed = false;
        isDraggingActive = false;
        isLongPressPossible = false;
        isMultiFinger = false;
        lastX1 = lastY1 = 0;
        lastX2 = lastY2 = 0;
    }

    private float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }

    // ========== 主线：鼠标/外设事件（不变） ==========
    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if (isMouseEvent(event)) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_HOVER_MOVE) {
                float x = event.getX();
                float y = event.getY();
                mouseX = clamp(x, 0, screenWidth);
                mouseY = clamp(y, 0, screenHeight);
                nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);
                return true;
            }
            if (action == MotionEvent.ACTION_SCROLL) {
                float v = event.getAxisValue(MotionEvent.AXIS_VSCROLL);
                float h = event.getAxisValue(MotionEvent.AXIS_HSCROLL);
                if (v != 0) nativeSendMouseScroll(0, -v * 10);
                if (h != 0) nativeSendMouseScroll(1, h * 10);
                return true;
            }
        }
        return super.onGenericMotionEvent(event);
    }

    // ---- 主线：鼠标判断辅助 ----
    private int savedBS = 0;
    private static final int[][] BUTTON_MAP = {
            {MotionEvent.BUTTON_PRIMARY,   0x110},
            {MotionEvent.BUTTON_SECONDARY, 0x111},
            {MotionEvent.BUTTON_TERTIARY,  0x112},
            {MotionEvent.BUTTON_BACK,      0x113},
            {MotionEvent.BUTTON_FORWARD,   0x114},
    };

    private boolean isMouseEvent(MotionEvent event) {
        int source = event.getSource();
        if ((source & InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN)
            return false;
        if ((source & InputDevice.SOURCE_MOUSE) != InputDevice.SOURCE_MOUSE)
            return false;
        int toolType = event.getToolType(event.getActionIndex());
        return toolType == MotionEvent.TOOL_TYPE_MOUSE
                || toolType == MotionEvent.TOOL_TYPE_FINGER;
    }

    private boolean handleMouseEvent(MotionEvent event) {
        float x = event.getX();
        float y = event.getY();
        mouseX = clamp(x, 0, screenWidth);
        mouseY = clamp(y, 0, screenHeight);
        nativeSendMouseMotion(mouseX, mouseY, 0f, 0f);

        int currentBS = event.getButtonState();
        for (int[] btn : BUTTON_MAP) {
            boolean wasDown = (savedBS & btn[0]) != 0;
            boolean isDown  = (currentBS & btn[0]) != 0;
            if (wasDown != isDown)
                nativeSendMouseButton(btn[1], isDown);
        }
        savedBS = currentBS;
        return true;
    }

    private boolean handleTouchpadScroll(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            float scrollX = event.getAxisValue(MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE);
            float scrollY = event.getAxisValue(MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE);
            if (scrollY != 0)
                nativeSendMouseScroll(0, scrollY);
            if (scrollX != 0)
                nativeSendMouseScroll(1, -scrollX);
        }
        return true;
    }
}