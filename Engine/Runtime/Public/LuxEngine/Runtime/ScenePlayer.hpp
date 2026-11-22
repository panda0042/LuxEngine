#pragma once

#include <LuxEngine/Application.hpp>
#include <LuxEngine/Renderer/Renderer2D/Renderer2D.hpp>
#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <LuxEngine/Scene/SceneManager.hpp>
#include <LuxEngine/Scripting/ScriptRuntime.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace LuxEngine {

// Fallback view rectangle when the scene has no valid primary camera.
struct ScenePlayerView {
    double centerX = 0.0f;
    double centerY = 0.0f;
    double halfHeight = 4.5f;
};

struct ScenePlayerOptions {
    std::string scenePath;
    std::string scriptAssemblyPath;
    std::string title = "LuxEngine Player";
    uint32 width = 1280;
    uint32 height = 720;
    bool fullscreen = false;
    std::optional<ScenePlayerView> view;
    // Stops after this many frames when non-zero (smoke tests).
    uint32 maxFrames = 0;
};

struct ScenePlayerCameraView {
    Vec3 position{0, 3, 9};
    Vec3 target{};
    Vec3 up{0, 1, 0};
    double fovDegrees = 60;
    double nearClip = .1f;
    double farClip = 1000.f;
};

// Primary perspective cameras follow their complete world transform: local -Z is forward and
// +Y is up. Scale is normalized away; collapsed/parallel axes use a finite orthonormal fallback.
ScenePlayerCameraView ResolveScenePlayerCamera(Scene& scene, const ScenePlayerView& fallback);

struct ScenePlayerCameras {
    bool perspective = false;
    OrthographicCamera camera2D;
    PerspectiveCamera camera3D;
    uint32 cameraEntity = 0;
};

// Shared by the editor Game View and standalone player. Authored primary cameras take
// precedence over the fallback view, including the editor's --view argument.
ScenePlayerCameras ResolveScenePlayerCameras(Scene& scene, double aspect,
                                             const ScenePlayerView& fallback);

// Parses: <scene> [--title T] [--size W H] [--fullscreen] [--view X Y HALF_HEIGHT]
//         [--frames N] [--scripts DLL]
// Returns std::nullopt and sets error on invalid arguments.
std::optional<ScenePlayerOptions> ParseScenePlayerArguments(const std::vector<std::string>& args,
                                                            std::string& error);

// Runs a saved scene in its own window: physics, animation, Renderer3D and Renderer2D drawing.
// An entity tagged "플레이어", "주인공" or "Player" is driven by WASD/arrows (Space jumps for
// bodies with Rigidbody2D). Escape closes the window; R reloads the scene file.
class ScenePlayer final : public Application {
public:
    explicit ScenePlayer(ScenePlayerOptions options);

    bool LoadedScene() const;
    bool RuntimeFailed() const;
    uint64 GetPresentedFrameCount() const noexcept;

protected:
    void OnInit() override;
    void OnUpdate(double dt) override;
    void OnRender(float elapsedSeconds) override;
    void OnFramePresented() override;
    void OnShutdown() override;

private:
    bool LoadScene();
    bool EnsureScripts(Scene& scene, ScriptRuntime& scripts);
    void FindPlayer();
    void UpdateCamera();

    ScenePlayerOptions options_;
    SceneManager sceneManager_;
    std::unique_ptr<ScriptRuntime> scripts_ = std::make_unique<ScriptRuntime>();
    std::shared_ptr<Scene> scene_ = std::make_shared<Scene>();
    Entity player_;
    OrthographicCamera camera_;
    PerspectiveCamera camera3D_;
    bool perspective_ = false;
    ScenePlayerView view_;
    double fittedAspect_ = 0;
    bool loaded_ = false;
    bool runtimeFailed_ = false;
    uint64 presentedFrames_ = 0;
    double streamingAccumulator_ = 0;
    std::string lastStreamingError_;
};

// Computes a view that frames every entity with a transform; used when no view is given.
ScenePlayerView FitSceneView(Scene& scene);
ScenePlayerView FitSceneView(Scene& scene, double aspect);
Vec3 ResolveSceneStreamingObserver(Scene& scene, const ScenePlayerView& view, uint32 playerId = 0);

} // namespace LuxEngine
