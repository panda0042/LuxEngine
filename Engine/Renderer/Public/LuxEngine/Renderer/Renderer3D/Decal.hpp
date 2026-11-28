#pragma once

#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>

namespace LuxEngine {
// Positions and projector axes are in the receiver mesh's coordinate system.
struct DecalProjector {
    Vec3 center;
    Vec3 normal{0, 1, 0};       // Out of the receiving surface.
    Vec3 tangent{1, 0, 0};      // Increasing texture U.
    Vec3 halfSize{.5, .5, .25}; // Half width, half height and projection depth.
    double normalOffset = .002;
    double minimumFacing = .2;
    Vec4 tint{1, 1, 1, 1};
    Vec2 uvOrigin{}, uvSize{1, 1}; // Region of a shared decal atlas.
    int32 layer = 0;               // Ascending painter order within a projected batch.
};

// Clips receiver triangles to the projector, preserving their surface normals.
// Append multiple decals sharing a material, then upload the result with Mesh3D.
// A valid projector missing the receiver succeeds with zero new indices.
LUX_API bool AppendProjectedDecal(const DecalProjector& projector,
                                  std::span<const Vertex3D> receiver,
                                  std::span<const uint32> triangles,
                                  std::vector<Vertex3D>& vertices, std::vector<uint32>& indices);
LUX_API bool AppendProjectedDecals(std::span<const DecalProjector> projectors,
                                   std::span<const Vertex3D> receiver,
                                   std::span<const uint32> triangles,
                                   std::vector<Vertex3D>& vertices, std::vector<uint32>& indices);
} // namespace LuxEngine
