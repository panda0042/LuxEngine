#include <LuxEngine/Application.hpp>
#include <LuxEngine/Assets/AssetManager.hpp>
#include <LuxEngine/Audio/AudioEngine.hpp>
#include <LuxEngine/Physics/PhysicsEngine.hpp>
#include <LuxEngine/Renderer/Renderer2D/Renderer2D.hpp>
#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace LuxEngine {

namespace {
template <typename Fn> void AttemptShutdown(Fn&& operation, const char* error) noexcept {
    try {
        operation();
    } catch (...) {
        try {
            Log::Error(error);
        } catch (...) {
        }
    }
}
} // namespace

Application* Application::s_Instance = nullptr;

Application::Application(const ApplicationSpecification& spec) : spec_(spec) {
    if (s_Instance) {
        throw std::logic_error("Application instance already exists");
    }
    s_Instance = this;
}

Application& Application::Get() {
    if (!s_Instance)
        throw std::logic_error("Application instance does not exist");
    return *s_Instance;
}

Window& Application::GetWindow() {
    if (!window_ || !window_->GetNativeHandle())
        throw std::logic_error("Window is not initialized");
    return *window_;
}

GpuDevice& Application::GetGpuDevice() {
    if (!gpuDevice_ || !gpuDevice_->IsReady())
        throw std::logic_error("GPU device is not initialized");
    return *gpuDevice_;
}

Application::~Application() {
    Shutdown();
    if (s_Instance == this) {
        s_Instance = nullptr;
    }
}

bool Application::Initialize() {
    if (shuttingDown_)
        return false;
    if (sdlInitialized_)
        return running_;
    Log::Init();

    ownsSdlRoot_ = SDL_WasInit(0) == 0;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        Log::Error(std::string("SDL_Init failed: ") + SDL_GetError());
        if (ownsSdlRoot_)
            SDL_Quit();
        ownsSdlRoot_ = false;
        Log::Shutdown();
        return false;
    }
    sdlInitialized_ = true;
    Input::Initialize();

    window_ = CreateScope<Window>();
    WindowProps props;
    props.title = spec_.name;
    props.width = spec_.width;
    props.height = spec_.height;
    props.resizable = spec_.resizable;
    props.vulkan = true;

    if (!window_->Initialize(props)) {
        Log::Error("Failed to initialize window.");
        Shutdown();
        return false;
    }

    window_->SetEventCallback([this](Event& e) { OnEvent(e); });

    gpuDevice_ = CreateScope<GpuDevice>();
    if (!gpuDevice_->Initialize(*window_)) {
        Log::Error("Failed to initialize GPU Device.");
        Shutdown();
        return false;
    }

    window_->SetPresentCallback(
        [this](bool enabled) { return gpuDevice_ && gpuDevice_->SetVSync(enabled); });
    initHookEntered_ = true;
    running_ = true;
    try {
        OnInit();
    } catch (...) {
        Shutdown();
        throw;
    }
    if (!running_) {
        Shutdown();
        return false;
    }
    lastTicksNs_ = SDL_GetTicksNS();
    return true;
}

void Application::OnEvent(Event& e) {
    EventDispatcher dispatcher(e);
    dispatcher.Dispatch<WindowCloseEvent>(
        [this](WindowCloseEvent& ev) { return OnWindowClose(ev); });
    dispatcher.Dispatch<WindowResizeEvent>(
        [this](WindowResizeEvent& ev) { return OnWindowResize(ev); });
}

bool Application::OnWindowClose(WindowCloseEvent& /*e*/) {
    running_ = false;
    return true;
}

bool Application::OnWindowResize(WindowResizeEvent& e) {
    spec_.width = e.GetWidth();
    spec_.height = e.GetHeight();
    if (gpuDevice_)
        gpuDevice_->NotifyResize();
    return false;
}

int Application::Run() {
    if (!sdlInitialized_)
        return 1;
    while (true) {
        const FrameResult result = RunFrame();
        if (result == FrameResult::Continue)
            continue;
        if (result == FrameResult::Skipped) {
            SDL_Delay(1);
            continue;
        }
        return result == FrameResult::Stopped ? 0 : 1;
    }
}

Application::FrameResult Application::RunFrame() {
    if (!sdlInitialized_)
        return FrameResult::Error;

    try {
        if (!running_ || !window_->IsRunning()) {
            Shutdown();
            return FrameResult::Stopped;
        }

        window_->PollEvents();
        if (!running_ || !window_->IsRunning()) {
            Shutdown();
            return FrameResult::Stopped;
        }

        const uint64 now = SDL_GetTicksNS();
        const double dt =
            std::clamp(static_cast<double>(now - lastTicksNs_) / 1'000'000'000.0, 0.0, 0.25);
        lastTicksNs_ = now;
        elapsedSeconds_ += dt;
        fixedUpdates_.AddElapsed(dt);

        int steps = 0;
        while (running_ && steps < FixedStepAccumulator::MaxUpdatesPerFrame) {
            const double step = GetFixedUpdateStep();
            if (!fixedUpdates_.Consume(step))
                break;
            OnUpdate(step);
            Input::AdvanceFrame();
            ++steps;
        }
        if (!running_) {
            Shutdown();
            return FrameResult::Stopped;
        }
        if (steps == FixedStepAccumulator::MaxUpdatesPerFrame)
            fixedUpdates_.DropBacklog(GetFixedUpdateStep());

        const auto frameStart = gpuDevice_->BeginFrame();
        if (frameStart == GpuDevice::FrameStart::Error) {
            Log::Error(std::string("GPU frame failed (") + ToString(gpuDevice_->GetStatus()) +
                       "): " + gpuDevice_->GetLastError());
            Shutdown();
            return FrameResult::Error;
        }
        if (frameStart == GpuDevice::FrameStart::Skipped) {
            return FrameResult::Skipped;
        }

        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(elapsedSeconds_));
        const bool cleared = gpuDevice_->Clear(0.05f + 0.05f * pulse, 0.08f + 0.08f * pulse,
                                               0.15f + 0.15f * pulse, 1.0f);
        if (!cleared) {
            gpuDevice_->EndFrame();
            Shutdown();
            return FrameResult::Error;
        }

        OnRender(static_cast<float>(elapsedSeconds_));
        if (!sdlInitialized_)
            return FrameResult::Stopped;
        const bool presented = gpuDevice_->EndFrame();
        if (!presented && !gpuDevice_->IsReady()) {
            Log::Error(std::string("GPU frame submission failed (") +
                       ToString(gpuDevice_->GetStatus()) + "): " + gpuDevice_->GetLastError());
            Shutdown();
            return FrameResult::Error;
        }
        if (presented)
            OnFramePresented();
        if (!running_) {
            Shutdown();
            return FrameResult::Stopped;
        }
        return FrameResult::Continue;
    } catch (...) {
        Shutdown();
        throw;
    }
}

void Application::Shutdown() noexcept {
    if (shuttingDown_)
        return;
    if (!running_ && !sdlInitialized_ && !window_ && !gpuDevice_)
        return;
    shuttingDown_ = true;

    running_ = false;
    lastTicksNs_ = 0;
    fixedUpdates_.Reset();
    elapsedSeconds_ = 0.0;
    if (initHookEntered_) {
        initHookEntered_ = false;
        AttemptShutdown([this] { OnShutdown(); }, "OnShutdown failed");
    }
    AttemptShutdown([] { AssetManager::Shutdown(); }, "AssetManager shutdown failed");
    AttemptShutdown([] { AudioEngine::Shutdown(); }, "AudioEngine shutdown failed");
    AttemptShutdown([] { PhysicsEngine::Shutdown(); }, "PhysicsEngine shutdown failed");
    AttemptShutdown([] { Renderer3D::Shutdown(); }, "Renderer3D shutdown failed");
    AttemptShutdown([] { Renderer2D::Shutdown(); }, "Renderer2D shutdown failed");

    if (gpuDevice_) {
        AttemptShutdown([this] { gpuDevice_->Shutdown(); }, "GpuDevice shutdown failed");
        AttemptShutdown([this] { gpuDevice_.reset(); }, "GpuDevice release failed");
    }

    if (window_) {
        AttemptShutdown([this] { window_->Shutdown(); }, "Window shutdown failed");
        AttemptShutdown([this] { window_.reset(); }, "Window release failed");
    }

    if (sdlInitialized_) {
        Input::Shutdown();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        if (ownsSdlRoot_)
            SDL_Quit();
        sdlInitialized_ = false;
        ownsSdlRoot_ = false;
    }

    AttemptShutdown([] { Log::Shutdown(); }, "Log shutdown failed");
    shuttingDown_ = false;
}

void Application::Close() {
    running_ = false;
}

void Application::OnInit() {}

void Application::OnUpdate(double /*dt*/) {}

void Application::OnRender(float /*elapsedSeconds*/) {}

void Application::OnShutdown() {}

double Application::GetFixedUpdateStep() const {
    return 1.0 / 60.0;
}
} // namespace LuxEngine
