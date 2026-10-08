package dev.skycraft.combat;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.phys.Vec3;

/** One effective vanilla hit, retaining its source and segment for native obstruction checks. */
public record NativeAttackTrace(int sourceKind, int sourceNativeKey, int sourceEntity,
        int flags, int weapon, float damage, Vec3 from, Vec3 point, int approval, int epoch) {
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
        var reply=NativeCombatHandshake.context(target.formId());
        return new NativeAttackTrace(kind, key, attacker != null ? attacker.getId() : 0,
            flags, weapon, damage, from, point, NativeCombatHandshake.approval(target.formId()),
            reply!=null ? reply.epoch() : dev.skycraft.world.SkyCollision.epoch());
    }

    public NativeAttackTrace critical() {
        return new NativeAttackTrace(sourceKind, sourceNativeKey, sourceEntity,
            flags | dev.skycraft.link.Proto.HIT_CRITICAL, weapon, damage, from, point, approval, epoch);
    }

    public static NativeAttackTrace projectile(SkyrimActorEntity target,Projectile projectile) {
        var owner=projectile.getOwner();
        int kind=owner instanceof SkyrimActorEntity ? 3 : owner instanceof Player ? 1 : owner!=null ? 2 : 0;
        int key=owner instanceof SkyrimActorEntity proxy ? proxy.formId() : 0;
        var impact=NativeProjectileImpact.current(projectile);
        Vec3 from=impact!=null ? impact.from() : projectile.position();
        Vec3 point=impact!=null ? impact.point() : target.getBoundingBox().getCenter();
        return new NativeAttackTrace(kind,key,owner!=null ? owner.getId() : 0,
            dev.skycraft.link.Proto.HIT_PROJECTILE,
            projectile instanceof net.minecraft.world.entity.projectile.arrow.ThrownTrident
                ? dev.skycraft.link.Proto.WEAPON_PIERCE : dev.skycraft.link.Proto.WEAPON_ARROW,
            0,from,point,0,dev.skycraft.world.SkyCollision.epoch());
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

    public ByteBuffer transaction(int request, int epoch, int target, float pushX, float pushZ, float strength) {
        ByteBuffer bytes=ByteBuffer.allocate(72).order(ByteOrder.LITTLE_ENDIAN);
        bytes.putInt(request).put(packet(epoch,target,pushX,pushZ,strength));
        return bytes.flip();
    }
}
