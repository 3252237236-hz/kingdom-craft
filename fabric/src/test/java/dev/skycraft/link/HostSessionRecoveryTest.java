package dev.skycraft.link;

import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class HostSessionRecoveryTest {
    @Test void ordinaryDeathWaitsForHostReload() {
        var recovery = new HostSessionRecovery();
        assertFalse(recovery.observe(true, 1, false));
        assertFalse(recovery.observe(true, 1, true));
        assertFalse(recovery.observe(false, 1, true));
        assertTrue(recovery.observe(true, 2, true));
        assertFalse(recovery.observe(true, 2, true));
        assertFalse(recovery.observe(true, 2, false));
        assertFalse(recovery.observe(true, 2, true));
    }

    @Test void sameLevelReloadCanRecoverWithoutMissingSnapshot() {
        var recovery = new HostSessionRecovery();
        assertFalse(recovery.observe(true, 10, false));
        assertFalse(recovery.observe(true, 10, true));
        assertTrue(recovery.observe(true, 11, true));
        assertFalse(recovery.observe(true, 11, true));
    }

    @Test void deadPlayerFromPreviousRuntimeRespawnsOnceOnConnect() {
        var recovery = new HostSessionRecovery();
        assertTrue(recovery.observe(true, 4, true));
        for (int frame = 0; frame < 100; frame++)
            assertFalse(recovery.observe(true, 4, true));
    }

    @Test void loadingAndDisconnectedHostsCannotRespawn() {
        var recovery = new HostSessionRecovery();
        assertFalse(recovery.observe(false, 1, true));
        assertFalse(recovery.observe(false, 2, true));
        assertTrue(recovery.observe(true, 2, true));
    }

    @Test void FocusChangesWithoutHostSessionChangeDoNotRevivePlayer() {
        var recovery = new HostSessionRecovery();
        assertFalse(recovery.observe(true, 8, false));
        assertFalse(recovery.observe(false, 8, true));
        for (int frame = 0; frame < 100; frame++)
            assertFalse(recovery.observe(true, 8, true));
    }
}
