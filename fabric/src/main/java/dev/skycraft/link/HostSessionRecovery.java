package dev.skycraft.link;

/** One normal MC respawn per newly loaded host session, never per death frame. */
public final class HostSessionRecovery {
    private boolean observed;
    private int session;
    private boolean recoveryAvailable;

    public boolean observe(boolean nowPlayable, int nowSession, boolean dead) {
        if (!nowPlayable) return false;
        // A stalled heartbeat or Alt-Tab is not a save reload. The native host
        // advances the session counter on an actual load/player replacement.
        if (!observed || nowSession != session) recoveryAvailable = true;
        observed = true;
        session = nowSession;
        if (!dead) {
            recoveryAvailable = false;
            return false;
        }
        if (!recoveryAvailable) return false;
        recoveryAvailable = false;
        return true;
    }
}
