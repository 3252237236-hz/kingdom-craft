package dev.skycraft.client.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.renderpearl.api.pipeline.CompiledRenderPipeline;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import dev.skycraft.client.DepthWritingParticles;
import dev.skycraft.client.PassthroughFrameExporter;
import net.minecraft.client.renderer.feature.QuadParticleFeatureRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

@Mixin(QuadParticleFeatureRenderer.class)
abstract class ParticleDepthMixin {
    @Redirect(method = "drawLayers", at = @At(value = "INVOKE", target =
        "Lcom/mojang/blaze3d/systems/RenderSystem;getCompiledPipeline(Lcom/mojang/renderpearl/api/pipeline/RenderPipeline;)Lcom/mojang/renderpearl/api/pipeline/CompiledRenderPipeline;"))
    private static CompiledRenderPipeline skycraft$particleDepth(RenderPipeline pipeline) {
        return RenderSystem.getCompiledPipeline(PassthroughFrameExporter.enabled()
            ? DepthWritingParticles.withDepth(pipeline) : pipeline);
    }
}
