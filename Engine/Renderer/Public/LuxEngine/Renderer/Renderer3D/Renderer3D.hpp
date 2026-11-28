#pragma once

#include <LuxEngine/Core/Core.hpp>
#include <LuxEngine/Core/Math.hpp>
#include <LuxEngine/Renderer/RHI/GpuDevice.hpp>
#include <LuxEngine/Renderer/Renderer3D/Render3DSettings.hpp>
#include <array>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace LuxEngine {
class ModelAsset;
struct ModelPose;

struct Vertex3D {
    Vec3 position;
    Vec3 normal;
    Vec2 texCoords;
    Vec4 color{1, 1, 1, 1};
    float windWeight = 0; // 0 at a rooted vertex, 1 at a flexible tip.
};

enum class MaterialAlphaMode { Opaque, Mask, Blend };

struct WaterMaterial3D {
    // Beer–Lambert absorption coefficients in inverse world units.
    Vec3 absorption{.36, .14, .065};
    float indexOfRefraction = 1.333f;
    float rippleStrength =
        0; // Strength of wind-driven surface slopes below the mesh resolution, 0..1.
};

// Secondary PBR layer for opaque custom meshes. Vertex3D::color.a selects the
// layer weight, and both layers use the mesh UVs. RGB vertex tint is shared.
struct TerrainLayer3D {
    std::string baseColorTexture;
    std::string normalTexture;
    std::string roughnessTexture;
    std::string heightTexture; // Linear red, white is the top of the surface.
    float heightBlend = 0;     // Height contrast in [0,1]; zero keeps the vertex-only blend.
};

struct Material3D {
    Vec4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    std::string baseColorTexture;
    std::string normalTexture;
    std::string metallicTexture;
    std::string roughnessTexture;
    std::string occlusionTexture;
    float normalScale = 1.f;
    float occlusionStrength = 1.f;
    TextureSampling textureSampling{true, TextureAddressMode::Repeat, true, false};
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Mask;
    float alphaCutoff = 0.001f;
    bool doubleSided = true;
    std::optional<WaterMaterial3D> water;
    std::optional<TerrainLayer3D> terrainLayer;
    // Nonzero, stable ID for this placement in one render context. Mesh buffers distinguish
    // its parts. Supply separate IDs for moving copies of the same mesh.
    uint64 motionInstance = 0;
    float wettingResponse = 0;     // Exposed surface fraction that can develop a rain film.
    float windAmplitude = 0;       // Maximum vertex displacement, in world metres.
    Vec3 subsurfaceRadius{};       // RGB diffusion lengths, in metres.
    float subsurfaceThickness = 0; // Authored thickness for thin-sheet transmission, in metres.
    Vec3 emissive{};             // Linear HDR radiance added after lighting, before camera effects.
    std::string emissiveTexture; // sRGB color multiplied by emissive.
    std::string heightTexture;   // Linear height, sampled for opaque mesh relief.
    float parallaxDepth = 0;     // Maximum virtual relief below the mesh surface, in metres.
};

// Enumerate all file references, including an optional terrain layer, for editing and packaging.
template <typename Material> auto MaterialTexturePaths(Material& material) {
    using Path = std::remove_reference_t<decltype((material.heightTexture))>;

    struct Paths {
        std::array<Path*, 11> values;
        size_t count;

        auto begin() const {
            return values.begin();
        }

        auto end() const {
            return values.begin() + count;
        }
    };

    Paths paths{{&material.baseColorTexture, &material.normalTexture, &material.metallicTexture,
                 &material.roughnessTexture, &material.occlusionTexture, &material.emissiveTexture,
                 &material.heightTexture},
                7};
    if (auto& layer = material.terrainLayer) {
        for (auto* path : {&layer->baseColorTexture, &layer->normalTexture,
                           &layer->roughnessTexture, &layer->heightTexture})
            paths.values[paths.count++] = path;
    }
    return paths;
}

struct MaterialTextures3D {
    GpuTexture* baseColor = nullptr;
    GpuTexture* normal = nullptr;
    GpuTexture* roughness = nullptr;
    bool unlitSprite = false;
    GpuTexture* metallic = nullptr;
    GpuTexture* occlusion = nullptr;
    GpuTexture* emissive = nullptr;
    GpuTexture* height = nullptr;
};

struct PointLight {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 10.0f;
};

class LUX_API PerspectiveCamera {
public:
    PerspectiveCamera();
    PerspectiveCamera(double fovRadians, double aspect, double nearClip, double farClip);

    void SetPerspective(double fovRadians, double aspect, double nearClip, double farClip);

    void SetPosition(const Vec3& position);

    void SetTarget(const Vec3& target);

    void SetUp(const Vec3& up);

    const Vec3& GetPosition() const;

    const Vec3& GetTarget() const;

    const Mat4& GetProjectionMatrix() const;

    const Mat4& GetViewMatrix() const;

    const Mat4& GetViewProjectionMatrix() const;

private:
    void SetView(const Vec3& position, const Vec3& target, const Vec3& up);

    Mat4 projectionMatrix_ = Mat4::Identity();
    Mat4 viewMatrix_ = Mat4::Identity();
    Mat4 viewProjectionMatrix_ = Mat4::Identity();
    Vec3 position_{0.0f, 0.0f, 5.0f};
    Vec3 target_{0.0f, 0.0f, 0.0f};
    Vec3 up_{0.0f, 1.0f, 0.0f};
};

class OceanSpectrum;

class LUX_API Mesh3D {
public:
    Mesh3D() = default;
    Mesh3D(const Mesh3D&) = delete;
    Mesh3D& operator=(const Mesh3D&) = delete;
    Mesh3D(Mesh3D&&) noexcept = default;
    Mesh3D& operator=(Mesh3D&&) noexcept = default;

    bool Upload(GpuDevice* device, std::span<const Vec3> positions,
                std::span<const uint32> indices);
    bool Upload(GpuDevice* device, std::span<const Vertex3D> vertices,
                std::span<const uint32> indices);
    bool UpdateVertices(std::span<const Vertex3D> vertices);
    // Creates a periodic patch centered on the mesh origin. Use WaterSurface::MakeMaterial
    // to shade the surface with the foam computed by the GPU.
    bool UploadOcean(GpuDevice* device, const OceanSpectrum& spectrum);
    // Records the GPU transform inside an open frame, before any draw that uses this mesh.
    bool UpdateOcean(float timeSeconds);
    void Release();
    bool IsValid() const;

    uint32 GetIndexCount() const;

private:
    friend class Renderer3D;
    Ref<GpuBuffer> vertices_;
    Ref<GpuBuffer> indices_;
    uint32 indexCount_ = 0;
    uint64 geometryRevision_ = 0;
    bool ocean_ = false;
    Vec3 origin_;
    Vec3 minimum_, maximum_;
};

struct CoastalWaterFrame {
    float timeSeconds = 0;
    float breakerHeight = 0;
};

class LUX_API Renderer3D {
public:
    struct Statistics {
        uint32 drawCalls = 0;
        uint32 meshCount = 0;
    };

    class LUX_API Context {
    public:
        Context();
        explicit Context(GpuDevice* device);
        ~Context();
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&& other) noexcept;
        Context& operator=(Context&& other) noexcept;

        void Init(GpuDevice* device = nullptr);
        void Shutdown();
        void DetachDevice(const GpuDevice* device);
        bool IsDeviceValid() const;

        void BeginScene(const PerspectiveCamera& camera, const DirectionalLight& light);
        void BeginScene(const PerspectiveCamera& camera, const DirectionalLight& light,
                        std::span<const PointLight> pointLights);
        void BeginScene(const PerspectiveCamera& camera, const Render3DSettings& settings,
                        float exposure, std::span<const PointLight> pointLights = {});
        void BeginScene(const PerspectiveCamera& camera, const Render3DSettings& settings,
                        float exposure, std::span<const PointLight> pointLights,
                        std::optional<CoastalWaterFrame> coastalWater);
        void EndScene();
        void AbortScene();

        void DrawMesh(const Mat4& transform, const Material3D& material);
        void DrawMesh(const Mesh3D& mesh, const Mat4& transform, const Material3D& material);
        void DrawMesh(const Mesh3D& mesh, const Mat4& transform, const Material3D& material,
                      GpuTexture* texture);
        void DrawMeshWithTextures(const Mesh3D& mesh, const Mat4& transform,
                                  const Material3D& material, const MaterialTextures3D& textures);
        bool DrawMeshAsset(const std::string& path, const Mat4& transform,
                           const Material3D& material, int32 modelSubmesh = -1, int32 clip = -1,
                           double time = 0.0, uint64 instance = 0, const ModelPose* pose = nullptr,
                           uint64 poseRevision = 0, const Ref<const ModelPose>& poseOwner = {});
        void DrawMesh(std::span<const Vec3> positions, std::span<const uint32> indices,
                      const Mat4& transform, const Material3D& material);
        void DrawCube(const Vec3& position, const Vec3& scale, const Material3D& material);

        void ResetStats();
        Statistics GetStats() const;
        const std::string& GetLastAssetError() const;
        bool IsEnvironmentReady() const;
        double GetEnvironmentPreparationMs() const;
        bool GetMeshBounds(const std::string& path, Vec3& minimum, Vec3& maximum) const;

    private:
        friend class Renderer3D;
        bool DrawMeshAssetImpl(const std::string& path, const Mat4& transform,
                               const Material3D& material, int32 modelSubmesh, int32 clip,
                               double time, uint64 instance, const ModelPose* pose,
                               uint64 poseRevision, const Ref<const ModelPose>& poseOwner,
                               const Ref<ModelAsset>* gltfModel = nullptr,
                               const std::string& gltfError = {});
        enum class TextureChannel { Red, Green, Blue };
        Ref<GpuTexture> ResolveTexture(const std::string& path, TextureSampling sampling,
                                       TextureColorSpace colorSpace, float alphaMaskCutoff = 0,
                                       const Ref<ModelAsset>& model = {},
                                       int32 modelTextureIndex = -1,
                                       TextureChannel channel = TextureChannel::Red);
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    class LUX_API ScopedContext {
    public:
        explicit ScopedContext(Context& context);
        ~ScopedContext();
        ScopedContext(const ScopedContext&) = delete;
        ScopedContext& operator=(const ScopedContext&) = delete;

    private:
        Context* previous_ = nullptr;
        Context* current_ = nullptr;
    };

    static void Init(GpuDevice* device = nullptr);
    static void Shutdown();
    static void DetachDevice(const GpuDevice* device);

    static void BeginScene(const PerspectiveCamera& camera, const DirectionalLight& light);
    static void BeginScene(const PerspectiveCamera& camera, const DirectionalLight& light,
                           std::span<const PointLight> pointLights);
    static void BeginScene(const PerspectiveCamera& camera, const Render3DSettings& settings,
                           float exposure, std::span<const PointLight> pointLights = {});
    static void BeginScene(const PerspectiveCamera& camera, const Render3DSettings& settings,
                           float exposure, std::span<const PointLight> pointLights,
                           std::optional<CoastalWaterFrame> coastalWater);
    static void EndScene();

    static void DrawMesh(const Mat4& transform, const Material3D& material);
    static void DrawMesh(const Mesh3D& mesh, const Mat4& transform, const Material3D& material);
    static void DrawMesh(const Mesh3D& mesh, const Mat4& transform, const Material3D& material,
                         GpuTexture* texture);
    static bool DrawMeshAsset(const std::string& path, const Mat4& transform,
                              const Material3D& material, int32 modelSubmesh = -1, int32 clip = -1,
                              double time = 0.0, uint64 instance = 0,
                              const ModelPose* pose = nullptr, uint64 poseRevision = 0,
                              const Ref<const ModelPose>& poseOwner = {});
    static bool DrawGltfSubmesh(const std::string& path, const Mat4& transform,
                                const Material3D& material, int32 modelSubmesh,
                                const Ref<ModelAsset>& model, const std::string& loadError);
    static void DrawGltfAsset(const std::string& path, const Mat4& transform,
                              const Material3D& material, int32 modelSubmesh,
                              const Ref<ModelAsset>& model, const std::string& loadError);
    static void DrawMesh(std::span<const Vec3> positions, std::span<const uint32> indices,
                         const Mat4& transform, const Material3D& material);
    static void DrawCube(const Vec3& position, const Vec3& scale, const Material3D& material);

    static void ResetStats();
    static Statistics GetStats();
    static const std::string& GetLastAssetError();
};

} // namespace LuxEngine
