package dev.skycraft.client;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.VarHandle;

/** Camera pose published by the KCD2 host in Minecraft coordinates. */
public final class HostCamera {
	private static final int MAGIC = 0x4332434B;
	private static final String NAME = "Local\\SkyCraftKCD2Camera";
	private static final long BYTES = 88;
	private static final VarHandle SEQUENCE = ValueLayout.JAVA_LONG.varHandle();
	private static final ValueLayout.OfInt INT = ValueLayout.JAVA_INT_UNALIGNED;
	private static final ValueLayout.OfLong LONG = ValueLayout.JAVA_LONG_UNALIGNED;
	private static final ValueLayout.OfDouble DOUBLE = ValueLayout.JAVA_DOUBLE_UNALIGNED;
	private static final ValueLayout.OfFloat FLOAT = ValueLayout.JAVA_FLOAT_UNALIGNED;
	private static SharedMemory mapping;
	private static long nextOpenAttempt;
	private static Pose frame;

	public record Pose(long hostFrame, double x, double y, double z,
		float yaw, float pitch, float roll, float fov, boolean firstPerson) {
	}

	private HostCamera() {
	}

	public static Pose frame() {
		return frame;
	}

	public static boolean vanillaOwned() {
		return SkyClient.linked() && (SkyClient.sky().flags & dev.skycraft.link.Proto.SKY_MC_CAMERA) != 0;
	}

	public static void renderedVanilla(net.minecraft.client.Camera camera) {
		Pose nativePose = frame;
		if (nativePose == null) return;
		var position = camera.position();
		frame = new Pose(nativePose.hostFrame(),position.x,position.y,position.z,
			camera.yRot(),camera.xRot(),0,nativePose.fov(),!camera.isDetached());
	}

	public static void beginFrame() {
		frame = null;
		if (!PassthroughFrameExporter.enabled()) {
			return;
		}
		if (mapping == null) {
			long now = System.nanoTime();
			if (now < nextOpenAttempt) {
				return;
			}
			nextOpenAttempt = now + 1_000_000_000L;
			try {
				mapping = SharedMemory.openReadOnly(NAME, BYTES);
			} catch (Throwable ignored) {
				return;
			}
			if (mapping == null) {
				return;
			}
		}
		MemorySegment m = mapping.segment;
		if (m.get(INT, 0) != MAGIC || m.get(INT, 4) != 1) {
			return;
		}
		for (int attempt = 0; attempt < 3; attempt++) {
			long sequence = (long)SEQUENCE.getAcquire(m, 8L);
			if ((sequence & 1L) != 0L || sequence == 0L) {
				continue;
			}
			long hostFrame = m.get(LONG, 16);
			double x = m.get(DOUBLE, 24);
			double y = m.get(DOUBLE, 32);
			double z = m.get(DOUBLE, 40);
			float yaw = m.get(FLOAT, 48);
			float pitch = m.get(FLOAT, 52);
			float roll = m.get(FLOAT, 56);
			float fov = m.get(FLOAT, 60);
			int flags = m.get(INT, 80);
			VarHandle.loadLoadFence();
			if ((long)SEQUENCE.getAcquire(m, 8L) != sequence) {
				continue;
			}
			if ((flags & 1) != 0 && Double.isFinite(x) && Double.isFinite(y)
				&& Double.isFinite(z) && Float.isFinite(fov) && fov >= 20 && fov <= 130) {
				frame = new Pose(hostFrame, x, y, z, yaw, pitch, roll, fov, (flags & 2) != 0);
			}
			return;
		}
	}
}
