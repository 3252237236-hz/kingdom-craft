package dev.skycraft.world;

import dev.skycraft.link.Proto;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.reflect.Method;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import static org.junit.jupiter.api.Assertions.*;

/** Exercises actual column-wire decoding used by client and integrated server. */
class Kcd2GroundSupportTest {
    @BeforeEach void clear() throws Exception {
        Method clear = SkyCollision.class.getDeclaredMethod("clear", int.class);
        clear.setAccessible(true);
        clear.invoke(null, 42);
    }

    private void column(int x, int y, int z, int layers, int epoch) throws Exception {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment packet = arena.allocate(Proto.COL_REGION_HEADER_BYTES + Proto.COL_BLOCK_BYTES);
            int[] header = {x, y - 8, z, x, y + 2, z, epoch, 1};
            for (int i = 0; i < header.length; i++) packet.set(ValueLayout.JAVA_INT, i * 4L, header[i]);
            long block = Proto.COL_REGION_HEADER_BYTES;
            packet.set(ValueLayout.JAVA_INT, block, x);
            packet.set(ValueLayout.JAVA_INT, block + 4, y);
            packet.set(ValueLayout.JAVA_INT, block + 8, z);
            for (int layer = 0; layer < layers; layer++)
                packet.set(ValueLayout.JAVA_LONG, block + 16 + layer * 8L, -1L);
            Method read = SkyCollision.class.getDeclaredMethod("readRegion", MemorySegment.class, long.class);
            read.setAccessible(true);
            read.invoke(null, packet, 0L);
        }
    }

    @Test void roundedVoxelSurfaceCanLiftNativeFeetOut() throws Exception {
        column(10, 95, -20, 6, 42);
        assertEquals(95.75, SkyCollision.groundBelow(10.5, 95.647, -19.5, 0.3, 2.5, 4.0));
        // The higher ready region is unnecessary when the footprint has support.
        assertFalse(SkyCollision.isKnown(10, 104, -20));
    }

    @Test void nearbyColumnCannotReleasePlayerOverUnknownVoid() throws Exception {
        column(11, 95, -20, 6, 42);
        assertTrue(Double.isNaN(SkyCollision.groundBelow(10.5, 95.647, -19.5, 0.3, 2.5, 4.0)));
    }

    @Test void footprintsCrossingNegativeCoordinateBoundarySeeBothCells() throws Exception {
        column(-1, 95, -1, 4, 42);
        column(0, 95, -1, 6, 42);
        assertEquals(95.75, SkyCollision.groundBelow(-0.1, 95.6, -0.5, 0.3, 2.5, 4.0));
    }

    @Test void staleColumnsAfterReloadCannotSupportNewSession() throws Exception {
        column(10, 95, -20, 6, 41);
        assertEquals(42, SkyCollision.epoch());
        assertTrue(Double.isNaN(SkyCollision.groundBelow(10.5, 95.647, -19.5, 0.3, 2.5, 4.0)));
    }
}
