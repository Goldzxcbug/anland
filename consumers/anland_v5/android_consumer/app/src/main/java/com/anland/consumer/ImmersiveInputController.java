package com.anland.consumer;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.KeyEvent;
import android.widget.Toast;

import java.security.SecureRandom;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Owns immersive input: which source is configured, which one is running, and
 * making sure only one of them ever is.
 *
 * <p>{@link ImmersiveMode} takes the physical nodes through the root helper;
 * {@link GoldUinputBusSession} takes nothing and only forwards what Gold's own
 * virtual keyboard already delivers. They contend for nothing, so a source may
 * ask for both — the direct half ends up with pointers and touch (Gold claims
 * every keyboard) and the bus half carries the keys. Which halves a source asks
 * for is {@link ImmersiveInputSource}'s business, not this class's.
 *
 * <p>One key press starts and ends whatever the source asked for, together. Two
 * keys for two halves would let a user arrive in a session that is missing the
 * half they thought they had.
 */
final class ImmersiveInputController {
    private static final String TAG = "AnlandImmersiveInput";
    private static final String PREFS_NAME = "anland_settings";

    /**
     * The file Gold watches to know this app wants its own profile. Its presence
     * is the whole message: no command, no restart, no signal.
     */
    private static final String PROFILE_REQUEST =
            "/data/adb/modules/Gold_Keyboardremaps/anland-profile";
    private static final long PROFILE_REQUEST_TIMEOUT_MS = 5000L;

    private final Context ctx;
    private final ImmersiveMode direct;
    private final GoldUinputBusSession bus;
    private final GoldStatusCache goldStatus;
    private final SuCommand.CommandRunner runner = new SuCommand.SuRunner();
    private final Handler main = new Handler(Looper.getMainLooper());

    private ImmersiveInputSource runningSource = ImmersiveInputSource.DIRECT_EVENT_NODES;
    private boolean screenOffRegistered;
    /** Whether this session's entry has been announced and its exit still owes one. */
    private boolean sessionAnnounced;

    ImmersiveInputController(ImmersiveMode.Host host) {
        this.ctx = host.context();
        this.direct = new ImmersiveMode(host);
        this.goldStatus = new GoldStatusCache(new GoldInputStatusClient(new GoldController(
                new SuCommand.SuRunner(), GoldController.DEFAULT_CONTROLLER, new SecureRandom())));
        this.bus = new GoldUinputBusSession(ctx, new GoldUinputBusSession.Host() {
            @Override
            public void sendKey(int action, int evdev) {
                host.sendKey(action, evdev);
            }

            @Override
            public void onBusActiveChanged(boolean active) {
                // The bus has no grab, but the session still owns the keyboard:
                // the screen going dark has to end it just as it does for direct.
                if (active)
                    registerScreenOff();
                else
                    unregisterScreenOff();
                host.onImmersiveChanged(active);
            }
        });

        // A flag left behind by a crash would keep Gold on the Anland profile
        // with nothing running to ever take it off again.
        setAnlandProfileRequest(false);
    }

    /**
     * Tells Gold to use the Anland profile, or to stop.
     *
     * <p>Runs off the main thread on purpose. Bus mode takes no device at all,
     * and an {@code su} round trip on the toggle would put back exactly the
     * delay this feature exists to avoid. Gold notices the file on its own
     * heartbeat, so arriving a moment late costs nothing.
     */
    private void setAnlandProfileRequest(final boolean wanted) {
        synchronized (profileRequestLock) {
            profileRequestWanted = wanted;
            if (profileRequestRunning)
                return;
            profileRequestRunning = true;
        }
        Thread worker = new Thread(this::drainProfileRequests, "anland-profile-request");
        worker.setDaemon(true);
        worker.start();
    }

    /**
     * Applies profile requests one at a time, coalescing to the latest.
     *
     * <p>A thread per request would let them overlap, and these are a {@code
     * touch} and an {@code rm} on the same path: whichever loses the race does
     * so silently, and the bad outcome is concrete -- an {@code rm} that finishes
     * before an earlier {@code touch} leaves the flag behind, Gold stays on the
     * Anland profile with nothing running to ever take it off, which is exactly
     * the state the constructor clears on startup.
     *
     * <p>Only the final state matters, so requests are folded rather than
     * queued: the loop re-reads what is wanted after each command and stops once
     * it has applied the latest.
     */
    private void drainProfileRequests() {
        for (;;) {
            boolean target;
            synchronized (profileRequestLock) {
                target = profileRequestWanted;
            }
            runner.run((target ? "touch " : "rm -f ") + SuCommand.shellQuote(PROFILE_REQUEST),
                    PROFILE_REQUEST_TIMEOUT_MS);
            synchronized (profileRequestLock) {
                if (profileRequestWanted == target) {
                    profileRequestRunning = false;
                    return;
                }
            }
        }
    }

    /** Guards {@link #drainProfileRequests} against overlapping runs. */
    private final Object profileRequestLock = new Object();
    private boolean profileRequestWanted;
    private boolean profileRequestRunning;

    /**
     * Called by the host whenever a session starts or ends, whichever source it
     * was. Entering immersive mode is what puts Gold on the Anland profile, and
     * leaving it is what takes Gold off again.
     */
    void onSessionActiveChanged(boolean active) {
        setAnlandProfileRequest(active);
        if (active || !sessionAnnounced)
            return;
        sessionAnnounced = false;
        // The device half announces its own endings, and it knows more about
        // them than this class does: a session that failed has a reason worth
        // naming, and "immersive mode off" is not it. Only when no device half
        // was part of the session is the ending ours to announce -- otherwise
        // the user gets this and ImmersiveMode's message, one on top of the
        // other, for a single key press.
        if (!runningSource.takesDevices())
            toast(R.string.immersive_exited);
    }

    /** Whether the app enters immersive mode on its own when it comes back up. */
    static final String KEY_AUTO_ENTER = "immersive_auto_enter";

    /**
     * Enters immersive mode without a key press and without saying so.
     *
     * <p>Offered only for the source that takes no devices, and deliberately so:
     * a session that grabs the touchscreen the moment the app appears would take
     * the device away from someone who never asked for it. This one changes
     * nothing they can see, so arriving unannounced is helpful rather than
     * alarming. It is silent at both ends -- a message on every resume and every
     * pause would be noise, not information.
     *
     * <p>Called on resume. Leaving and coming back is what re-arms it; the
     * ordinary lifecycle teardown already ends the session on the way out.
     */
    void enterAutomatically() {
        SharedPreferences prefs = prefs();
        if (!prefs.getBoolean(ImmersiveMode.KEY_ENABLED, false))
            return;
        if (!prefs.getBoolean(KEY_AUTO_ENTER, false))
            return;
        ImmersiveInputSource current = ImmersiveInputSource.read(prefs);
        if (current != ImmersiveInputSource.EXISTING_UINPUT_BUS)
            return;
        // Silently, and without a message: this runs on every resume, and a
        // toast each time would be noise about something the user did not ask
        // for in that moment. The picker is where they find out.
        if (!current.isAvailable(ctx))
            return;
        if (isActive())
            return;
        runningSource = current;
        // Nothing was announced on the way in, so nothing is owed on the way out.
        sessionAnnounced = false;
        bus.start();
    }

    ImmersiveInputSource source() {
        return ImmersiveInputSource.read(prefs());
    }

    boolean isActive() {
        return direct.isActive() || bus.isActive();
    }

    // ---- key routing -------------------------------------------------------

    /**
     * Consumes the bound immersive key, whichever source is configured.
     *
     * @return true when the event was the toggle and must go no further
     */
    boolean handleKey(KeyEvent event) {
        ImmersiveInputSource current = source();
        if (current != runningSource) {
            // The source was changed underneath a live session. Whatever was
            // running ends before the new source can be toggled.
            stop();
            runningSource = current;
        }

        if (!direct.isBoundToggle(event))
            return false;
        if (direct.consumeSuppressedToggle(event))
            return true;

        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            if (isActive()) {
                stopSession();
            } else {
                startSession(current);
            }
        }
        return true;
    }

    /**
     * Starts whichever halves the source asks for, under the one key press that
     * asked for them. A source that wants devices and cannot have them starts
     * neither half: a session that quietly came up short is worse than one that
     * said why it did not start.
     */
    private void startSession(ImmersiveInputSource source) {
        // Checked again here, not just in the picker: the module can be removed
        // between choosing a source and using it, and a session that came up
        // with nothing to read from would look exactly like one that worked.
        if (!source.isAvailable(ctx)) {
            toast(R.string.immersive_source_unavailable);
            return;
        }
        if (source.takesDevices() && !startDirectNow())
            return;
        if (source.listensToGoldKeyboard())
            bus.start();

        // One press, one message, saying which source it produced. Announcing
        // it here rather than in each half is what keeps the combined source
        // from stacking two toasts the user has to read past each other.
        sessionAnnounced = true;
        toast(getString(R.string.immersive_entering_mode, sourceLabel(source)));
    }

    /** The source's own name, the same words the picker lists it under. */
    private String sourceLabel(ImmersiveInputSource source) {
        String[] names = ctx.getResources().getStringArray(R.array.immersive_source_options);
        int index = source.menuIndex();
        return index < names.length ? names[index] : source.preferenceValue;
    }

    private String getString(int res, Object... args) {
        return ctx.getString(res, args);
    }

    private void stopSession() {
        // Leaves the toggle's own release swallowed, so the key that ended the
        // session does not then reach the desktop.
        direct.requestStop();
        bus.stop();
    }

    /**
     * Offers a key to the bus session.
     *
     * <p>Called for every key on every ingress, not only while a session runs:
     * after one ends, a key it force-released can still send a late UP, and that
     * one has to be swallowed rather than forwarded. The session's own early-out
     * keeps this free for anyone who never turns the feature on.
     */
    boolean routeBusKey(KeyEvent event) {
        return bus.handleKeyEvent(event);
    }

    // ---- direct start -------------------------------------------------------

    /**
     * Starts from the cached occupancy, synchronously.
     *
     * <p>Nothing on this path may wait. The toggle is a key press, and asking
     * Gold costs an {@code su} round trip; a session that hesitates before
     * starting is worse than one that starts without knowing which nodes Gold
     * holds, because the only cost of not knowing is a grab that fails and moves
     * on. The cache is refilled in the background instead, so the next press has
     * an answer without ever having paid for one here.
     */
    private boolean startDirectNow() {
        SharedPreferences prefs = prefs();
        boolean automatic = ImmersiveInputSource.isAutomatic(prefs);
        Set<String> saved = ImmersiveInputSource.savedNodes(prefs);

        // Only a verified snapshot may contribute exclusions. An unreadable Gold
        // is not an idle Gold, so it contributes none.
        //
        // The exclusions are Gold's whole claimed set, not just the node it holds
        // this second: a keyboard offered over two transports can be held over
        // either, and taking the other one would put this session and Gold on the
        // same keyboard from opposite sides.
        GoldInputStatusClient.Result cached = goldStatus.fresh(now());
        Set<String> occupied = null;
        if (cached != null && cached.state == GoldInputStatusClient.State.VERIFIED) {
            occupied = new LinkedHashSet<>(cached.snapshot.claimed);
        } else {
            // Either Gold answered and could not be read, or nothing has asked
            // it recently and the answer has aged out. The two are the same
            // thing to this decision -- no exclusions -- and saying so matters
            // more for the aged-out case than the other: it is the one that
            // happens in ordinary use, several seconds after the last refresh,
            // and it used to pass in silence.
            toast(R.string.immersive_gold_unknown);
        }

        ImmersiveDirectSelection.Result selection = ImmersiveDirectSelection.resolve(
                automatic, saved, occupied, InputGrab.MAX_SELECTED_NODES);

        if (selection.invalid) {
            toast(R.string.immersive_selection_invalid);
            return false;
        }
        if (selection.allFiltered) {
            // Every saved node is Gold's. Falling back to automatic here would
            // quietly take nodes the user explicitly did not choose.
            toast(R.string.immersive_selection_filtered);
            return false;
        }
        direct.startWith(selection.selectedNodes, selection.excludedNodes);

        // Gold's answer for the next press, and for the settings page. Started
        // after the session is up so it cannot delay it.
        refreshGoldStatus();
        return true;
    }

    /**
     * Refills the occupancy cache in the background. Cheap to call: a refresh
     * already in flight is left alone, and the answer is only kept if it is
     * still current when it lands.
     */
    void refreshGoldStatus() {
        if (!goldStatus.beginRefresh())
            return;
        final long requestedAt = now();
        Thread worker = new Thread(() -> {
            GoldInputStatusClient.Result result = goldStatus.query();
            main.post(() -> {
                goldStatus.endRefresh();
                goldStatus.store(result, requestedAt, now());
            });
        }, "anland-gold-status");
        worker.setDaemon(true);
        worker.start();
    }

    private static long now() {
        return android.os.SystemClock.uptimeMillis();
    }

    // ---- lifecycle ---------------------------------------------------------

    /**
     * Ends whichever source is running and invalidates anything in flight.
     * Called for focus loss, pause, destroy, surface loss, screen off and source
     * switches alike: a session must never outlive the foreground.
     */
    void stop() {
        direct.stop();
        bus.stop();
        // Warm the occupancy answer for the next press. Left to the resume
        // refresh, a session started more than a few seconds later finds the
        // cache cold, drops Gold's exclusions, and can take nodes Gold is
        // holding -- the contention the selection design exists to avoid.
        refreshGoldStatus();
    }

    private final BroadcastReceiver screenOffReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (Intent.ACTION_SCREEN_OFF.equals(intent.getAction())) {
                Log.i(TAG, "screen off; leaving the uinput bus session");
                stop();
            }
        }
    };

    private void registerScreenOff() {
        if (screenOffRegistered)
            return;
        IntentFilter filter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            ctx.registerReceiver(screenOffReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        else
            ctx.registerReceiver(screenOffReceiver, filter);
        screenOffRegistered = true;
    }

    private void unregisterScreenOff() {
        if (!screenOffRegistered)
            return;
        screenOffRegistered = false;
        try {
            ctx.unregisterReceiver(screenOffReceiver);
        } catch (IllegalArgumentException ignored) {
            // The activity auto-unregisters its receivers on destroy.
        }
    }

    private SharedPreferences prefs() {
        return ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    private void toast(int stringRes) {
        Toast.makeText(ctx, stringRes, Toast.LENGTH_SHORT).show();
    }

    /** For messages that need a formatted argument, such as the bound key's name. */
    private void toast(String message) {
        Toast.makeText(ctx, message, Toast.LENGTH_SHORT).show();
    }
}
