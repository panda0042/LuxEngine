#include <LuxEditor/Animation/AnimationAssetDocument.hpp>

namespace LuxEditor {
AnimationAssetDocument::AnimationAssetDocument() {
    New(false);
    savedRevision_ = revision_;
}

const AnimationAssetState& AnimationAssetDocument::State() const {
    return state_;
}

AnimationAssetState& AnimationAssetDocument::BeginEdit() {
    if (!before_)
        before_ = std::make_unique<AnimationAssetState>(state_);
    return state_;
}

bool AnimationAssetDocument::CommitEdit() {
    if (!before_)
        return false;
    undo_.push_back(std::move(*before_));
    undoRevisions_.push_back(revision_);
    before_.reset();
    redo_.clear();
    redoRevisions_.clear();
    revision_ = ++nextRevision_;
    // The authoring history is bounded; large imported skeletons remain external FBX assets.
    if (undo_.size() > 64) {
        undo_.erase(undo_.begin());
        undoRevisions_.erase(undoRevisions_.begin());
    }
    return true;
}

void AnimationAssetDocument::CancelEdit() {
    if (before_) {
        state_ = std::move(*before_);
        before_.reset();
    }
}

void AnimationAssetDocument::Edit(const std::function<void(AnimationAssetState&)>& edit) {
    BeginEdit();
    try {
        edit(state_);
        CommitEdit();
    } catch (...) {
        CancelEdit();
        throw;
    }
}

void AnimationAssetDocument::New(bool controller) {
    state_ = {};
    state_.isController = controller;
    state_.clip.name = "Animation";
    if (controller)
        state_.controller.layers.emplace_back();
    path_.clear();
    undo_.clear();
    redo_.clear();
    undoRevisions_.clear();
    redoRevisions_.clear();
    before_.reset();
    revision_ = nextRevision_ = 1;
    savedRevision_ = 0;
}

bool AnimationAssetDocument::Open(const std::string& path, std::string& error) {
    auto clip = LuxEngine::AnimationClip::Load(path, error);
    if (clip) {
        New(false);
        state_.clip = *clip;
    } else {
        auto controller = LuxEngine::AnimatorController::Load(path, error);
        if (!controller)
            return false;
        New(true);
        state_.controller = *controller;
    }
    path_ = path;
    revision_ = savedRevision_ = 0;
    return true;
}

bool AnimationAssetDocument::Save(const std::string& path, std::string& error) {
    const bool saved =
        state_.isController ? state_.controller.Save(path, error) : state_.clip.Save(path, error);
    if (saved) {
        path_ = path;
        state_.controller.sourcePath = path;
        savedRevision_ = revision_;
    }
    return saved;
}

bool AnimationAssetDocument::Undo() {
    CancelEdit();
    if (undo_.empty())
        return false;
    redo_.push_back(state_);
    redoRevisions_.push_back(revision_);
    state_ = std::move(undo_.back());
    undo_.pop_back();
    revision_ = undoRevisions_.back();
    undoRevisions_.pop_back();
    return true;
}

bool AnimationAssetDocument::Redo() {
    CancelEdit();
    if (redo_.empty())
        return false;
    undo_.push_back(state_);
    undoRevisions_.push_back(revision_);
    state_ = std::move(redo_.back());
    redo_.pop_back();
    revision_ = redoRevisions_.back();
    redoRevisions_.pop_back();
    return true;
}

bool AnimationAssetDocument::Dirty() const {
    return revision_ != savedRevision_;
}

const std::string& AnimationAssetDocument::Path() const {
    return path_;
}
} // namespace LuxEditor
