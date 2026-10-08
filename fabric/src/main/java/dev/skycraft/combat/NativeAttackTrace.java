package dev.skycraft.combat;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.phys.Vec3;

/** One effective vanilla hit, retaining its source and segment for native obstruction checks. */
public record NativeAttackTrace(int sourceKind, int sourceNativeKey, int sourceEntity,
        int flags, int weapon, float damage, Vec3 from, Vec3 point) {
    public static NativeAttackTrace capture(SkyrimActorEntity target, DamageSource source,
            float damage, int flags, int weapon) {
        var attacker = source.getEntity();
        int kind = attacker instanceof SkyrimActorEntity ? 3 : attacker instanceof Player ? 1
            : attacker != null ? 2 : 0;
        int key = attacker instanceof SkyrimActorEntity proxy ? proxy.formId() : 0;
        Vec3 point = target.getBoundingBox().getCenter();
        Vec3 from = attacker != null ? attacker.getEyePosition() : point;
        if (source.getDirectEntity() instanceof Projectile projectile) {
            var impact = NativeProjectileImpact.current(projectile);
            from = impact != null ? impact.from() : projectile.position();
            point = impact != null ? impact.point() : target.getBoundingBox().clip(from,
                from.add(projectile.getDeltaMovement())).orElse(point);
        } else if (attacker != null) {
            point = target.getBoundingBox().clip(from, point).orElse(point);
        }
        return new NativeAttackTrace(kind, key, attacker != null ? attacker.getId() : 0,
            flags, weapon, damage, from, point);
    }

    public NativeAttackTrace critical() {
        return new NativeAttackTrace(sourceKind, sourceNativeKey, sourceEntity,
            flags | dev.skycraft.link.Proto.HIT_CRITICAL, weapon, damage, from, point);
    }

    public ByteBuffer packet(int epoch, int target, float pushX, float pushZ, float strength) {
        ByteBuffer bytes = ByteBuffer.allocate(68).order(ByteOrder.LITTLE_ENDIAN);
        bytes.putInt(epoch).putInt(target).putInt(sourceKind).putInt(sourceNativeKey).putInt(sourceEntity)
            .putInt(flags).putInt(weapon).putFloat(damage)
            .putFloat((float)from.x).putFloat((float)from.y).putFloat((float)from.z)
            .putFloat((float)point.x).putFloat((float)point.y).putFloat((float)point.z)
            .putFloat(pushX).putFloat(pushZ).putFloat(strength);
        return bytes.flip();
    }
}
