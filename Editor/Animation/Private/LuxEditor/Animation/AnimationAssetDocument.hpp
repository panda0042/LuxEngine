#pragma once
#include <LuxEngine/Animation/Animator.hpp>
#include <functional>
#include <vector>

namespace LuxEditor {
struct AnimationAssetState {
    LuxEngine::AnimationClip clip;
    LuxEngine::AnimatorController controller;
    bool isController = false;
};

class AnimationAssetDocument {
public:
    AnimationAssetDocument();
    const AnimationAssetState& State() const;
    AnimationAssetState& BeginEdit();
    bool CommitEdit();
    void CancelEdit();
    void Edit(const std::function<void(AnimationAssetState&)>& edit);
    void New(bool controller);
    bool Open(const std::string& path, std::string& error);
    bool Save(const std::string& path, std::string& error);
    bool Undo();
    bool Redo();
    bool Dirty() const;
    const std::string& Path() const;

private:
    AnimationAssetState state_;
    std::vector<AnimationAssetState> undo_, redo_;
    std::unique_ptr<AnimationAssetState> before_;
    std::string path_;
    size_t revision_ = 0, savedRevision_ = 0, nextRevision_ = 1;
    std::vector<size_t> undoRevisions_, redoRevisions_;
};
} // namespace LuxEditor
