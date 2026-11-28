#pragma once

#include "World.hpp"
#include <deque>
#include <filesystem>
#include <map>
#include <string>

namespace Frontier {

class PropModels {
public:
    void Load(LuxEngine::GpuDevice& gpu, const std::filesystem::path& assets);
    void Draw(LuxEngine::Renderer3D::Context& renderer, std::string_view key, Vec3 ground,
              Vec3 dimensions, double yaw, float lodTransition = 0) const;
    void Draw(LuxEngine::Renderer3D::Context& renderer, std::string_view key,
              const LuxEngine::Mat4& placement, Vec3 dimensions, uint64_t motionInstance = 0,
              float lodTransition = 0) const;

    struct Part {
        std::unique_ptr<LuxEngine::Mesh3D> mesh;
        LuxEngine::Material3D material;
    };

    struct Geometry {
        MeshData mesh;
        LuxEngine::Material3D material;
    };

    std::deque<Geometry> BuildGroundCover(ChunkKey tile, unsigned seed) const;
    void Release();

private:
    struct Model {
        Vec3 center, dimensions;
        std::vector<Part> parts;
        std::vector<Geometry> batchGeometry;
    };

    void LoadGroundScans(const std::filesystem::path& assets);

    void BuildBatches(std::string_view key, std::span<const Plant> plants, Vec3 origin,
                      std::deque<Geometry>& batches) const;
    std::map<std::string, Model, std::less<>> models_;
};

} // namespace Frontier
