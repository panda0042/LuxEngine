#pragma once

#include <LuxEngine/Renderer/RHI/Texture.hpp>
#include <LuxEngine/Renderer/Renderer3D/Render3DSettings.hpp>
#include <cstdint>

struct ImGuiContext;

namespace LuxEngine {
class Event;
class Window;
} // namespace LuxEngine

namespace Frontier {
class RenderDebugPanel {
public:
    ~RenderDebugPanel();
    void Initialize(LuxEngine::GpuDevice& gpu, const LuxEngine::Render3DSettings& defaults);
    void Shutdown();
    void FeedEvent(LuxEngine::Event& event);
    void Apply(LuxEngine::Render3DSettings& settings) const;
    bool Draw(LuxEngine::Window& window, LuxEngine::GpuDevice& gpu, bool open, bool cursorFree,
              double dt);

private:
    ImGuiContext* context_ = nullptr;
    LuxEngine::GpuTexture font_;
    std::uint32_t effects_ = 0, defaults_ = 0;
    std::size_t diagnostic_ = 0;
};
} // namespace Frontier
