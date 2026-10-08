package dev.skycraft.mixin;

import dev.skycraft.world.SkyClip;
import dev.skycraft.combat.NativeProjectileImpact;
import dev.skycraft.combat.NativeAttackTrace;
import dev.skycraft.combat.NativeCombatHandshake;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.link.Kcd2Protocol;
import com.llamalad7.mixinextras.injector.wrapmethod.WrapMethod;
import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.entity.projectile.ProjectileDeflection;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.EntityHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft ignores projectile hits on air, and to Minecraft a Skyrim wall is air. Treat a hit on
 * Skyrim geometry as a real hit: arrows stick in it, snowballs and eggs break on it.
 */
@Mixin(Projectile.class)
public abstract class ProjectileMixin {
	@Shadow
	protected abstract void onHit(HitResult hitResult);

	@WrapMethod(method = "onHit")
	private void skycraft$impactSegment(HitResult hitResult, Operation<Void> original) {
		if (!Kcd2Protocol.ENABLED) {
			original.call(hitResult);
			return;
		}
        var projectile=(Projectile)(Object)this;
        var previous = NativeProjectileImpact.enter(projectile, hitResult.getLocation());
        try {
            if (!projectile.level().isClientSide() && hitResult instanceof EntityHitResult entityHit
                && entityHit.getEntity() instanceof SkyrimActorEntity target && target.formId()!=0) {
                var trace=NativeAttackTrace.projectile(target,projectile);
                var reply=NativeCombatHandshake.probe(target.formId(),trace);
                if (reply.decision()==NativeCombatHandshake.WORLD && NativeCombatHandshake.current(reply)) {
                    original.call(SkyClip.nativeSurfaceHit(reply.point(),reply.normal(),trace.from(),trace.point()));
                } else {
                    NativeCombatHandshake.withReply(reply,()->original.call(hitResult));
                }
                return;
            }
            original.call(hitResult);
		} finally {
			NativeProjectileImpact.restore(previous);
		}
	}

	@Inject(method = "hitTargetOrDeflectSelf", at = @At("HEAD"), cancellable = true)
	private void skycraft$hitSkyrim(HitResult hitResult, CallbackInfoReturnable<ProjectileDeflection> cir) {
		if (hitResult instanceof SkyClip.SkyrimHitResult) {
			this.onHit(hitResult);
			cir.setReturnValue(ProjectileDeflection.NONE);
		}
	}
}
