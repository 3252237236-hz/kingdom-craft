package dev.skycraft.combat;

import dev.skycraft.link.Kcd2Protocol;
import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyCollision;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.player.Player;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.damagesource.DamageType;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/** Independent mob identities and acknowledged native hurts. Entity work stays on the server thread. */
public final class NativeMobBridge {
    private static final ResourceKey<DamageType> OBSERVED_DAMAGE=ResourceKey.create(Registries.DAMAGE_TYPE,
        Identifier.fromNamespaceAndPath("skycraft","native_damage"));
    public record Identity(UUID session, UUID entity) {}
    private record Session(MinecraftServer server, ServerLevel level, int epoch, int generation,
            long revision, UUID uuid) {}
    private record Receipt(int epoch, int entityId, long serial, Identity identity, float damage,
            int kind, int attacker, int flags, Vec3 from, Vec3 point) {}
    public interface Transport {
        boolean active(); int epoch(); int generation(); boolean send(int type, ByteBuffer bytes);
    }
    private static final Transport LIVE = new Transport() {
        public boolean active() { return Kcd2Protocol.ENABLED && SkyLink.active(); }
        public int epoch() { return SkyCollision.epoch(); }
        public int generation() { return SkyLink.generation(); }
        public boolean send(int type, ByteBuffer bytes) { return SkyLink.tryWriteRender(type, bytes, null); }
    };
    private static volatile Transport transport = LIVE;
    private static volatile @Nullable Session session;
    private static long revision, sequence, completedFloor;
    private static final Map<Long, Receipt> pending = new LinkedHashMap<>();
    private static final Map<Long, Receipt> completed = new LinkedHashMap<>();
    private static final Map<Integer, UUID> published = new LinkedHashMap<>();

    public static boolean eligible(Entity entity) {
        return entity.getId()>0 && (entity.getUUID().getMostSignificantBits()!=0 || entity.getUUID().getLeastSignificantBits()!=0)
            && entity instanceof LivingEntity living && !(entity instanceof Player)
            && !(entity instanceof SkyrimActorEntity) && living.isAlive() && !entity.isRemoved()
            && Float.isFinite(living.getHealth()) && Float.isFinite(living.getMaxHealth())
            && living.getHealth()>0 && living.getHealth()<=living.getMaxHealth() && living.getMaxHealth()<=1e6F
            && Float.isFinite(entity.getYRot()) && Math.abs(entity.getYRot())<=1e7F
            && Float.isFinite(entity.getBbWidth()) && entity.getBbWidth()>=.05F && entity.getBbWidth()<=16
            && Float.isFinite(entity.getBbHeight()) && entity.getBbHeight()>=.05F && entity.getBbHeight()<=16
            && finite(entity.position());
    }

    /** Run before mobs/players can attack, including after death, readback and dimension changes. */
    public static synchronized void beginTick(MinecraftServer server) {
        var players = server.getPlayerList().getPlayers();
        if (!transport.active() || transport.epoch()<0 || players.isEmpty()) {
            if (session!=null) reset();
            return;
        }
        var player=players.getFirst();
        var level=player.level();
        var current=session;
        if (current==null || current.server()!=server || current.level()!=level
                || current.epoch()!=transport.epoch() || current.generation()!=transport.generation()
                || current.revision()!=revision) {
            reset();
            current=new Session(server,level,transport.epoch(),transport.generation(),revision,UUID.randomUUID());
            session=current;
            NativeCombatHandshake.reset();
        }
        drain(level,current);
        var mobs=level.getEntitiesOfClass(LivingEntity.class,player.getBoundingBox().inflate(32),NativeMobBridge::eligible);
        mobs.sort(Comparator.<LivingEntity>comparingDouble(entity->entity.distanceToSqr(player))
            .thenComparingInt(Entity::getId));
        if (mobs.size()>64) mobs=new ArrayList<>(mobs.subList(0,64));
        var bytes=snapshot(current.epoch(),++sequence,current.uuid(),mobs);
        if (transport.send(Kcd2Protocol.REN_MOB_SNAPSHOT,bytes)) {
            published.clear();
            for (var mob:mobs) published.put(mob.getId(),mob.getUUID());
        }
    }

    public static synchronized @Nullable Identity identity(@Nullable Entity entity) {
        var current=session;
        if (current==null || entity==null || !eligible(entity) || entity.level()!=current.level()
                || !transport.active() || transport.epoch()!=current.epoch()
                || transport.generation()!=current.generation()
                || !entity.getUUID().equals(published.get(entity.getId()))) return null;
        return new Identity(current.uuid(),entity.getUUID());
    }

    public static ByteBuffer snapshot(int epoch,long sequence,UUID session,List<? extends LivingEntity> mobs) {
        if (mobs.size()>64 || sequence<=0 || sequence>0x1fffffffffffffL) throw new IllegalArgumentException("snapshot bounds");
        var bytes=ByteBuffer.allocate(32+64*mobs.size()).order(ByteOrder.LITTLE_ENDIAN);
        bytes.putInt(epoch).putInt(mobs.size()).putLong(sequence); putUuid(bytes,session);
        for (var mob:mobs) {
            if (!eligible(mob)) throw new IllegalArgumentException("invalid mob snapshot");
            bytes.putInt(mob.getId()); putUuid(bytes,mob.getUUID());
            bytes.putDouble(mob.getX()).putDouble(mob.getY()).putDouble(mob.getZ())
                .putFloat(mob.getYRot()).putFloat(mob.getBbWidth()).putFloat(mob.getBbHeight())
                .putFloat(mob.getHealth()).putFloat(mob.getMaxHealth());
        }
        return bytes.flip();
    }
    public static void putUuid(ByteBuffer bytes,UUID uuid) {
        bytes.putLong(uuid.getMostSignificantBits()).putLong(uuid.getLeastSignificantBits());
    }
    private static UUID uuid(ByteBuffer bytes) { return new UUID(bytes.getLong(),bytes.getLong()); }
    private static Vec3 vector(ByteBuffer bytes) { return new Vec3(bytes.getFloat(),bytes.getFloat(),bytes.getFloat()); }
    private static boolean finite(Vec3 value) {
        return Double.isFinite(value.x) && Double.isFinite(value.y) && Double.isFinite(value.z)
            && Math.abs(value.x)<=3e7 && Math.abs(value.y)<=3e7 && Math.abs(value.z)<=3e7;
    }

    /** Called by the collision consumer. No access to levels or entities here. */
    public static synchronized void accept(MemorySegment bytes,long offset,int size) {
        if (size!=88 || offset<0 || offset>bytes.byteSize()-size) return;
        var current=session;
        if (current==null || !transport.active() || transport.epoch()!=current.epoch()
            || transport.generation()!=current.generation()) return;
        var data=bytes.asSlice(offset,size).asByteBuffer().order(ByteOrder.LITTLE_ENDIAN);
        int epoch=data.getInt(),id=data.getInt(); long serial=data.getLong();
        var identity=new Identity(uuid(data),uuid(data));
        float damage=data.getFloat(); int kind=data.getInt(),attacker=data.getInt(),flags=data.getInt();
        var from=vector(data); var point=vector(data);
        if (epoch!=current.epoch() || !identity.session().equals(current.uuid()) || id<=0
            || serial<=0 || serial>0x1fffffffffffffL || !Float.isFinite(damage) || damage<=0 || damage>1e6F
            || kind<0 || kind>3 || attacker<0 || !finite(from) || !finite(point)) return;
        var receipt=new Receipt(epoch,id,serial,identity,damage,kind,attacker,flags,from,point);
        var previous=pending.get(serial);
        var applied=completed.get(serial);
        if (previous!=null && !previous.equals(receipt) || applied!=null && !applied.equals(receipt)) return;
        if (pending.size()<512) pending.putIfAbsent(serial,receipt);
    }

    private static void drain(ServerLevel level,Session current) {
        for (var iterator=pending.entrySet().iterator();iterator.hasNext();) {
            var receipt=iterator.next().getValue();
            if (!completed.containsKey(receipt.serial()) && receipt.serial()>completedFloor) {
                var entity=level.getEntity(receipt.entityId());
                if (entity instanceof LivingEntity living && eligible(living)
                        && living.getUUID().equals(receipt.identity().entity())) {
                    var attacker=SkyCombat.proxy(receipt.attacker());
                    if (attacker!=null && (attacker.level()!=level || !attacker.isAlive()
                        || attacker.distanceToSqr(living)>128*128)) attacker=null;
                    var sources=level.damageSources();
                    var unknown=new DamageSource(level.registryAccess().lookupOrThrow(Registries.DAMAGE_TYPE)
                        .getOrThrow(OBSERVED_DAMAGE));
                    var source=switch(receipt.kind()) {
                        case 1 -> attacker!=null ? sources.mobAttack(attacker) : unknown;
                        case 2 -> attacker!=null ? sources.mobProjectile(attacker,attacker) : unknown;
                        case 3 -> attacker!=null ? sources.indirectMagic(attacker,attacker) : sources.magic();
                        default -> unknown; // observed HP loss lacks exact type/attacker
                    };
                    living.hurtServer(level,source,receipt.damage());
                }
                // Missing/dead/reused entities are terminal receipts too. Never hurt their replacements.
                completed.put(receipt.serial(),receipt);
                if (completed.size()>4096) {
                    long oldest=completed.keySet().stream().min(Long::compare).orElseThrow();
                    completed.remove(oldest); completedFloor=Math.max(completedFloor,oldest);
                }
            }
            var ack=ByteBuffer.allocate(48).order(ByteOrder.LITTLE_ENDIAN);
            ack.putInt(current.epoch()).putInt(receipt.entityId()).putLong(receipt.serial());
            putUuid(ack,current.uuid()); putUuid(ack,receipt.identity().entity()); ack.flip();
            if (transport.send(Kcd2Protocol.REN_MOB_DAMAGE_ACK,ack)) iterator.remove();
        }
    }
    public static synchronized void reset() {
        revision++; session=null; sequence=0; completedFloor=0;
        pending.clear(); completed.clear(); published.clear();
    }
    public static synchronized void isolatedTransport(@Nullable Transport replacement) {
        if (!Boolean.getBoolean("skycraft.isolatedTest")) throw new IllegalStateException("isolated runtime required");
        reset(); transport=replacement==null ? LIVE : replacement;
    }
    private NativeMobBridge() {}
}
