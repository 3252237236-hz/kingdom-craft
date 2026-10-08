package dev.skycraft.world;

import dev.skycraft.link.Kcd2Protocol;
import dev.skycraft.link.Proto;
import dev.skycraft.link.SkyLink;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import net.fabricmc.fabric.api.event.lifecycle.v1.ServerTickEvents;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;

/**
 * One native scene cache used by movement, support, picks, projectiles and environment queries.
 * Samples are bounded approximations of native collision surfaces, not editable Minecraft blocks.
 */
public final class NativeScene {
    private static final int MAX_PENDING = 512;
    private static final Map<Long, Cell> CELLS = new ConcurrentHashMap<>();
    private static final Map<Long, Long> REQUESTS = new ConcurrentHashMap<>();
    private static final Set<Long> SERVER_CHANGED = ConcurrentHashMap.newKeySet();
    private static final Set<Long> CLIENT_CHANGED = ConcurrentHashMap.newKeySet();
    private record Anchor(double x, double y, double z, int epoch) {}
    private static volatile Anchor anchor;
    private static volatile int epoch = -1;
    private static int frames;

    private record Combined(VoxelShape ground, VoxelShape merged) {}
    private static final class Cell {
        final VoxelShape shape;
        final SkyTri[] triangles;
        final float floor;
        final int hash;
        final long received;
        volatile Combined combined;
        Cell(VoxelShape shape, SkyTri[] triangles, int hash, int baseY) {
            this.shape = shape; this.triangles = triangles; this.hash = hash;
            this.received = System.currentTimeMillis();
            float top = 0;
            for (SkyTri triangle : triangles)
                if (triangle.ny >= SkyTri.WALKABLE_NY)
                    top = Math.max(top, (float)(triangle.maxY - baseY));
            this.floor = Math.min(1, top);
        }
    }

    private NativeScene() {}

    public static void init() {
        ServerTickEvents.END_SERVER_TICK.register(server -> {
            if (!SkyLink.active() || server.getPlayerList().getPlayers().isEmpty()) return;
            var level = server.getPlayerList().getPlayers().getFirst().level();
            int processed = 0;
            for (long key : SERVER_CHANGED) {
                if (++processed > 32) break;
                SERVER_CHANGED.remove(key);
                BlockPos pos = BlockPos.of(key);
                if (!level.hasChunkAt(pos)) continue;
                level.updateNeighborsAt(pos, Blocks.AIR);
                for (Direction direction : Direction.values()) {
                    BlockPos neighbour = pos.relative(direction);
                    if (!level.hasChunkAt(neighbour)) continue;
                    var fluid = level.getFluidState(neighbour);
                    if (!fluid.isEmpty()) level.scheduleTick(neighbour, fluid.getType(), 1);
                }
                var fluid = level.getFluidState(pos);
                if (!fluid.isEmpty()) level.scheduleTick(pos, fluid.getType(), 1);
            }
        });
    }

    public static void clear(int newEpoch) {
        epoch = newEpoch;
        CELLS.clear(); REQUESTS.clear(); SERVER_CHANGED.clear(); CLIENT_CHANGED.clear();
    }

    private static boolean inRange(BlockPos pos, Anchor a) {
        double dx = pos.getX()+0.5-a.x(), dz = pos.getZ()+0.5-a.z();
        return dx*dx+dz*dz <= 169 && Math.abs(pos.getY()+0.5-a.y()) <= 9;
    }

    public static void request(BlockPos pos) {
        Anchor a = anchor;
        if (!Kcd2Protocol.ENABLED || a == null || a.epoch()!=epoch || !inRange(pos,a)) return;
        long key = pos.asLong(), now = System.currentTimeMillis();
        Cell cell = CELLS.get(key);
        if (cell != null && now-cell.received < 2000) return;
        if (REQUESTS.size() < MAX_PENDING || REQUESTS.containsKey(key))
            REQUESTS.putIfAbsent(key, 0L);
    }

    /** Only the client thread writes request batches to the transport. */
    public static void pump(double x,double y,double z,int currentEpoch) {
        if (!Kcd2Protocol.ENABLED || currentEpoch != epoch) return;
        anchor = new Anchor(x,y,z,currentEpoch);
        if (++frames % 60 == 0) {
            CELLS.keySet().removeIf(key -> !inRange(BlockPos.of(key),anchor));
            REQUESTS.keySet().removeIf(key -> !inRange(BlockPos.of(key),anchor));
        }
        long now = System.currentTimeMillis();
        var batch = REQUESTS.entrySet().stream()
            .filter(entry -> now-entry.getValue() >= 1000)
            .sorted(java.util.Comparator.comparingDouble(entry -> {
                BlockPos pos = BlockPos.of(entry.getKey());
                return Math.pow(pos.getX()+0.5-x,2)+Math.pow(pos.getY()+0.5-y,2)+Math.pow(pos.getZ()+0.5-z,2);
            })).limit(16).toList();
        if (batch.isEmpty()) return;
        ByteBuffer packet = ByteBuffer.allocate(8+batch.size()*12).order(ByteOrder.LITTLE_ENDIAN);
        packet.putInt(currentEpoch).putInt(batch.size());
        for (var entry : batch) {
            BlockPos pos = BlockPos.of(entry.getKey());
            packet.putInt(pos.getX()).putInt(pos.getY()).putInt(pos.getZ());
        }
        packet.flip();
        if (SkyLink.tryWriteRender(Kcd2Protocol.REN_SCENE_REQUEST,packet,null))
            for (var entry : batch) REQUESTS.replace(entry.getKey(),entry.getValue(),now);
    }

    /** Packet: ColRegion (one cell, count=triangles), ColBlock, count ColTri records. */
    static void accept(MemorySegment segment,long offset,int bytes) {
        if (offset < 0 || bytes < Proto.COL_REGION_HEADER_BYTES+Proto.COL_BLOCK_BYTES
            || offset > segment.byteSize() || bytes > segment.byteSize()-offset) return;
        int x=segment.get(ValueLayout.JAVA_INT,offset),
            y=segment.get(ValueLayout.JAVA_INT,offset+4),
            z=segment.get(ValueLayout.JAVA_INT,offset+8);
        int messageEpoch=segment.get(ValueLayout.JAVA_INT,offset+24);
        int count=segment.get(ValueLayout.JAVA_INT,offset+28);
        if (messageEpoch!=epoch || count<0 || count>256
            || bytes!=Proto.COL_REGION_HEADER_BYTES+Proto.COL_BLOCK_BYTES+count*Proto.COL_TRI_BYTES
            || segment.get(ValueLayout.JAVA_INT,offset+12)!=x
            || segment.get(ValueLayout.JAVA_INT,offset+16)!=y
            || segment.get(ValueLayout.JAVA_INT,offset+20)!=z) return;
        long block=offset+Proto.COL_REGION_HEADER_BYTES;
        if (segment.get(ValueLayout.JAVA_INT,block)!=x
            || segment.get(ValueLayout.JAVA_INT,block+4)!=y
            || segment.get(ValueLayout.JAVA_INT,block+8)!=z) return;
        var triangles = new java.util.ArrayList<SkyTri>(count);
        long cursor=block+Proto.COL_BLOCK_BYTES;
        for (int i=0;i<count;i++,cursor+=Proto.COL_TRI_BYTES) {
            float[] vertices=new float[9];
            for (int k=0;k<9;k++) {
                float value=segment.get(ValueLayout.JAVA_FLOAT,cursor+k*4L);
                int base=k%3==0 ? x : k%3==1 ? y : z;
                if (!Float.isFinite(value) || value<base-0.003 || value>base+1.003) return;
                vertices[k]=value;
            }
            int flags=segment.get(ValueLayout.JAVA_INT,cursor+36)&Proto.TRI_TERRAIN;
            SkyTri triangle=new SkyTri(vertices,0,flags);
            if (!triangle.degenerate()) triangles.add(triangle);
        }
        VoxelShape shape=SkyCollision.buildShape(segment,block+16);
        int hash=Arrays.hashCode(segment.asSlice(block,bytes-Proto.COL_REGION_HEADER_BYTES)
            .toArray(ValueLayout.JAVA_BYTE));
        long key=BlockPos.asLong(x,y,z);
        Cell previous=CELLS.put(key,new Cell(shape,triangles.toArray(SkyTri[]::new),hash,y));
        REQUESTS.remove(key);
        if (previous==null || previous.hash!=hash) {
            SERVER_CHANGED.add(key); CLIENT_CHANGED.add(key);
        }
    }

    public static VoxelShape merge(BlockPos pos,VoxelShape ground) {
        request(pos);
        Cell cell=CELLS.get(pos.asLong());
        if (cell==null || cell.shape==null) return ground;
        if (ground==null) return cell.shape;
        Combined cached=cell.combined;
        if (cached==null || cached.ground()!=ground) {
            cached=new Combined(ground,Shapes.or(ground,cell.shape));
            cell.combined=cached;
        }
        return cached.merged();
    }

    public static boolean known(BlockPos pos) {
        request(pos);
        return CELLS.containsKey(pos.asLong());
    }

    public static boolean active() { return !CELLS.isEmpty(); }

    public static float floor(BlockPos pos) {
        request(pos);
        Cell cell=CELLS.get(pos.asLong());
        return cell==null ? 0 : cell.floor;
    }

    public static void trianglesNear(AABB box,List<SkyTri> out) {
        if (anchor==null && CELLS.isEmpty()) return;
        int x0=(int)Math.floor(box.minX),x1=(int)Math.floor(box.maxX);
        int y0=(int)Math.floor(box.minY),y1=(int)Math.floor(box.maxY);
        int z0=(int)Math.floor(box.minZ),z1=(int)Math.floor(box.maxZ);
        Anchor a=anchor;
        if (a!=null) {
            x0=Math.max(x0,(int)Math.floor(a.x()-13)); x1=Math.min(x1,(int)Math.floor(a.x()+13));
            y0=Math.max(y0,(int)Math.floor(a.y()-9)); y1=Math.min(y1,(int)Math.floor(a.y()+9));
            z0=Math.max(z0,(int)Math.floor(a.z()-13)); z1=Math.min(z1,(int)Math.floor(a.z()+13));
        }
        int budget=512;
        for (int y=y0;y<=y1;y++) for (int x=x0;x<=x1;x++) for (int z=z0;z<=z1;z++) {
            BlockPos pos=new BlockPos(x,y,z);
            if (budget-->0) request(pos);
            Cell cell=CELLS.get(pos.asLong());
            if (cell==null) continue;
            for (SkyTri triangle : cell.triangles)
                if (triangle.maxX>=box.minX && triangle.minX<=box.maxX
                    && triangle.maxY>=box.minY && triangle.minY<=box.maxY
                    && triangle.maxZ>=box.minZ && triangle.minZ<=box.maxZ) out.add(triangle);
        }
    }

    public static void takeClientChanges(java.util.function.LongConsumer consumer) {
        int processed=0;
        for (long key : CLIENT_CHANGED) {
            if (++processed>32) break;
            CLIENT_CHANGED.remove(key); consumer.accept(key);
        }
    }
}
