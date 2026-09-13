package com.anland.consumer;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.view.InputDevice;

import org.junit.Test;

/**
 * The device matcher. It decides whether a {@code KeyEvent} belongs to Gold, so
 * getting it wrong in either direction is costly: too loose and Anland forwards
 * a stranger's keystrokes, too tight and the feature silently never fires.
 */
public class GoldUinputBusSessionTest {
    private static final String NAME = "Gold Keyboardremaps event12";
    // Read from the class rather than repeated here: what is under test is the
    // matcher, and a second copy of the ids would only be one more place to
    // forget when fn_remap.c changes.
    private static final int VENDOR = GoldUinputBusSession.GOLD_VENDOR;
    private static final int PRODUCT = GoldUinputBusSession.GOLD_PRODUCT;
    private static final int KEYBOARD = InputDevice.SOURCE_KEYBOARD;

    @Test
    public void acceptsTheDeviceFnRemapCreates() {
        assertTrue(GoldUinputBusSession.matchesGoldKeyboard(
                true, NAME, VENDOR, PRODUCT, KEYBOARD));
    }

    @Test
    public void acceptsTheBarePrefixWithoutANodeSuffix() {
        assertTrue(GoldUinputBusSession.matchesGoldKeyboard(
                true, "Gold Keyboardremaps", VENDOR, PRODUCT, KEYBOARD));
    }

    @Test
    public void acceptsAKeyboardThatAlsoReportsOtherSources() {
        int keyboardAndDpad = KEYBOARD | InputDevice.SOURCE_DPAD;

        assertTrue(GoldUinputBusSession.matchesGoldKeyboard(
                true, NAME, VENDOR, PRODUCT, keyboardAndDpad));
    }

    @Test
    public void rejectsAnythingThatIsNotAKeyboard() {
        // Same name and ids, but the kernel does not describe it as a keyboard.
        assertFalse(GoldUinputBusSession.matchesGoldKeyboard(
                true, NAME, VENDOR, PRODUCT, InputDevice.SOURCE_MOUSE));
        assertFalse(GoldUinputBusSession.matchesGoldKeyboard(
                true, NAME, VENDOR, PRODUCT, 0));
    }

    @Test
    public void rejectsThePhysicalKeyboardGoldRemaps() {
        // The physical device is product 0x3869; only the virtual one is ours.
        assertFalse(GoldUinputBusSession.matchesGoldKeyboard(
                true, "Gold Keyboardremaps", VENDOR, 0x3869, KEYBOARD));
        assertFalse(GoldUinputBusSession.matchesGoldKeyboard(
                true, "Some Other Keyboard", VENDOR, PRODUCT, KEYBOARD));
    }

    @Test
    public void rejectsWhenThereIsNoDeviceAtAll() {
        assertFalse(GoldUinputBusSession.matchesGoldKeyboard(
                false, NAME, VENDOR, PRODUCT, KEYBOARD));
        assertFalse(GoldUinputBusSession.matchesGoldKeyboard(
                true, null, VENDOR, PRODUCT, KEYBOARD));
    }

    @Test
    public void theNameMustStartWithThePrefixNotMerelyContainIt() {
        assertFalse(GoldUinputBusSession.matchesGoldKeyboard(
                true, "Not Gold Keyboardremaps", VENDOR, PRODUCT, KEYBOARD));
    }

    /**
     * {@code isVirtual()} is deliberately not a parameter: Gold's keyboard is a
     * uinput device and therefore virtual, so any rule that rejected virtual
     * devices would reject this one. This test exists to keep it that way.
     */
    @Test
    public void virtualDevicesAreNotRejectedForBeingVirtual() {
        assertTrue(GoldUinputBusSession.matchesGoldKeyboard(
                true, NAME, VENDOR, PRODUCT, KEYBOARD));
    }
}
