package dev.skycraft.link;

/** Optional extensions; the supplied SkyCraft v11 protocol stays unchanged. */
public final class Kcd2Protocol {
    public static final boolean ENABLED = Boolean.getBoolean("skycraft.kcd2.passthrough");
    public static final int REN_COLLISION_SHAPES = 12;
    public static final int REN_SCENE_REQUEST = 13;
    public static final int REN_ATTACK_TRACE = 14;
    public static final int REN_ATTACK_PROBE = 15;
    public static final int REN_ATTACK_COMMIT = 16;
    public static final int COL_ATTACK_RESULT = 5;
    public static final int COL_SCENE_CELL = 4;
    public static final int EV_HEAL_ACTOR = 6;
    public static final int HIT_MOB_SOURCE = 1 << 4;
    private Kcd2Protocol() {}
}
