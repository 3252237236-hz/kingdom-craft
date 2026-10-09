package skycraft.test;

import dev.skycraft.combat.NativeActorTargetGoal;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.combat.NativeProjectileImpact;
import dev.skycraft.combat.NativeCombatHandshake;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.lang.foreign.MemorySegment;
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
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

/** Test-only Fabric mod, run in a disposable copy of the MC world. Never installed. */
public final class CombatHarness implements ClientModInitializer {
    private static final class Wire implements NativeCombatHandshake.Transport {
        int decision=NativeCombatHandshake.CLEAR,epoch=77,requests,mode,generation;
        boolean online=true;
        volatile boolean backgroundReply;
        ByteBuffer last;
        public boolean active() { return online; }
        public int epoch() { return epoch; }
        public int generation() { return generation; }
        public boolean send(ByteBuffer bytes) {
            requests++;last=bytes.duplicate().order(ByteOrder.LITTLE_ENDIAN);
            if (mode==1) return false;
            if (mode==2) return true;
            if (mode==3) throw new IllegalStateException("simulated closed ring");
            int request=bytes.getInt(0),target=bytes.getInt(8);
            if (mode==4) target++;
            var reply=ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN);
            reply.putInt(epoch).putInt(request).putInt(target).putInt(decision).putFloat(1);
            // Packet layout: request + 68-byte AttackTrace; intercept at segment midpoint.
            for (int i=0;i<3;i++) reply.putFloat((bytes.getFloat(36+i*4)+bytes.getFloat(48+i*4))*.5F);
            reply.putFloat(-1).putFloat(0).putFloat(0);
            if (mode==8) {
                Thread.ofPlatform().daemon().start(()->{
                    NativeCombatHandshake.accept(MemorySegment.ofArray(reply.array()),0,44);
                    backgroundReply=true;
                });
                return true;
            }
            NativeCombatHandshake.accept(MemorySegment.ofArray(reply.array()),0,44);
            if (mode==5) online=false;
            if (mode==6) epoch++;
            if (mode==7) NativeCombatHandshake.reset();
            if (mode==9) generation++;
            return true;
        }
    }
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
                    CameraAlignmentHarness.run(mc);
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
            Wire wire=new Wire();
            try {
                SharedSceneHarness.run(level, player);
                NativeEnvironmentHarness.run(level, player);
                NativeMobHarness.run(server,player);
                NativeCombatHandshake.isolatedTransport(wire);
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
                var swordTrace=proxy.nativeHits().peekLast();
                check(swordTrace!=null && swordTrace.sourceKind()==1
                    && swordTrace.sourceEntity()==player.getId()
                    && swordTrace.from().distanceTo(player.getEyePosition())<1e-6,
                    "sword bridge retains real player identity and eye origin");
                check(proxy.getHealth() == proxy.getMaxHealth(), "native host retains health ownership");
                check(swordTrace.approval()>0 && swordTrace.epoch()==wire.epoch && wire.requests==1 && wire.last.remaining()==72,
                    "vanilla sword damage follows exactly one native collision approval");
                check(NativeCombatHandshake.context(proxy.formId())==null,
                    "melee approval context is cleaned after vanilla dispatch");
                clearCooldown(proxy);
                wire.decision=NativeCombatHandshake.EQUIPMENT;
                player.attack(proxy);
                check(proxy.nativeHits().isEmpty() && proxy.peekHit()==null,
                    "native equipment rejection prevents sword hurt, critical and knockback");
                wire.decision=NativeCombatHandshake.WORLD;
                check(!proxy.hurtServer(level,level.damageSources().playerAttack(player),8)
                    && proxy.nativeHits().isEmpty(),"native wall rejection prevents vanilla melee damage");
                wire.decision=NativeCombatHandshake.CLEAR;
                for (int mode : new int[]{1,3,5,6,7,9}) {
                    wire.mode=mode;wire.online=true;
                    check(!proxy.hurtServer(level,level.damageSources().playerAttack(player),8)
                        && proxy.nativeHits().isEmpty(),"failed or invalidated approval rejects damage, mode "+mode);
                }
                wire.mode=0;wire.online=false;
                check(!proxy.hurtServer(level,level.damageSources().playerAttack(player),8),
                    "inactive native transport cannot bypass collision approval");
                wire.online=true;
                NativeCombatHandshake.beginTick();
                long waitStart=System.nanoTime();
                wire.mode=2;
                boolean timedOut=!proxy.hurtServer(level,level.damageSources().playerAttack(player),8);
                boolean secondTimedOut=!proxy.hurtServer(level,level.damageSources().playerAttack(player),8);
                long waited=(System.nanoTime()-waitStart)/1000000;
                check(timedOut && secondTimedOut && proxy.nativeHits().isEmpty() && waited<200,
                    "multiple missing replies share the bounded server-tick wait budget ("+waited+" ms)");
                wire.mode=4;NativeCombatHandshake.beginTick();
                check(!proxy.hurtServer(level,level.damageSources().playerAttack(player),8),
                    "reply for a different target cannot authorize this hit");
                wire.mode=0;NativeCombatHandshake.beginTick();
                wire.mode=8;
                check(proxy.hurtServer(level,level.damageSources().playerAttack(player),8)
                    && proxy.nativeHits().peekLast().approval()>0,
                    "independent collision consumer can authorize vanilla hit while server waits");
                clearCooldown(proxy);wire.mode=0;
                clearCooldown(proxy);
                proxy.syncHostHealth(.01F);
                float nativeHp=proxy.getHealth();
                proxy.hurtServer(level,level.damageSources().playerAttack(player),8);
                check(proxy.nativeHits().peekLast()!=null
                    && Math.abs(proxy.nativeHits().peekLast().damage()-8)<.001,
                    "low-health native proxy exports full lethal damage before HP clamping");
                check(Math.abs(proxy.getHealth()-nativeHp)<.001 && !proxy.isDeadOrDying(),
                    "lethal vanilla hit leaves native health and death ownership intact");
                clearCooldown(proxy);
                proxy.getAttribute(Attributes.MAX_ABSORPTION).setBaseValue(10);
                proxy.setAbsorptionAmount(3);
                proxy.hurtServer(level,level.damageSources().playerAttack(player),8);
                check(proxy.nativeHits().peekLast()!=null
                    && Math.abs(proxy.nativeHits().peekLast().damage()-5)<.001,
                    "unclamped native damage still applies vanilla absorption");
                clearCooldown(proxy);
                proxy.setAbsorptionAmount(10);
                proxy.hurtServer(level,level.damageSources().playerAttack(player),8);
                check(proxy.nativeHits().isEmpty(),"fully absorbed vanilla hit exports no native damage");
                proxy.setAbsorptionAmount(0);
                proxy.getAttribute(Attributes.MAX_ABSORPTION).setBaseValue(0);
                clearCooldown(proxy);
                proxy.getAttribute(Attributes.ARMOR).setBaseValue(12);
                proxy.getAttribute(Attributes.ARMOR_TOUGHNESS).setBaseValue(8);
                proxy.syncHostHealth(1);
                proxy.hurtServer(level,level.damageSources().playerAttack(player),8);
                float armoredDamage=proxy.nativeHits().peekLast().damage();
                clearCooldown(proxy);
                proxy.syncHostHealth(.01F);
                proxy.hurtServer(level,level.damageSources().playerAttack(player),8);
                check(armoredDamage>0 && armoredDamage<8
                    && Math.abs(proxy.nativeHits().peekLast().damage()-armoredDamage)<.001,
                    "armor mitigation is identical for healthy and nearly dead native proxies");
                proxy.getAttribute(Attributes.ARMOR).setBaseValue(0);
                proxy.getAttribute(Attributes.ARMOR_TOUGHNESS).setBaseValue(0);
                proxy.syncHostHealth(1);
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
                check((proxy.nativeHits().peekLast().flags() & Proto.HIT_CRITICAL)!=0,
                    "critical flags are retained on the individual trace");
                clearCooldown(proxy);
                var arrow = new Arrow(level, player, new ItemStack(Items.ARROW), new ItemStack(Items.BOW));
                Vec3 arrowStart=proxy.position().add(-2,.9,0);
                Vec3 arrowImpact=proxy.position().add(-.3,.9,0);
                arrow.setPos(arrowStart);
                arrow.setDeltaMovement(3, 0, 0);
                arrow.setBaseDamage(2);
                var hit = Projectile.class.getDeclaredMethod("onHit", HitResult.class);
                hit.setAccessible(true);
                hit.invoke(arrow, new EntityHitResult(proxy,arrowImpact));
                check(proxy.peekHit() != null && proxy.peekHit()[0] > 0
                    && (Float.floatToRawIntBits(proxy.peekHit()[4]) & Proto.HIT_PROJECTILE) != 0,
                    "vanilla arrow impact reaches projectile damage bridge");
                var arrowTrace=proxy.nativeHits().peekLast();
                check(arrowTrace!=null && arrowTrace.sourceKind()==1
                    && (arrowTrace.flags() & Proto.HIT_PROJECTILE)!=0
                    && arrowTrace.from().distanceTo(arrowStart)<1e-6
                    && arrowTrace.point().distanceTo(arrowImpact)<1e-6,
                    "arrow bridge retains ownership and exact current vanilla impact segment");
                check(NativeProjectileImpact.current(arrow)==null,
                    "projectile impact context is cleared after vanilla dispatch");
                check(arrowTrace.approval()>0 && NativeCombatHandshake.context(proxy.formId())==null,
                    "arrow approval and effective damage share one cleaned call-stack context");
                arrow.discard();
                clearCooldown(proxy);
                wire.decision=NativeCombatHandshake.WORLD;
                var wallArrow=new Arrow(level,player,new ItemStack(Items.ARROW),new ItemStack(Items.BOW));
                wallArrow.setPos(arrowStart);wallArrow.setDeltaMovement(3,0,0);wallArrow.setBaseDamage(2);
                hit.invoke(wallArrow,new EntityHitResult(proxy,arrowImpact));
                var embedded=AbstractArrow.class.getDeclaredMethod("isInGround");embedded.setAccessible(true);
                check((boolean)embedded.invoke(wallArrow) && proxy.nativeHits().isEmpty(),
                    "native wall intercept dispatches vanilla arrow block impact instead of damaging NPC");
                wallArrow.tick();
                check((boolean)embedded.invoke(wallArrow),"arrow remains embedded in native surface on subsequent tick");
                wallArrow.discard();
                wire.decision=NativeCombatHandshake.EQUIPMENT;
                var shieldArrow=new Arrow(level,player,new ItemStack(Items.ARROW),new ItemStack(Items.BOW));
                shieldArrow.setPos(arrowStart);shieldArrow.setDeltaMovement(3,0,0);shieldArrow.setBaseDamage(2);
                hit.invoke(shieldArrow,new EntityHitResult(proxy,arrowImpact));
                check(proxy.nativeHits().isEmpty() && shieldArrow.getDeltaMovement().x<0,
                    "native equipment interception retains vanilla rejected-arrow ricochet with no NPC damage");
                shieldArrow.discard();wire.decision=NativeCombatHandshake.CLEAR;
                clearCooldown(proxy);
                proxy.syncHostHealth(.01F);
                var lethalArrow=new Arrow(level,player,new ItemStack(Items.ARROW),new ItemStack(Items.BOW));
                lethalArrow.setPos(arrowStart);lethalArrow.setDeltaMovement(3,0,0);lethalArrow.setBaseDamage(2);
                hit.invoke(lethalArrow,new EntityHitResult(proxy,arrowImpact));
                check(proxy.nativeHits().peekLast()!=null && proxy.nativeHits().peekLast().damage()>=6,
                    "arrow preserves full vanilla lethal damage against a nearly dead native NPC");
                lethalArrow.discard();
                proxy.syncHostHealth(1);
                clearCooldown(proxy);
                NativeMobHarness.publish(server,wire.epoch,wire.generation);
                zombie.doHurtTarget(level, proxy);
                check(proxy.peekHit() != null && proxy.peekHit()[0] > 0
                    && (Float.floatToRawIntBits(proxy.peekHit()[4]) & Kcd2Protocol.HIT_MOB_SOURCE) != 0,
                    "vanilla zombie melee reaches mob damage bridge");
                check(proxy.nativeHits().peekLast().sourceKind()==2
                    && proxy.nativeHits().peekLast().sourceEntity()==zombie.getId(),
                    "mob damage retains its own identity instead of becoming a player hit");
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
                NativeCombatHandshake.isolatedTransport(null);
                dev.skycraft.combat.NativeMobBridge.isolatedTransport(null);
                proxy.discard(); zombie.discard(); skeleton.discard(); wolf.discard();
                Minecraft.getInstance().execute(() -> Minecraft.getInstance().stop());
            }
        });
    }
}
