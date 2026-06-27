package com.anland.consumer;

import android.content.Context;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.widget.Button;
import android.widget.GridLayout;
import android.widget.PopupWindow;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Self-contained Termux-style bottom extra-keys bar. Built on the framework
 * GridLayout (no androidx). Replicates Termux's operational behaviour:
 *   - modifier keys (CTRL/ALT/SHIFT) toggle active on tap, lock on long-press;
 *   - directional / editing keys auto-repeat while held;
 *   - keys with a popup fire the popup key on swipe-up.
 *
 * Keys are sent out through a {@link Sender} bridge so this view never touches
 * MainActivity's native methods directly. Ordinary keys are sent as evdev
 * scancodes (down+up); text keys ('/', '-', '|') are sent as UTF-8 text.
 */
public class ExtraKeysBar extends GridLayout {

    /** Bridge to the host: native key/text injection, IME toggle and settings. */
    public interface Sender {
        void key(int action, int evdev);   // action: 0 = down, 1 = up
        void text(String s);
        void toggleKeyboard();
        void openSettings();
    }

    // evdev keycodes (linux/input-event-codes.h)
    private static final int EV_ESC = 1, EV_TAB = 15, EV_DELETE = 111;
    private static final int EV_HOME = 102, EV_END = 107, EV_PAGEUP = 104, EV_PAGEDOWN = 109;
    private static final int EV_UP = 103, EV_DOWN = 108, EV_LEFT = 105, EV_RIGHT = 106;
    private static final int EV_LEFTCTRL = 29, EV_LEFTSHIFT = 42, EV_LEFTALT = 56;

    private static final int TEXT_COLOR = 0xFFFFFFFF;
    private static final int ACTIVE_TEXT_COLOR = 0xFF80DEEA;
    private static final int BG_COLOR = 0x00000000;
    private static final int ACTIVE_BG_COLOR = 0xFF7F7F7F;

    private static final long REPEAT_DELAY_MS = 80;

    private static final int TYPE_KEY = 0;       // evdev down+up
    private static final int TYPE_TEXT = 1;      // text out
    private static final int TYPE_MODIFIER = 2;  // CTRL/ALT/SHIFT toggle
    private static final int TYPE_KEYBOARD = 3;  // toggle IME
    private static final int TYPE_SETTINGS = 4;  // open settings

    // Glyphs for icon-style keys.
    private static final String GLYPH_KEYBOARD = "⌨";  // ⌨
    private static final String GLYPH_SETTINGS = "⚙";  // ⚙

    private static final int ROWS = 2;
    private static final int COLS = 8;

    /** A single key definition. */
    private static final class Key {
        final String display;
        final int type;
        final int evdev;      // for TYPE_KEY / TYPE_MODIFIER
        final String text;    // for TYPE_TEXT
        final boolean repeatable;
        final Key popup;      // swipe-up secondary key (may be null)

        Key(String display, int type, int evdev, String text, boolean repeatable, Key popup) {
            this.display = display;
            this.type = type;
            this.evdev = evdev;
            this.text = text;
            this.repeatable = repeatable;
            this.popup = popup;
        }

        static Key key(String d, int evdev) { return new Key(d, TYPE_KEY, evdev, null, false, null); }
        static Key rep(String d, int evdev) { return new Key(d, TYPE_KEY, evdev, null, true, null); }
        static Key text(String d) { return new Key(d, TYPE_TEXT, 0, d, false, null); }
        static Key textPopup(String d, Key popup) { return new Key(d, TYPE_TEXT, 0, d, false, popup); }
        static Key mod(String d, int evdev) { return new Key(d, TYPE_MODIFIER, evdev, null, false, null); }
        static Key kbd(String d) { return new Key(d, TYPE_KEYBOARD, 0, null, false, null); }
        static Key settings(String d) { return new Key(d, TYPE_SETTINGS, 0, null, false, null); }
    }

    /** Mutable modifier state shared by name. */
    private static final class ModState {
        boolean active;
        boolean locked;
        final List<Button> buttons = new ArrayList<>();
    }

    private final Sender mSender;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final long mLongPressTimeout = ViewConfiguration.getLongPressTimeout();

    // modifier name ("CTRL"/"ALT"/"SHIFT") -> state
    private final Map<String, ModState> mModifiers = new LinkedHashMap<>();

    private PopupWindow mPopupWindow;

    public ExtraKeysBar(Context context, Sender sender) {
        super(context);
        mSender = sender;
        setColumnCount(COLS);
        setRowCount(ROWS);
        setBackgroundColor(0xCC000000);
        buildKeys();
    }

    /** Number of rows, used by the host to compute the bar height. */
    public static int rowCount() { return ROWS; }

    private Key[][] layout() {
        return new Key[][] {
            {
                Key.key("ESC", EV_ESC),
                Key.text("/"),
                Key.textPopup("-", Key.text("|")),
                Key.rep("HOME", EV_HOME),
                Key.rep("↑", EV_UP),
                Key.rep("END", EV_END),
                Key.rep("PGUP", EV_PAGEUP),
                Key.settings(GLYPH_SETTINGS),
            },
            {
                Key.key("TAB", EV_TAB),
                Key.mod("CTRL", EV_LEFTCTRL),
                Key.mod("ALT", EV_LEFTALT),
                Key.rep("←", EV_LEFT),
                Key.rep("↓", EV_DOWN),
                Key.rep("→", EV_RIGHT),
                Key.rep("PGDN", EV_PAGEDOWN),
                Key.kbd(GLYPH_KEYBOARD),
            },
        };
    }

    private void buildKeys() {
        Key[][] rows = layout();
        for (int r = 0; r < rows.length; r++) {
            for (int c = 0; c < rows[r].length; c++) {
                Key key = rows[r][c];
                Button button = new Button(getContext(), null, android.R.attr.buttonBarButtonStyle);
                button.setText(key.display);
                button.setTextColor(TEXT_COLOR);
                button.setAllCaps(true);
                button.setPadding(0, 0, 0, 0);
                button.setBackgroundColor(BG_COLOR);

                if (key.type == TYPE_MODIFIER) {
                    ModState state = mModifiers.get(key.display);
                    if (state == null) {
                        state = new ModState();
                        mModifiers.put(key.display, state);
                    }
                    state.buttons.add(button);
                }

                GridLayout.LayoutParams param = new GridLayout.LayoutParams();
                param.width = 0;
                param.height = 0;
                param.setMargins(0, 0, 0, 0);
                param.columnSpec = GridLayout.spec(c, GridLayout.FILL, 1f);
                param.rowSpec = GridLayout.spec(r, GridLayout.FILL, 1f);
                button.setLayoutParams(param);

                attachListeners(button, key);
                addView(button);
            }
        }
    }

    private void attachListeners(final Button button, final Key key) {
        button.setOnClickListener(v -> onKeyClick(key));

        button.setOnTouchListener((v, event) -> {
            switch (event.getAction()) {
                case MotionEvent.ACTION_DOWN:
                    button.setBackgroundColor(ACTIVE_BG_COLOR);
                    startScheduled(button, key);
                    return true;

                case MotionEvent.ACTION_MOVE:
                    if (key.popup != null) {
                        if (mPopupWindow == null && event.getY() < 0) {
                            stopScheduled();
                            button.setBackgroundColor(BG_COLOR);
                            showPopup(button, key.popup);
                        }
                        if (mPopupWindow != null && event.getY() > 0) {
                            button.setBackgroundColor(ACTIVE_BG_COLOR);
                            dismissPopup();
                        }
                    }
                    return true;

                case MotionEvent.ACTION_CANCEL:
                    button.setBackgroundColor(BG_COLOR);
                    stopScheduled();
                    return true;

                case MotionEvent.ACTION_UP:
                    button.setBackgroundColor(BG_COLOR);
                    boolean repeated = mRepeatActive;
                    stopScheduled();
                    if (mPopupWindow != null) {
                        Key popup = key.popup;
                        dismissPopup();
                        if (popup != null)
                            onKeyClick(popup);
                    } else if (!repeated && !mLongHoldFired) {
                        button.performClick();
                    }
                    return true;
            }
            return false;
        });
    }

    // ------- click / send -------

    private void onKeyClick(Key key) {
        switch (key.type) {
            case TYPE_KEYBOARD:
                mSender.toggleKeyboard();
                return;
            case TYPE_SETTINGS:
                mSender.openSettings();
                return;
            case TYPE_MODIFIER:
                toggleModifier(key.display);
                return;
            case TYPE_TEXT:
                sendWithModifiers(() -> mSender.text(key.text));
                return;
            case TYPE_KEY:
            default:
                sendWithModifiers(() -> {
                    mSender.key(0, key.evdev);
                    mSender.key(1, key.evdev);
                });
        }
    }

    /** Wrap a key emission with currently-active modifier down/up, then clear unlocked ones. */
    private void sendWithModifiers(Runnable emit) {
        List<ModState> held = new ArrayList<>();
        for (ModState m : mModifiers.values())
            if (m.active) held.add(m);

        for (ModState m : held) mSender.key(0, modEvdev(m));
        emit.run();
        for (int i = held.size() - 1; i >= 0; i--) mSender.key(1, modEvdev(held.get(i)));

        // Auto-release modifiers that aren't locked.
        for (ModState m : held) {
            if (!m.locked)
                setModifierActive(m, false);
        }
    }

    private int modEvdev(ModState state) {
        for (Map.Entry<String, ModState> e : mModifiers.entrySet())
            if (e.getValue() == state) return evdevForModifier(e.getKey());
        return 0;
    }

    private static int evdevForModifier(String name) {
        switch (name) {
            case "CTRL": return EV_LEFTCTRL;
            case "ALT": return EV_LEFTALT;
            case "SHIFT": return EV_LEFTSHIFT;
            default: return 0;
        }
    }

    private void toggleModifier(String name) {
        ModState state = mModifiers.get(name);
        if (state == null) return;
        setModifierActive(state, !state.active);
        if (!state.active) state.locked = false;
    }

    private void setModifierActive(ModState state, boolean active) {
        state.active = active;
        if (!active) state.locked = false;
        for (Button b : state.buttons)
            b.setTextColor(active ? ACTIVE_TEXT_COLOR : TEXT_COLOR);
    }

    // ------- long press / repeat scheduling -------

    private Runnable mRepeatRunnable;
    private Runnable mLongHoldRunnable;
    private boolean mRepeatActive;
    private boolean mLongHoldFired;

    private void startScheduled(Button button, Key key) {
        stopScheduled();
        mRepeatActive = false;
        mLongHoldFired = false;

        if (key.repeatable) {
            mRepeatRunnable = new Runnable() {
                @Override public void run() {
                    mRepeatActive = true;
                    onKeyClick(key);
                    mHandler.postDelayed(this, REPEAT_DELAY_MS);
                }
            };
            mHandler.postDelayed(mRepeatRunnable, mLongPressTimeout);
        } else if (key.type == TYPE_MODIFIER) {
            mLongHoldRunnable = () -> {
                ModState state = mModifiers.get(key.display);
                if (state == null) return;
                // Lock and toggle active (Termux semantics).
                setModifierActive(state, !state.active);
                state.locked = state.active;
                mLongHoldFired = true;
            };
            mHandler.postDelayed(mLongHoldRunnable, mLongPressTimeout);
        }
    }

    private void stopScheduled() {
        if (mRepeatRunnable != null) {
            mHandler.removeCallbacks(mRepeatRunnable);
            mRepeatRunnable = null;
        }
        if (mLongHoldRunnable != null) {
            mHandler.removeCallbacks(mLongHoldRunnable);
            mLongHoldRunnable = null;
        }
    }

    // ------- popup -------

    private void showPopup(View anchor, Key popupKey) {
        TextView tv = new TextView(getContext());
        tv.setText(popupKey.display);
        tv.setTextColor(ACTIVE_TEXT_COLOR);
        tv.setTextSize(18);
        tv.setBackgroundColor(ACTIVE_BG_COLOR);
        int pad = Math.round(8 * getResources().getDisplayMetrics().density);
        tv.setPadding(pad, pad, pad, pad);
        tv.setGravity(Gravity.CENTER);

        mPopupWindow = new PopupWindow(tv,
                anchor.getWidth(), anchor.getHeight(), false);
        mPopupWindow.setClippingEnabled(false);
        int[] loc = new int[2];
        anchor.getLocationInWindow(loc);
        mPopupWindow.showAtLocation(anchor, Gravity.NO_GRAVITY, loc[0], loc[1] - anchor.getHeight());
    }

    private void dismissPopup() {
        if (mPopupWindow != null) {
            mPopupWindow.dismiss();
            mPopupWindow = null;
        }
    }

    /** True if any modifier (CTRL/ALT/SHIFT) is currently active. */
    public boolean hasActiveModifier() {
        for (ModState m : mModifiers.values())
            if (m.active) return true;
        return false;
    }

    /**
     * Send an evdev key wrapped by the currently-active modifiers, then clear the
     * unlocked ones. Used to combine a soft-keyboard character with a held bar
     * modifier (e.g. bar CTRL + IME 'c' -> Ctrl+C).
     */
    public void sendKeyComboFromExternal(int evdev) {
        sendWithModifiers(() -> {
            mSender.key(0, evdev);
            mSender.key(1, evdev);
        });
    }

    /** Release any held modifiers (e.g. when the bar is hidden). */
    public void reset() {
        stopScheduled();
        dismissPopup();
        for (ModState m : mModifiers.values())
            setModifierActive(m, false);
    }
}
