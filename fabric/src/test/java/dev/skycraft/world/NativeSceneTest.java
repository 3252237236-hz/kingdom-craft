package dev.skycraft.world;

import dev.skycraft.link.Proto;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.reflect.Method;
import java.util.ArrayList;
import net.minecraft.core.BlockPos;
import net.minecraft.world.phys.AABB;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

class NativeSceneTest {
    private static final BlockPos POS=new BlockPos(-2,10,-3);

    @BeforeEach void reset() throws Exception {
        Method clear=SkyCollision.class.getDeclaredMethod("clear",int.class);
        clear.setAccessible(true); clear.invoke(null,42);
    }

    private void sample(long[] layers,float[] triangle,int epoch) {
        int count=triangle==null ? 0 : 1;
        int bytes=Proto.COL_REGION_HEADER_BYTES+Proto.COL_BLOCK_BYTES+count*Proto.COL_TRI_BYTES;
        try (Arena arena=Arena.ofConfined()) {
            MemorySegment packet=arena.allocate(bytes);
            int[] header={-2,10,-3,-2,10,-3,epoch,count};
            for (int i=0;i<header.length;i++) packet.set(ValueLayout.JAVA_INT,i*4L,header[i]);
            int block=Proto.COL_REGION_HEADER_BYTES;
            packet.set(ValueLayout.JAVA_INT,block,-2);
            packet.set(ValueLayout.JAVA_INT,block+4,10);
            packet.set(ValueLayout.JAVA_INT,block+8,-3);
            for (int i=0;i<8;i++) packet.set(ValueLayout.JAVA_LONG,block+16+i*8L,layers[i]);
            if (triangle!=null) for (int i=0;i<9;i++)
                packet.set(ValueLayout.JAVA_FLOAT,block+Proto.COL_BLOCK_BYTES+i*4L,triangle[i]);
            NativeScene.accept(packet,0,bytes);
        }
    }

    @Test void nativeWallIsSharedByCollisionAndProjectiles() {
        long[] layers=new long[8];
        for (int y=0;y<8;y++) for (int z=0;z<8;z++) layers[y]|=1L<<(3+8*z);
        float[] wall={-1.5f,10,-3,-1.5f,11,-3,-1.5f,11,-2};
        sample(layers,wall,42);
        assertTrue(NativeScene.known(POS));
        var shape=SkyCollision.shapeAt(POS);
        assertNotNull(shape);
        assertEquals(0.375,shape.min(net.minecraft.core.Direction.Axis.X));
        assertEquals(0.5,shape.max(net.minecraft.core.Direction.Axis.X));
        var triangles=new ArrayList<SkyTri>();
        SkyCollision.trianglesNear(new AABB(-2,10,-3,-1,11,-2),triangles);
        assertNotNull(SkyRay.cast(triangles,-1.9,10.75,-2.75,-1.1,10.75,-2.75));
        assertEquals(0,SkyCollision.groundTop(POS));
    }

    @Test void ceilingBlocksBodiesWithoutBecomingFloor() {
        long[] layers=new long[8]; layers[4]=-1L;
        sample(layers,new float[]{-2,10.5f,-3,-1,10.5f,-3,-1,10.5f,-2},42);
        assertNotNull(SkyCollision.shapeAt(POS));
        assertEquals(0,SkyCollision.groundTop(POS));
        assertFalse(SkyCollision.supportsFromBelow(POS.above()));
    }

    @Test void upwardSurfaceSupportsBuildingAndMovement() {
        long[] layers=new long[8]; layers[3]=-1L;
        sample(layers,new float[]{-2,10.5f,-3,-1,10.5f,-2,-1,10.5f,-3},42);
        assertEquals(0.5,SkyCollision.groundTop(POS));
        assertTrue(SkyCollision.supportsFromBelow(POS.above()));
        assertEquals(10.5,SkyCollision.groundBelow(-1.5,10.5,-2.5,0.2,1,2));
    }

    @Test void doorOpeningClearsItsOldSurface() {
        long[] full=new long[8]; java.util.Arrays.fill(full,-1L);
        sample(full,null,42);
        assertNotNull(SkyCollision.shapeAt(POS));
        sample(new long[8],null,42);
        assertTrue(NativeScene.known(POS));
        assertNull(SkyCollision.shapeAt(POS));
    }

    @Test void reloadRejectsLateOldWorldSamples() {
        long[] full=new long[8]; java.util.Arrays.fill(full,-1L);
        sample(full,null,41);
        assertFalse(NativeScene.known(POS));
        sample(full,null,42);
        assertNotNull(SkyCollision.shapeAt(POS));
        NativeScene.clear(43);
        assertNull(SkyCollision.shapeAt(POS));
        sample(full,null,42);
        assertFalse(NativeScene.known(POS));
    }

    @Test void malformedCoordinatesCannotReplaceGeometry() {
        long[] full=new long[8]; java.util.Arrays.fill(full,-1L);
        sample(full,new float[]{Float.NaN,10,-3,-2,11,-3,-1,11,-3},42);
        assertFalse(NativeScene.known(POS));
        sample(full,new float[]{20,10,-3,-2,11,-3,-1,11,-3},42);
        assertFalse(NativeScene.known(POS));
    }

    @Test void emptySceneRefreshPreservesOriginalGround() throws Exception {
        var field=SkyCollision.class.getDeclaredField("SHAPES");
        field.setAccessible(true);
        @SuppressWarnings("unchecked")
        var ground=(java.util.Map<Long,net.minecraft.world.phys.shapes.VoxelShape>)field.get(null);
        var floor=net.minecraft.world.phys.shapes.Shapes.box(0,0,0,1,.25,1);
        ground.put(POS.asLong(),floor);
        long[] full=new long[8]; java.util.Arrays.fill(full,-1L);
        sample(full,null,42);
        assertEquals(1,SkyCollision.shapeAt(POS).max(net.minecraft.core.Direction.Axis.Y));
        sample(new long[8],null,42);
        assertSame(floor,SkyCollision.shapeAt(POS));
    }

    @Test void truncatedPacketsAndNegativeOffsetsCannotReplaceGeometry() {
        long[] full=new long[8]; java.util.Arrays.fill(full,-1L);
        sample(full,null,42);
        var previous=SkyCollision.shapeAt(POS);
        try (Arena arena=Arena.ofConfined()) {
            MemorySegment packet=arena.allocate(8);
            assertDoesNotThrow(() -> NativeScene.accept(packet,0,200));
            assertDoesNotThrow(() -> NativeScene.accept(packet,-1,200));
            assertDoesNotThrow(() -> NativeScene.accept(packet,20,200));
        }
        assertSame(previous,SkyCollision.shapeAt(POS));
    }
}
