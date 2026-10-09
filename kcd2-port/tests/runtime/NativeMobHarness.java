package skycraft.test;

import dev.skycraft.combat.NativeMobBridge;
import dev.skycraft.combat.NativeAttackTrace;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.link.Kcd2Protocol;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.entity.monster.zombie.Zombie;
import net.minecraft.world.item.Items;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.phys.Vec3;

/** Actual vanilla entities with only the shared-memory endpoint replaced. */
public final class NativeMobHarness {
    private static int checks;
    static final class Wire implements NativeMobBridge.Transport {
        final int epoch,generation;
        boolean online=true, rejectAck;
        UUID session;
        ByteBuffer snapshot;
        final List<ByteBuffer> acks=new ArrayList<>();
        Wire(int epoch,int generation) {this.epoch=epoch;this.generation=generation;}
        public boolean active() {return online;}
        public int epoch() {return epoch;}
        public int generation() {return generation;}
        public boolean send(int type,ByteBuffer bytes) {
            if (type==Kcd2Protocol.REN_MOB_SNAPSHOT) {
                snapshot=ByteBuffer.allocate(bytes.remaining()).order(ByteOrder.LITTLE_ENDIAN);
                snapshot.put(bytes.duplicate()).flip();
                session=new UUID(snapshot.getLong(16),snapshot.getLong(24));
                return true;
            }
            if (type==Kcd2Protocol.REN_MOB_DAMAGE_ACK) {
                if (rejectAck) return false;
                var copy=ByteBuffer.allocate(48).order(ByteOrder.LITTLE_ENDIAN);
                copy.put(bytes.duplicate()).flip();acks.add(copy);return true;
            }
            throw new AssertionError("unexpected mob message "+type);
        }
    }
    private static void check(boolean condition,String label) {
        if (!condition) throw new AssertionError(label);
        checks++;System.out.println("MOB_RUNTIME_PASS: "+label);
    }
    private static void receipt(Wire wire,Zombie mob,long serial,float damage,UUID session,UUID uuid) {
        var bytes=ByteBuffer.allocate(88).order(ByteOrder.LITTLE_ENDIAN);
        bytes.putInt(wire.epoch).putInt(mob.getId()).putLong(serial);
        NativeMobBridge.putUuid(bytes,session);NativeMobBridge.putUuid(bytes,uuid);
        bytes.putFloat(damage).putInt(0).putInt(0).putInt(1);
        for (int i=0;i<6;i++) bytes.putFloat(0);
        NativeMobBridge.accept(MemorySegment.ofArray(bytes.array()),0,88);
    }
    public static void publish(MinecraftServer server,int epoch,int generation) {
        NativeMobBridge.isolatedTransport(new Wire(epoch,generation));
        NativeMobBridge.beginTick(server);
    }
    public static void run(MinecraftServer server,ServerPlayer player) {
        var level=player.level();
        var mob=new Zombie(level);
        mob.setPos(player.position().add(1,0,1));mob.setNoAi(true);mob.setNoGravity(true);
        level.addFreshEntity(mob);
        var proxy=new SkyrimActorEntity(SkyCombat.SKYRIM_ACTOR,level);
        proxy.setFormId(900100);proxy.setPos(player.position().add(2,0,0));
        level.addFreshEntity(proxy);
        var wire=new Wire(77,3);
        try {
            NativeMobBridge.isolatedTransport(wire);
            NativeMobBridge.beginTick(server);
            var identity=NativeMobBridge.identity(mob);
            check(identity!=null && identity.entity().equals(mob.getUUID()) && identity.session().equals(wire.session),
                "mob receives full UUID and fresh session before attacks");
            check(NativeMobBridge.identity(player)==null && NativeMobBridge.identity(proxy)==null,
                "players and native NPC mirrors are excluded from native doubles");
            check(wire.snapshot.remaining()==32+64*wire.snapshot.getInt(4),"snapshot has exact packed record length");
            var packet=NativeAttackTrace.capture(proxy,level.damageSources().mobAttack(mob),0,0,0).transaction(12,77,900100,0,0,0);
            check(packet.remaining()==104 && packet.getLong(72)==wire.session.getMostSignificantBits()
                && packet.getLong(88)==mob.getUUID().getMostSignificantBits(),"attack transaction carries full session and entity UUID");
            float hp=mob.getHealth();
            receipt(wire,mob,1,4,wire.session,mob.getUUID());
            check(mob.getHealth()==hp,"collision consumer only queues receipts without changing entity HP");
            NativeMobBridge.beginTick(server);
            check(mob.getHealth()<hp && wire.acks.size()==1,"server tick applies vanilla native hurt and acknowledges it");
            var ack=wire.acks.getFirst();
            check(ack.remaining()==48 && ack.getInt(0)==77 && ack.getInt(4)==mob.getId()
                && ack.getLong(8)==1 && ack.getLong(16)==wire.session.getMostSignificantBits()
                && ack.getLong(32)==mob.getUUID().getMostSignificantBits(),"ack retains exact epoch, serial, ID and UUIDs");
            hp=mob.getHealth();
            receipt(wire,mob,1,4,wire.session,mob.getUUID());NativeMobBridge.beginTick(server);
            check(mob.getHealth()==hp && wire.acks.size()==2,"receipt retransmission is acknowledged without duplicate damage");
            receipt(wire,mob,2,4,UUID.randomUUID(),mob.getUUID());NativeMobBridge.beginTick(server);
            check(mob.getHealth()==hp && wire.acks.size()==2,"old client session cannot hurt the current world");
            receipt(wire,mob,3,4,wire.session,UUID.randomUUID());NativeMobBridge.beginTick(server);
            check(mob.getHealth()==hp && wire.acks.size()==3,"reused numerical ID with a different UUID is terminal without hurting replacement");
            mob.damageCooldownTime=0;wire.rejectAck=true;
            receipt(wire,mob,4,5,wire.session,mob.getUUID());NativeMobBridge.beginTick(server);
            float after=mob.getHealth();
            check(after<hp && wire.acks.size()==3,"blocked acknowledgement leaves one already applied receipt pending");
            mob.damageCooldownTime=0;
            receipt(wire,mob,4,5,wire.session,mob.getUUID());NativeMobBridge.beginTick(server);
            check(mob.getHealth()==after,"failed acknowledgement cannot reapply damage even after vanilla cooldown");
            wire.rejectAck=false;NativeMobBridge.beginTick(server);
            check(wire.acks.size()==4 && mob.getHealth()==after,"acknowledgement retry completes without another hurt");
            mob.setHealth(mob.getMaxHealth());mob.damageCooldownTime=0;
            mob.setItemSlot(EquipmentSlot.CHEST,new ItemStack(Items.DIAMOND_CHESTPLATE));
            mob.tick(); // Vanilla applies equipment attribute changes during its living tick.
            mob.damageCooldownTime=0;
            hp=mob.getHealth();receipt(wire,mob,5,5,wire.session,mob.getUUID());NativeMobBridge.beginTick(server);
            check(mob.getHealth()<hp && hp-mob.getHealth()<5,"native hurt passes through vanilla armor damage reduction (armor="
                +mob.getArmorValue()+", loss="+(hp-mob.getHealth())+")");
            hp=mob.getHealth();receipt(wire,mob,6,1,wire.session,mob.getUUID());NativeMobBridge.beginTick(server);
            check(mob.getHealth()==hp,"vanilla hurt immunity handles a weaker immediate native hit");
            var oldSession=wire.session;NativeMobBridge.reset();NativeMobBridge.beginTick(server);
            check(!wire.session.equals(oldSession),"readback reset creates a fresh session");
            receipt(wire,mob,7,5,oldSession,mob.getUUID());NativeMobBridge.beginTick(server);
            check(mob.getHealth()==hp,"queued damage from pre-readback session is rejected");
            var identityBefore=NativeMobBridge.identity(mob);wire.online=false;NativeMobBridge.beginTick(server);
            check(identityBefore!=null && NativeMobBridge.identity(mob)==null,"link loss immediately invalidates published mob identity");
            System.out.println("MOB_RUNTIME_RESULT: PASSED ("+checks+" checks)");
        } finally {
            NativeMobBridge.isolatedTransport(null);mob.discard();proxy.discard();
        }
    }
    private NativeMobHarness() {}
}
