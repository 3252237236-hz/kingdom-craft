package dev.skycraft.combat;

import dev.skycraft.link.SkyLink;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.target.NearestAttackableTargetGoal;

/** Native actors are invisible proxies; keep vanilla sight/range and attack goals. */
public final class NativeActorTargetGoal extends NearestAttackableTargetGoal<SkyrimActorEntity> {
    public NativeActorTargetGoal(Mob mob) {
        super(mob, SkyrimActorEntity.class, 10, true, false,
            (actor, level) -> actor instanceof SkyrimActorEntity proxy && proxy.formId() != 0);
        this.targetConditions.ignoreInvisibilityTesting();
    }

    @Override
    public boolean canUse() {
        return SkyLink.active() && super.canUse();
    }

    @Override
    public boolean canContinueToUse() {
        return SkyLink.active() && super.canContinueToUse();
    }
}
