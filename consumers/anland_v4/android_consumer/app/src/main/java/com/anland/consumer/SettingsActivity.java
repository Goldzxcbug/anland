package com.anland.consumer;

import android.app.Activity;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.CountDownTimer;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.util.SparseArray;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

public class SettingsActivity extends Activity {
    private static final String TAG = "AnlandSettings";
    private static final String PREFS_NAME = "anland_settings";
    private static final String KEY_BOUND_KEYCODE = "bound_keycode";
    private static final String KEY_SOCKET_PATH = "socket_path";
    private static final String KEY_USE_ROOT = "use_root";
    private static final String DEFAULT_SOCKET_PATH = "/data/local/tmp/display_daemon.sock";
    private static final int UNBOUND = -1;

    private Button bindButton;
    private TextView statusText;
    private CountDownTimer listenTimer;
    private boolean isListening = false;

    // Android keycode → human-readable name
    private static final SparseArray<String> KEY_NAMES = new SparseArray<>();
    static {
        KEY_NAMES.put(KeyEvent.KEYCODE_VOLUME_UP, "Volume Up");
        KEY_NAMES.put(KeyEvent.KEYCODE_VOLUME_DOWN, "Volume Down");
        KEY_NAMES.put(KeyEvent.KEYCODE_VOLUME_MUTE, "Volume Mute");
        KEY_NAMES.put(KeyEvent.KEYCODE_POWER, "Power");
        KEY_NAMES.put(KeyEvent.KEYCODE_CAMERA, "Camera");
        KEY_NAMES.put(KeyEvent.KEYCODE_HEADSETHOOK, "Headset Hook");
        KEY_NAMES.put(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, "Media Play/Pause");
        KEY_NAMES.put(KeyEvent.KEYCODE_MEDIA_NEXT, "Media Next");
        KEY_NAMES.put(KeyEvent.KEYCODE_MEDIA_PREVIOUS, "Media Previous");
        KEY_NAMES.put(KeyEvent.KEYCODE_BRIGHTNESS_UP, "Brightness Up");
        KEY_NAMES.put(KeyEvent.KEYCODE_BRIGHTNESS_DOWN, "Brightness Down");
        KEY_NAMES.put(KeyEvent.KEYCODE_HOME, "Home");
        KEY_NAMES.put(KeyEvent.KEYCODE_BACK, "Back");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(24), dp(24), dp(24), dp(24));
        root.setBackgroundColor(Color.WHITE);

        // Title
        TextView title = new TextView(this);
        title.setText("Settings");
        title.setTextSize(24);
        title.setTypeface(null, Typeface.BOLD);
        title.setGravity(Gravity.START);
        title.setPadding(0, 0, 0, dp(32));
        root.addView(title);

        // Bind key section
        TextView bindLabel = new TextView(this);
        bindLabel.setText("Virtual Keyboard Key");
        bindLabel.setTextSize(16);
        bindLabel.setTypeface(null, Typeface.BOLD);
        bindLabel.setPadding(0, 0, 0, dp(8));
        root.addView(bindLabel);

        statusText = new TextView(this);
        statusText.setTextSize(14);
        statusText.setTextColor(Color.GRAY);
        statusText.setPadding(0, 0, 0, dp(16));
        root.addView(statusText);

        bindButton = new Button(this);
        bindButton.setText("Bind Virtual Keyboard Key");
        bindButton.setOnClickListener(v -> startListening());
        root.addView(bindButton);

        addConnectionSection(root);

        setContentView(root);

        updateStatus();
    }

    // Connection settings: a custom daemon socket path and a "connect with root"
    // toggle. In root mode the app launches the bundled helper via `su -c`, which
    // connects to the socket and passes the fd back (see MainActivity).
    private void addConnectionSection(LinearLayout root) {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        TextView header = new TextView(this);
        header.setText("Connection");
        header.setTextSize(16);
        header.setTypeface(null, Typeface.BOLD);
        header.setPadding(0, dp(32), 0, dp(8));
        root.addView(header);

        // Socket path
        TextView sockLabel = new TextView(this);
        sockLabel.setText("Daemon socket path");
        sockLabel.setTextSize(14);
        sockLabel.setTextColor(Color.GRAY);
        sockLabel.setPadding(0, 0, 0, dp(4));
        root.addView(sockLabel);

        EditText socketInput = new EditText(this);
        socketInput.setSingleLine(true);
        socketInput.setText(prefs.getString(KEY_SOCKET_PATH, DEFAULT_SOCKET_PATH));
        socketInput.setHint(DEFAULT_SOCKET_PATH);
        socketInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                    .putString(KEY_SOCKET_PATH, s.toString().trim()).apply();
            }
        });
        root.addView(socketInput);

        // Connect with root
        Switch rootSwitch = new Switch(this);
        rootSwitch.setText("Connect with root (helper)");
        rootSwitch.setTextSize(14);
        rootSwitch.setPadding(0, dp(16), 0, 0);
        rootSwitch.setChecked(prefs.getBoolean(KEY_USE_ROOT, false));
        rootSwitch.setOnCheckedChangeListener((v, checked) ->
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                .putBoolean(KEY_USE_ROOT, checked).apply());
        root.addView(rootSwitch);

        TextView rootHint = new TextView(this);
        rootHint.setText("Launches a root helper (su) that connects to the socket "
            + "and passes the connection back to the app. Takes effect on next "
            + "connect (reopen the app).");
        rootHint.setTextSize(12);
        rootHint.setTextColor(Color.GRAY);
        rootHint.setPadding(0, dp(4), 0, 0);
        root.addView(rootHint);
    }

    private void startListening() {
        if (isListening) return;
        isListening = true;
        bindButton.setText("Listening... (5s)");

        listenTimer = new CountDownTimer(5000, 1000) {
            @Override
            public void onTick(long millisUntilFinished) {
                bindButton.setText("Listening... (" + (millisUntilFinished / 1000) + "s)");
            }

            @Override
            public void onFinish() {
                finishListening(UNBOUND);
            }
        }.start();
    }

    private void finishListening(int keycode) {
        isListening = false;
        listenTimer.cancel();

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        prefs.edit().putInt(KEY_BOUND_KEYCODE, keycode).apply();

        bindButton.setText("Bind Virtual Keyboard Key");
        updateStatus();
    }

    private void updateStatus() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        int bound = prefs.getInt(KEY_BOUND_KEYCODE, UNBOUND);
        if (bound == UNBOUND) {
            statusText.setText("Current: None");
        } else {
            String name = KEY_NAMES.get(bound);
            if (name == null) name = "Keycode " + bound;
            statusText.setText("Current: " + name);
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (!isListening) return super.onKeyDown(keyCode, event);

        // Ignore generic Virtual Keyboard keycode (it's a placeholder)
        if (keyCode == KeyEvent.KEYCODE_UNKNOWN) return true;

        finishListening(keyCode);
        Log.i(TAG, "Bound keycode: " + keyCode);
        return true;
    }

    private int dp(int dp) {
        return (int) (dp * getResources().getDisplayMetrics().density);
    }
}
