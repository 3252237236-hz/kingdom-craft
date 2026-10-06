package dev.skycraft.link;

/** Optional extensions; the supplied SkyCraft v11 protocol stays unchanged. */
public final class Kcd2Protocol {
    public static final boolean ENABLED = Boolean.getBoolean("skycraft.kcd2.passthrough");
    public static final int REN_COLLISION_SHAPES = 12;
    public static final int EV_HEAL_ACTOR = 6;
    public static final int HIT_MOB_SOURCE = 1 << 4;
    private Kcd2Protocol() {}
}
