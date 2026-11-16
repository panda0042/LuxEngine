#pragma once
#include <LuxEditor/Animation/AnimationAssetDocument.hpp>
#include <LuxUI/Widgets.hpp>
#include <memory>

namespace LuxEditor {
class EditorDocument;

class AnimationAssetEditor final : public LuxUI::Widget {
public:
    AnimationAssetEditor(
        LuxUI::Widget* parent, EditorDocument& scene,
        std::function<void(const LuxUI::String&, const std::function<void()>&)> recordedEdit);
    ~AnimationAssetEditor() override;
    AnimationAssetDocument& Document();
    bool OpenAsset(const std::string& path);
    bool SaveAsset(const std::string& path);
    void Seek(double time);
    void Play(bool playing);
    double Time() const;
    bool PreviewSourceUnchanged() const;
    bool ConfirmDiscard();

protected:
    bool event(LuxUI::Event* event) override;
    void keyPressEvent(LuxUI::KeyEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace LuxEditor
