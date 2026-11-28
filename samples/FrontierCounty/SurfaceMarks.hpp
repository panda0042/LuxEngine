#pragma once

#include "World.hpp"
#include <LuxEngine/Renderer/Renderer3D/Decal.hpp>
#include <deque>
#include <filesystem>
#include <optional>

namespace Frontier {
// Short-lived, local ground detail. Terrain and the road surface share the sampled triangle grid.
class SurfaceMarks {
public:
    void Initialize(const std::filesystem::path& assets);
    void Update(LuxEngine::GpuDevice& gpu, Vec3 eye, Vec3 actor, bool driving, double wetness,
                unsigned seed);
    void Draw(LuxEngine::Renderer3D::Context& renderer) const;

    void Release() {
        mesh_.Release();
    }

private:
    struct Mark {
        Vec2 center, direction;
        Vec2 size;
        unsigned tile;
        float opacity;
        bool onRoad;
    };

    void Rebuild(LuxEngine::GpuDevice& gpu, Vec2 eye, double wetness, unsigned seed);

    LuxEngine::Mesh3D mesh_;
    LuxEngine::Material3D material_;
    std::array<std::string, 3> rockMeshes_;
    LuxEngine::Material3D rockMaterial_;
    std::vector<std::pair<unsigned, LuxEngine::Mat4>> visibleRocks_;
    std::deque<Mark> trail_;
    std::optional<Vec2> previous_;
    std::optional<ChunkKey> viewTile_;
    int lastWetness_ = -1;
    bool driving_ = false, rightFoot_ = false;
};
} // namespace Frontier
