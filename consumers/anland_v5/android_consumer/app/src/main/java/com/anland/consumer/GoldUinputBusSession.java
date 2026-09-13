package com.anland.consumer;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Immersive input taken from Gold's existing uinput bus.
 *
 * <p>This session takes nothing. It does not open {@code /dev/uinput}, does not
 * touch Gold's virtual event node, and never calls {@code EVIOCGRAB}: Gold keeps
 * every grab it already holds. All this does is recognise the {@code KeyEvent}s
 * that Gold's virtual keyboard has already delivered to the foreground window
 * and forward them to the desktop.
 *
 * <p>What that buys and what it costs is worth stating plainly. Because nothing
 * is grabbed, only the keys Android actually routes to this app arrive — the
 * frontmost Activity or the accessibility service. Raw touch, mice, wheels,
 * gestures, the lock screen and background windows are not part of this, and the
 * session must not pretend otherwise.
 */
final class GoldUinputBusSession {
    private static final String TAG = "AnlandGoldBus";

    /**
     * Must match VIRTUAL_NAME_PREFIX / VIRTUAL_VENDOR / VIRTUAL_PRODUCT in
     * fn_remap.c. The ids are deliberately synthetic rather than borrowed from
     * real hardware -- Android hands a key layout to a device by vendor and
     * product, so a borrowed id would hand Gold's layout to that hardware as
     * well. A change on one side only makes the keyboard unrecognisable here,
     * with nothing to say so, which is why the Gold repository's
     * tests/test_device_identity.sh compares these three against fn_remap.c
     * rather than trusting the comment.
     */
    static final String GOLD_NAME_PREFIX = "Gold Keyboardremaps";
    static final int GOLD_VENDOR = 0xffff;
    static final int GOLD_PRODUCT = 0xffff;

    private static final int ACTION_PRESS = 0;
    private static final int ACTION_RELEASE = 1;

    interface Host {
        /** The single funnel to the desktop, shared with the direct path. */
        void sendKey(int action, int evdev);

        /** The session started or stopped; the host re-syncs its UI state. */
        void onBusActiveChanged(boolean active);
    }

    /**
     * The device test, as a pure function so it can be unit tested.
     *
     * <p>Identity comes from what the kernel reports, never from
     * {@link InputDevice#isVirtual()}: Gold's keyboard <em>is</em> a uinput
     * device, so treating "virtual" as a reason to reject would refuse exactly
     * the device this whole feature exists to find. {@code isVirtual} is only
     * ever useful as a diagnostic.
     */
    static boolean matchesGoldKeyboard(boolean present, String name, int vendorId,
                                       int productId, int sources) {
        if (!present || name == null || !name.startsWith(GOLD_NAME_PREFIX))
            return false;
        if (vendorId != GOLD_VENDOR || productId != GOLD_PRODUCT)
            return false;
        // SOURCE_KEYBOARD, not a "SOURCE_CLASS_KEYBOARD": the SDK has no such
        // constant. A keyboard's source is 0x101 and its class bits are the
        // generic SOURCE_CLASS_BUTTON, so testing the class would be wrong as
        // well as unbuildable.
        return (sources & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD;
    }

    private static boolean isGoldKeyboard(InputDevice device) {
        if (device == null)
            return false;
        return matchesGoldKeyboard(true, device.getName(), device.getVendorId(),
                device.getProductId(), device.getSources());
    }

    /** Whether Android can see a Gold keyboard right now, for the settings page. */
    static boolean goldKeyboardPresent(Context context) {
        InputManager manager = context.getSystemService(InputManager.class);
        if (manager == null)
            return false;
        for (int id : manager.getInputDeviceIds()) {
            if (isGoldKeyboard(InputDevice.getDevice(id)))
                return true;
        }
        return false;
    }

    private final Context ctx;
    private final Host host;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final GoldKeyLedger ledger = new GoldKeyLedger();
    /** Device ids a Gold keyboard has actually been seen at this session. */
    private final Set<Integer> goldDeviceIds = new HashSet<>();

    private boolean active;
    private InputManager inputManager;
    private boolean listenerRegistered;

    GoldUinputBusSession(Context context, Host host) {
        this.ctx = context.getApplicationContext();
        this.host = host;
    }

    boolean isActive() {
        return active;
    }

    /**
     * Starts the session. This cannot fail on account of Gold not being there:
     * the keyboard may be attached at any time, and a session that is simply
     * waiting for one is a perfectly good session.
     */
    boolean start() {
        if (active)
            return true;
        active = true;
        registerDeviceListener();
        Log.i(TAG, "uinput bus session started"
                + (goldKeyboardPresent(ctx) ? "" : " (no Gold keyboard visible yet)"));
        host.onBusActiveChanged(true);
        return true;
    }

    /** Ends the session, releasing every key it believes is held. */
    void stop() {
        boolean wasActive = active;
        active = false;
        releaseEverything();
        goldDeviceIds.clear();
        unregisterDeviceListener();
        if (wasActive) {
            Log.i(TAG, "uinput bus session stopped");
            host.onBusActiveChanged(false);
        }
    }

    /**
     * Handles a key event, if it is ours.
     *
     * @return true when the event was consumed and must not be routed further
     */
    boolean handleKeyEvent(KeyEvent event) {
        // Nothing running and nothing in flight: skip the device lookup, which
        // is the common case for every user who never turns this on.
        if (!active && ledger.tombstoneCount() == 0)
            return false;

        InputDevice device = InputDevice.getDevice(event.getDeviceId());
        if (!isGoldKeyboard(device))
            return false;

        int action = event.getAction();
        if (!active) {
            // No session, but the last one force-released keys. A late UP for one
            // of those is swallowed, because forwarding it would lift a key the
            // user has pressed again since.
            return action == KeyEvent.ACTION_UP && ledger.consumeTombstone(identityOf(event));
        }

        goldDeviceIds.add(event.getDeviceId());

        if (action == KeyEvent.ACTION_DOWN)
            return handleDown(event);
        if (action == KeyEvent.ACTION_UP)
            return handleUp(event);
        // ACTION_MULTIPLE carries no usable key state; swallow it rather than
        // letting it through as a normal key.
        return true;
    }

    private boolean handleDown(KeyEvent event) {
        // Resolved once, here. The matching release reuses this code instead of
        // re-reading the UP, whose metadata describes the key as it is now and
        // is not guaranteed to resolve the same way.
        int evdev = KeyResolver.resolveEvdevCode(event.getKeyCode(), event.getScanCode(), false);
        if (evdev < 0)
            return true;

        GoldKeyLedger.Identity identity = identityOf(event);
        if (ledger.press(identity, evdev)) {
            Log.d(TAG, "down evdev=" + evdev + " " + identity);
            host.sendKey(ACTION_PRESS, evdev);
        }
        // A repeat of a key already held sends nothing: the desktop is already
        // holding it down.
        return true;
    }

    private boolean handleUp(KeyEvent event) {
        GoldKeyLedger.Release release = ledger.release(identityOf(event));
        if (release.outcome == GoldKeyLedger.Outcome.SEND) {
            Log.d(TAG, "up evdev=" + release.evdevCode + " " + identityOf(event));
            host.sendKey(ACTION_RELEASE, release.evdevCode);
        }
        // SWALLOW: a forced cleanup already released it, so sending again would
        // release a key that may since have been pressed again.
        // IGNORE: ours, but never pressed by this session — there is nothing to
        // release, and it must not be allowed to release anything else.
        return true;
    }

    /**
     * Releases every key this session pressed, in reverse press order so a
     * modifier is lifted after the key it was modifying.
     */
    private void releaseEverything() {
        sendReleases(ledger.releaseAll());
    }

    private void releaseDevice(int deviceId) {
        sendReleases(ledger.releaseDevice(deviceId));
    }

    private void sendReleases(List<GoldKeyLedger.Held> released) {
        for (int index = released.size() - 1; index >= 0; index--) {
            Log.i(TAG, "forced release evdev=" + released.get(index).evdevCode
                    + " " + released.get(index).identity);
            host.sendKey(ACTION_RELEASE, released.get(index).evdevCode);
        }
    }

    private static GoldKeyLedger.Identity identityOf(KeyEvent event) {
        return new GoldKeyLedger.Identity(event.getDeviceId(), event.getKeyCode(),
                event.getScanCode());
    }

    // ---- device changes ----------------------------------------------------

    private final InputManager.InputDeviceListener deviceListener =
            new InputManager.InputDeviceListener() {
                @Override
                public void onInputDeviceAdded(int deviceId) {
                    // Nothing to do: the next key event from it will match.
                }

                @Override
                public void onInputDeviceRemoved(int deviceId) {
                    forgetDevice(deviceId);
                }

                @Override
                public void onInputDeviceChanged(int deviceId) {
                    forgetDevice(deviceId);
                }
            };

    /**
     * A Gold keyboard went away, or changed shape. Its keys are released and
     * remembered, and its entries are dropped outright: Android reuses device
     * ids, and a keyboard that comes back on the same id must not inherit the
     * previous one's held state.
     */
    private void forgetDevice(int deviceId) {
        if (!goldDeviceIds.remove(deviceId))
            return;
        releaseDevice(deviceId);
        Log.i(TAG, "Gold keyboard " + deviceId + " changed or left; released its keys");
    }

    private void registerDeviceListener() {
        if (listenerRegistered)
            return;
        InputManager manager = ctx.getSystemService(InputManager.class);
        if (manager == null)
            return;
        manager.registerInputDeviceListener(deviceListener, main);
        inputManager = manager;
        listenerRegistered = true;
    }

    private void unregisterDeviceListener() {
        if (!listenerRegistered)
            return;
        listenerRegistered = false;
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(deviceListener);
            inputManager = null;
        }
    }
}
