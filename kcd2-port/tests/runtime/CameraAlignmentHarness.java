package skycraft.test;

import dev.skycraft.client.HostCamera;
import dev.skycraft.client.SkyClient;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import net.minecraft.client.CameraType;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.Blocks;
import dev.skycraft.client.InputBridge;
import dev.skycraft.link.Proto;
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
            vanillaAndInput(mc, camera, align, linked, frame);
            System.out.println("CAMERA_RUNTIME_RESULT: PASSED ("+checks+" checks)");
        } finally {
            linked.setBoolean(null,oldLinked);frame.set(null,oldFrame);
            align.invoke(camera,1.f);
        }
    }

    private static void vanillaAndInput(Minecraft mc, Camera camera,
            java.lang.reflect.Method align, java.lang.reflect.Field linked,
            java.lang.reflect.Field frame) throws Exception {
        var player = mc.player;
        if (player == null) throw new AssertionError("no local player for camera tests");
        var original = player.position();
        double ox=player.xo, oy=player.yo, oz=player.zo;
        float yaw=player.getYRot(),pitch=player.getXRot(),yawO=player.yRotO,pitchO=player.xRotO;
        var mode=mc.options.getCameraType();
        int flags=SkyClient.sky().flags, sourceW=SkyClient.sky().viewportW,sourceH=SkyClient.sky().viewportH;
        var takeover=SkyClient.class.getDeclaredField("tookOver");takeover.setAccessible(true);
        boolean oldTakeover=takeover.getBoolean(null);
        var grabbed=net.minecraft.client.MouseHandler.class.getDeclaredField("mouseGrabbed");grabbed.setAccessible(true);
        boolean oldGrabbed=grabbed.getBoolean(mc.mouseHandler);
        var eye=Camera.class.getDeclaredField("eyeHeight");eye.setAccessible(true);
        var eyeOld=Camera.class.getDeclaredField("eyeHeightOld");eyeOld.setAccessible(true);
        float oldEye=eye.getFloat(camera),oldEyeO=eyeOld.getFloat(camera);
        double fixtureX=(((int)Math.floor(original.x))&~15)+8.5;
        double fixtureZ=(((int)Math.floor(original.z))&~15)+8.5;
        BlockPos wall=new BlockPos((int)Math.floor(fixtureX),101,(int)Math.floor(fixtureZ)-2);
        var oldBlock=mc.level.getBlockState(wall);
        try {
            linked.setBoolean(null,true);takeover.setBoolean(null,true);
            SkyClient.sky().flags=Proto.SKY_MC_CAMERA;
            frame.set(null,new HostCamera.Pose(999,original.x,100,original.z,60,15,0,73,true));
            player.setPos(fixtureX,100,fixtureZ);
            player.xo=player.getX();player.yo=player.getY();player.zo=player.getZ();
            player.setYRot(60);player.yRotO=60;player.setXRot(15);player.xRotO=15;
            camera.setEntity(player);camera.setLevel(mc.level);
            eye.setFloat(camera,1.62f);eyeOld.setFloat(camera,1.62f);
            mc.options.setCameraType(CameraType.FIRST_PERSON);
            align.invoke(camera,1.f);
            var first=camera.position();
            check(!camera.isDetached() && Math.abs(first.y-101.62)<.001,
                "negotiated first-person camera uses the vanilla interpolated eye");
            mc.options.setCameraType(CameraType.THIRD_PERSON_BACK);align.invoke(camera,1.f);
            check(camera.isDetached() && camera.position().distanceTo(first)>3.9,
                "F5 rear camera remains behind the player instead of being overwritten");
            check(Math.abs(camera.yRot()-60)<.001 && Math.abs(camera.xRot()-15)<.001,
                "rear camera keeps the vanilla view angles");
            var rear=camera.position();
            player.setPos(player.getX()+2,100,player.getZ());player.xo=player.getX();
            align.invoke(camera,1.f);
            check(Math.abs(camera.position().x-rear.x-2)<1e-6,
                "third-person camera moves exactly two blocks with a two-meter player translation");
            mc.options.setCameraType(CameraType.THIRD_PERSON_FRONT);align.invoke(camera,1.f);
            check(Math.abs(camera.yRot()-240)<.001 && Math.abs(camera.xRot()+15)<.001,
                "F5 front camera reverses only the view yaw and pitch");
            check(player.getYRot()==60 && player.getXRot()==15,
                "front camera leaves attack direction and movement orientation unchanged");
            check(HostCamera.frame().x()==camera.position().x() && !HostCamera.frame().firstPerson(),
                "exported camera describes the actual third-person pose");
            player.setPos(fixtureX,100,fixtureZ);player.xo=player.getX();
            player.setYRot(0);player.yRotO=0;player.setXRot(0);player.xRotO=0;
            mc.options.setCameraType(CameraType.THIRD_PERSON_BACK);
            check(mc.level.setBlock(wall,Blocks.STONE.defaultBlockState(),3),"camera wall fixture is loaded");
            align.invoke(camera,1.f);
            var eyePosition=player.position().add(0,1.62,0);
            check(camera.position().distanceTo(eyePosition)<2,
                "vanilla third-person camera pulls in before a Minecraft wall");
            mc.level.setBlock(wall,oldBlock,3);
            var clear=dev.skycraft.world.SkyCollision.class.getDeclaredMethod("clear",int.class);
            clear.setAccessible(true);
            int oldEpoch=dev.skycraft.world.SkyCollision.epoch();
            try {
                clear.invoke(null,4242);
                var sample=SharedSceneHarness.class.getDeclaredMethod("sample",BlockPos.class,long[].class,float[][].class);
                sample.setAccessible(true);
                long[] full=new long[8];java.util.Arrays.fill(full,-1L);
                float nx=wall.getX(),ny=wall.getY(),nz=wall.getZ()+1;
                sample.invoke(null,wall,full,new float[][]{
                    {nx,ny,nz,nx+1,ny,nz,nx+1,ny+1,nz},
                    {nx,ny,nz,nx+1,ny+1,nz,nx,ny+1,nz}});
                align.invoke(camera,1.f);
                check(mc.level.getBlockState(wall).isAir() && camera.position().distanceTo(eyePosition)<2,
                    "third-person camera also pulls in against sampled native wall geometry");
            } finally { clear.invoke(null,oldEpoch); }
            var dispatch=InputBridge.class.getDeclaredMethod("dispatch",Minecraft.class,int.class,int.class,int.class,int.class,int.class);
            dispatch.setAccessible(true);
            SkyClient.sky().viewportW=1280;SkyClient.sky().viewportH=720;
            mc.mouseHandler.releaseMouse();
            dispatch.invoke(null,mc,Proto.IN_CURSOR,0,853,479,0);
            double x=mc.mouseHandler.getScaledXPos(mc.getWindow()),y=mc.mouseHandler.getScaledYPos(mc.getWindow());
            check(Math.abs(x-(853.5*mc.getWindow().getGuiScaledWidth()/1280-.5*mc.getWindow().getGuiScaledWidth()/mc.getWindow().getScreenWidth()))<1e-6,
                "host GUI cursor maps render pixels through the SDL window to GUI coordinates");
            mc.mouseHandler.onMove(mc.getWindow().handle(),0,0,99,99);
            check(x==mc.mouseHandler.getScaledXPos(mc.getWindow()) && y==mc.mouseHandler.getScaledYPos(mc.getWindow()),
                "hidden-window SDL motion cannot overwrite the host GUI cursor");
            mc.mouseHandler.handleAccumulatedMovement();
            mc.mouseHandler.grabMouse();
            check(mc.isWindowActive() && mc.mouseHandler.isMouseGrabbed(),
                "linked hidden client accepts vanilla look input without grabbing the host window");
            float before=player.getYRot();
            dispatch.invoke(null,mc,Proto.IN_MOUSE_MOTION,0,10,0,0);
            mc.mouseHandler.handleAccumulatedMovement();
            float after=player.getYRot();
            check(after>before,"captured relative mouse motion rotates the vanilla player");
            mc.mouseHandler.handleAccumulatedMovement();
            check(player.getYRot()==after,"the next frame does not repeat captured mouse motion");
            check(!InputBridge.replayingMouse(),"captured-input replay guard resets after dispatch");
        } finally {
            mc.level.setBlock(wall,oldBlock,3);
            player.setPos(original);player.xo=ox;player.yo=oy;player.zo=oz;
            player.setYRot(yaw);player.yRotO=yawO;player.setXRot(pitch);player.xRotO=pitchO;
            mc.options.setCameraType(mode);
            eye.setFloat(camera,oldEye);eyeOld.setFloat(camera,oldEyeO);
            SkyClient.sky().flags=flags;SkyClient.sky().viewportW=sourceW;SkyClient.sky().viewportH=sourceH;
            takeover.setBoolean(null,oldTakeover);grabbed.setBoolean(mc.mouseHandler,oldGrabbed);
        }
    }
}
