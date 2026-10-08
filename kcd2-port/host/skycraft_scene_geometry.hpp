#pragma once

#include "../bridge/skycraft_bridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace skycraft::scene
{
    struct Vec { float x, y, z; };
    struct Sample { Vec point, normal; bool terrain; };
    inline Vec Add(Vec a, Vec b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
    inline Vec Sub(Vec a, Vec b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
    inline Vec Mul(Vec a, float b) { return {a.x*b, a.y*b, a.z*b}; }
    inline float Dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    inline Vec Cross(Vec a, Vec b) {
        return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
    }
    inline Vec Normalize(Vec a) {
        const float len = std::sqrt(Dot(a, a));
        return len > 0.001f ? Mul(a, 1.0f/len) : Vec{};
    }
    inline float Axis(Vec a, int axis) {
        return axis == 0 ? a.x : axis == 1 ? a.y : a.z;
    }

    // Clip each sampled patch to its owning cell; do not invent geometry in a neighbour.
    inline void Clip(std::vector<Vec>& polygon, int axis, float bound, bool lower)
    {
        std::vector<Vec> result;
        if (polygon.empty()) return;
        Vec previous = polygon.back();
        float before = (Axis(previous, axis)-bound) * (lower ? 1.0f : -1.0f);
        for (Vec current : polygon) {
            const float after = (Axis(current, axis)-bound) * (lower ? 1.0f : -1.0f);
            if ((before >= 0) != (after >= 0)) {
                result.push_back(Add(previous, Mul(Sub(current, previous),
                    before/(before-after))));
            }
            if (after >= 0) result.push_back(current);
            previous = current;
            before = after;
        }
        polygon.swap(result);
    }

    struct Cell
    {
        proto::ColRegion header{};
        proto::ColBlock block{};
        std::vector<proto::ColTri> triangles;

        Cell(int x, int y, int z, std::uint32_t epoch)
        {
            header = {x,y,z,x,y,z,epoch,0};
            block.x = x; block.y = y; block.z = z;
        }

        bool AddSample(const Sample& sample)
        {
            const Vec n = Normalize(sample.normal);
            const Vec p = sample.point;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)
                || !std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)
                || Dot(n,n) < 0.9f
                || p.x < block.x-0.002f || p.x > block.x+1.002f
                || p.y < block.y-0.002f || p.y > block.y+1.002f
                || p.z < block.z-0.002f || p.z > block.z+1.002f) return false;
            const Vec u = Normalize(Cross(std::abs(n.y)<0.9f ? Vec{0,1,0} : Vec{1,0,0}, n));
            const Vec v = Cross(n,u);
            constexpr float radius = 0.375f;
            for (int y=0; y<8; ++y) for (int z=0; z<8; ++z) for (int x=0; x<8; ++x) {
                const Vec q = Sub(Vec{block.x+(x+0.5f)/8, block.y+(y+0.5f)/8,
                    block.z+(z+0.5f)/8}, p);
                const float depth = Dot(q,n);
                if (depth >= -0.125f && depth <= 0.01f
                    && std::abs(Dot(q,u)) <= radius && std::abs(Dot(q,v)) <= radius)
                    block.bits[y] |= 1ull << (x+8*z);
            }
            std::vector<Vec> polygon{
                Add(p, Add(Mul(u,-radius),Mul(v,-radius))),
                Add(p, Add(Mul(u,radius),Mul(v,-radius))),
                Add(p, Add(Mul(u,radius),Mul(v,radius))),
                Add(p, Add(Mul(u,-radius),Mul(v,radius)))};
            for (int axis=0; axis<3; ++axis) {
                const float start = float(axis==0 ? block.x : axis==1 ? block.y : block.z);
                Clip(polygon,axis,start,true);
                Clip(polygon,axis,start+1,false);
            }
            for (std::size_t i=1; i+1<polygon.size() && triangles.size()<256; ++i) {
                if (Dot(Cross(Sub(polygon[i],polygon[0]),Sub(polygon[i+1],polygon[0])),
                    Cross(Sub(polygon[i],polygon[0]),Sub(polygon[i+1],polygon[0]))) < 1e-12f) continue;
                proto::ColTri triangle{};
                const std::array<Vec,3> vertices{polygon[0],polygon[i],polygon[i+1]};
                for (int k=0; k<3; ++k) {
                    triangle.v[k*3] = vertices[k].x;
                    triangle.v[k*3+1] = vertices[k].y;
                    triangle.v[k*3+2] = vertices[k].z;
                }
                // Sampling does not grant permission to edit the native game's meshes.
                triangle.flags = sample.terrain ? proto::kTriTerrain : 0;
                triangles.push_back(triangle);
            }
            return true;
        }

        std::vector<std::uint8_t> Packet()
        {
            header.count = static_cast<std::uint32_t>(triangles.size());
            std::vector<std::uint8_t> data(sizeof(header)+sizeof(block)
                +triangles.size()*sizeof(proto::ColTri));
            std::memcpy(data.data(),&header,sizeof(header));
            std::memcpy(data.data()+sizeof(header),&block,sizeof(block));
            if (!triangles.empty()) std::memcpy(data.data()+sizeof(header)+sizeof(block),
                triangles.data(),triangles.size()*sizeof(proto::ColTri));
            return data;
        }
    };
}
