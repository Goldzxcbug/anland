package com.anland.consumer;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Tracks which keys the Gold uinput bus has pressed on the desktop, so their
 * releases can be forwarded correctly.
 *
 * <p>Pure Java on purpose: this is the part that decides whether a key gets sent
 * twice, stuck down, or released on the wrong device, and it is unit tested on
 * the JVM.
 *
 * <p>Two rules give it its shape:
 *
 * <ul>
 *   <li><b>A release uses the code the press was sent with.</b> The UP that comes
 *       back is described by whatever metadata the device happens to carry at
 *       that moment, which is not necessarily what the DOWN resolved to. Re-
 *       resolving on release is how a key ends up stuck down while a different
 *       one is released in its place.</li>
 *   <li><b>Identity is per device.</b> Two keyboards can hold the same key at the
 *       same time; a release from one must not lift the other. Android device ids
 *       are not stable across a reconnect, which is why a device being removed
 *       drops its entries rather than keeping them for the id to be reused.</li>
 * </ul>
 */
final class GoldKeyLedger {

    /** How many forced releases are remembered to swallow their trailing UP. */
    static final int MAX_TOMBSTONES = 64;

    /** One key, on one device. */
    static final class Identity {
        final int deviceId;
        final int keyCode;
        final int scanCode;

        Identity(int deviceId, int keyCode, int scanCode) {
            this.deviceId = deviceId;
            this.keyCode = keyCode;
            this.scanCode = scanCode;
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof Identity))
                return false;
            Identity that = (Identity) other;
            return deviceId == that.deviceId && keyCode == that.keyCode
                    && scanCode == that.scanCode;
        }

        @Override
        public int hashCode() {
            return (deviceId * 31 + keyCode) * 31 + scanCode;
        }

        @Override
        public String toString() {
            return "dev" + deviceId + ":key" + keyCode + ":scan" + scanCode;
        }
    }

    /** A key the ledger believes is down, and the code that was sent for it. */
    static final class Held {
        final Identity identity;
        final int evdevCode;

        Held(Identity identity, int evdevCode) {
            this.identity = identity;
            this.evdevCode = evdevCode;
        }
    }

    /** What the caller should do with an UP it was handed. */
    enum Outcome {
        /** Send this release; the code is the one the press used. */
        SEND,
        /** Already released by a forced cleanup: swallow the late UP, send nothing. */
        SWALLOW,
        /** Not a key this ledger pressed: leave the event alone. */
        IGNORE
    }

    static final class Release {
        final Outcome outcome;
        final int evdevCode;

        private Release(Outcome outcome, int evdevCode) {
            this.outcome = outcome;
            this.evdevCode = evdevCode;
        }

        static final Release ignored() {
            return new Release(Outcome.IGNORE, -1);
        }

        static final Release swallowed() {
            return new Release(Outcome.SWALLOW, -1);
        }

        static Release send(int evdevCode) {
            return new Release(Outcome.SEND, evdevCode);
        }
    }

    private final LinkedHashMap<Identity, Integer> held = new LinkedHashMap<>();
    /** Bounded, oldest first: a forced release must not grow without limit. */
    private final Set<Identity> tombstones = new LinkedHashSet<>();

    /**
     * Records a press.
     *
     * @param evdevCode the code resolved from this DOWN; it is what the matching
     *                  release will be sent with
     * @return true when the DOWN should be sent. A repeat of a key already held
     *         returns false: the desktop is already holding it.
     */
    boolean press(Identity identity, int evdevCode) {
        if (held.containsKey(identity))
            return false;
        // A fresh press of a key we force-released belongs to the new press, so
        // its eventual release is a real one again.
        tombstones.remove(identity);
        held.put(identity, evdevCode);
        return true;
    }

    /** Resolves an UP. See {@link Outcome}. */
    Release release(Identity identity) {
        Integer code = held.remove(identity);
        if (code != null)
            return Release.send(code);
        if (tombstones.remove(identity))
            return Release.swallowed();
        return Release.ignored();
    }

    /**
     * Forgets every held key, remembering each so its late UP is swallowed
     * instead of sent a second time.
     *
     * @return what to release, in the order the keys were pressed
     */
    List<Held> releaseAll() {
        List<Held> released = new ArrayList<>(held.size());
        for (Map.Entry<Identity, Integer> entry : held.entrySet()) {
            released.add(new Held(entry.getKey(), entry.getValue()));
            remember(entry.getKey());
        }
        held.clear();
        return released;
    }

    /**
     * Forgets everything held on one device. Used when that device goes away:
     * its entries must not survive for the next device that reuses the id.
     */
    List<Held> releaseDevice(int deviceId) {
        List<Held> released = new ArrayList<>();
        Iterator<Map.Entry<Identity, Integer>> entries = held.entrySet().iterator();
        while (entries.hasNext()) {
            Map.Entry<Identity, Integer> entry = entries.next();
            if (entry.getKey().deviceId != deviceId)
                continue;
            released.add(new Held(entry.getKey(), entry.getValue()));
            remember(entry.getKey());
            entries.remove();
        }
        return released;
    }

    /**
     * Consumes a force-released key's trailing UP without sending anything.
     *
     * <p>Used once a session has ended: the keys it released can still produce a
     * late UP, and forwarding one would lift a key the user has since pressed
     * again. Each tombstone is consumed at most once.
     *
     * @return true when this identity was one we force-released
     */
    boolean consumeTombstone(Identity identity) {
        return tombstones.remove(identity);
    }

    boolean isEmpty() {
        return held.isEmpty();
    }

    int heldCount() {
        return held.size();
    }

    int tombstoneCount() {
        return tombstones.size();
    }

    private void remember(Identity identity) {
        tombstones.remove(identity);
        tombstones.add(identity);
        while (tombstones.size() > MAX_TOMBSTONES) {
            Iterator<Identity> oldest = tombstones.iterator();
            oldest.next();
            oldest.remove();
        }
    }
}
