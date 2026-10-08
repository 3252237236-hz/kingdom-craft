package dev.skycraft.mixin;

import dev.skycraft.world.SkyCollision;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.EntityCollisionContext;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Skyrim ground and walls hold things up: torches, lanterns, rails, carpets and the like can be
 * placed on terrain and against walls, and stay there.
 */
@Mixin(BlockBehaviour.BlockStateBase.class)
public abstract class BlockStateBaseMixin {
    @Inject(method={
        "getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
        "getBlockSupportShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/phys/shapes/VoxelShape;"
    },at=@At("RETURN"),cancellable=true)
    private void skycraft$nativeShape(BlockGetter level,BlockPos pos,CallbackInfoReturnable<VoxelShape> result) {
        if (level==null || pos==null || !SkyCollision.active()) return;
        VoxelShape nativeShape=SkyCollision.shapeAt(pos), original=result.getReturnValue();
        if (nativeShape!=null && original!=nativeShape)
            result.setReturnValue(original.isEmpty() ? nativeShape : Shapes.or(original,nativeShape));
    }

    @Inject(method="getCollisionShape(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/phys/shapes/CollisionContext;)Lnet/minecraft/world/phys/shapes/VoxelShape;",
        at=@At("RETURN"),cancellable=true)
    private void skycraft$nativeEntityShape(BlockGetter level,BlockPos pos,CollisionContext context,
        CallbackInfoReturnable<VoxelShape> result) {
        if (context instanceof EntityCollisionContext entity
            && SkyCollision.usesSmoothCollider(entity.getEntity())) return;
        skycraft$nativeShape(level,pos,result);
    }
	@Inject(
		method = "isFaceSturdy(Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/core/Direction;Lnet/minecraft/world/level/block/SupportType;)Z",
		at = @At("HEAD"),
		cancellable = true
	)
	private void skycraft$skyrimIsSturdy(BlockGetter level, BlockPos pos, Direction direction, SupportType type, CallbackInfoReturnable<Boolean> cir) {
		if (!((BlockBehaviour.BlockStateBase) (Object) this).isAir()) {
			return;
		}
        var state=(net.minecraft.world.level.block.state.BlockState)(Object)this;
        boolean sturdy = type.isSupporting(state,level,pos,direction)
            || (direction == Direction.UP ? SkyCollision.supportsFromBelow(pos.above()) : SkyCollision.solidFraction(pos) >= 0.4F);
		if (sturdy) {
			cir.setReturnValue(true);
		}
	}
}
