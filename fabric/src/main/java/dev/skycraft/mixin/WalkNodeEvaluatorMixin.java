package dev.skycraft.mixin;

import dev.skycraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Native floors and walls participate in the same navigation classification as Minecraft blocks. */
@Mixin(WalkNodeEvaluator.class)
public abstract class WalkNodeEvaluatorMixin {
    @Inject(method="getPathTypeFromState",at=@At("HEAD"),cancellable=true)
    private static void skycraft$nativePath(BlockGetter level,BlockPos pos,
        CallbackInfoReturnable<PathType> result) {
        if (level.getBlockState(pos).isAir() && SkyCollision.hasGeometry(pos))
            result.setReturnValue(PathType.BLOCKED);
    }
}
