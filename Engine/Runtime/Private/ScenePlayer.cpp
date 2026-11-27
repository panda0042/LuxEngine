#include <LuxEngine/Assets/AssetManager.hpp>
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/ECS/ECS.hpp>
#include <LuxEngine/Physics/PhysicsEngine.hpp>
#include <LuxEngine/Platform/Input.hpp>
#include <LuxEngine/Platform/Window.hpp>
#include <LuxEngine/Runtime/ScenePlayer.hpp>
#include <SDL3/SDL_filesystem.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace LuxEngine {
namespace {
bool ParseNumber(const std::string& text, double& value) {
    if (text.empty() || text.front() == '+')
        return false;
    std::istringstream input(text);
    input.imbue(std::locale::classic());
    input >> std::noskipws >> value;
    return !input.fail() && input.eof() && std::isfinite(value);
}

bool ParseNumber(const std::string& text, uint32& value) {
    const auto* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

constexpr std::pair<const char*, LuxKey> PlayerBindings[] = {
    {"Left", LuxKey::A},      {"Left", LuxKey::Left}, {"Right", LuxKey::D},
    {"Right", LuxKey::Right}, {"Up", LuxKey::W},      {"Up", LuxKey::Up},
    {"Down", LuxKey::S},      {"Down", LuxKey::Down}, {"Jump", LuxKey::Space},
};

bool IsPlayerTag(const std::string& tag) {
    return tag == "플레이어" || tag == "주인공" || tag == "Player";
}

bool Finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Vec3 Unit(Vec3 value, Vec3 fallback) {
    const double length = std::hypot(static_cast<double>(value.x), static_cast<double>(value.y),
                                     static_cast<double>(value.z));
    if (!std::isfinite(length) || length < 1e-12)
        return fallback;
    return {static_cast<double>(value.x / length), static_cast<double>(value.y / length),
            static_cast<double>(value.z / length)};
}

Vec3 CameraUp(Vec3 forward, Vec3 preferred) {
    preferred = Unit(preferred, {0, 1, 0});
    auto right = forward.Cross(preferred);
    if (right.Dot(right) < 1e-10f) {
        preferred = std::abs(forward.y) < .95f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
        right = forward.Cross(preferred);
    }
    return Unit(Unit(right, {1, 0, 0}).Cross(forward), {0, 1, 0});
}

struct Bounds {
    Vec3 minimum;
    Vec3 maximum;

    void Include(Vec3 point) {
        minimum = {std::min(minimum.x, point.x), std::min(minimum.y, point.y),
                   std::min(minimum.z, point.z)};
        maximum = {std::max(maximum.x, point.x), std::max(maximum.y, point.y),
                   std::max(maximum.z, point.z)};
    }
};

constexpr Bounds UnitBounds{{-.5, -.5, -.5}, {.5, .5, .5}};

using Models = std::unordered_map<std::string, Ref<ModelAsset>>;

Bounds MeshBounds(const Scene& scene, const MeshRendererComponent& mesh, Models& models) {
    if (mesh.meshPath.empty())
        return UnitBounds;
    const auto path = scene.ResolveAssetPath(mesh.meshPath);
    if (mesh.modelSubmesh >= 0) {
        const auto [entry, inserted] = models.try_emplace(path);
        auto& model = entry->second;
        if (inserted) {
            std::string error;
            model = ModelAsset::LoadFbx(path, error);
        }
        const auto index = static_cast<size_t>(mesh.modelSubmesh);
        if (model && index < model->submeshes.size()) {
            const auto& part = model->submeshes[index];
            return {part.boundsMin, part.boundsMax};
        }
        return UnitBounds;
    }
    Bounds bounds;
    return AssetManager::GetMeshBounds(path, bounds.minimum, bounds.maximum) ? bounds : UnitBounds;
}

std::optional<Bounds> TransformBounds(const Mat4& matrix, const Bounds& local) {
    const auto first = matrix.TransformPoint(local.minimum);
    Bounds world{first, first};
    for (const auto x : {local.minimum.x, local.maximum.x})
        for (const auto y : {local.minimum.y, local.maximum.y})
            for (const auto z : {local.minimum.z, local.maximum.z}) {
                const auto point = matrix.TransformPoint({x, y, z});
                if (!Finite(point))
                    return std::nullopt;
                world.Include(point);
            }
    return world;
}

std::optional<ScenePlayerCameraView>
PerspectiveCameraView(const Scene& scene, uint32 id, const CameraComponent& camera, double aspect) {
    Mat4 projection;
    try {
        projection =
            Mat4::Perspective(ToRadians(camera.fov), aspect, camera.nearClip, camera.farClip);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    const auto matrix = scene.GetWorldMatrix(id);
    const auto* m = matrix.elements;
    const Vec3 position{m[12], m[13], m[14]};
    if (!Finite(position))
        return std::nullopt;
    const Vec3 forward = Unit({-m[8], -m[9], -m[10]}, {0, 0, -1});
    const Vec3 up = CameraUp(forward, {m[4], m[5], m[6]});
    // At very large coordinates, adding a unit direction can round back to the eye itself.
    const double distance = std::max({1., std::abs(position.x) * 1e-5, std::abs(position.y) * 1e-5,
                                      std::abs(position.z) * 1e-5});
    const Vec3 target = position + forward * distance;
    const auto delta = target - position;
    const double separation = delta.Dot(delta);
    if (!Finite(target) || !std::isfinite(separation) || separation <= 0)
        return std::nullopt;
    const auto viewProjection = projection * Mat4::LookAt(position, target, up);
    if (!std::all_of(std::begin(viewProjection.elements), std::end(viewProjection.elements),
                     [](double value) { return std::isfinite(value); }))
        return std::nullopt;
    return ScenePlayerCameraView{position, target, up, camera.fov, camera.nearClip, camera.farClip};
}

ScenePlayerCameraView FallbackPerspectiveView(const Scene& scene, const ScenePlayerView& fallback) {
    double minimumZ = std::numeric_limits<double>::infinity();
    double maximumZ = -minimumZ;
    Models models;
    for (const auto id : scene.GetComponentEntityIds<MeshRendererComponent>()) {
        if (!scene.HasComponent<TransformComponent>(id))
            continue;
        const auto matrix = scene.GetWorldMatrix(id);
        const auto local = MeshBounds(scene, scene.GetComponent<MeshRendererComponent>(id), models);
        if (const auto bounds = TransformBounds(matrix, local)) {
            minimumZ = std::min(minimumZ, bounds->minimum.z);
            maximumZ = std::max(maximumZ, bounds->maximum.z);
        }
    }
    const auto center = minimumZ <= maximumZ ? std::midpoint(minimumZ, maximumZ) : 0.;
    const auto extent = minimumZ <= maximumZ ? maximumZ - center : 0.;
    const auto retreat = std::max(5., fallback.halfHeight * 2);
    const auto distance = retreat + extent;
    const auto elevation = 3. * (retreat / distance);
    ScenePlayerCameraView result;
    result.position = {fallback.centerX, fallback.centerY + elevation, center + distance};
    result.target = {fallback.centerX, fallback.centerY, center};
    result.farClip = std::max(result.farClip, std::hypot(distance, elevation) + extent +
                                                  std::abs(fallback.halfHeight) * 2);
    if (!Finite(result.position) || !Finite(result.target) || !std::isfinite(result.farClip))
        return {};
    return result;
}

ScenePlayerCameraView ResolvePerspectiveView(const Scene& scene, double aspect,
                                             const ScenePlayerView& fallback) {
    for (const auto id : scene.GetComponentEntityIds<CameraComponent>()) {
        const auto& camera = scene.GetComponent<CameraComponent>(id);
        if (camera.isPrimary && camera.isPerspective && scene.HasComponent<TransformComponent>(id))
            if (const auto view = PerspectiveCameraView(scene, id, camera, aspect))
                return *view;
    }
    return FallbackPerspectiveView(scene, fallback);
}
} // namespace

ScenePlayerCameraView ResolveScenePlayerCamera(Scene& scene, const ScenePlayerView& fallback) {
    return ResolvePerspectiveView(scene, 1, fallback);
}

ScenePlayerCameras ResolveScenePlayerCameras(Scene& scene, double aspect,
                                             const ScenePlayerView& fallback) {
    ScenePlayerCameras result;
    auto view = fallback;
    double rotation = 0;
    std::optional<ScenePlayerCameraView> perspective;
    for (const auto id : scene.GetComponentEntityIds<CameraComponent>()) {
        const auto& camera = scene.GetComponent<CameraComponent>(id);
        if (!camera.isPrimary || !scene.HasComponent<TransformComponent>(id))
            continue;
        if (camera.isPerspective) {
            perspective = PerspectiveCameraView(scene, id, camera, aspect);
            if (!perspective)
                continue;
            result.perspective = true;
        } else {
            if (!std::isfinite(camera.orthoSize) || camera.orthoSize <= 0 ||
                !std::isfinite(camera.orthoSize * aspect))
                continue;
            const auto matrix = scene.GetWorldMatrix(id);
            double angle = 0;
            try {
                angle = scene.GetWorldTransform(id).rotation.z;
            } catch (const std::logic_error&) {
                // Collapsed parents still have an authored 2D orientation.
                for (auto node = id; node; node = scene.GetParent(node))
                    if (const auto* transform = scene.TryGetComponent<TransformComponent>(node))
                        angle += transform->rotation.z;
            }
            if (!std::isfinite(matrix.elements[12]) || !std::isfinite(matrix.elements[13]) ||
                !std::isfinite(angle))
                continue;
            view = {matrix.elements[12], matrix.elements[13], camera.orthoSize};
            rotation = angle;
        }
        result.cameraEntity = id;
        break;
    }
    if (!result.cameraEntity)
        result.perspective = !scene.GetComponentEntityIds<MeshRendererComponent>().empty();
    const double halfWidth = view.halfHeight * aspect;
    result.camera2D.SetProjection(-halfWidth, halfWidth, -view.halfHeight, view.halfHeight);
    result.camera2D.SetPosition({view.centerX, view.centerY, 0});
    result.camera2D.SetRotation(rotation);
    const auto primary = perspective           ? *perspective
                         : result.cameraEntity ? ResolvePerspectiveView(scene, aspect, view)
                                               : FallbackPerspectiveView(scene, view);
    result.camera3D.SetPerspective(ToRadians(primary.fovDegrees), aspect, primary.nearClip,
                                   primary.farClip);
    result.camera3D.SetPosition(primary.position);
    result.camera3D.SetTarget(primary.target);
    result.camera3D.SetUp(primary.up);
    return result;
}

std::optional<ScenePlayerOptions> ParseScenePlayerArguments(const std::vector<std::string>& args,
                                                            std::string& error) {
    error.clear();
    ScenePlayerOptions options;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        auto need = [&](size_t count) {
            if (i + count >= args.size()) {
                error = arg + " needs " + std::to_string(count) + " value(s)";
                return false;
            }
            return true;
        };
        if (arg == "--scripts") {
            if (++i >= args.size() || args[i].empty() || args[i].front() == '-') {
                error = "C# 스크립트 어셈블리 경로를 지정해 주세요.";
                return std::nullopt;
            }
            options.scriptAssemblyPath = args[i];
        } else if (arg == "--asset-root") {
            if (++i >= args.size() || args[i].empty() || args[i].front() == '-') {
                error = "--asset-root expects a directory";
                return std::nullopt;
            }
            options.assetRoot = args[i];
        } else if (arg == "--title") {
            if (!need(1))
                return std::nullopt;
            options.title = args[++i];
        } else if (arg == "--size") {
            if (!need(2) || !ParseNumber(args[i + 1], options.width) ||
                !ParseNumber(args[i + 2], options.height) || options.width < 64 ||
                options.height < 64 || options.width > 16384 || options.height > 16384) {
                if (error.empty())
                    error = "--size expects two integers between 64 and 16384";
                return std::nullopt;
            }
            i += 2;
        } else if (arg == "--fullscreen") {
            options.fullscreen = true;
        } else if (arg == "--view") {
            ScenePlayerView view;
            if (!need(3) || !ParseNumber(args[i + 1], view.centerX) ||
                !ParseNumber(args[i + 2], view.centerY) ||
                !ParseNumber(args[i + 3], view.halfHeight) || view.halfHeight <= 0.0f) {
                if (error.empty())
                    error = "--view expects X Y HALF_HEIGHT with HALF_HEIGHT > 0";
                return std::nullopt;
            }
            options.view = view;
            i += 3;
        } else if (arg == "--frames") {
            if (!need(1) || !ParseNumber(args[i + 1], options.maxFrames)) {
                if (error.empty())
                    error = "--frames expects a non-negative integer";
                return std::nullopt;
            }
            ++i;
        } else if (!arg.empty() && arg[0] == '-') {
            error = "unknown option: " + arg;
            return std::nullopt;
        } else if (options.scenePath.empty()) {
            options.scenePath = arg;
        } else {
            error = "only one scene file can be given";
            return std::nullopt;
        }
    }
    if (options.scenePath.empty()) {
        error = "no scene file given";
        return std::nullopt;
    }
    return options;
}

ScenePlayerView FitSceneView(Scene& scene) {
    return FitSceneView(scene, 16.0 / 9.0);
}

ScenePlayerView FitSceneView(Scene& scene, double aspect) {
    if (!std::isfinite(aspect) || aspect <= 0)
        aspect = 16.0 / 9.0;
    std::optional<Bounds> bounds;
    Models models;
    for (Entity entity : scene.GetEntities()) {
        if (!entity.HasComponent<TransformComponent>())
            continue;
        const auto matrix = scene.GetWorldMatrix(entity.GetId());
        const auto* mesh = entity.TryGetComponent<MeshRendererComponent>();
        const auto* map = entity.TryGetComponent<TilemapComponent>();
        const auto include = [&](const Bounds& local) {
            if (const auto world = TransformBounds(matrix, local)) {
                if (bounds) {
                    bounds->Include(world->minimum);
                    bounds->Include(world->maximum);
                } else
                    bounds = world;
            }
        };
        if (mesh)
            include(MeshBounds(scene, *mesh, models));
        if (map)
            include({{0, 0, -.5}, {map->width * map->tileSize, map->height * map->tileSize, .5}});
        if ((!mesh && !map) || entity.HasComponent<SpriteRendererComponent>())
            include(UnitBounds);
    }
    if (!bounds)
        return {};
    const auto& [minimum, maximum] = *bounds;
    const auto halfWidth = maximum.x * .5 - minimum.x * .5;
    const auto halfHeight = maximum.y * .5 - minimum.y * .5;
    const auto fitted = std::max({halfHeight, halfWidth / aspect, 1.}) * 1.1;
    if (!std::isfinite(fitted) || !std::isfinite(fitted * aspect))
        return {};
    return {std::midpoint(minimum.x, maximum.x), std::midpoint(minimum.y, maximum.y), fitted};
}

Vec3 ResolveSceneStreamingObserver(Scene& scene, const ScenePlayerView& view, uint32 playerId) {
    const auto position = [&](uint32 id) -> std::optional<Vec3> {
        if (!scene.HasComponent<TransformComponent>(id))
            return std::nullopt;
        const auto matrix = scene.GetWorldMatrix(id);
        const Vec3 world{matrix.elements[12], matrix.elements[13], matrix.elements[14]};
        if (!Finite(world))
            return std::nullopt;
        return world;
    };
    if (const auto observer = position(playerId))
        return *observer;
    for (const auto id : scene.GetComponentEntityIds<TagComponent>())
        if (id != playerId && IsPlayerTag(scene.GetComponent<TagComponent>(id).tag))
            if (const auto observer = position(id))
                return *observer;
    const auto cameras = ResolveScenePlayerCameras(scene, 1, view);
    if (!cameras.cameraEntity)
        return {view.centerX, view.centerY, 0};
    return cameras.perspective ? cameras.camera3D.GetPosition() : cameras.camera2D.GetPosition();
}

ScenePlayer::ScenePlayer(ScenePlayerOptions options)
    : Application({.name = options.title, .width = options.width, .height = options.height}),
      options_(std::move(options)) {}

bool ScenePlayer::LoadScene() {
    const auto path = [](const std::string& value) {
        return std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    };
    auto loaded = options_.assetRoot.empty()
                      ? sceneManager_.Load(path(options_.scenePath))
                      : sceneManager_.LoadWithSpriteAssetRoot(path(options_.scenePath),
                                                              path(options_.assetRoot));
    if (!loaded) {
        Log::Error("씬을 불러올 수 없습니다: " + loaded.error);
        return false;
    }
    auto preparedScripts = std::make_unique<ScriptRuntime>();
    if (!EnsureScripts(*loaded.scene, *preparedScripts))
        return false;
    if (preparedScripts->IsRunning() && !preparedScripts->PrepareInstances()) {
        Log::Error(preparedScripts->GetLastError());
        return false;
    }
    scripts_.swap(preparedScripts);
    preparedScripts->Shutdown();
    scene_ = std::move(loaded.scene);
    fittedAspect_ = 0;
    streamingAccumulator_ = 0;
    lastStreamingError_.clear();
    player_ = FindScenePlayer(*scene_);
    return true;
}

bool ScenePlayer::EnsureScripts(Scene& scene, ScriptRuntime& scripts) {
    if (scripts.IsRunning())
        return true;
    const auto ids = scene.GetComponentEntityIds<ScriptComponent>();
    const bool hasActiveScripts = std::any_of(ids.begin(), ids.end(), [&scene](uint32 id) {
        const auto& script = scene.GetComponent<ScriptComponent>(id);
        return script.enabled && !script.typeName.empty();
    });
    if (hasActiveScripts) {
        ScriptRuntimeConfig config;
        config.isolateUpdateFailures = true;
        const auto base = SDL_GetBasePath();
        const auto directory =
            base ? std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(base)))
                 : std::filesystem::path{};
        config.assemblyPath =
            options_.scriptAssemblyPath.empty()
                ? directory / "Content/Scripts/GameScripts.dll"
                : std::filesystem::path(std::u8string(
                      reinterpret_cast<const char8_t*>(options_.scriptAssemblyPath.data()),
                      options_.scriptAssemblyPath.size()));
        if (!scripts.Initialize(scene, config)) {
            Log::Error(scripts.GetLastError());
            return false;
        }
    }
    return true;
}

Entity FindScenePlayer(Scene& scene) {
    for (const auto id : scene.GetComponentEntityIds<TagComponent>())
        if (IsPlayerTag(scene.GetComponent<TagComponent>(id).tag))
            return {id, &scene};
    return {};
}

void ScenePlayerControls::Bind() {
    static_assert(std::size(PlayerBindings) == std::tuple_size_v<decltype(bindings_)>);
    for (size_t i = 0; i < bindings_.size(); ++i) {
        const auto& [name, key] = PlayerBindings[i];
        if (!bindings_[i])
            bindings_[i] = Input::BindAction(name, key);
    }
}

void ScenePlayerControls::Unbind() {
    for (size_t i = 0; i < bindings_.size(); ++i) {
        const auto& [name, key] = PlayerBindings[i];
        if (bindings_[i])
            Input::UnbindAction(name, key);
    }
    bindings_.fill(false);
}

void ScenePlayerControls::Apply(Entity player, double step) const {
    const auto* script = player.TryGetComponent<ScriptComponent>();
    if (!player.HasComponent<TransformComponent>() ||
        (script && script->enabled && !script->typeName.empty()))
        return;

    const double horizontal = double(Input::IsActionDown("Right")) - Input::IsActionDown("Left");
    const double vertical = double(Input::IsActionDown("Up")) - Input::IsActionDown("Down");
    if (auto* body = player.TryGetComponent<Rigidbody2DComponent>()) {
        body->velocity.x = horizontal * 4;
        if (body->isGrounded && Input::WasActionPressed("Jump"))
            body->velocity.y = 7;
    } else if (const double length = std::hypot(horizontal, vertical); length > 0) {
        auto& scene = *player.GetScene();
        TransformComponent transform;
        try {
            transform = scene.GetWorldTransform(player.GetId());
        } catch (const std::logic_error&) {
            return;
        }
        transform.position.x += horizontal * 4 * step / length;
        transform.position.y += vertical * 4 * step / length;
        scene.SetWorldTransform(player.GetId(), transform);
    }
}

bool UpdateSceneSimulation(Scene& scene, ScriptRuntime& scripts, double step) {
    if (scripts.IsRunning() && !scripts.Update(step))
        return false;
    scene.OnUpdate(step);
    return true;
}

void ScenePlayer::OnInit() {
    presentedFrames_ = 0;
    runtimeFailed_ = false;
    Renderer2D::Init(&GetGpuDevice());
    Renderer3D::Init(&GetGpuDevice());
    PhysicsEngine::Init();
    if (!LoadScene()) {
        Close();
        return;
    }
    loaded_ = true;
    controls_.Bind();
    Input::BindAction("Reload", LuxKey::R);
    Input::BindAction("Quit", LuxKey::Escape);
    if (options_.fullscreen)
        GetWindow().SetFullscreen(true);
    UpdateCamera();
}

void ScenePlayer::UpdateCamera() {
    uint32 width = 0, height = 0;
    double aspect = 16.0f / 9.0f;
    if (GetWindow().GetFramebufferSize(width, height) && width > 0 && height > 0)
        aspect = static_cast<double>(width) / static_cast<double>(height);
    if (options_.view) {
        view_ = *options_.view;
    } else if (aspect != fittedAspect_) {
        view_ = FitSceneView(*scene_, aspect);
        fittedAspect_ = aspect;
    }
    const auto cameras = ResolveScenePlayerCameras(*scene_, aspect, view_);
    camera_ = cameras.camera2D;
    camera3D_ = cameras.camera3D;
    perspective_ = cameras.perspective;
}

double ScenePlayer::GetFixedUpdateStep() const {
    return scene_->GetSettings().fixedStep;
}

void ScenePlayer::OnUpdate(double dt) {
    if (Input::WasActionPressed("Quit")) {
        Close();
        return;
    }
    if (Input::WasActionPressed("Reload") && LoadScene()) {
        UpdateCamera();
        if (dt != GetFixedUpdateStep())
            return;
    }
    controls_.Apply(player_, dt);
    streamingAccumulator_ += dt;
    if (streamingAccumulator_ >= .1) {
        streamingAccumulator_ = 0;
        const auto observer = ResolveSceneStreamingObserver(*scene_, view_, player_.GetId());
        std::string error;
        bool changed = false;
        if (!sceneManager_.UpdateStreaming(*scene_, {observer.x, observer.y}, error, &changed)) {
            if (error != lastStreamingError_)
                Log::Error(error);
            lastStreamingError_ = std::move(error);
        } else if (changed)
            lastStreamingError_.clear();
        if (changed) {
            player_ = FindScenePlayer(*scene_);
            if (!EnsureScripts(*scene_, *scripts_)) {
                runtimeFailed_ = true;
                Close();
                return;
            }
        }
    }
    if (!UpdateSceneSimulation(*scene_, *scripts_, dt)) {
        Log::Error(scripts_->GetLastError());
        runtimeFailed_ = true;
        Close();
        return;
    }
    if (scripts_->IsRunning() && !scripts_->GetLastError().empty())
        Log::Error(scripts_->GetLastError());
}

void ScenePlayer::OnRender(float) {
    UpdateCamera();
    const auto clear = scene_->GetSettings().clearColor;
    GetGpuDevice().Clear(clear.x, clear.y, clear.z, clear.w);
    if (perspective_) {
        auto renderSettings = scene_->GetSettings().render3D;
        for (auto* path :
             {&renderSettings.environmentPath, &renderSettings.waterReflection.environmentPath})
            if (!path->empty())
                *path = scene_->ResolveAssetPath(*path);
        Renderer3D::BeginScene(camera3D_, renderSettings, scene_->GetSettings().exposure);
        scene_->OnRender3D();
        Renderer3D::EndScene();
    }
    Renderer2D::BeginScene(camera_);
    scene_->OnRender();
    Renderer2D::EndScene();
}

void ScenePlayer::OnFramePresented() {
    ++presentedFrames_;
    if (options_.maxFrames != 0 && presentedFrames_ >= options_.maxFrames)
        Close();
}

uint64 ScenePlayer::GetPresentedFrameCount() const noexcept {
    return presentedFrames_;
}

void ScenePlayer::OnShutdown() {
    loaded_ = false;
    controls_.Unbind();
    scripts_->Shutdown();
    Input::ClearBindings();
}

bool ScenePlayer::RuntimeFailed() const {
    return runtimeFailed_;
}

bool ScenePlayer::LoadedScene() const {
    return loaded_;
}
} // namespace LuxEngine
