#pragma once

#include "World.hpp"
#include <filesystem>
#include <map>
#include <string>

namespace Frontier {

class PropModels {
public:
    void Load(LuxEngine::GpuDevice& gpu, const std::filesystem::path& assets);
    void Draw(LuxEngine::Renderer3D::Context& renderer, std::string_view key, Vec3 ground,
              Vec3 dimensions, double yaw) const;

    struct Part {
        std::unique_ptr<LuxEngine::Mesh3D> mesh;
        LuxEngine::Material3D material;
    };

    std::vector<Part> Batch(LuxEngine::GpuDevice& gpu, std::string_view key,
                            std::span<const Plant> plants, Vec3 origin) const;
    void Release();

private:
    struct Geometry {
        MeshData mesh;
        LuxEngine::Material3D material;
    };

    struct Model {
        Vec3 center, dimensions;
        std::vector<Part> parts;
        std::vector<Geometry> batchGeometry;
    };

    std::map<std::string, Model, std::less<>> models_;
};

} // namespace Frontier
