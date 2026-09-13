package com.anland.consumer;

import android.content.SharedPreferences;

import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Where immersive mode gets its input from. The two sources are mutually
 * exclusive: a session is running one or the other, never both.
 *
 * <p>{@link #DIRECT_EVENT_NODES} is the original behaviour — Anland's root helper
 * takes the physical nodes under {@code EVIOCGRAB} and replays them.
 *
 * <p>{@link #EXISTING_UINPUT_BUS} takes nothing at all. Gold keeps its grabs, and
 * Anland only consumes the {@code KeyEvent}s that Gold's own uinput keyboard
 * already delivers to the foreground window.
 */
enum ImmersiveInputSource {
    DIRECT_EVENT_NODES("direct_event_nodes"),
    EXISTING_UINPUT_BUS("existing_uinput_bus"),
    /**
     * Both at once. They contend for nothing: the direct session takes physical
     * nodes, the bus session takes none and only listens to what Gold's own
     * keyboard already delivers. The node sets are kept disjoint — Gold claims
     * every keyboard, so the direct half only ever ends up with pointers and
     * touch — and each carries the input the other cannot.
     */
    DIRECT_PLUS_GOLD_KEYBOARD("direct_plus_gold_keyboard");

    /**
     * Whether this source takes physical nodes itself.
     *
     * <p>Note what this does <em>not</em> say: the bus source is not exclusive
     * with it. Only {@link #EXISTING_UINPUT_BUS} declines to grab anything.
     */
    boolean takesDevices() {
        return this != EXISTING_UINPUT_BUS;
    }

    /**
     * Whether this source forwards Gold's keyboard. Everything except plain
     * direct does; direct on its own leaves the keyboard to the ordinary
     * forwarding path.
     */
    boolean listensToGoldKeyboard() {
        return this != DIRECT_EVENT_NODES;
    }

    /**
     * Whether this source can run at all right now.
     *
     * <p>The two sources that forward Gold's keyboard have nothing to forward
     * without it, and Gold is a separate Magisk module that may simply not be
     * installed. Asking here rather than at the point of use is what lets the
     * picker say so before the user chooses, instead of accepting the choice
     * and then doing nothing with it.
     */
    boolean isAvailable(android.content.Context context) {
        return !listensToGoldKeyboard() || GoldUinputBusSession.goldKeyboardPresent(context);
    }

    static final String KEY_PREFERENCE = "immersive_input_source";

    /**
     * The order the picker lists them in, and the order the names in
     * {@code R.array.immersive_source_options} are written in. One list, so a
     * fourth source cannot be added to the menu and forgotten by the code that
     * has to name it in a message.
     */
    static final ImmersiveInputSource[] ORDER = {
        DIRECT_EVENT_NODES, EXISTING_UINPUT_BUS, DIRECT_PLUS_GOLD_KEYBOARD,
    };

    /** Index into {@link #ORDER} and the parallel string array. */
    int menuIndex() {
        for (int i = 0; i < ORDER.length; i++) {
            if (ORDER[i] == this)
                return i;
        }
        return 0;
    }

    final String preferenceValue;

    ImmersiveInputSource(String preferenceValue) {
        this.preferenceValue = preferenceValue;
    }

    /**
     * Anything unrecognised — missing, empty, or a value written by a future
     * version — falls back to direct, so an existing install keeps behaving
     * exactly as it did before this preference existed.
     */
    static ImmersiveInputSource fromPreference(String value) {
        for (ImmersiveInputSource source : values()) {
            if (source.preferenceValue.equals(value))
                return source;
        }
        return DIRECT_EVENT_NODES;
    }

    static ImmersiveInputSource read(SharedPreferences preferences) {
        return fromPreference(preferences == null ? null
                : preferences.getString(KEY_PREFERENCE, null));
    }

    // ---- direct-source node selection -------------------------------------

    /** Whether the direct source picks nodes itself instead of using the list. */
    static final String KEY_AUTO = "immersive_input_auto";

    /** The saved node list, comma separated, in the legacy grammar. */
    static final String KEY_NODES = "immersive_input_nodes";

    /**
     * Missing means automatic, so an install that predates this preference keeps
     * taking whatever the helper finds — the behaviour it already had.
     */
    static boolean isAutomatic(SharedPreferences preferences) {
        return preferences == null || preferences.getBoolean(KEY_AUTO, true);
    }

    /**
     * The saved node list as written. Not validated here: the resolver decides
     * what the list means and whether it can be honoured, and it never rewrites
     * what the user saved.
     */
    static Set<String> savedNodes(SharedPreferences preferences) {
        Set<String> nodes = new LinkedHashSet<>();
        if (preferences == null)
            return nodes;
        String raw = preferences.getString(KEY_NODES, "");
        if (raw == null)
            return nodes;
        for (String part : raw.split(",")) {
            String node = part.trim();
            if (!node.isEmpty())
                nodes.add(node);
        }
        return nodes;
    }
}
