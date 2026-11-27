#pragma once

#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <array>
#include <filesystem>

class Swimmer {
public:
    enum class Motion { Idle, Walk, Tread, Swim };

    void Load(LuxEngine::GpuDevice& gpu, const std::filesystem::path& file, bool recording);
    void Advance(Motion motion, double dt, double playbackRate);
    void Draw(LuxEngine::Renderer3D::Context& renderer, LuxEngine::Vec3 position, double yaw);
    void Release();

    bool IsLoaded() const {
        return bool(model_);
    }

private:
    struct Part {
        size_t submesh;
        size_t firstVertex;
    };

    struct Batch {
        LuxEngine::int32 material;
        LuxEngine::int32 bone;
        LuxEngine::Mesh3D mesh;
        std::vector<Part> parts;
        std::vector<LuxEngine::Vertex3D> vertices;
        std::vector<LuxEngine::uint32> indices;
    };

    struct SwimFrame {
        std::vector<LuxEngine::Mat4> transforms;
        std::vector<LuxEngine::Mesh3D> meshes;
    };

    void FillDeformed(Batch& batch);
    void UpdatePose();

    LuxEngine::Ref<LuxEngine::ModelAsset> model_;
    LuxEngine::GpuDevice* gpu_ = nullptr;
    std::vector<LuxEngine::Material3D> materials_;
    std::vector<Batch> batches_;
    std::array<std::vector<SwimFrame>, 4> frames_;
    const SwimFrame* activeFrame_ = nullptr;
    std::array<LuxEngine::int32, 4> clips_{};
    LuxEngine::ModelPose pose_, previous_, target_;
    std::vector<LuxEngine::ModelPoseLayer> layers_;
    LuxEngine::Vec3 feet_;
    double scale_ = 1;
    double poseRate_ = 30;
    double phase_ = 0, transition_ = 1, sampledPhase_ = -1;
    Motion motion_ = Motion::Idle;
};
