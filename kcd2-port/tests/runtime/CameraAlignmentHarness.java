package skycraft.test;

import dev.skycraft.client.HostCamera;
import dev.skycraft.client.SkyClient;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import org.joml.Matrix4f;

/** Runs against actual transformed Minecraft classes without attaching to the user's game. */
final class CameraAlignmentHarness {
    private static int checks;
    private static void check(boolean result, String message) {
        if (!result) throw new AssertionError(message);
        checks++;
        System.out.println("CAMERA_RUNTIME_PASS: "+message);
    }

    static void run(Minecraft mc) throws Exception {
        var linked=SkyClient.class.getDeclaredField("linked");linked.setAccessible(true);
        var frame=HostCamera.class.getDeclaredField("frame");frame.setAccessible(true);
        boolean oldLinked=linked.getBoolean(null);
        Object oldFrame=frame.get(null);
        Camera camera=mc.gameRenderer.mainCamera();
        var align=Camera.class.getDeclaredMethod("alignWithEntity",float.class);align.setAccessible(true);
        var fov=Camera.class.getDeclaredMethod("calculateFov",float.class);fov.setAccessible(true);
        try {
            var old=camera.position();
            var first=new HostCamera.Pose(777,old.x,old.y,old.z,180,0,17,73,true);
            frame.set(null,first);
            align.invoke(camera,1.f);
            var before=camera.position();
            check(before.distanceTo(old)<1e-8,"render camera uses the supplied world position without scaling");
            frame.set(null,new HostCamera.Pose(778,old.x+2,old.y,old.z,180,0,17,73,true));
            align.invoke(camera,1.f);
            check(Math.abs(camera.position().x-before.x-2)<1e-8,
                "two native world units move the Minecraft render camera exactly two blocks");
            check(Math.abs((float)fov.invoke(camera,1.f)-73)<1e-6,
                "render camera preserves the calibrated vertical field of view");
            var angles=camera.rotation().getEulerAnglesYXZ(new org.joml.Vector3f());
            check(Math.abs(Math.toDegrees(angles.z)-17)<.001,
                "render camera preserves native roll");
            var projection=java.util.Arrays.stream(mc.gameRenderer.getClass().getDeclaredMethods())
                .filter(m->m.getName().contains("sharedWorldProjection")).findFirst().orElseThrow();
            projection.setAccessible(true);
            Matrix4f fakeBob=new Matrix4f().translate(.25f,.4f,0).rotateZ(.2f);
            linked.setBoolean(null,true);
            Object world=projection.invoke(mc.gameRenderer,fakeBob);
            check(world==mc.gameRenderer.gameRenderState().levelRenderState.cameraRenderState.projectionMatrix,
                "linked world bypasses the second walk-bob/hurt projection transform");
            linked.setBoolean(null,false);
            check(projection.invoke(mc.gameRenderer,fakeBob)==fakeBob,
                "standalone Minecraft retains its original world projection");
            System.out.println("CAMERA_RUNTIME_RESULT: PASSED ("+checks+" checks)");
        } finally {
            linked.setBoolean(null,oldLinked);frame.set(null,oldFrame);
            align.invoke(camera,1.f);
        }
    }
}
