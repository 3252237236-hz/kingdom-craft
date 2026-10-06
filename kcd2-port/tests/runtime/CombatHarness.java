package skycraft.test;

import dev.skycraft.combat.NativeActorTargetGoal;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.link.Kcd2Protocol;
import dev.skycraft.link.Proto;
import dev.skycraft.client.MirrorWorld;
import dev.skycraft.client.DepthWritingParticles;
import com.mojang.blaze3d.systems.RenderSystem;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.fabricmc.api.ClientModInitializer;
import net.fabricmc.fabric.api.client.event.lifecycle.v1.ClientTickEvents;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.client.Minecraft;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.Mob;
import net.minecraft.world.entity.ai.goal.GoalSelector;
import net.minecraft.world.entity.ai.goal.target.NearestAttackableTargetGoal;
import net.minecraft.world.entity.monster.zombie.Zombie;
import net.minecraft.world.entity.monster.skeleton.Skeleton;
import net.minecraft.world.entity.animal.wolf.Wolf;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.arrow.Arrow;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.phys.EntityHitResult;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

/** Test-only Fabric mod, run in a disposable copy of the MC world. Never installed. */
public final class CombatHarness implements ClientModInitializer {
    private boolean finished;
    private int ticks;
    private static int checks;
    private boolean particleChecked;
    private volatile Throwable particleError;

    private static void check(boolean result, String message) {
        if (!result) throw new AssertionError(message);
        checks++;
        System.out.println("COMBAT_RUNTIME_PASS: " + message);
    }

    private static void clearCooldown(SkyrimActorEntity proxy) {
        proxy.clearHit();
        for (int i = 0; i < 25; i++) proxy.tick();
    }

    private static GoalSelector targets(Mob mob) throws Exception {
        var field = Mob.class.getDeclaredField("targetSelector");
        field.setAccessible(true);
        return (GoalSelector) field.get(mob);
    }

    private static NativeActorTargetGoal nativeGoal(Mob mob) throws Exception {
        return targets(mob).getAvailableGoals().stream().map(g -> g.getGoal())
            .filter(g -> g instanceof NativeActorTargetGoal).map(g -> (NativeActorTargetGoal)g)
            .findFirst().orElse(null);
    }

    @Override
    public void onInitializeClient() {
        ClientTickEvents.END_CLIENT_TICK.register(mc -> {
            if (!finished) MirrorWorld.openWhenReady(mc);
            if (mc.level != null && !particleChecked) {
                particleChecked = true;
                try {
                    var original = RenderPipelines.TRANSLUCENT_PARTICLE;
                    var pipeline = DepthWritingParticles.withDepth(original);
                    check(pipeline.getDepthStencilState().writeDepth()
                        && pipeline.getDepthStencilState().depthTest() == original.getDepthStencilState().depthTest(),
                        "transparent vanilla particles retain depth testing and export depth");
                    check(pipeline.getShaders().equals(original.getShaders())
                        && !RenderSystem.getCompiledPipeline(pipeline).isClosed(),
                        "vanilla particle shader compiles with depth-writing pipeline");
                } catch (Throwable error) { particleError = error; }
            }
        });
        ServerTickEvents.END_SERVER_TICK.register(server -> {
            if (finished || server.getPlayerList().getPlayers().isEmpty() || ++ticks < 40) return;
            finished = true;
            var player = server.getPlayerList().getPlayers().getFirst();
            ServerLevel level = player.level();
            SkyrimActorEntity proxy = new SkyrimActorEntity(SkyCombat.SKYRIM_ACTOR, level);
            Zombie zombie = new Zombie(level);
            Skeleton skeleton = new Skeleton(EntityTypes.SKELETON, level);
            Wolf wolf = new Wolf(EntityTypes.WOLF, level);
            try {
                if (particleError != null) throw particleError;
                check(particleChecked, "particle pipeline check executed on render thread");
                proxy.setFormId(900001);
                proxy.setPos(player.getX() + 3, player.getY() + 1, player.getZ());
                zombie.setPos(player.getX() + 5, player.getY() + 1, player.getZ());
                proxy.syncHostHealth(1);
                level.addFreshEntity(proxy);
                level.addFreshEntity(zombie);
                player.setItemSlot(EquipmentSlot.MAINHAND, new ItemStack(Items.DIAMOND_SWORD));
                player.attack(proxy);
                check(proxy.peekHit() != null && proxy.peekHit()[0] > 0, "vanilla sword attack reaches proxy damage");
                check(proxy.getHealth() == proxy.getMaxHealth(), "native host retains health ownership");
                clearCooldown(proxy);
                proxy.knockback(.4, -1, 0, level.damageSources().playerAttack(player), 5, false);
                check(proxy.peekHit() != null && Math.abs(proxy.peekHit()[3]-.4)<.001
                    && proxy.peekHit()[1]>.99, "vanilla knockback exports direction and velocity");
                clearCooldown(proxy);
                proxy.getAttribute(Attributes.KNOCKBACK_RESISTANCE).setBaseValue(1);
                proxy.knockback(.4, -1, 0, level.damageSources().playerAttack(player), 5, false);
                check(proxy.peekHit() != null && proxy.peekHit()[3]==0,
                    "full vanilla knockback resistance prevents a native push");
                proxy.getAttribute(Attributes.KNOCKBACK_RESISTANCE).setBaseValue(0);
                clearCooldown(proxy);
                for (String name : new String[]{"attackStrengthTicker", "itemSwapTicker"}) {
                    for (Class<?> c=player.getClass(); c!=null; c=c.getSuperclass()) {
                        try {
                            var field=c.getDeclaredField(name); field.setAccessible(true); field.setInt(player,100); break;
                        } catch (NoSuchFieldException ignored) {}
                    }
                }
                player.setOnGround(false);
                player.fallDistance=1;
                player.setSprinting(false);
                player.attack(proxy);
                check(proxy.peekHit()!=null
                    && (Float.floatToRawIntBits(proxy.peekHit()[4]) & Proto.HIT_CRITICAL)!=0,
                    "falling vanilla sword attack retains critical flag and animation event");
                player.fallDistance=0;
                clearCooldown(proxy);
                var arrow = new Arrow(level, player, new ItemStack(Items.ARROW), new ItemStack(Items.BOW));
                arrow.setDeltaMovement(0, 0, 3);
                arrow.setBaseDamage(2);
                var hit = AbstractArrow.class.getDeclaredMethod("onHitEntity", EntityHitResult.class);
                hit.setAccessible(true);
                hit.invoke(arrow, new EntityHitResult(proxy));
                check(proxy.peekHit() != null && proxy.peekHit()[0] > 0
                    && (Float.floatToRawIntBits(proxy.peekHit()[4]) & Proto.HIT_PROJECTILE) != 0,
                    "vanilla arrow impact reaches projectile damage bridge");
                arrow.discard();
                clearCooldown(proxy);
                zombie.doHurtTarget(level, proxy);
                check(proxy.peekHit() != null && proxy.peekHit()[0] > 0
                    && (Float.floatToRawIntBits(proxy.peekHit()[4]) & Kcd2Protocol.HIT_MOB_SOURCE) != 0,
                    "vanilla zombie melee reaches mob damage bridge");
                clearCooldown(proxy);
                zombie.doHurtTarget(level, proxy);
                check(proxy.peekHit() != null && proxy.peekHit()[0] > 0,
                    "subsequent attacks work after vanilla damage cooldown");
                check(nativeGoal(zombie) != null, "zombie constructor installs native target goal");
                check(nativeGoal(skeleton) != null, "skeleton constructor installs native target goal");
                check(nativeGoal(wolf) == null, "neutral wolf keeps its original aggression rules");
                // Exercise exactly the vanilla superclass search used by the goal, without a
                // live game link. This bypasses only the link-active gate in this isolated test.
                var search = MethodHandles.privateLookupIn(NativeActorTargetGoal.class, MethodHandles.lookup())
                    .findSpecial(NearestAttackableTargetGoal.class, "canUse", MethodType.methodType(boolean.class), NativeActorTargetGoal.class);
                var goal = nativeGoal(zombie);
                boolean found = false;
                for (int i = 0; i < 200 && !found; i++) found = (boolean)search.invoke(goal);
                check(found, "vanilla target search can see invisible native proxy");
                goal.start();
                check(zombie.getTarget() == proxy, "native proxy becomes the actual vanilla mob target");
                goal.stop();
                System.out.println("COMBAT_RUNTIME_RESULT: PASSED ("+checks+" checks)");
            } catch (Throwable error) {
                System.out.println("COMBAT_RUNTIME_RESULT: FAILED");
                error.printStackTrace();
            } finally {
                proxy.discard(); zombie.discard(); skeleton.discard(); wolf.discard();
                Minecraft.getInstance().execute(() -> Minecraft.getInstance().stop());
            }
        });
    }
}
