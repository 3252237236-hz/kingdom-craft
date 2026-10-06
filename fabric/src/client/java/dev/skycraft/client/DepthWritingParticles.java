package dev.skycraft.client;

import com.mojang.renderpearl.api.pipeline.DepthStencilState;
import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import com.mojang.renderpearl.api.pipeline.ColorTargetState;
import com.mojang.renderpearl.api.vertex.VertexFormat;
import net.minecraft.resources.Identifier;
import java.util.IdentityHashMap;
import java.util.Map;

/** Give transparent vanilla particles a world depth for the host compositor. */
public final class DepthWritingParticles extends RenderPipeline {
    private static final Map<RenderPipeline, RenderPipeline> CACHE = new IdentityHashMap<>();

    private DepthWritingParticles(RenderPipeline original) {
        super(Identifier.fromNamespaceAndPath("skycraft", "particle_depth/"
                + original.getLocation().getNamespace() + "/" + original.getLocation().getPath()),
            original.getShaders(), original.getShaderDefines(), original.getBindGroupLayouts(),
            original.getColorTargetStates().toArray(ColorTargetState[]::new),
            new DepthStencilState(original.getDepthStencilState().depthTest(), true,
                original.getDepthStencilState().depthBiasScaleFactor(),
                original.getDepthStencilState().depthBiasConstant()),
            original.getPolygonMode(), original.isCull(),
            original.getVertexFormatBindings().toArray(VertexFormat[]::new),
            original.getPrimitiveTopology(), original.pushConstantSize(), original.getSortKey());
    }

    public static RenderPipeline withDepth(RenderPipeline original) {
        if (original.getDepthStencilState() == null || original.getDepthStencilState().writeDepth()) return original;
        return CACHE.computeIfAbsent(original, DepthWritingParticles::new);
    }
}
