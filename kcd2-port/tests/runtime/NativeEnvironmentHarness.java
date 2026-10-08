package skycraft.test;

import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.link.Proto;
import java.util.HashMap;
import java.util.Map;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.effect.MobEffectInstance;
import net.minecraft.world.effect.MobEffects;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.CampfireBlock;
import net.minecraft.world.level.block.PressurePlateBlock;
import net.minecraft.world.level.block.state.BlockState;

/** Vanilla environmental interactions on a native-owned actor in an isolated Minecraft world. */
public final class NativeEnvironmentHarness {
    private static int checks;

    private static void check(boolean result, String message) {
        if (!result) throw new AssertionError(message);
        checks++;
        System.out.println("ENV_RUNTIME_PASS: " + message);
    }

    private static SkyrimActorEntity actor(ServerLevel level, BlockPos pos, double height) {
        var actor = new SkyrimActorEntity(SkyCombat.SKYRIM_ACTOR, level);
        actor.setFormId(900002);
        actor.syncHostHealth(1);
        actor.setPos(pos.getX() + .5, pos.getY() + height, pos.getZ() + .5);
        level.addFreshEntity(actor);
        return actor;
    }

    private static boolean damaged(SkyrimActorEntity actor) {
        return actor.peekHit() != null && actor.peekHit()[0] > 0;
    }

    public static void run(ServerLevel level, ServerPlayer player) {
        BlockPos pos = player.blockPosition().offset(24, 6, 6);
        Map<BlockPos, BlockState> restore = new HashMap<>();
        for (BlockPos block : BlockPos.betweenClosed(pos.offset(-1,-1,-1),pos.offset(1,2,1)))
            restore.put(block.immutable(),level.getBlockState(block));
        SkyrimActorEntity actor = null;
        try {
            for (BlockPos block : restore.keySet()) level.setBlock(block,Blocks.AIR.defaultBlockState(),2);
            level.setBlock(pos.below(),Blocks.STONE.defaultBlockState(),2);

            level.setBlock(pos,Blocks.CACTUS.defaultBlockState(),2);
            actor = actor(level,pos,0);
            actor.tick();
            check(damaged(actor),"vanilla cactus contact enters native actor damage bridge");
            check(actor.getHealth() == actor.getMaxHealth(),"environment damage retains native health ownership");
            actor.discard();

            level.setBlock(pos,Blocks.CAMPFIRE.defaultBlockState().setValue(CampfireBlock.LIT,true),2);
            actor = actor(level,pos,.25);
            actor.tick();
            check(damaged(actor),"lit vanilla campfire damages native actor");
            actor.discard();

            level.setBlock(pos,Blocks.FIRE.defaultBlockState(),2);
            actor = actor(level,pos,0);
            actor.tick();
            check(actor.isOnFire() || damaged(actor),"vanilla fire ignites or damages native actor");
            actor.discard();

            level.setBlock(pos,Blocks.LAVA.defaultBlockState(),2);
            actor = actor(level,pos,0);
            actor.tick();
            check(damaged(actor) && (Float.floatToRawIntBits(actor.peekHit()[4]) & Proto.HIT_FIRE) != 0,
                "vanilla lava contact exports fire damage");
            actor.discard();
            actor = actor(level,pos,0);
            actor.addEffect(new MobEffectInstance(MobEffects.FIRE_RESISTANCE,100));
            actor.tick();
            check(!damaged(actor),"vanilla fire resistance prevents lava damage before export");
            actor.discard();

            level.setBlock(pos,Blocks.WATER.defaultBlockState(),2);
            level.setBlock(pos.above(),Blocks.WATER.defaultBlockState(),2);
            actor = actor(level,pos,0);
            actor.setAirSupply(-19);
            actor.tick();
            check(damaged(actor),"submerged native actor follows vanilla drowning damage");
            actor.discard();
            actor = actor(level,pos,0);
            actor.setRemainingFireTicks(80);
            actor.tick();
            check(!actor.isOnFire(),"vanilla water extinguishes native proxy fire state");
            actor.discard();

            level.setBlock(pos,Blocks.STONE_PRESSURE_PLATE.defaultBlockState(),2);
            level.setBlock(pos.above(),Blocks.AIR.defaultBlockState(),2);
            actor = actor(level,pos,0);
            actor.tick();
            check(level.getBlockState(pos).getValue(PressurePlateBlock.POWERED),
                "native actor contact powers vanilla redstone pressure plate");
            actor.discard();

            level.setBlock(pos,Blocks.AIR.defaultBlockState(),2);
            actor = actor(level,pos,0);
            actor.addEffect(new MobEffectInstance(MobEffects.POISON,100,1));
            for (int i=0;i<20;i++) actor.tick();
            check(damaged(actor),"vanilla poison ticks reach native damage bridge");
            actor.discard();
            actor = actor(level,pos,0);
            MobEffects.INSTANT_DAMAGE.value().applyInstantaneousEffect(level,player,player,actor,0,1);
            check(damaged(actor),"vanilla instant damage potion reaches native damage bridge");
            actor.discard();
            actor = actor(level,pos,0);
            actor.syncHostHealth(.5f);
            float before = actor.getHealth();
            MobEffects.INSTANT_HEALTH.value().applyInstantaneousEffect(level,player,player,actor,0,1);
            check(actor.pendingHealing()>0 && actor.getHealth()==before,
                "vanilla healing potion exports healing without taking native health ownership");
            System.out.println("ENV_RUNTIME_RESULT: PASSED ("+checks+" checks)");
        } finally {
            if (actor != null) actor.discard();
            for (var entry : restore.entrySet()) level.setBlock(entry.getKey(),entry.getValue(),2);
        }
    }
}
