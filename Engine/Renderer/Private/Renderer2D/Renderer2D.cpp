#include "RHI/RenderBackend.hpp"
#include <LuxEngine/Assets/AssetManager.hpp>
#include <LuxEngine/Renderer/Renderer2D/Renderer2D.hpp>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace LuxEngine {
namespace {
struct Batch {
    std::weak_ptr<Rhi::RenderBackend> owner;
    uint64 texture = 0;
    std::vector<RenderVertex> vertices;
};

bool SameOwner(const std::weak_ptr<Rhi::RenderBackend>& a,
               const std::weak_ptr<Rhi::RenderBackend>& b) {
    return !a.owner_before(b) && !b.owner_before(a);
}

bool FiniteColor(const Vec4& color) {
    return std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z) &&
           std::isfinite(color.w);
}

thread_local Renderer2D::Context* t_ActiveContext2D = nullptr;
std::recursive_mutex s_DefaultContext2DMutex;

Renderer2D::Context& DefaultContext2D() {
    static Renderer2D::Context s_Default;
    return s_Default;
}
} // namespace

struct Renderer2D::Context::Impl {
    Vec3 origin;
    std::vector<Ref<GpuTexture>> textures;
    GpuDevice* device = nullptr;
    std::weak_ptr<Rhi::RenderBackend> deviceOwner;
    Mat4 viewProjection;
    std::vector<Batch> batches;
    size_t usedBatches = 0;
    Renderer2D::Statistics stats;
    bool active = false;

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

    std::span<RenderVertex> Quad(const Vec3& position, const Vec2& size, double angle,
                                 const std::weak_ptr<Rhi::RenderBackend>& owner, uint64 texture,
                                 const Vec4& color) {
        if (!active)
            return {};
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z) || !std::isfinite(size.x) || !std::isfinite(size.y) ||
            !std::isfinite(angle) || !FiniteColor(color))
            return {};
        if (!usedBatches || batches[usedBatches - 1].texture != texture ||
            !SameOwner(batches[usedBatches - 1].owner, owner) ||
            batches[usedBatches - 1].vertices.size() >= 6000) {
            if (usedBatches == batches.size())
                batches.emplace_back();
            auto& batch = batches[usedBatches++];
            batch.owner = owner;
            batch.texture = texture;
            batch.vertices.clear();
            batch.vertices.reserve(6000);
        }
        const double c = std::cos(angle), s = std::sin(angle);
        const std::array<Vec2, 4> corners{
            {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}}};
        const std::array<Vec2, 4> uv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
        auto& vertices = batches[usedBatches - 1].vertices;
        for (auto i : {0, 1, 2, 2, 3, 0}) {
            const double x = corners[i].x * size.x, y = corners[i].y * size.y;
            vertices.push_back({{position.x - origin.x + c * x - s * y,
                                 position.y - origin.y + s * x + c * y, position.z - origin.z},
                                color,
                                uv[i]});
        }
        ++stats.quadCount;
        return std::span(vertices).last(6);
    }
};

OrthographicCamera::OrthographicCamera(double left, double right, double bottom, double top) {
    SetProjection(left, right, bottom, top);
}

void OrthographicCamera::SetProjection(double left, double right, double bottom, double top) {
    projectionMatrix_ = Mat4::Orthographic(left, right, bottom, top, -1.0f, 1.0f);
    RecalculateMatrix();
}

void OrthographicCamera::RecalculateMatrix() {
    viewMatrix_ =
        Mat4::RotateZ(-rotation_) * Mat4::Translate(Vec3(-position_.x, -position_.y, -position_.z));
    viewProjectionMatrix_ = projectionMatrix_ * viewMatrix_;
}

Renderer2D::Context::Context() : impl_(std::make_unique<Impl>()) {}

Renderer2D::Context::Context(GpuDevice* gpu) : impl_(std::make_unique<Impl>()) {
    Init(gpu);
}

Renderer2D::Context::~Context() {
    Shutdown();
}

Renderer2D::Context::Context(Context&& other) noexcept = default;
Renderer2D::Context& Renderer2D::Context::operator=(Context&& other) noexcept = default;

void Renderer2D::Context::Init(GpuDevice* gpu) {
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    const std::weak_ptr<Rhi::RenderBackend> nextOwner =
        gpu ? std::weak_ptr<Rhi::RenderBackend>(gpu->backend_)
            : std::weak_ptr<Rhi::RenderBackend>{};
    if (impl_->device == gpu && SameOwner(impl_->deviceOwner, nextOwner) && !impl_->active) {
        for (size_t i = 0; i < impl_->usedBatches; ++i)
            impl_->batches[i].owner.reset();
        impl_->usedBatches = 0;
        impl_->stats = {};
        return;
    }
    Shutdown();
    impl_->device = gpu;
    impl_->deviceOwner = nextOwner;
}

void Renderer2D::Context::Shutdown() {
    if (!impl_)
        return;
    impl_->batches.clear();
    impl_->usedBatches = 0;
    impl_->device = nullptr;
    impl_->deviceOwner.reset();
    impl_->active = false;
    impl_->stats = {};
}

void Renderer2D::Context::DetachDevice(const GpuDevice* gpu) {
    if (!impl_ || !gpu)
        return;
    if (impl_->device == gpu) {
        AbortScene();
        impl_->device = nullptr;
        impl_->deviceOwner.reset();
    }
}

bool Renderer2D::Context::IsDeviceValid() const {
    if (!impl_ || !impl_->device)
        return false;
    auto locked = impl_->deviceOwner.lock();
    return locked && locked->Usable();
}

void Renderer2D::Context::BeginScene(const OrthographicCamera& camera) {
    if (!impl_)
        impl_ = std::make_unique<Impl>();
    if (impl_->active)
        throw std::logic_error("2D scene already active");
    impl_->textures.clear();
    impl_->origin = camera.GetPosition();
    impl_->viewProjection =
        camera.GetProjectionMatrix() * camera.GetViewMatrix() * Mat4::Translate(impl_->origin);
    impl_->usedBatches = 0;
    impl_->stats = {};
    impl_->active = true;
}

void Renderer2D::Context::EndScene() {
    if (!impl_ || !impl_->active)
        return;
    impl_->active = false;
    auto lockedState = impl_->LockValidState();
    GpuDevice* activeDevice = lockedState ? impl_->device : nullptr;
    for (size_t i = 0; i < impl_->usedBatches; ++i) {
        auto& batch = impl_->batches[i];
        const bool sameDevice =
            !batch.texture || (activeDevice && batch.owner.lock() == lockedState);
        if (activeDevice && sameDevice &&
            activeDevice->DrawTexturedTriangles(batch.vertices, impl_->viewProjection,
                                                batch.texture, false))
            ++impl_->stats.drawCalls;
        batch.owner.reset();
    }
    impl_->usedBatches = 0;
}

void Renderer2D::Context::AbortScene() {
    if (!impl_)
        return;
    impl_->active = false;
    for (size_t i = 0; i < impl_->usedBatches; ++i)
        impl_->batches[i].owner.reset();
    impl_->usedBatches = 0;
}

bool Renderer2D::Context::GetVisibleLocalBounds(const Mat4& transform, Vec2& minimum,
                                                Vec2& maximum) const {
    if (!impl_ || !impl_->active)
        return false;
    const auto projection =
        impl_->viewProjection * (Mat4::Translate(impl_->origin * -1) * transform);
    const auto& m = projection.elements;
    if (m[3] != 0 || m[7] != 0 || m[15] != 1)
        return false;
    const double determinant = m[0] * m[5] - m[4] * m[1];
    const double scale = std::hypot(m[0], m[1]) * std::hypot(m[4], m[5]);
    if (!(std::abs(determinant) > scale * 1e-12))
        return false;
    bool first = true;
    for (const double x : {-1.0, 1.0})
        for (const double y : {-1.0, 1.0}) {
            const auto dx = x - m[12], dy = y - m[13];
            const Vec2 local{(m[5] * dx - m[4] * dy) / determinant,
                             (m[0] * dy - m[1] * dx) / determinant};
            if (!std::isfinite(local.x) || !std::isfinite(local.y))
                return false;
            minimum =
                first ? local : Vec2{std::min(minimum.x, local.x), std::min(minimum.y, local.y)};
            maximum =
                first ? local : Vec2{std::max(maximum.x, local.x), std::max(maximum.y, local.y)};
            first = false;
        }
    return true;
}

void Renderer2D::Context::DrawQuad(const Vec2& p, const Vec2& size, const Vec4& color) {
    if (impl_)
        impl_->Quad({p.x, p.y, 0}, size, 0, {}, 0, color);
}

void Renderer2D::Context::DrawQuad(const Vec3& p, const Vec2& size, const Vec4& color) {
    if (impl_)
        impl_->Quad(p, size, 0, {}, 0, color);
}

void Renderer2D::Context::DrawQuad(const Mat4& transform, const Vec4& color) {
    if (!impl_ || !impl_->active)
        return;
    for (const auto value : transform.elements)
        if (!std::isfinite(value))
            return;
    if (!FiniteColor(color))
        return;
    constexpr std::array<Vec2, 6> points{
        {{-.5f, -.5f}, {.5f, -.5f}, {.5f, .5f}, {.5f, .5f}, {-.5f, .5f}, {-.5f, -.5f}}};
    std::array<Vec3, 6> positions;
    const auto* m = transform.elements;
    for (size_t index = 0; index < points.size(); ++index) {
        const auto point = points[index];
        positions[index] = {m[0] * point.x + m[4] * point.y + m[12] - impl_->origin.x,
                            m[1] * point.x + m[5] * point.y + m[13] - impl_->origin.y, 0};
        if (!std::isfinite(positions[index].x) || !std::isfinite(positions[index].y))
            return;
    }
    // 2D Z ordering is applied by Scene before batching; flatten Z as DrawRotatedQuad does.
    auto vertices = impl_->Quad({}, {1, 1}, 0, {}, 0, color);
    for (size_t index = 0; index < vertices.size(); ++index)
        vertices[index].position = positions[index];
}

void Renderer2D::Context::DrawRotatedQuad(const Vec2& p, const Vec2& size, double angle,
                                          const Vec4& color) {
    if (impl_)
        impl_->Quad({p.x, p.y, 0}, size, angle, {}, 0, color);
}

void Renderer2D::Context::DrawSprite(const Vec2& p, const Vec2& size, GpuTexture* texture,
                                     const Vec4& tint) {
    if (!impl_ || !texture || !texture->IsValid())
        return;
    impl_->Quad({p.x, p.y, 0}, size, 0, texture->owner_, texture->handle_, tint);
}

bool Renderer2D::Context::DrawSpriteAsset(const Mat4& transform, const std::string& path,
                                          uint32 frame, uint32 columns, uint32 rows,
                                          const Vec4& tint) {
    if (!impl_ || !impl_->active || !FiniteColor(tint) || !impl_->LockValidState() || !columns ||
        !rows || columns > 4096 || rows > 4096 || frame >= columns * rows)
        return false;
    auto texture = AssetManager::LoadTexture(path, impl_->device);
    if (!texture || !texture->IsValid())
        return false;
    auto vertices = impl_->Quad({}, {1, 1}, 0, texture->owner_, texture->handle_, tint);
    if (vertices.empty())
        return false;
    impl_->textures.push_back(texture);
    constexpr std::array<Vec2, 6> corners{
        {{-.5, -.5}, {.5, -.5}, {.5, .5}, {.5, .5}, {-.5, .5}, {-.5, -.5}}};
    for (size_t i = 0; i < 6; ++i) {
        auto& vertex = vertices[i];
        vertex.position = transform.TransformPoint({corners[i].x, corners[i].y, 0}) - impl_->origin;
        vertex.position.z = 0;
        vertex.uv = {(double(frame % columns) + corners[i].x + .5) / columns,
                     (double(frame / columns) + .5 - corners[i].y) / rows};
    }
    return true;
}

void Renderer2D::Context::ResetStats() {
    if (impl_)
        impl_->stats = {};
}

Renderer2D::Statistics Renderer2D::Context::GetStats() const {
    return impl_ ? impl_->stats : Statistics{};
}

Renderer2D::ScopedContext::ScopedContext(Context& context)
    : previous_(t_ActiveContext2D), current_(&context) {
    t_ActiveContext2D = current_;
}

Renderer2D::ScopedContext::~ScopedContext() {
    if (current_)
        current_->AbortScene();
    t_ActiveContext2D = previous_;
}

void Renderer2D::Init(GpuDevice* gpu) {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->Init(gpu);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().Init(gpu);
}

void Renderer2D::Shutdown() {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->Shutdown();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().Shutdown();
}

void Renderer2D::DetachDevice(const GpuDevice* gpu) {
    if (t_ActiveContext2D)
        t_ActiveContext2D->DetachDevice(gpu);
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().DetachDevice(gpu);
}

void Renderer2D::BeginScene(const OrthographicCamera& camera) {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->BeginScene(camera);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().BeginScene(camera);
}

void Renderer2D::EndScene() {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->EndScene();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().EndScene();
}

void Renderer2D::DrawQuad(const Vec2& p, const Vec2& size, const Vec4& color) {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->DrawQuad(p, size, color);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().DrawQuad(p, size, color);
}

bool Renderer2D::GetVisibleLocalBounds(const Mat4& transform, Vec2& minimum, Vec2& maximum) {
    if (t_ActiveContext2D)
        return t_ActiveContext2D->GetVisibleLocalBounds(transform, minimum, maximum);
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    return DefaultContext2D().GetVisibleLocalBounds(transform, minimum, maximum);
}

void Renderer2D::DrawQuad(const Vec3& p, const Vec2& size, const Vec4& color) {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->DrawQuad(p, size, color);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().DrawQuad(p, size, color);
}

void Renderer2D::DrawQuad(const Mat4& transform, const Vec4& color) {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->DrawQuad(transform, color);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().DrawQuad(transform, color);
}

void Renderer2D::DrawRotatedQuad(const Vec2& p, const Vec2& size, double angle, const Vec4& color) {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->DrawRotatedQuad(p, size, angle, color);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().DrawRotatedQuad(p, size, angle, color);
}

void Renderer2D::DrawSprite(const Vec2& p, const Vec2& size, GpuTexture* texture,
                            const Vec4& tint) {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->DrawSprite(p, size, texture, tint);
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().DrawSprite(p, size, texture, tint);
}

bool Renderer2D::DrawSpriteAsset(const Mat4& transform, const std::string& path, uint32 frame,
                                 uint32 columns, uint32 rows, const Vec4& tint) {
    if (t_ActiveContext2D)
        return t_ActiveContext2D->DrawSpriteAsset(transform, path, frame, columns, rows, tint);
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    return DefaultContext2D().DrawSpriteAsset(transform, path, frame, columns, rows, tint);
}

void Renderer2D::ResetStats() {
    if (t_ActiveContext2D) {
        t_ActiveContext2D->ResetStats();
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    DefaultContext2D().ResetStats();
}

Renderer2D::Statistics Renderer2D::GetStats() {
    if (t_ActiveContext2D)
        return t_ActiveContext2D->GetStats();
    std::lock_guard<std::recursive_mutex> lock(s_DefaultContext2DMutex);
    return DefaultContext2D().GetStats();
}

void OrthographicCamera::SetPosition(const Vec3& position) {
    position_ = position;
    RecalculateMatrix();
}

void OrthographicCamera::SetRotation(double rotationRadians) {
    rotation_ = rotationRadians;
    RecalculateMatrix();
}

const Vec3& OrthographicCamera::GetPosition() const {
    return position_;
}

double OrthographicCamera::GetRotation() const {
    return rotation_;
}

const Mat4& OrthographicCamera::GetProjectionMatrix() const {
    return projectionMatrix_;
}

const Mat4& OrthographicCamera::GetViewMatrix() const {
    return viewMatrix_;
}

const Mat4& OrthographicCamera::GetViewProjectionMatrix() const {
    return viewProjectionMatrix_;
}

OrthographicCamera::OrthographicCamera() = default;
} // namespace LuxEngine
