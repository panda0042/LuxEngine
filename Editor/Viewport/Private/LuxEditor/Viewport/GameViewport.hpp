#pragma once

#include <LuxUI/Widgets.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace LuxEngine {
class Scene;
}

namespace LuxEditor {
class EngineSceneRenderer;

// Primary-camera preview in edit and play modes, without editor navigation or transform handles.
class GameViewport final : public LuxUI::Widget {
public:
    explicit GameViewport(LuxUI::Widget* parent = nullptr);
    ~GameViewport() override;

    void SetScene(LuxEngine::Scene* scene);
    void SetSceneProvider(std::function<LuxEngine::Scene*()> provider);
    LuxEngine::Scene* GetScene() const noexcept;
    void SetInputEnabled(bool enabled);
    bool IsInputEnabled() const noexcept;
    void SetPaused(bool paused);
    bool IsPaused() const noexcept;
    void RefreshScene();
    std::shared_ptr<EngineSceneRenderer> GetRenderer() const;

private:
    bool event(LuxUI::Event*) override;
    void paintEvent(LuxUI::PaintEvent*) override;
    void mousePressEvent(LuxUI::MouseEvent*) override;
    void mouseReleaseEvent(LuxUI::MouseEvent*) override;
    void mouseMoveEvent(LuxUI::MouseEvent*) override;
    void PaintSoftware(LuxUI::Painter& painter, LuxEngine::Scene& scene);
    LuxUI::Image ReadSoftwareTexture(const std::string& path);

    struct SoftwareTexture {
        std::uint64_t revision;
        LuxUI::Image image;
    };

    LuxEngine::Scene* scene_ = nullptr;
    std::weak_ptr<int> sceneLifetime_;
    std::function<LuxEngine::Scene*()> sceneProvider_;
    std::shared_ptr<EngineSceneRenderer> renderer_;
    LuxUI::Image frame_;
    std::unordered_map<std::string, SoftwareTexture> softwareTextures_;
    std::size_t softwareTextureBytes_ = 0;
    bool paused_ = false;
    bool inputEnabled_ = false;
};
} // namespace LuxEditor
