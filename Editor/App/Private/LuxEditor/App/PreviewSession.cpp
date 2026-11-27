#include <LuxEditor/App/PreviewSession.hpp>
#include <LuxEngine/Animation/AnimationEngine.hpp>
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Platform/Input.hpp>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace LuxEditor {
PreviewSession::PreviewSession(LuxEngine::SceneManager& sceneManager)
    : sceneManager_(sceneManager) {}

PreviewSession::~PreviewSession() {
    Stop();
}

std::optional<std::string> PreviewSession::Start(LuxEngine::Scene& authored,
                                                 std::filesystem::path assembly) {
    if (IsActive())
        return std::nullopt;
    std::string serialized;
    auto runtime = std::make_unique<LuxEngine::Scene>();
    if (!authored.SerializeToString(serialized) || !runtime->DeserializeFromString(serialized))
        return "게임 실행용 씬 복사본을 만들지 못했습니다.";
    runtime->SetSourcePath(authored.GetSourcePath());
    for (const auto id : authored.GetComponentEntityIds<LuxEngine::AnimationComponent>())
        runtime->GetComponent<LuxEngine::AnimationComponent>(id).currentTime =
            authored.GetComponent<LuxEngine::AnimationComponent>(id).currentTime;
    if (!EnsureScripts(*runtime, assembly))
        return scripts_.GetLastError();

    assembly_ = std::move(assembly);
    scene_ = std::move(runtime);
    player_ = LuxEngine::FindScenePlayer(*scene_);
    controls_.Bind();
    updates_.Reset();
    frame_ = 0;
    streamingElapsed_ = 0;
    lastUpdateMs_ = 0;
    logStartSequence_ = LuxEngine::Log::LatestSequence();
    state_ = State::Running;
    return std::nullopt;
}

void PreviewSession::Stop() {
    if (!IsActive())
        return;
    state_ = State::Stopped;
    scripts_.Shutdown();
    assembly_.clear();
    controls_.Unbind();
    player_ = {};
    scene_.reset();
}

void PreviewSession::Pause() {
    if (IsRunning())
        state_ = State::Paused;
}

void PreviewSession::Resume() {
    if (!IsPaused())
        return;
    updates_.Reset();
    state_ = State::Running;
}

void PreviewSession::Advance(double elapsed, const LuxEngine::ScenePlayerView& view,
                             const UpdateObserver& onUpdate) {
    if (!IsRunning())
        return;
    updates_.AddElapsed(elapsed);
    for (int i = 0; i < LuxEngine::FixedStepAccumulator::MaxUpdatesPerFrame; ++i) {
        if (!IsRunning())
            return;
        const double step = scene_->GetSettings().fixedStep;
        if (!updates_.Consume(step))
            return;
        onUpdate(Tick(step, view));
    }
    if (IsActive())
        updates_.DropBacklog(scene_->GetSettings().fixedStep);
}

void PreviewSession::Step(const LuxEngine::ScenePlayerView& view, const UpdateObserver& onUpdate) {
    if (IsPaused())
        onUpdate(Tick(scene_->GetSettings().fixedStep, view));
}

bool PreviewSession::IsActive() const noexcept {
    return state_ != State::Stopped;
}

bool PreviewSession::IsPaused() const noexcept {
    return state_ == State::Paused;
}

bool PreviewSession::IsRunning() const noexcept {
    return state_ == State::Running;
}

LuxEngine::Scene* PreviewSession::GetScene() noexcept {
    return scene_.get();
}

const LuxEngine::Scene* PreviewSession::GetScene() const noexcept {
    return scene_.get();
}

std::uint64_t PreviewSession::Frame() const noexcept {
    return frame_;
}

std::uint64_t PreviewSession::LogStartSequence() const noexcept {
    return logStartSequence_;
}

double PreviewSession::LastUpdateMs() const noexcept {
    return lastUpdateMs_;
}

bool PreviewSession::EnsureScripts(LuxEngine::Scene& scene, const std::filesystem::path& assembly) {
    if (scripts_.IsRunning())
        return true;
    for (const auto id : scene.GetComponentEntityIds<LuxEngine::ScriptComponent>()) {
        const auto& script = scene.GetComponent<LuxEngine::ScriptComponent>(id);
        if (!script.enabled || script.typeName.empty())
            continue;
        LuxEngine::ScriptRuntimeConfig config;
        config.assemblyPath = assembly;
        return scripts_.Initialize(scene, config);
    }
    return true;
}

PreviewSession::Update PreviewSession::Tick(double step, const LuxEngine::ScenePlayerView& view) {
    auto& scene = *scene_;
    const auto start = std::chrono::steady_clock::now();
    Update result;
    try {
        controls_.Apply(player_, step);
        streamingElapsed_ += step;
        if (streamingElapsed_ >= .1) {
            streamingElapsed_ = 0;
            const auto observer =
                LuxEngine::ResolveSceneStreamingObserver(scene, view, player_.GetId());
            bool changed = false;
            sceneManager_.UpdateStreaming(scene, {observer.x, observer.y}, result.streamingError,
                                          &changed);
            if (changed) {
                player_ = LuxEngine::FindScenePlayer(scene);
                if (!EnsureScripts(scene, assembly_))
                    throw std::runtime_error(scripts_.GetLastError());
            }
        }
        if (!LuxEngine::UpdateSceneSimulation(scene, scripts_, step))
            throw std::runtime_error(scripts_.GetLastError());
        if (!scene.GetLastPhysicsError2D().empty())
            throw std::runtime_error(scene.GetLastPhysicsError2D());
        if (!scene.GetLastPhysicsError3D().empty())
            throw std::runtime_error(scene.GetLastPhysicsError3D());
        LuxEngine::Input::AdvanceFrame();
    } catch (const std::exception& error) {
        Pause();
        result.failure = error.what();
        return result;
    }
    ++frame_;
    lastUpdateMs_ =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}
} // namespace LuxEditor
