package dev.skycraft.mixin;

import dev.skycraft.combat.NativeActorTargetGoal;
import dev.skycraft.link.Kcd2Protocol;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.NeutralMob;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import net.minecraft.world.entity.monster.Enemy;
import net.minecraft.world.level.Level;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Mob.class)
public abstract class MobNativeTargetMixin {
    @Shadow @Final protected GoalSelector targetSelector;

    // Base constructor has already called the subclass's registerGoals here.
    @Inject(method = "<init>", at = @At("RETURN"))
    private void skycraft$nativeTarget(EntityType<? extends Mob> type, Level level, CallbackInfo ci) {
        Mob self = (Mob)(Object)this;
        if (Kcd2Protocol.ENABLED && !level.isClientSide()
            && self instanceof Enemy && !(self instanceof NeutralMob)) {
            this.targetSelector.addGoal(3, new NativeActorTargetGoal(self));
        }
    }
}
