package dev.skycraft.combat;

import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/** Exact segment while vanilla dispatches one projectile impact. Scoped to its server thread. */
public final class NativeProjectileImpact {
    public record Impact(Projectile projectile, Vec3 from, Vec3 point) {}
    private static final ThreadLocal<Impact> CURRENT = new ThreadLocal<>();

    public static @Nullable Impact current(Projectile projectile) {
        Impact value = CURRENT.get();
        return value != null && value.projectile() == projectile ? value : null;
    }

    public static @Nullable Impact enter(Projectile projectile, Vec3 point) {
        Impact previous = CURRENT.get();
        CURRENT.set(new Impact(projectile, projectile.position(), point));
        return previous;
    }

    public static void restore(@Nullable Impact previous) {
        if (previous == null) CURRENT.remove();
        else CURRENT.set(previous);
    }

    private NativeProjectileImpact() {}
}
