#include "LuxVirtualShadowPages.hpp"
#include "RHI/RenderBackend.hpp"
#include "ShadowProjection.hpp"
#include "TextureMipChain.hpp"
#include <LuxEngine/Assets/AssetManager.hpp>
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <LuxEngine/Renderer/Water/OceanSpectrum.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace LuxEngine {
namespace {
bool FiniteVertexColor(const Vec4& color) {
    return std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z) &&
           std::isfinite(color.w);
}

bool FitsGpuVector(Vec3 value) {
    constexpr auto limit = std::numeric_limits<float>::max();
    return std::abs(value.x) <= limit && std::abs(value.y) <= limit && std::abs(value.z) <= limit;
}

Mat4 ViewProjection(const Mat4& projection, const Mat4& view) {
    const auto matrix = projection * view;
    if (!std::all_of(std::begin(matrix.elements), std::end(matrix.elements),
                     [](double value) { return std::isfinite(value); }))
        throw std::invalid_argument("View-projection exceeds the representable range");
    return matrix;
}

float AlphaMaskCutoff(const Material3D& material) {
    if (material.alphaMode != MaterialAlphaMode::Mask || material.alphaCutoff <= 0 ||
        material.albedoColor.w <= 0)
        return 0;
    const float cutoff = material.alphaCutoff / material.albedoColor.w;
    return cutoff <= 1 ? std::ceil(cutoff * 255.f) / 255.f : 0;
}

constexpr size_t MaxSceneSubmissions = 65536;
constexpr size_t MaxShadowOnlySubmissions = 16384;
constexpr size_t MaxPointLights = 16;

void ExtendBounds(Vec3& minimum, Vec3& maximum, const Vec3& position) {
    minimum = {std::min(minimum.x, position.x), std::min(minimum.y, position.y),
               std::min(minimum.z, position.z)};
    maximum = {std::max(maximum.x, position.x), std::max(maximum.y, position.y),
               std::max(maximum.z, position.z)};
}

bool OutsideClipVolume(const Mat4& clip, const Vec3& minimum, const Vec3& maximum) {
    const Vec3 center = minimum * .5 + maximum * .5;
    const Vec3 extent = maximum * .5 - minimum * .5;
    const auto outside = [&](int row, double sign, double w) {
        const auto& m = clip.elements;
        const double x = sign * m[row] + w * m[3];
        const double y = sign * m[row + 4] + w * m[7];
        const double z = sign * m[row + 8] + w * m[11];
        const double d = sign * m[row + 12] + w * m[15];
        const double distance = x * center.x + y * center.y + z * center.z + d;
        const double radius =
            std::abs(x) * extent.x + std::abs(y) * extent.y + std::abs(z) * extent.z;
        return distance + radius < -1e-6 * (std::abs(distance) + radius + 1);
    };
    return outside(0, 1, 1) || outside(0, -1, 1) || outside(1, 1, 1) || outside(1, -1, 1) ||
           outside(2, 1, 0) || outside(2, -1, 1);
}

// Entry cap of the mesh and texture caches. The 256 MiB byte budgets do the real limiting; a
// small cap (it was 32) made scenes with more unique assets evict and re-upload every frame (#35).
constexpr size_t MaxCachedAssets = 4096;
constexpr std::array<Vec3, 8> CubePositions{{{-0.5f, -0.5f, -0.5f},
                                             {0.5f, -0.5f, -0.5f},
                                             {0.5f, 0.5f, -0.5f},
                                             {-0.5f, 0.5f, -0.5f},
                                             {-0.5f, -0.5f, 0.5f},
                                             {0.5f, -0.5f, 0.5f},
                                             {0.5f, 0.5f, 0.5f},
                                             {-0.5f, 0.5f, 0.5f}}};
constexpr std::array<uint32, 36> CubeIndices{{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
                                              0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5,
                                              0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2}};

bool SameOwner(const std::weak_ptr<Rhi::RenderBackend>& a,
               const std::weak_ptr<Rhi::RenderBackend>& b) {
    return !a.owner_before(b) && !b.owner_before(a);
}

thread_local Renderer3D::Context* t_ActiveContext3D = nullptr;
std::recursive_mutex s_DefaultContext3DMutex;

Renderer3D::Context& DefaultContext3D() {
    static Renderer3D::Context s_Default;
    return s_Default;
}
} // namespace

struct Renderer3D::Context::Impl {
    GpuDevice* device = nullptr;
    std::weak_ptr<Rhi::RenderBackend> deviceOwner;
    Mat4 viewProjection;
    Renderer3D::Statistics stats;
    bool active = false;
    Vec3 camera;
    Render3DSettings settings;
    float exposure = 1;
    PerspectiveCamera sceneCamera;
    std::optional<Mat4> shadowProjection;
    std::unique_ptr<Rhi::Scene3DRenderer> modern;

    struct CachedMesh {
        Ref<Mesh3D> mesh;
        Ref<ModelAsset> model;
        int32 clip = -1;
        double time = std::numeric_limits<double>::quiet_NaN();
        std::weak_ptr<const ModelPose> pose;
        uint64 poseRevision = 0;
        bool usesOverride = false;
        uint64 used = 0;
        size_t bytes = 0;
        uint64 attempted = std::numeric_limits<uint64>::max();
        std::string error;
        Vec3 minimum{-.5f, -.5f, -.5f}, maximum{.5f, .5f, .5f};
        bool hasUvs = false, uvBlocked = false;
    };

    struct CachedTexture {
        Ref<GpuTexture> texture;
        uint64 attempted = std::numeric_limits<uint64>::max();
        uint64 used = 0;
        size_t bytes = 0;
        std::string error;
    };

    std::unordered_map<std::string, CachedTexture> textures;
    // Scene draws retain GPU IDs until Render() records them. Cache eviction must not retire
    // buffers or textures that a previously submitted draw in this scene still references.
    std::vector<Ref<GpuTexture>> frameTextures;
    std::unordered_map<std::string, std::string> warnings;
    std::string lastError;
    size_t textureBytes = 0;

    void Report(const std::string& key, const std::string& message) {
        if (message.empty()) {
            warnings.erase(key);
            return;
        }
        if (lastError.empty())
            lastError = message;
        const auto previous = warnings.find(key);
        if (previous == warnings.end() || previous->second != message) {
            if (warnings.size() >= 128)
                warnings.erase(warnings.begin());
            warnings[key] = message;
            Log::Warn(message);
        }
    }

    std::unordered_map<std::string, CachedMesh> meshes;
    uint64 sequence = 0;
    size_t meshBytes = 0;

    std::shared_ptr<Rhi::RenderBackend> LockValidState() {
        if (!device)
            return nullptr;
        auto locked = deviceOwner.lock();
        if (!locked || !locked->Usable()) {
            device = nullptr;
            deviceOwner.reset();
            return nullptr;
        }
        return locked;
    }
};

bool Renderer3D::Context::IsEnvironmentReady() const {
    return impl_ && impl_->modern && impl_->modern->EnvironmentReady();
}

double Renderer3D::Context::GetEnvironmentPreparationMs() const {
    return impl_ && impl_->modern ? impl_->modern->EnvironmentPreparationMs() : 0;
}

PerspectiveCamera::PerspectiveCamera(double fovRadians, double aspect, double nearClip,
                                     double farClip)
    : PerspectiveCamera() {
    SetPerspective(fovRadians, aspect, nearClip, farClip);
}

void PerspectiveCamera::SetPerspective(double fovRadians, double aspect, double nearClip,
                                       double farClip) {
    const auto projection = Mat4::Perspective(fovRadians, aspect, nearClip, farClip);
    const auto combined = ViewProjection(projection, viewMatrix_);
    projectionMatrix_ = projection;
    viewProjectionMatrix_ = combined;
}

void PerspectiveCamera::SetView(const Vec3& position, const Vec3& target, const Vec3& up) {
    const auto view = Mat4::LookAt(position, target, up);
    const auto combined = ViewProjection(projectionMatrix_, view);
    position_ = position;
    target_ = target;
    up_ = up;
    viewMatrix_ = view;
    viewProjectionMatrix_ = combined;
}

bool Mesh3D::Upload(GpuDevice* gpu, std::span<const Vec3> positions,
                    std::span<const uint32> indices) {
    if (!gpu || positions.empty() || indices.empty() || indices.size() % 3 != 0 ||
        positions.size() > std::numeric_limits<uint32>::max() / sizeof(RenderVertex) ||
        indices.size() > std::numeric_limits<uint32>::max() / sizeof(uint32))
        return false;
    std::vector<Vertex3D> vertices;
    vertices.reserve(positions.size());
    for (const auto& position : positions)
        vertices.push_back({position, {}, {}});
    return Upload(gpu, vertices, indices);
}

bool Mesh3D::Upload(GpuDevice* gpu, std::span<const Vertex3D> vertices,
                    std::span<const uint32> indices) {
    if (!gpu || vertices.empty() || indices.empty() || indices.size() % 3 != 0 ||
        vertices.size() > std::numeric_limits<uint32>::max() / sizeof(RenderVertex) ||
        indices.size() > std::numeric_limits<uint32>::max() / sizeof(uint32))
        return false;
    for (uint32 index : indices)
        if (index >= vertices.size())
            return false;
    const Vec3 origin = vertices.front().position;
    Vec3 minimum, maximum;
    std::vector<RenderVertex> data;
    data.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        const auto& position = vertex.position;
        const auto& normal = vertex.normal;
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) || !FitsGpuVector(normal) ||
            !std::isfinite(vertex.texCoords.x) || !std::isfinite(vertex.texCoords.y) ||
            !FiniteVertexColor(vertex.color))
            return false;
        const auto relative = position - origin;
        if (!FitsGpuVector(relative))
            return false;
        ExtendBounds(minimum, maximum, relative);
        data.push_back({relative, vertex.color, vertex.texCoords, normal});
    }
    const auto vertexBytes = static_cast<uint32>(data.size() * sizeof(RenderVertex));
    const auto indexBytes = static_cast<uint32>(indices.size_bytes());
    auto verticesBuffer = CreateRef<GpuBuffer>();
    auto indicesBuffer = CreateRef<GpuBuffer>();
    if (!verticesBuffer->Initialize(gpu, BufferType::Vertex, vertexBytes) ||
        !verticesBuffer->SetData(data.data(), vertexBytes) ||
        !indicesBuffer->Initialize(gpu, BufferType::Index, indexBytes) ||
        !indicesBuffer->SetData(indices.data(), indexBytes)) {
        return false;
    }
    Release();
    vertices_ = std::move(verticesBuffer);
    indices_ = std::move(indicesBuffer);
    origin_ = origin;
    minimum_ = minimum;
    maximum_ = maximum;
    indexCount_ = static_cast<uint32>(indices.size());
    return true;
}

void Mesh3D::Release() {
    vertices_.reset();
    indices_.reset();
    indexCount_ = 0;
    ocean_ = false;
}

bool Mesh3D::IsValid() const {
    return indexCount_ != 0 && vertices_ && indices_ && vertices_->IsValid() && indices_->IsValid();
}

bool Mesh3D::UpdateVertices(std::span<const Vertex3D> vertices) {
    if (ocean_ || !IsValid() ||
        vertices.size_bytes() / sizeof(Vertex3D) != vertices_->GetSize() / sizeof(RenderVertex))
        return false;
    const auto origin = vertices.front().position;
    Vec3 minimum, maximum;
    std::vector<RenderVertex> data;
    data.reserve(vertices.size());
    for (const auto& vertex : vertices) {
        const auto& p = vertex.position;
        const auto& n = vertex.normal;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
            !FitsGpuVector(n) || !std::isfinite(vertex.texCoords.x) ||
            !std::isfinite(vertex.texCoords.y) || !FiniteVertexColor(vertex.color))
            return false;
        const auto relative = p - origin;
        if (!FitsGpuVector(relative))
            return false;
        ExtendBounds(minimum, maximum, relative);
        data.push_back({relative, vertex.color, vertex.texCoords, n});
    }
    const auto size = static_cast<uint32>(data.size() * sizeof(RenderVertex));
    if (vertices_.use_count() > 1) {
        auto next = CreateRef<GpuBuffer>();
        if (!next->InitializeFromBackend(vertices_->owner_.lock(), BufferType::Vertex, size) ||
            !next->SetData(data.data(), size))
            return false;
        vertices_ = std::move(next);
    } else if (!vertices_->SetData(data.data(), size)) {
        return false;
    }
    origin_ = origin;
    minimum_ = minimum;
    maximum_ = maximum;
    return true;
}

bool Mesh3D::UploadOcean(GpuDevice* gpu, const OceanSpectrum& spectrum) {
    if (!gpu || !gpu->IsReady())
        return false;
    OceanSpectrumSettings settings;
    std::vector<float> modes;
    Vec3 extent;
    if (!spectrum.BuildGpuSeed(settings, modes, extent))
        return false;

    const uint32 n = settings.resolution;
    const uint32 stride = n + 1;
    std::vector<uint32> indices;
    indices.reserve(n * n * 6);
    for (uint32 z = 0; z < n; ++z)
        for (uint32 x = 0; x < n; ++x) {
            const uint32 top = z * stride + x;
            const uint32 bottom = top + stride;
            indices.insert(indices.end(), {top, bottom, top + 1, top + 1, bottom, bottom + 1});
        }

    Mesh3D mesh;
    mesh.vertices_ = CreateRef<GpuBuffer>();
    mesh.indices_ = CreateRef<GpuBuffer>();
    const auto indexBytes = static_cast<uint32>(indices.size() * sizeof(uint32));
    if (!mesh.indices_->Initialize(gpu, BufferType::Index, indexBytes) ||
        !mesh.indices_->SetData(indices.data(), indexBytes))
        return false;
    if (!gpu->backend_->CreateOceanBuffer(n, modes, static_cast<float>(settings.patchLength),
                                          static_cast<float>(settings.foamThreshold),
                                          static_cast<float>(settings.foamSoftness),
                                          mesh.vertices_->handle_))
        return false;
    mesh.vertices_->owner_ = gpu->backend_;
    mesh.vertices_->sizeBytes_ = stride * stride * sizeof(RenderVertex);
    mesh.indexCount_ = static_cast<uint32>(indices.size());
    mesh.ocean_ = true;
    const double halfPatch = settings.patchLength * .5;
    extent = extent * 1.001 + Vec3{halfPatch + .001, .001, halfPatch + .001};
    mesh.minimum_ = extent * -1;
    mesh.maximum_ = extent;
    *this = std::move(mesh);
    return true;
}

bool Mesh3D::UpdateOcean(float timeSeconds) {
    if (!ocean_ || !vertices_)
        return false;
    const auto owner = vertices_->owner_.lock();
    return owner && owner->UpdateOceanBuffer(vertices_->handle_, timeSeconds);
}

Renderer3D::Context::Context() : impl_(std::make_unique<Impl>()) {}

Renderer3D::Context::Context(GpuDevice* gpu) : impl_(std::make_unique<Impl>()) {
    Init(gpu);
}

Renderer3D::Context::~Context() {
    Shutdown();
}

Renderer3D::Context::Context(Context&& other) noexcept = default;
Renderer3D::Context& Renderer3D::Context::operator=(Context&& other) noexcept = default;

void Renderer3D::Context::Init(GpuDevice* gpu) {
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    const std::weak_ptr<Rhi::RenderBackend> nextOwner =
        gpu ? std::weak_ptr<Rhi::RenderBackend>(gpu->backend_)
            : std::weak_ptr<Rhi::RenderBackend>{};
    if (impl_->device == gpu && SameOwner(impl_->deviceOwner, nextOwner) && !impl_->active) {
        impl_->stats = {};
        return;
    }
    Shutdown();
    impl_->device = gpu;
    impl_->deviceOwner = nextOwner;
}

void Renderer3D::Context::Shutdown() {
    if (!impl_)
        return;
    if (impl_->modern)
        impl_->modern->ClearDraws();
    impl_->modern.reset();
    impl_->frameTextures.clear();
    impl_->meshes.clear();
    impl_->textures.clear();
    impl_->warnings.clear();
    impl_->settings = {};
    impl_->exposure = 1.0f;
    impl_->lastError.clear();
    impl_->textureBytes = 0;
    impl_->meshBytes = 0;
    impl_->device = nullptr;
    impl_->deviceOwner.reset();
    impl_->active = false;
    impl_->stats = {};
}

void Renderer3D::Context::DetachDevice(const GpuDevice* gpu) {
    if (!impl_ || !gpu)
        return;
    if (impl_->device == gpu) {
        AbortScene();
        impl_->device = nullptr;
        impl_->deviceOwner.reset();
    }
}

bool Renderer3D::Context::IsDeviceValid() const {
    if (!impl_ || !impl_->device)
        return false;
    auto locked = impl_->deviceOwner.lock();
    return locked && locked->Usable();
}

void Renderer3D::Context::BeginScene(const PerspectiveCamera& camera, const DirectionalLight& sun) {
    BeginScene(camera, sun, {});
}

void Renderer3D::Context::BeginScene(const PerspectiveCamera& camera, const DirectionalLight& sun,
                                     std::span<const PointLight> points) {
    Render3DSettings settings;
    settings.sun = sun;
    settings.shadows = false;
    settings.imageBasedLighting = false;
    settings.toneMapping = false;
    BeginScene(camera, settings, 1, points);
}

void Renderer3D::Context::BeginScene(const PerspectiveCamera& camera,
                                     const Render3DSettings& settings, float exposure,
                                     std::span<const PointLight> points) {
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    if (impl_->active)
        throw std::logic_error("3D scene already active");
    if (!ValidateRender3DSettings(settings) || !std::isfinite(exposure) || exposure < 0 ||
        exposure > 16)
        throw std::invalid_argument("Invalid 3D rendering settings");
    impl_->settings = settings;
    impl_->settings.mode = Render3DMode::Pbr;
    impl_->exposure = exposure;
    impl_->camera = camera.GetPosition();
    impl_->sceneCamera = camera;
    impl_->viewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix() *
                            Mat4::Translate(camera.GetPosition());
    impl_->shadowProjection.reset();
    if (settings.shadows)
        impl_->shadowProjection = SceneShadowProjection(camera, settings);
    if (impl_->modern)
        impl_->modern->ClearDraws();
    impl_->lastError.clear();

    if (points.size() > MaxPointLights)
        Log::Warn("Renderer3D uses the first " + std::to_string(MaxPointLights) + " of " +
                  std::to_string(points.size()) + " point lights");
    if (impl_->LockValidState()) {
        const auto finite = [](Vec3 v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        std::array<GpuPointLight, MaxPointLights> gpuPoints{};
        const auto count = std::min(points.size(), MaxPointLights);
        for (size_t i = 0; i < count; ++i) {
            const auto& p = points[i];
            gpuPoints[i] = {finite(p.position) ? p.position : Vec3{},
                            std::isfinite(p.radius) ? std::max(0.f, p.radius) : 0.f,
                            finite(p.color) ? p.color : Vec3{},
                            std::isfinite(p.intensity) ? std::max(0.f, p.intensity) : 0.f};
        }
        impl_->device->SetLighting({settings.sun.direction, settings.sun.color,
                                    settings.sun.intensity, std::span(gpuPoints.data(), count),
                                    camera.GetPosition(), true});
    }
    impl_->stats = {};
    impl_->frameTextures.clear();
    impl_->active = true;
}

void Renderer3D::Context::EndScene() {
    if (!impl_ || !impl_->active)
        return;
    impl_->active = false;
    if ((!impl_->modern || !impl_->modern->DrawCount()) && !impl_->settings.showSkybox)
        return;
    if (auto state = impl_->LockValidState()) {
        if (!impl_->modern)
            impl_->modern = state->CreateScene3DRenderer();
        if (impl_->modern) {
            std::string error;
            impl_->modern->Render(impl_->sceneCamera, impl_->settings, impl_->exposure, error);
            impl_->Report("render3D", error);
        }
    }
    if (impl_->modern)
        impl_->modern->ClearDraws();
}

void Renderer3D::Context::AbortScene() {
    if (!impl_)
        return;
    impl_->active = false;
    if (impl_->modern)
        impl_->modern->ClearDraws();
    impl_->frameTextures.clear();
}

void Renderer3D::Context::DrawMesh(std::span<const Vec3> positions, std::span<const uint32> indices,
                                   const Mat4& transform, const Material3D& material) {
    if (!impl_ || !impl_->active || positions.empty() || indices.empty() || indices.size() % 3)
        return;
    const auto state = impl_->LockValidState();
    if (!state)
        return;
    for (auto index : indices)
        if (index >= positions.size())
            return;
    Mesh3D mesh;
    if (mesh.Upload(impl_->device, positions, indices))
        DrawMesh(mesh, transform, material);
}

void Renderer3D::Context::DrawMesh(const Mat4& transform, const Material3D& material) {
    DrawMeshAsset({}, transform, material);
}

void Renderer3D::Context::DrawMesh(const Mesh3D& mesh, const Mat4& transform,
                                   const Material3D& material) {
    auto texture = ResolveBaseColorTexture(material.baseColorTexture, material.textureSampling,
                                           TextureColorSpace::Srgb, AlphaMaskCutoff(material));
    DrawMesh(mesh, transform, material, texture.get());
}

void Renderer3D::Context::DrawMesh(const Mesh3D& mesh, const Mat4& transform,
                                   const Material3D& material, GpuTexture* texture) {
    DrawMeshWithTextures(mesh, transform, material, {texture});
}

void Renderer3D::Context::DrawMeshWithTextures(const Mesh3D& mesh, const Mat4& transform,
                                               const Material3D& material,
                                               const MaterialTextures3D& textures) {
    if (!impl_ || !impl_->active || !mesh.IsValid())
        return;
    auto lockedState = impl_->LockValidState();
    if (!lockedState)
        return;
    if (!mesh.vertices_->BelongsTo(*impl_->device) || !mesh.indices_->BelongsTo(*impl_->device))
        return;
    const auto* texture = textures.baseColor;
    if ((texture && !texture->BelongsTo(*impl_->device)) ||
        (textures.normal && !textures.normal->BelongsTo(*impl_->device)) ||
        (textures.roughness && !textures.roughness->BelongsTo(*impl_->device)))
        return;
    const auto model = transform * Mat4::Translate(mesh.origin_);
    const auto relativeModel = Mat4::Translate(impl_->camera * -1) * model;
    const bool shadowOnly =
        OutsideClipVolume(impl_->viewProjection * relativeModel, mesh.minimum_, mesh.maximum_);
    const bool outsideShadow =
        impl_->shadowProjection &&
        OutsideClipVolume(*impl_->shadowProjection * relativeModel, mesh.minimum_, mesh.maximum_);
    if (shadowOnly) {
        const bool opaqueCaster = !material.water && material.alphaMode != MaterialAlphaMode::Blend;
        const bool rasterCaster = opaqueCaster && impl_->shadowProjection && !outsideShadow;
        const auto& settings = impl_->settings;
        const bool useRays = settings.rayTracing.Enabled() ||
                             settings.radianceCascades.strength > 0 ||
                             (settings.fog.density > 0 && settings.fog.rayTracedShadows);
        const bool rayCaster =
            opaqueCaster && !mesh.ocean_ && useRays && impl_->device->GetInfo().rayTracing;
        if (!rasterCaster && !rayCaster) {
            ++impl_->stats.meshCount;
            return;
        }
    }
    if (!impl_->modern)
        impl_->modern = lockedState->CreateScene3DRenderer();
    if (!impl_->modern || impl_->modern->DrawCount() >= MaxSceneSubmissions)
        return;
    if (shadowOnly && impl_->modern->DrawCount() >= MaxShadowOnlySubmissions) {
        ++impl_->stats.meshCount;
        return;
    }
    auto normalSampling = material.textureSampling;
    normalSampling.normalMap = true;
    std::array<Ref<GpuTexture>, 6> maps;
    const auto map = [this, &maps](size_t index, const std::string& path,
                                   TextureSampling sampling) {
        maps[index] = ResolveBaseColorTexture(path, sampling, TextureColorSpace::Linear);
        return maps[index] ? maps[index]->handle_ : uint64{0};
    };
    const uint64 normal =
        textures.normal ? textures.normal->handle_ : map(0, material.normalTexture, normalSampling);
    const uint64 metallic = map(1, material.metallicTexture, material.textureSampling);
    const uint64 roughness = textures.roughness
                                 ? textures.roughness->handle_
                                 : map(2, material.roughnessTexture, material.textureSampling);
    const uint64 occlusion = map(3, material.occlusionTexture, material.textureSampling);
    Ref<GpuTexture> terrainBase;
    uint64 terrainColor = 0, terrainNormal = 0, terrainRoughness = 0;
    Vec4 albedo = material.albedoColor;
    if (material.terrainLayer && material.alphaMode == MaterialAlphaMode::Opaque &&
        !material.water) {
        const auto& layer = *material.terrainLayer;
        terrainBase = ResolveBaseColorTexture(layer.baseColorTexture, material.textureSampling,
                                              TextureColorSpace::Srgb);
        if (terrainBase) {
            terrainColor = terrainBase->handle_;
            terrainNormal = map(4, layer.normalTexture, normalSampling);
            terrainRoughness = map(5, layer.roughnessTexture, material.textureSampling);
            albedo.w = 1;
        }
    }
    const bool alphaCoverage = texture && texture->alphaMaskCutoff_ > 0 &&
                               texture->alphaMaskCutoff_ == AlphaMaskCutoff(material);
    const uint32 sampling =
        (material.textureSampling.addressMode == TextureAddressMode::Repeat ? 1u : 0u) |
        (material.textureSampling.anisotropic ? 2u : 0u) | (alphaCoverage ? 4u : 0u);
    Vec4 water{0, 0, 0, 0};
    if (material.water) {
        const auto airToWater = material.water->indexOfRefraction;
        const auto absorption = material.water->absorption;
        if (!std::isfinite(airToWater) || airToWater <= 1 || airToWater > 2.5 ||
            !std::isfinite(absorption.x) || !std::isfinite(absorption.y) ||
            !std::isfinite(absorption.z) ||
            std::min({absorption.x, absorption.y, absorption.z}) < 0 ||
            std::max({absorption.x, absorption.y, absorption.z}) > 100) {
            impl_->Report("water", "Invalid water material absorption or refractive index");
            return;
        }
        water = {static_cast<float>(absorption.x), static_cast<float>(absorption.y),
                 static_cast<float>(absorption.z), airToWater};
    }
    const auto& settings = impl_->settings;
    uint32 shadowPageMask =
        settings.virtualShadows && impl_->shadowProjection
            ? Rhi::VirtualShadowPageMask(*impl_->shadowProjection, relativeModel, mesh.minimum_,
                                         mesh.maximum_, settings.shadowResolution,
                                         settings.shadowNormalBias)
            : LUX_VIRTUAL_SHADOW_ALL_PAGES;
    if (outsideShadow)
        shadowPageMask = 0;
    if (!impl_->modern->AddDraw({mesh.vertices_->handle_,
                                 mesh.indices_->handle_,
                                 texture ? texture->handle_ : 0,
                                 mesh.indexCount_,
                                 model,
                                 albedo,
                                 material.metallic,
                                 material.roughness,
                                 static_cast<uint32>(material.alphaMode),
                                 material.alphaCutoff,
                                 material.doubleSided,
                                 normal,
                                 metallic,
                                 roughness,
                                 occlusion,
                                 material.normalScale,
                                 material.occlusionStrength,
                                 sampling,
                                 shadowOnly,
                                 water,
                                 shadowPageMask,
                                 terrainColor,
                                 terrainNormal,
                                 terrainRoughness}))
        return;
    ++impl_->stats.meshCount;
    lockedState->RetainGeometry(mesh.vertices_, mesh.indices_);
    ++impl_->stats.drawCalls;
}

Ref<GpuTexture> Renderer3D::Context::ResolveBaseColorTexture(const std::string& path,
                                                             TextureSampling sampling,
                                                             TextureColorSpace colorSpace,
                                                             float alphaMaskCutoff) {
    if (path.empty() || !impl_ || !impl_->LockValidState())
        return nullptr;
    if (!sampling.mipmaps)
        alphaMaskCutoff = 0;
    const std::string key =
        path + "\n" + std::to_string(static_cast<int>(colorSpace)) +
        std::to_string(sampling.mipmaps) + std::to_string(static_cast<int>(sampling.addressMode)) +
        std::to_string(sampling.anisotropic) + std::to_string(sampling.normalMap) + ":" +
        std::to_string(std::bit_cast<uint32>(alphaMaskCutoff));
    auto found = impl_->textures.find(key);
    if (found == impl_->textures.end()) {
        if (impl_->textures.size() >= MaxCachedAssets) {
            const auto oldest = std::min_element(
                impl_->textures.begin(), impl_->textures.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            impl_->textureBytes -= oldest->second.bytes;
            impl_->textures.erase(oldest);
        }
        found = impl_->textures.emplace(key, Impl::CachedTexture{}).first;
    }
    auto& cached = found->second;
    cached.used = ++impl_->sequence;
    const auto revision = AssetManager::GetTextureRevision(path);
    if (cached.attempted != revision) {
        cached.attempted = revision;
        try {
            int width = 0, height = 0;
            std::vector<uint8> pixels;
            const bool decoded = AssetManager::IsBaseColorTextureFile(path) &&
                                 AssetManager::DecodeTextureCpu(path, width, height, pixels);
            const size_t bytes = Rhi::GetTextureLayout(width, height, sampling.mipmaps).bytes;
            constexpr size_t Budget = 512ull * 1024 * 1024;
            bool room = decoded && bytes && bytes <= Budget;
            if (room) {
                while (impl_->textureBytes - cached.bytes + bytes > Budget &&
                       impl_->textures.size() > 1) {
                    auto oldest = impl_->textures.end();
                    for (auto item = impl_->textures.begin(); item != impl_->textures.end(); ++item)
                        if (item != found && (oldest == impl_->textures.end() ||
                                              item->second.used < oldest->second.used))
                            oldest = item;
                    if (oldest == impl_->textures.end())
                        break;
                    impl_->textureBytes -= oldest->second.bytes;
                    impl_->textures.erase(oldest);
                }
                room = impl_->textureBytes - cached.bytes + bytes <= Budget;
            }
            auto next = CreateRef<GpuTexture>();
            if (room &&
                next->Initialize(impl_->device, static_cast<uint32>(width),
                                 static_cast<uint32>(height), pixels.data(), TextureFilter::Linear,
                                 colorSpace, sampling, alphaMaskCutoff)) {
                impl_->textureBytes = impl_->textureBytes - cached.bytes + bytes;
                cached.texture = std::move(next);
                cached.bytes = bytes;
                cached.error.clear();
            } else {
                cached.error =
                    !decoded ? "텍스처 파일을 디코딩하지 못했습니다: " + path
                    : !room
                        ? "텍스처 캐시 예산을 초과했습니다: " + path
                        : "GPU 텍스처 업로드 실패: " + impl_->device->GetLastError() + " / " + path;
            }
        } catch (const std::exception&) {
            cached.error = "기본 색상 텍스처 갱신에 실패했습니다: " + path;
        }
    }
    auto error = AssetManager::GetAssetError(path, true);
    if (error.empty())
        error = cached.error;
    if (!error.empty() && cached.texture)
        error += " (이전 정상 텍스처 유지)";
    impl_->Report("texture:" + path, error);
    if (impl_->active && cached.texture)
        impl_->frameTextures.push_back(cached.texture);
    return cached.texture;
}

bool Renderer3D::Context::DrawMeshAsset(const std::string& path, const Mat4& transform,
                                        const Material3D& material, int32 modelSubmesh, int32 clip,
                                        double time, uint64 instance, const ModelPose* overridePose,
                                        uint64 poseRevision,
                                        const Ref<const ModelPose>& poseOwner) {
    if (!impl_ || !impl_->active || !impl_->LockValidState())
        return false;
    std::string key = path;
    if (modelSubmesh >= 0) {
        key.push_back('\0');
        key += std::to_string(modelSubmesh) + ":" + std::to_string(instance);
        // A host without an instance ID can still draw distinct poses in the same frame.
        if (instance == 0 && clip >= 0)
            key += ":" + std::to_string(clip) + ":" + std::to_string(std::bit_cast<uint64>(time));
    }
    auto found = impl_->meshes.find(key);
    if (found == impl_->meshes.end()) {
        if (impl_->meshes.size() >= MaxCachedAssets) {
            const auto oldest = std::min_element(
                impl_->meshes.begin(), impl_->meshes.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            impl_->meshBytes -= oldest->second.bytes;
            impl_->meshes.erase(oldest);
        }
        found = impl_->meshes.emplace(key, Impl::CachedMesh{}).first;
    }
    auto& cached = found->second;
    cached.used = ++impl_->sequence;
    const auto revision = path.empty() ? uint64{1} : AssetManager::GetMeshRevision(path);
    const bool requiresUv = !material.baseColorTexture.empty() || !material.normalTexture.empty() ||
                            !material.metallicTexture.empty() ||
                            !material.roughnessTexture.empty() ||
                            !material.occlusionTexture.empty() || material.terrainLayer.has_value();
    if (modelSubmesh >= 0) {
        std::string error;
        auto model = ModelAsset::LoadFbx(path, error);
        if ((!overridePose && !std::isfinite(time)) || clip < -1) {
            model.reset();
            error = "FBX 애니메이션 시간 또는 클립 번호가 올바르지 않습니다.";
        }
        if (model && static_cast<size_t>(modelSubmesh) < model->submeshes.size()) {
            const auto index = static_cast<size_t>(modelSubmesh);
            const auto& part = model->submeshes[index];
            if (overridePose && (index >= overridePose->submeshes.size() ||
                                 overridePose->submeshes[index].size() != part.vertices.size())) {
                impl_->Report("mesh:" + key,
                              "FBX 포즈의 정점 데이터가 하위 메시와 일치하지 않습니다: " + path);
                return false;
            }
            const bool ownsPose = overridePose && poseOwner.get() == overridePose;
            const bool poseMatches = overridePose ? ownsPose && cached.usesOverride &&
                                                        cached.pose.lock() == poseOwner &&
                                                        cached.poseRevision == poseRevision
                                                  : !cached.usesOverride && cached.time == time;
            if (requiresUv && !part.hasTexCoords) {
                error = "텍스처를 적용할 UV 좌표가 없는 모델입니다: " + path;
            } else if (cached.model != model || cached.clip != clip || !poseMatches) {
                ModelPose pose;
                const std::vector<ModelVertex>* source = &part.vertices;
                if (overridePose) {
                    source = &overridePose->submeshes[index];
                } else if (clip >= 0 || part.skinned || part.morphTargets) {
                    if (model->Evaluate(time, clip, pose, error))
                        source = &pose.submeshes[static_cast<size_t>(modelSubmesh)];
                }
                if (error.empty()) {
                    std::vector<Vertex3D> vertices;
                    vertices.reserve(source->size());
                    for (const auto& vertex : *source)
                        vertices.push_back({vertex.position, vertex.normal, vertex.texCoords});
                    const size_t bytes = vertices.size() * sizeof(RenderVertex) +
                                         part.indices.size() * sizeof(uint32);
                    constexpr size_t Budget = 256ull * 1024 * 1024;
                    bool room = bytes <= Budget;
                    while (room && impl_->meshBytes - cached.bytes + bytes > Budget &&
                           impl_->meshes.size() > 1) {
                        auto oldest = impl_->meshes.end();
                        for (auto item = impl_->meshes.begin(); item != impl_->meshes.end(); ++item)
                            if (item != found && (oldest == impl_->meshes.end() ||
                                                  item->second.used < oldest->second.used))
                                oldest = item;
                        if (oldest == impl_->meshes.end())
                            break;
                        impl_->meshBytes -= oldest->second.bytes;
                        impl_->meshes.erase(oldest);
                    }
                    room = room && impl_->meshBytes - cached.bytes + bytes <= Budget;
                    bool uploaded = room && cached.model == model && cached.mesh &&
                                    cached.mesh->UpdateVertices(vertices);
                    if (room && !uploaded) {
                        auto mesh = CreateRef<Mesh3D>();
                        uploaded = mesh->Upload(impl_->device, vertices, part.indices);
                        if (uploaded)
                            cached.mesh = std::move(mesh);
                    }
                    if (uploaded) {
                        impl_->meshBytes = impl_->meshBytes - cached.bytes + bytes;
                        cached.bytes = bytes;
                        cached.model = std::move(model);
                        cached.clip = clip;
                        cached.time = time;
                        cached.usesOverride = overridePose != nullptr;
                        cached.pose = ownsPose ? poseOwner : Ref<const ModelPose>{};
                        cached.poseRevision = poseRevision;
                        cached.hasUvs = part.hasTexCoords;
                        cached.error.clear();
                    } else {
                        error = room ? "FBX 메시 업로드에 실패했습니다: " + path
                                     : "메시 캐시 예산을 초과했습니다: " + path;
                    }
                }
            } else {
                cached.error.clear();
            }
        } else if (model) {
            error = "FBX 하위 메시 번호가 올바르지 않습니다: " + path;
        }
        if (!error.empty())
            cached.error = std::move(error);
    } else if (cached.attempted != revision || (cached.uvBlocked && !requiresUv) ||
               (requiresUv && !cached.hasUvs && !cached.uvBlocked)) {
        cached.attempted = revision;
        cached.uvBlocked = false;
        try {
            RawMeshData raw;
            std::vector<Vertex3D> vertices;
            std::vector<uint32> indices;
            bool loaded = path.empty();
            if (path.empty()) {
                const std::array<Vec2, 6> uv{{{0, 1}, {1, 0}, {1, 1}, {0, 1}, {0, 0}, {1, 0}}};
                for (size_t face = 0; face < 6; ++face) {
                    const auto normal = (CubePositions[CubeIndices[face * 6 + 1]] -
                                         CubePositions[CubeIndices[face * 6]])
                                            .Cross(CubePositions[CubeIndices[face * 6 + 2]] -
                                                   CubePositions[CubeIndices[face * 6]])
                                            .Normalized();
                    for (size_t corner = 0; corner < 6; ++corner) {
                        indices.push_back(static_cast<uint32>(vertices.size()));
                        vertices.push_back(
                            {CubePositions[CubeIndices[face * 6 + corner]], normal, uv[corner]});
                    }
                }
            } else if ((loaded = AssetManager::LoadMesh(path, raw))) {
                cached.attempted = raw.revision;
                vertices.reserve(raw.vertices.size() / 3);
                for (size_t i = 0; i < raw.vertices.size() / 3; ++i)
                    vertices.push_back(
                        {{raw.vertices[i * 3], raw.vertices[i * 3 + 1], raw.vertices[i * 3 + 2]},
                         {raw.normals[i * 3], raw.normals[i * 3 + 1], raw.normals[i * 3 + 2]},
                         {raw.texCoords[i * 2], raw.texCoords[i * 2 + 1]}});
                indices = std::move(raw.indices);
            }
            const bool missingUv = loaded && requiresUv && !path.empty() && !raw.hasTexCoords;
            if (missingUv) {
                loaded = false;
                cached.uvBlocked = true;
            }
            const size_t bytes =
                vertices.size() * sizeof(RenderVertex) + indices.size() * sizeof(uint32);
            constexpr size_t Budget = 256ull * 1024 * 1024;
            bool room = loaded && bytes <= Budget;
            if (room) {
                while (impl_->meshBytes - cached.bytes + bytes > Budget &&
                       impl_->meshes.size() > 1) {
                    auto oldest = impl_->meshes.end();
                    for (auto item = impl_->meshes.begin(); item != impl_->meshes.end(); ++item)
                        if (item != found && (oldest == impl_->meshes.end() ||
                                              item->second.used < oldest->second.used))
                            oldest = item;
                    if (oldest == impl_->meshes.end())
                        break;
                    impl_->meshBytes -= oldest->second.bytes;
                    impl_->meshes.erase(oldest);
                }
                room = impl_->meshBytes - cached.bytes + bytes <= Budget;
            }
            auto next = CreateRef<Mesh3D>();
            if (room && next->Upload(impl_->device, vertices, indices)) {
                impl_->meshBytes = impl_->meshBytes - cached.bytes + bytes;
                cached.mesh = std::move(next);
                cached.bytes = bytes;
                cached.error.clear();
                cached.hasUvs = path.empty() || raw.hasTexCoords;
                cached.minimum = path.empty() ? Vec3{-.5f, -.5f, -.5f} : raw.boundsMin;
                cached.maximum = path.empty() ? Vec3{.5f, .5f, .5f} : raw.boundsMax;
            } else {
                const auto asset = path.empty() ? std::string("기본 큐브") : path;
                cached.error = missingUv ? "텍스처를 적용할 UV 좌표가 없는 모델입니다: " + asset
                               : !loaded ? "모델 메시를 읽지 못했습니다: " + asset
                               : !room   ? "메시 캐시 예산을 초과했습니다: " + asset
                                       : "GPU 메시 업로드 실패: " + impl_->device->GetLastError() +
                                             " / " + asset;
            }
        } catch (const std::exception&) {
            cached.error = "3D 메시 갱신에 실패했습니다: " + path;
        }
    }
    auto error =
        path.empty() || modelSubmesh >= 0 ? std::string{} : AssetManager::GetAssetError(path);
    if (error.empty())
        error = cached.error;
    if (!error.empty() && cached.mesh)
        error += " (이전 정상 메시 유지)";
    impl_->Report("mesh:" + key, error);
    if (!cached.mesh) {
        Material3D placeholder;
        placeholder.albedoColor = {.85f, .15f, .25f, 1};
        DrawMesh(CubePositions, CubeIndices, transform, placeholder);
        return false;
    }
    const auto previous = impl_->stats.meshCount;
    auto drawable = material;
    if (!cached.hasUvs) {
        drawable.baseColorTexture.clear();
        drawable.normalTexture.clear();
        drawable.metallicTexture.clear();
        drawable.roughnessTexture.clear();
        drawable.occlusionTexture.clear();
        drawable.terrainLayer.reset();
    }
    DrawMesh(*cached.mesh, transform, drawable);
    return impl_->stats.meshCount > previous;
}

const std::string& Renderer3D::Context::GetLastAssetError() const {
    static const std::string empty;
    return impl_ ? impl_->lastError : empty;
}

bool Renderer3D::Context::GetMeshBounds(const std::string& path, Vec3& minimum,
                                        Vec3& maximum) const {
    if (!impl_)
        return false;
    const auto found = impl_->meshes.find(path);
    if (found == impl_->meshes.end())
        return false;
    minimum = found->second.minimum;
    maximum = found->second.maximum;
    return true;
}

void Renderer3D::Context::DrawCube(const Vec3& p, const Vec3& scale, const Material3D& material) {
    DrawMesh(Mat4::Translate(p) * Mat4::Scale(scale), material);
}

void Renderer3D::Context::ResetStats() {
    if (impl_)
        impl_->stats = {};
}

Renderer3D::Statistics Renderer3D::Context::GetStats() const {
    return impl_ ? impl_->stats : Statistics{};
}

Renderer3D::ScopedContext::ScopedContext(Context& context)
    : previous_(t_ActiveContext3D), current_(&context) {
    t_ActiveContext3D = current_;
}

Renderer3D::ScopedContext::~ScopedContext() {
    if (current_)
        current_->AbortScene();
    t_ActiveContext3D = previous_;
}

void Renderer3D::Init(GpuDevice* gpu) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->Init(gpu);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().Init(gpu);
}

void Renderer3D::Shutdown() {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->Shutdown();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().Shutdown();
}

void Renderer3D::DetachDevice(const GpuDevice* gpu) {
    if (t_ActiveContext3D)
        t_ActiveContext3D->DetachDevice(gpu);
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().DetachDevice(gpu);
}

void Renderer3D::BeginScene(const PerspectiveCamera& camera, const DirectionalLight& sun) {
    BeginScene(camera, sun, {});
}

void Renderer3D::BeginScene(const PerspectiveCamera& camera, const DirectionalLight& sun,
                            std::span<const PointLight> points) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->BeginScene(camera, sun, points);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().BeginScene(camera, sun, points);
}

void Renderer3D::BeginScene(const PerspectiveCamera& camera, const Render3DSettings& settings,
                            float exposure, std::span<const PointLight> points) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->BeginScene(camera, settings, exposure, points);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().BeginScene(camera, settings, exposure, points);
}

void Renderer3D::EndScene() {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->EndScene();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().EndScene();
}

void Renderer3D::DrawMesh(std::span<const Vec3> positions, std::span<const uint32> indices,
                          const Mat4& transform, const Material3D& material) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->DrawMesh(positions, indices, transform, material);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().DrawMesh(positions, indices, transform, material);
}

void Renderer3D::DrawMesh(const Mat4& transform, const Material3D& material) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->DrawMesh(transform, material);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().DrawMesh(transform, material);
}

void Renderer3D::DrawMesh(const Mesh3D& mesh, const Mat4& transform, const Material3D& material) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->DrawMesh(mesh, transform, material);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().DrawMesh(mesh, transform, material);
}

void Renderer3D::DrawMesh(const Mesh3D& mesh, const Mat4& transform, const Material3D& material,
                          GpuTexture* texture) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->DrawMesh(mesh, transform, material, texture);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().DrawMesh(mesh, transform, material, texture);
}

bool Renderer3D::DrawMeshAsset(const std::string& path, const Mat4& transform,
                               const Material3D& material, int32 modelSubmesh, int32 clip,
                               double time, uint64 instance, const ModelPose* overridePose,
                               uint64 poseRevision, const Ref<const ModelPose>& poseOwner) {
    if (t_ActiveContext3D)
        return t_ActiveContext3D->DrawMeshAsset(path, transform, material, modelSubmesh, clip, time,
                                                instance, overridePose, poseRevision, poseOwner);
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    return DefaultContext3D().DrawMeshAsset(path, transform, material, modelSubmesh, clip, time,
                                            instance, overridePose, poseRevision, poseOwner);
}

void Renderer3D::DrawCube(const Vec3& p, const Vec3& scale, const Material3D& material) {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->DrawCube(p, scale, material);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().DrawCube(p, scale, material);
}

void Renderer3D::ResetStats() {
    if (t_ActiveContext3D) {
        t_ActiveContext3D->ResetStats();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    DefaultContext3D().ResetStats();
}

Renderer3D::Statistics Renderer3D::GetStats() {
    if (t_ActiveContext3D)
        return t_ActiveContext3D->GetStats();
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    return DefaultContext3D().GetStats();
}

const std::string& Renderer3D::GetLastAssetError() {
    if (t_ActiveContext3D)
        return t_ActiveContext3D->GetLastAssetError();
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext3DMutex);
    return DefaultContext3D().GetLastAssetError();
}

void PerspectiveCamera::SetPosition(const Vec3& position) {
    SetView(position, target_, up_);
}

void PerspectiveCamera::SetTarget(const Vec3& target) {
    SetView(position_, target, up_);
}

void PerspectiveCamera::SetUp(const Vec3& up) {
    SetView(position_, target_, up);
}

const Vec3& PerspectiveCamera::GetPosition() const {
    return position_;
}

const Vec3& PerspectiveCamera::GetTarget() const {
    return target_;
}

const Mat4& PerspectiveCamera::GetProjectionMatrix() const {
    return projectionMatrix_;
}

const Mat4& PerspectiveCamera::GetViewMatrix() const {
    return viewMatrix_;
}

const Mat4& PerspectiveCamera::GetViewProjectionMatrix() const {
    return viewProjectionMatrix_;
}

uint32 Mesh3D::GetIndexCount() const {
    return IsValid() ? indexCount_ : 0;
}

PerspectiveCamera::PerspectiveCamera() {
    SetView(position_, target_, up_);
}
} // namespace LuxEngine
