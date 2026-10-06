package dev.skycraft.client.mixin;

import dev.skycraft.client.SkyClient;
import net.minecraft.client.player.AbstractClientPlayer;
import net.minecraft.client.player.LocalPlayer;
import net.minecraft.core.ClientAsset;
import net.minecraft.resources.Identifier;
import net.minecraft.world.entity.player.PlayerModelType;
import net.minecraft.world.entity.player.PlayerSkin;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(AbstractClientPlayer.class)
public abstract class SteveSkinMixin {
	private static final PlayerSkin STEVE = new PlayerSkin(
		new ClientAsset.ResourceTexture(Identifier.withDefaultNamespace("entity/player/wide/steve")),
		null, null, PlayerModelType.WIDE, true);

	@Inject(method = "getSkin", at = @At("HEAD"), cancellable = true)
	private void skycraft$classicSteve(CallbackInfoReturnable<PlayerSkin> cir) {
		if ((Object)this instanceof LocalPlayer && SkyClient.linked()
			&& Boolean.getBoolean("skycraft.kcd2.passthrough")) {
			cir.setReturnValue(STEVE);
		}
	}
}
