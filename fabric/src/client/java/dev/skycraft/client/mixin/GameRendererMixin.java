package dev.skycraft.client.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import dev.skycraft.client.PassthroughFrameExporter;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyArg;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import net.minecraft.client.Minecraft;
import org.joml.Matrix4f;

/** No sky while a host is attached (the host's sky shows through), and the frame is split into world and overlay layers. */
@Mixin(GameRenderer.class)
abstract class GameRendererMixin {
	@Shadow @Final private RenderTarget mainRenderTarget;

	@ModifyArg(method = "renderLevel", at = @At(value = "INVOKE",
		target = "Lnet/minecraft/client/renderer/ProjectionMatrixBuffer;getBuffer(Lorg/joml/Matrix4f;)Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;"), index = 0)
	private Matrix4f passthrough$sharedWorldProjection(Matrix4f transformed) {
		// The native camera already supplies walking/head motion. GameRenderer
		// adds a second view-bob translation, hurt rotation and portal distortion
		// to only the MC world, invalidating its exported camera/depth metadata.
		// The hand/HUD pass keeps vanilla's own animation path.
		return PassthroughFrameExporter.enabled()
			? Minecraft.getInstance().gameRenderer.gameRenderState().levelRenderState.cameraRenderState.projectionMatrix
			: transformed;
	}

	@ModifyArg(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/LevelRenderer;render(Lcom/mojang/blaze3d/resource/GraphicsResourceAllocator;ZLnet/minecraft/client/renderer/state/level/CameraRenderState;Lcom/mojang/renderpearl/api/buffers/GpuBufferSlice;Lorg/joml/Vector4f;ZZ)V"),
		index = 5
	)
	private boolean passthrough$noSky(final boolean shouldRenderSky) {
		return shouldRenderSky && !PassthroughFrameExporter.enabled();
	}

	@Inject(
		method = "renderLevel",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render3dHud(Lnet/minecraft/client/renderer/state/level/CameraRenderState;Lnet/minecraft/client/renderer/state/level/PlayerRenderState;Lnet/minecraft/client/renderer/state/OptionsRenderState;Z)V")
	)
	private void passthrough$captureWorld(final CallbackInfo ci) {
		PassthroughFrameExporter.captureWorld(this.mainRenderTarget);
	}

	@Inject(method = "render", at = @At("TAIL"))
	private void passthrough$captureOverlay(final CallbackInfo ci) {
		PassthroughFrameExporter.captureOverlay(this.mainRenderTarget);
	}
}
