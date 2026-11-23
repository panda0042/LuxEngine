#pragma once

#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <RainyCreek/Catalog.hpp>
#include <RainyCreek/CreekState.hpp>
#include <array>
#include <map>
#include <memory>

namespace RainyCreek {

class CreekRenderer {
public:
    CreekRenderer();
    ~CreekRenderer();
    void Initialize(LuxEngine::GpuDevice& device, Catalog catalog);
    void Shutdown();
    void Draw(LuxEngine::GpuDevice& device, const CreekState& state,
              const LuxEngine::PerspectiveCamera& camera);
    LuxEngine::Renderer3D::Statistics Statistics() const;
    bool EnvironmentReady() const;

private:
    struct Part {
        LuxEngine::Mesh3D mesh;
        std::shared_ptr<LuxEngine::GpuTexture> texture;
        LuxEngine::Material3D material;
    };

    struct GpuModel {
        std::vector<Part> near;
        std::vector<Part> high;
        std::vector<Part> far;
    };

    Part LoadPart(LuxEngine::GpuDevice& device, const ModelPart& part);
    std::shared_ptr<LuxEngine::GpuTexture> LoadTexture(LuxEngine::GpuDevice& device,
                                                       const std::filesystem::path& path,
                                                       float alphaMaskCutoff = 0);
    Catalog catalog_;
    LuxEngine::Renderer3D::Context context_;
    std::map<std::string, GpuModel> models_;
    std::map<std::pair<std::filesystem::path, float>, std::shared_ptr<LuxEngine::GpuTexture>>
        textures_;
    LuxEngine::Mesh3D terrain_;
    LuxEngine::Mesh3D lampPost_;
    LuxEngine::Mesh3D lampGlass_;
    LuxEngine::WaterSurface water_;
    std::array<LuxEngine::Mesh3D, 24> rain_;
    std::shared_ptr<LuxEngine::GpuTexture> ground_;
};

} // namespace RainyCreek
