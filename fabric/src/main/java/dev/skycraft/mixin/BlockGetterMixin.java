package dev.skycraft.mixin;

import dev.skycraft.world.SkyClip;
import dev.skycraft.world.SkyCollision;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import net.minecraft.core.BlockPos;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** Every vanilla ray query sees native surfaces, including AI sight and explosion exposure. */
@Mixin(BlockGetter.class)
public interface BlockGetterMixin {
    @Inject(method="clip(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;",
        at=@At("RETURN"),cancellable=true)
    private void skycraft$nativeClip(ClipContext context,CallbackInfoReturnable<BlockHitResult> result) {
        if (!SkyCollision.active()) return;
        BlockHitResult original=result.getReturnValue();
        boolean nativeVoxel=original.getType()!=HitResult.Type.MISS
            && ((BlockGetter)(Object)this).getBlockState(original.getBlockPos()).isAir()
            && SkyCollision.hasGeometry(original.getBlockPos());
        BlockHitResult base=nativeVoxel ? BlockHitResult.miss(context.getTo(),
            original.getDirection(),BlockPos.containing(context.getTo())) : original;
        BlockHitResult refined=SkyClip.refine(context.getFrom(),context.getTo(),base,SkyClip.Use.PROJECTILE);
        if (nativeVoxel && refined.getType()==HitResult.Type.MISS)
            refined=SkyClip.nativeVoxelHit(original,context.getFrom(),context.getTo());
        result.setReturnValue(refined);
    }
}
