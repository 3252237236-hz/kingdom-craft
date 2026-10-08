package skycraft.test;

import dev.skycraft.world.NativeScene;
import dev.skycraft.world.SkyCollision;
import dev.skycraft.world.SkyClip;
import dev.skycraft.combat.SkyrimActorEntity;
import dev.skycraft.combat.SkyCombat;
import dev.skycraft.link.Proto;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.reflect.Method;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.world.entity.MoverType;
import net.minecraft.world.entity.item.ItemEntity;
import net.minecraft.world.entity.monster.zombie.Zombie;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.phys.Vec3;

/** Synthetic native packets in an actual Fabric server; never modifies the KCD2 save. */
public final class SharedSceneHarness {
    private static int checks;
    private static void check(boolean okay,String message) {
        if (!okay) throw new AssertionError(message);
        checks++; System.out.println("SCENE_RUNTIME_PASS: "+message);
    }

    private static void sample(BlockPos pos,long[] bits,float[][] triangles) throws Exception {
        int size=Proto.COL_REGION_HEADER_BYTES+Proto.COL_BLOCK_BYTES+triangles.length*Proto.COL_TRI_BYTES;
        try (Arena arena=Arena.ofConfined()) {
            MemorySegment packet=arena.allocate(size);
            int[] header={pos.getX(),pos.getY(),pos.getZ(),pos.getX(),pos.getY(),pos.getZ(),4242,triangles.length};
            for (int i=0;i<8;i++) packet.set(ValueLayout.JAVA_INT,i*4L,header[i]);
            int block=Proto.COL_REGION_HEADER_BYTES;
            for (int i=0;i<3;i++) packet.set(ValueLayout.JAVA_INT,block+i*4L,header[i]);
            for (int i=0;i<8;i++) packet.set(ValueLayout.JAVA_LONG,block+16+i*8L,bits[i]);
            for (int i=0;i<triangles.length;i++) for (int k=0;k<9;k++)
                packet.set(ValueLayout.JAVA_FLOAT,block+Proto.COL_BLOCK_BYTES+i*Proto.COL_TRI_BYTES+k*4L,triangles[i][k]);
            Method accept=NativeScene.class.getDeclaredMethod("accept",MemorySegment.class,long.class,int.class);
            accept.setAccessible(true); accept.invoke(null,packet,0L,size);
        }
    }

    private static void reset() throws Exception {
        Method clear=SkyCollision.class.getDeclaredMethod("clear",int.class);
        clear.setAccessible(true); clear.invoke(null,4242);
    }

    public static void run(ServerLevel level,ServerPlayer player) throws Exception {
        reset();
        BlockPos pos=player.blockPosition().offset(20,4,0);
        float x=pos.getX(), y=pos.getY(), z=pos.getZ();
        long[] wall=new long[8];
        for (int layer=0;layer<8;layer++) for (int row=0;row<8;row++) wall[layer]|=1L<<(3+row*8);
        float[][] faces={
            {x+0.5f,y,z,x+0.5f,y+1,z,x+0.5f,y+1,z+1},
            {x+0.5f,y,z,x+0.5f,y+1,z+1,x+0.5f,y,z+1}};
        Zombie zombie=new Zombie(level);
        ItemEntity item=new ItemEntity(level,x+0.75,y+2,z+0.75,new ItemStack(Items.STONE));
        SkyrimActorEntity npc=new SkyrimActorEntity(SkyCombat.SKYRIM_ACTOR,level);
        BlockPos realBlock=pos.west();
        var previous=level.getBlockState(realBlock);
        try {
            sample(pos,wall,faces);
            check(level.getBlockState(pos).isAir(),"native surface stays outside the Minecraft block palette");
            check(!level.getBlockState(pos).getCollisionShape(level,pos).isEmpty(),
                "vanilla collision query receives native wall");
            Vec3 from=new Vec3(x-0.5,y+0.75,z+0.5), to=new Vec3(x+1.5,y+0.75,z+0.5);
            var hit=level.clip(new ClipContext(from,to,ClipContext.Block.COLLIDER,ClipContext.Fluid.NONE,player));
            check(hit instanceof SkyClip.SkyrimHitResult && Math.abs(hit.getLocation().x-x-0.5)<0.01,
                "general vanilla ray clips against native surface instead of air");
            check(WalkNodeEvaluator.getPathTypeStatic(zombie,pos)==PathType.BLOCKED,
                "vanilla navigation classifies native wall as blocked");
            item.setPos(x+0.85,y+0.65,z+0.5);
            check(ServerExplosion.getSeenPercent(from,item)<0.01f,
                "native wall blocks vanilla explosion exposure rays");
            item.setPos(x+0.75,y+2,z+0.75);
            sample(pos.west(),new long[8],new float[0][]);
            Method flow=FlowingFluid.class.getDeclaredMethod("canPassThroughWall",Direction.class,
                BlockGetter.class,BlockPos.class,BlockState.class,BlockPos.class,BlockState.class);
            flow.setAccessible(true);
            check(!(boolean)flow.invoke(null,Direction.EAST,level,pos.west(),
                Blocks.WATER.defaultBlockState(),pos,Blocks.AIR.defaultBlockState()),
                "vanilla fluid boundary rejects native wall");
            zombie.setNoGravity(true); zombie.setPos(x+0.05,y,z+0.5);
            zombie.move(MoverType.SELF,new Vec3(1,0,0));
            check(zombie.getX()<x+0.2,"Minecraft mob movement collides with native wall");
            long[] full=new long[8]; java.util.Arrays.fill(full,-1L);
            sample(pos.below(),full,new float[][]{
                {x,y,z,x+1,y,z+1,x+1,y,z},
                {x,y,z,x,y,z+1,x+1,y,z+1}});
            item.move(MoverType.SELF,new Vec3(0,-4,0));
            check(Math.abs(item.getY()-y)<0.01,"Minecraft dropped item settles on native support");
            check(level.getBlockState(pos.below()).isFaceSturdy(level,pos.below(),Direction.UP),
                "vanilla building support query receives native floor");
            level.setBlock(realBlock,Blocks.STONE.defaultBlockState(),3);
            var nearer=level.clip(new ClipContext(new Vec3(x-2,y+0.75,z+0.5),to,
                ClipContext.Block.COLLIDER,ClipContext.Fluid.NONE,player));
            check(nearer.getType()==HitResult.Type.BLOCK && nearer.getBlockPos().equals(realBlock)
                && !(nearer instanceof SkyClip.SkyrimHitResult),"nearer Minecraft block wins over native surface");
            level.setBlock(realBlock,previous,3);
            npc.setPos(x+3,y,z+0.5); level.addFreshEntity(npc);
            zombie.setPos(x+2,y,z+0.5); zombie.move(MoverType.SELF,new Vec3(1,0,0));
            check(zombie.getX()<x+2.5,"native NPC proxy is a body obstacle for Minecraft mobs");
            sample(pos,new long[8],new float[0][]);
            var open=level.clip(new ClipContext(from,to,ClipContext.Block.COLLIDER,ClipContext.Fluid.NONE,player));
            check(open.getType()==HitResult.Type.MISS,"opening a native door clears collision and ray geometry together");
            check((boolean)flow.invoke(null,Direction.EAST,level,pos.west(),
                Blocks.WATER.defaultBlockState(),pos,Blocks.AIR.defaultBlockState()),
                "opening native door updates fluid rule despite identical block states");
            System.out.println("SCENE_RUNTIME_RESULT: PASSED ("+checks+" checks)");
        } finally {
            level.setBlock(realBlock,previous,3);
            zombie.discard(); item.discard(); npc.discard(); reset();
        }
    }
}
