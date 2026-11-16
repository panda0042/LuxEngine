#include <LuxEditor/Animation/AnimationAssetDocument.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <fstream>
#include <optional>

namespace LuxEditor {
namespace {
std::optional<std::string> ReadAsset(const std::string& path) {
    std::ifstream input(PathFromUtf8(path), std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || input.tellg() > 128 * 1024 * 1024)
        return {};
    std::string data(static_cast<size_t>(input.tellg()), '\0');
    input.seekg(0);
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size())))
        return {};
    return data;
}

bool SameAsset(const std::string& a, const std::string& b) {
    std::error_code error;
    return !a.empty() && std::filesystem::equivalent(PathFromUtf8(a), PathFromUtf8(b), error) &&
           !error;
}
} // namespace

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
    disk_.clear();
    hasDisk_ = false;
    undo_.clear();
    redo_.clear();
    undoRevisions_.clear();
    redoRevisions_.clear();
    before_.reset();
    revision_ = nextRevision_ = 1;
    savedRevision_ = 0;
}

bool AnimationAssetDocument::Open(const std::string& path, std::string& error) {
    const auto bytes = ReadAsset(path);
    if (!bytes) {
        error = "애니메이션 파일을 읽지 못했습니다.";
        return false;
    }
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
    disk_ = *bytes;
    hasDisk_ = true;
    revision_ = savedRevision_ = 0;
    return true;
}

bool AnimationAssetDocument::Save(const std::string& path, std::string& error) {
    error.clear();
    std::error_code pathError, originalError;
    const auto target = std::filesystem::weakly_canonical(PathFromUtf8(path), pathError);
    const auto original = std::filesystem::weakly_canonical(PathFromUtf8(path_), originalError);
    const bool samePath = !path_.empty() && ((!pathError && !originalError && target == original) ||
                                             SameAsset(path, path_));
    if (hasDisk_ && samePath && ReadAsset(path) != std::optional<std::string>(disk_)) {
        error = "파일이 외부에서 변경되거나 삭제됐습니다. 다시 읽거나 다른 이름으로 저장해 주세요.";
        return false;
    }
    const bool saved =
        state_.isController ? state_.controller.Save(path, error) : state_.clip.Save(path, error);
    if (saved) {
        auto bytes = ReadAsset(path);
        if (!bytes) {
            error = "저장한 애니메이션 파일을 확인하지 못했습니다.";
            return false;
        }
        disk_ = std::move(*bytes);
        hasDisk_ = true;
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

bool AnimationAssetDocument::CanUndo() const {
    return !undo_.empty();
}

bool AnimationAssetDocument::CanRedo() const {
    return !redo_.empty();
}

bool AnimationAssetDocument::Dirty() const {
    return revision_ != savedRevision_;
}

const std::string& AnimationAssetDocument::Path() const {
    return path_;
}
} // namespace LuxEditor
