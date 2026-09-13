package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.util.List;

import org.junit.Test;

public class GoldKeyLedgerTest {
    private static GoldKeyLedger.Identity id(int device, int keyCode, int scanCode) {
        return new GoldKeyLedger.Identity(device, keyCode, scanCode);
    }

    @Test
    public void releaseUsesTheCodeThePressWasSentWith() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        GoldKeyLedger.Identity key = id(3, 29, 30);

        assertTrue(ledger.press(key, 30));

        // The UP describes the key however it likes; the ledger's stored code is
        // what gets sent, because that is what the desktop was told to press.
        GoldKeyLedger.Release release = ledger.release(key);
        assertEquals(GoldKeyLedger.Outcome.SEND, release.outcome);
        assertEquals(30, release.evdevCode);
    }

    @Test
    public void aRepeatPressIsNotSentTwice() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        GoldKeyLedger.Identity key = id(3, 29, 30);

        assertTrue(ledger.press(key, 30));
        assertFalse(ledger.press(key, 30));
        assertFalse(ledger.press(key, 31));   // even re-resolved differently
        assertEquals(1, ledger.heldCount());
    }

    @Test
    public void anUntrackedReleaseIsLeftAlone() {
        GoldKeyLedger ledger = new GoldKeyLedger();

        GoldKeyLedger.Release release = ledger.release(id(3, 29, 30));

        assertEquals(GoldKeyLedger.Outcome.IGNORE, release.outcome);
    }

    @Test
    public void theSameKeyOnTwoDevicesIsTwoKeys() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        GoldKeyLedger.Identity first = id(3, 29, 30);
        GoldKeyLedger.Identity second = id(7, 29, 30);

        assertTrue(ledger.press(first, 30));
        assertTrue(ledger.press(second, 30));
        assertEquals(2, ledger.heldCount());

        // Releasing one must not lift the other: that is the whole reason the
        // identity carries a device id.
        assertEquals(GoldKeyLedger.Outcome.SEND, ledger.release(first).outcome);
        assertEquals(1, ledger.heldCount());
        assertTrue(ledger.press(second, 30) == false);
        assertEquals(GoldKeyLedger.Outcome.SEND, ledger.release(second).outcome);
    }

    @Test
    public void forcedReleaseRemembersWhatItReleased() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        GoldKeyLedger.Identity held = id(3, 29, 30);
        ledger.press(held, 30);

        List<GoldKeyLedger.Held> released = ledger.releaseAll();
        assertEquals(1, released.size());
        assertEquals(30, released.get(0).evdevCode);
        assertTrue(ledger.isEmpty());
        assertEquals(1, ledger.tombstoneCount());

        // The trailing UP is swallowed, and only once.
        assertEquals(GoldKeyLedger.Outcome.SWALLOW, ledger.release(held).outcome);
        assertEquals(GoldKeyLedger.Outcome.IGNORE, ledger.release(held).outcome);
        assertEquals(0, ledger.tombstoneCount());
    }

    @Test
    public void aFreshPressClearsTheTombstone() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        GoldKeyLedger.Identity key = id(3, 29, 30);

        ledger.press(key, 30);
        ledger.releaseAll();

        // Pressed again before the stale UP arrived: the release that follows
        // this press is a real one and must be sent.
        assertTrue(ledger.press(key, 30));
        assertEquals(0, ledger.tombstoneCount());
        assertEquals(GoldKeyLedger.Outcome.SEND, ledger.release(key).outcome);
    }

    @Test
    public void deviceRemovalOnlyTouchesThatDevice() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        GoldKeyLedger.Identity leaving = id(3, 29, 30);
        GoldKeyLedger.Identity staying = id(7, 42, 44);
        ledger.press(leaving, 30);
        ledger.press(staying, 44);

        List<GoldKeyLedger.Held> released = ledger.releaseDevice(3);

        assertEquals(1, released.size());
        assertEquals(30, released.get(0).evdevCode);
        assertFalse(ledger.isEmpty());
        assertEquals(GoldKeyLedger.Outcome.SEND, ledger.release(staying).outcome);
    }

    @Test
    public void aReusedDeviceIdInheritsNothing() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        GoldKeyLedger.Identity old = id(3, 29, 30);
        ledger.press(old, 30);
        ledger.releaseDevice(3);

        // A replacement keyboard arriving on the same id gets a clean slate: its
        // presses are new, and the old identity is only a tombstone.
        assertTrue(ledger.isEmpty());
        assertEquals(GoldKeyLedger.Outcome.SWALLOW, ledger.release(old).outcome);
        assertEquals(GoldKeyLedger.Outcome.IGNORE, ledger.release(id(3, 29, 31)).outcome);
    }

    @Test
    public void tombstonesStayBounded() {
        GoldKeyLedger ledger = new GoldKeyLedger();

        for (int i = 0; i < GoldKeyLedger.MAX_TOMBSTONES + 20; i++) {
            GoldKeyLedger.Identity key = id(3, i, i);
            ledger.press(key, i);
            ledger.releaseAll();
        }

        assertEquals(GoldKeyLedger.MAX_TOMBSTONES, ledger.tombstoneCount());
        // The oldest were evicted; the most recent survive.
        GoldKeyLedger.Identity newest =
                id(3, GoldKeyLedger.MAX_TOMBSTONES + 19, GoldKeyLedger.MAX_TOMBSTONES + 19);
        assertEquals(GoldKeyLedger.Outcome.SWALLOW, ledger.release(newest).outcome);
    }

    @Test
    public void releasesComeBackInPressOrderSoCallersCanReverseThem() {
        GoldKeyLedger ledger = new GoldKeyLedger();
        ledger.press(id(3, 1, 1), 11);
        ledger.press(id(3, 2, 2), 22);
        ledger.press(id(3, 3, 3), 33);

        List<GoldKeyLedger.Held> released = ledger.releaseAll();

        assertEquals(3, released.size());
        assertEquals(11, released.get(0).evdevCode);
        assertEquals(33, released.get(2).evdevCode);
    }
}
