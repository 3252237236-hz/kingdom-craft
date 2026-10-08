#include "../host/skycraft_scene_geometry.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

using namespace skycraft;

int main()
{
    scene::Cell floor(-2,10,-3,7);
    for (float x : {-1.75f,-1.25f}) for (float z : {-2.75f,-2.25f})
        floor.AddSample({{x,10.5f,z},{0,1,0},false});
    assert(floor.block.bits[3]==~0ull);
    assert(floor.block.bits[4]==0);
    assert(!floor.triangles.empty());
    for (const auto& triangle : floor.triangles) for (int k=0;k<3;k++) {
        assert(triangle.v[k*3]>=-2 && triangle.v[k*3]<=-1);
        assert(triangle.v[k*3+1]>=10 && triangle.v[k*3+1]<=11);
        assert(triangle.v[k*3+2]>=-3 && triangle.v[k*3+2]<=-2);
        assert(!(triangle.flags&proto::kTriDiggable));
    }
    const auto packet=floor.Packet();
    assert(packet.size()==sizeof(proto::ColRegion)+sizeof(proto::ColBlock)
        +floor.triangles.size()*sizeof(proto::ColTri));
    proto::ColRegion header{};
    std::memcpy(&header,packet.data(),sizeof(header));
    assert(header.epoch==7 && header.minX==-2 && header.maxX==-2);
    assert(header.count==floor.triangles.size());

    scene::Cell wall(0,0,0,7);
    for (float y : {0.25f,0.75f}) for (float z : {0.25f,0.75f})
        wall.AddSample({{0.5f,y,z},{1,0,0},false});
    for (auto bits : wall.block.bits) {
        for (int z=0;z<8;z++) assert(bits&(1ull<<(3+z*8)));
        for (int z=0;z<8;z++) assert(!(bits&(1ull<<(4+z*8))));
    }
    scene::Cell ceiling(0,0,0,7);
    for (float x : {0.25f,0.75f}) for (float z : {0.25f,0.75f})
        ceiling.AddSample({{x,0.5f,z},{0,-1,0},false});
    assert(ceiling.block.bits[4]==~0ull && ceiling.block.bits[3]==0);

    scene::Cell empty(0,0,0,7);
    assert(!empty.AddSample({{0.5f,0.5f,0.5f},{0,0,0},false}));
    assert(!empty.AddSample({{2,0.5f,0.5f},{0,1,0},false}));
    assert(!empty.AddSample({{std::numeric_limits<float>::quiet_NaN(),0,0},{0,1,0},false}));
    assert(empty.triangles.empty());
    assert(empty.Packet().size()==sizeof(proto::ColRegion)+sizeof(proto::ColBlock));
    std::cout<<"shared scene geometry contracts passed\n";
}
