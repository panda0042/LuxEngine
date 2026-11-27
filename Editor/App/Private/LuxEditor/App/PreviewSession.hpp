#pragma once
#include <LuxEngine/Core/Timer.hpp>
#include <LuxEngine/Runtime/ScenePlayer.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace LuxEditor {
class PreviewSession final {
public:
    struct Update {
        std::string streamingError;
        std::optional<std::string> failure;
    };

    using UpdateObserver = std::function<void(const Update&)>;

    explicit PreviewSession(LuxEngine::SceneManager& sceneManager);
    ~PreviewSession();

    std::optional<std::string> Start(LuxEngine::Scene& authored, std::filesystem::path assembly);
    void Stop();
    void Pause();
    void Resume();
    void Advance(double elapsed, const LuxEngine::ScenePlayerView& view,
                 const UpdateObserver& onUpdate);
    void Step(const LuxEngine::ScenePlayerView& view, const UpdateObserver& onUpdate);

    bool IsActive() const noexcept;
    bool IsPaused() const noexcept;
    bool IsRunning() const noexcept;
    LuxEngine::Scene* GetScene() noexcept;
    const LuxEngine::Scene* GetScene() const noexcept;
    std::uint64_t Frame() const noexcept;
    std::uint64_t LogStartSequence() const noexcept;
    double LastUpdateMs() const noexcept;

private:
    enum class State { Stopped, Running, Paused };

    bool EnsureScripts(LuxEngine::Scene& scene, const std::filesystem::path& assembly);
    Update Tick(double step, const LuxEngine::ScenePlayerView& view);

    LuxEngine::SceneManager& sceneManager_;
    std::unique_ptr<LuxEngine::Scene> scene_;
    LuxEngine::ScriptRuntime scripts_;
    std::filesystem::path assembly_;
    LuxEngine::Entity player_;
    LuxEngine::ScenePlayerControls controls_;
    LuxEngine::FixedStepAccumulator updates_;
    State state_ = State::Stopped;
    std::uint64_t frame_ = 0;
    std::uint64_t logStartSequence_ = 0;
    double streamingElapsed_ = 0;
    double lastUpdateMs_ = 0;
};
} // namespace LuxEditor
