package dev.skycraft.combat;

import dev.skycraft.link.Kcd2Protocol;
import dev.skycraft.link.SkyLink;
import dev.skycraft.world.SkyCollision;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;
import static java.lang.foreign.ValueLayout.*;

/** Collision approval before vanilla applies a native-target hit; the consumer never touches entities. */
public final class NativeCombatHandshake {
    public static final int CLEAR=1, WORLD=2, EQUIPMENT=3, REJECTED=4;
    public record Reply(int epoch,int request,int target,int decision,float distance,Vec3 point,Vec3 normal,long session,int generation) {
        public boolean clear() { return decision==CLEAR; }
    }
    private record Pending(int epoch,int target,long session,int generation,CompletableFuture<Reply> future) {}
    private record Context(int target,Reply reply) {}
    private static final ConcurrentHashMap<Integer,Pending> PENDING=new ConcurrentHashMap<>();
    private static final AtomicInteger NEXT=new AtomicInteger();
    private static final AtomicLong SESSION=new AtomicLong();
    private static final ThreadLocal<Context> CONTEXT=new ThreadLocal<>();
    private static final ThreadLocal<Long> DEADLINE=new ThreadLocal<>();
    public interface Transport {
        boolean active(); int epoch(); boolean send(ByteBuffer bytes);
        default int generation() { return 0; }
    }
    private static final Transport LIVE=new Transport() {
        public boolean active() { return Kcd2Protocol.ENABLED && SkyLink.active(); }
        public int epoch() { return SkyCollision.epoch(); }
        public int generation() { return SkyLink.generation(); }
        public boolean send(ByteBuffer bytes) { return SkyLink.tryWriteRender(Kcd2Protocol.REN_ATTACK_PROBE,bytes,null); }
    };
    private static volatile Transport transport=LIVE;

    public static boolean active() { return transport.active() && transport.epoch()>=0; }
    /** All native probes in one server tick share a single bounded wait budget. */
    public static void beginTick() { DEADLINE.set(Long.MAX_VALUE); }
    public static int approval(int target) {
        var context=CONTEXT.get();
        return context!=null && context.target()==target && valid(context.reply()) ? context.reply().request() : 0;
    }
    public static boolean valid(Reply reply) {
        return reply.clear() && current(reply);
    }
    public static boolean current(Reply reply) {
        return reply.session()==SESSION.get() && active() && reply.epoch()==transport.epoch()
            && reply.generation()==transport.generation();
    }
    public static @Nullable Reply context(int target) {
        var context=CONTEXT.get();
        return context!=null && context.target()==target ? context.reply() : null;
    }
    public static void withReply(Reply reply,Runnable action) {
        var previous=CONTEXT.get();
        CONTEXT.set(new Context(reply.target(),reply));
        try { action.run(); }
        finally { if (previous==null) CONTEXT.remove(); else CONTEXT.set(previous); }
    }

    public static Reply probe(int target,NativeAttackTrace trace) {
        Transport wire=transport;
        int epoch=wire.epoch();
        if (!wire.active() || epoch<0 || PENDING.size()>=64) return rejected(epoch,0,target,trace.point());
        int request=NEXT.updateAndGet(value->value==Integer.MAX_VALUE ? 1 : value+1);
        var pending=new Pending(epoch,target,SESSION.get(),wire.generation(),new CompletableFuture<>());
        if (PENDING.putIfAbsent(request,pending)!=null) return rejected(epoch,0,target,trace.point());
        try {
            if (!wire.send(trace.transaction(request,epoch,target,0,0,0))) return rejected(epoch,request,target,trace.point());
            // Only the MC server waits, for one native frame at most 50ms. The
            // independent collision consumer delivers the reply. Neither thread
            // holds render-ring, engine, world or entity locks during the wait.
            Long deadline=DEADLINE.get();
            if (deadline==null || deadline==Long.MAX_VALUE) {
                deadline=System.nanoTime()+TimeUnit.MILLISECONDS.toNanos(50);
                DEADLINE.set(deadline);
            }
            long remaining=Math.max(0,deadline-System.nanoTime());
            var reply=pending.future().get(remaining,TimeUnit.NANOSECONDS);
            if (wire!=transport || !wire.active() || wire.epoch()!=epoch || pending.session()!=SESSION.get()
                || wire.generation()!=pending.generation())
                return rejected(epoch,request,target,trace.point());
            return reply;
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return rejected(epoch,request,target,trace.point());
        } catch (java.util.concurrent.TimeoutException | java.util.concurrent.ExecutionException | RuntimeException error) {
            return rejected(epoch,request,target,trace.point());
        } finally { PENDING.remove(request,pending); }
    }
    private static Reply rejected(int epoch,int request,int target,Vec3 point) {
        return new Reply(epoch,request,target,REJECTED,0,point,Vec3.ZERO,SESSION.get(),transport.generation());
    }

    public static void accept(MemorySegment bytes,long offset,int size) {
        if (size!=44 || offset<0 || offset>bytes.byteSize()-size) return;
        int epoch=bytes.get(JAVA_INT_UNALIGNED,offset),request=bytes.get(JAVA_INT_UNALIGNED,offset+4),target=bytes.get(JAVA_INT_UNALIGNED,offset+8);
        int decision=bytes.get(JAVA_INT_UNALIGNED,offset+12);
        var pending=PENDING.get(request);
        if (pending==null || epoch!=pending.epoch() || target!=pending.target() || decision<CLEAR || decision>REJECTED) return;
        float distance=bytes.get(JAVA_FLOAT_UNALIGNED,offset+16);
        Vec3 point=vector(bytes,offset+20),normal=vector(bytes,offset+32);
        if (!Float.isFinite(distance) || distance<0 || !finite(point) || !finite(normal)) return;
        pending.future().complete(new Reply(epoch,request,target,decision,distance,point,normal,pending.session(),pending.generation()));
    }
    private static Vec3 vector(MemorySegment bytes,long offset) {
        return new Vec3(bytes.get(JAVA_FLOAT_UNALIGNED,offset),bytes.get(JAVA_FLOAT_UNALIGNED,offset+4),bytes.get(JAVA_FLOAT_UNALIGNED,offset+8));
    }
    private static boolean finite(Vec3 value) {
        return Double.isFinite(value.x) && Double.isFinite(value.y) && Double.isFinite(value.z);
    }
    public static void reset() {
        SESSION.incrementAndGet();
        PENDING.forEach((id,pending)->pending.future().complete(rejected(pending.epoch(),id,pending.target(),Vec3.ZERO)));
        PENDING.clear();
    }
    /** Test-only injection is guarded; installed clients always use the shared-memory transport. */
    public static void isolatedTransport(@Nullable Transport replacement) {
        if (!Boolean.getBoolean("skycraft.isolatedTest")) throw new IllegalStateException("isolated runtime required");
        reset(); transport=replacement==null ? LIVE : replacement;
    }
    private NativeCombatHandshake() {}
}
