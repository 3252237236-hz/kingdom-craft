package dev.skycraft.client.mixin;

import dev.skycraft.client.InputBridge;
import dev.skycraft.client.SkyClient;
import net.minecraft.client.MouseHandler;
import net.minecraft.client.input.MouseButtonInfo;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Hidden SDL window events must not overwrite the host's GUI cursor or repeat input. */
@Mixin(MouseHandler.class)
abstract class MouseHandlerMixin {
    @Shadow private boolean ignoreFirstMove;
    private static boolean foreign() { return SkyClient.linked() && !InputBridge.replayingMouse(); }
    @Inject(method="onMove",at=@At("HEAD"),cancellable=true)
    private void skycraft$move(long window,double x,double y,double dx,double dy,CallbackInfo ci) {
        if (foreign()) ci.cancel();
        else if (SkyClient.linked() && InputBridge.replayingMouse()) ignoreFirstMove = false;
    }
    @Inject(method="onButton",at=@At("HEAD"),cancellable=true)
    private void skycraft$button(long window,MouseButtonInfo button,int action,CallbackInfo ci) {
        if (foreign()) ci.cancel();
    }
    @Inject(method="onScroll",at=@At("HEAD"),cancellable=true)
    private void skycraft$scroll(long window,double x,double y,CallbackInfo ci) {
        if (foreign()) ci.cancel();
    }
}
