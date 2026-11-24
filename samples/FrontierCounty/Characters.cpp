#include "Characters.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace Frontier {
namespace {
using namespace LuxEngine;

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

} // namespace

bool Characters::Character::Evaluate(unsigned state, double time, ModelPose& pose,
                                     std::string& error) const {
    if (clipSource != model)
        return model->EvaluateAnimation(*clipSource, time, clips.at(state), pose, error);
    return model->Evaluate(time, clips.at(state), pose, error);
}

void Characters::Load(const std::filesystem::path& assets) {
    constexpr std::array names{"dog-walker", "hiker", "road-worker"};
    for (size_t c = 0; c < characters_.size(); ++c) {
        auto& asset = characters_[c];
        const auto root = assets / "FrontierPeople" / names[c];
        asset.path = Utf8Path(root / "model.fbx");
        std::string error;
        asset.model = ModelAsset::LoadFbx(asset.path, error);
        ModelPose rest;
        if (!asset.model || !asset.model->Evaluate(0, -1, rest, error))
            throw std::runtime_error("Character model " + asset.path + ": " + error);
        asset.clipSource = asset.model;
        if (c == 0) {
            asset.clipSource = ModelAsset::LoadFbxAnimation(Utf8Path(root / "motion.fbx"), error);
            if (!asset.clipSource)
                throw std::runtime_error("Character motion: " + error);
        }
        constexpr std::array clips{"idle", "walk"};
        const auto& stacks = asset.clipSource->animations;
        for (size_t i = 0; i < clips.size(); ++i) {
            const auto found = std::ranges::find_if(stacks, [&](const ModelAnimation& clip) {
                return clip.name == clips[i] || clip.name.ends_with(std::string("|") + clips[i]);
            });
            if (found == stacks.end() || !std::isfinite(found->duration) || found->duration <= 0)
                throw std::runtime_error("FBX file has no expected motion: " +
                                         asset.clipSource->sourcePath);
            asset.clips[i] = static_cast<int32>(found - stacks.begin());
        }
        if (!std::ranges::all_of(asset.model->submeshes,
                                 [](const ModelSubmesh& part) { return part.skinned; }))
            throw std::runtime_error("Unexpected unskinned geometry in character: " + asset.path);
        const double inf = std::numeric_limits<double>::infinity();
        Vec3 low{inf, inf, inf}, high{-inf, -inf, -inf};
        for (size_t i = 0; i < asset.model->submeshes.size(); ++i) {
            const auto& mat = rest.nodeWorldTransforms.at(asset.model->submeshes[i].node);
            for (const auto& vertex : rest.submeshes[i]) {
                const auto p = mat.TransformPoint(vertex.position);
                low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
            }
        }
        if (!(high.y - low.y > .01))
            throw std::runtime_error("Character bind pose has no usable height");
        asset.foot = {(low.x + high.x) * .5, low.y, (low.z + high.z) * .5};
        asset.scale = (c == 1 ? 1.68 : 1.80) / (high.y - low.y);
        asset.material.alphaMode = MaterialAlphaMode::Opaque;
        asset.material.baseColorTexture = Utf8Path(root / "albedo.png");
        asset.material.normalTexture = Utf8Path(root / "normal.png");
        asset.material.roughnessTexture = Utf8Path(root / "roughness.png");
    }
}

bool Characters::VerifyClips() const {
    for (const auto& asset : characters_) {
        for (unsigned i = 0; i < asset.clips.size(); ++i) {
            const double duration =
                asset.clipSource->animations.at(size_t(asset.clips[i])).duration;
            ModelPose first, later;
            std::string error;
            if (!asset.Evaluate(i, duration * .19, first, error) ||
                !asset.Evaluate(i, duration * .67, later, error))
                throw std::runtime_error("FBX motion evaluation " + asset.path + ": " + error);
            bool changed = false;
            for (size_t m = 0; m < first.submeshes.size(); ++m)
                for (size_t j = 0; j < first.submeshes[m].size(); ++j) {
                    const auto delta =
                        first.submeshes[m][j].position - later.submeshes[m][j].position;
                    changed |= delta.Dot(delta) > 1e-9;
                }
            if (!changed)
                return false;
        }
    }
    std::string error;
    return !ModelAsset::LoadFbx(characters_.front().clipSource->sourcePath, error);
}

void Characters::Draw(Renderer3D::Context& renderer, size_t actor, Vec3 position, double yaw,
                      bool crouched, double speed, double clock) {
    constexpr std::array<size_t, 4> outfits{1, 0, 2, 0};
    const auto& asset = characters_.at(outfits.at(actor));
    auto& state = actors_.at(actor);
    const unsigned motion = unsigned(speed > .14);
    const double duration = asset.clipSource->animations.at(size_t(asset.clips[motion])).duration;
    const double dt = std::clamp(clock - state.previousClock, 0., .10);
    state.previousClock = clock;
    if (!state.pose || state.state != motion) {
        state.state = motion;
        state.sampledTime = -1;
        state.phase = 0;
    }
    state.phase =
        std::fmod(state.phase + dt * (motion ? std::clamp(speed / 1.55, .48, 1.75) : 1), duration);
    const double sample = std::floor(state.phase * 24) / 24;
    if (sample != state.sampledTime) {
        auto pose = CreateRef<ModelPose>();
        std::string error;
        if (!asset.Evaluate(motion, sample, *pose, error))
            throw std::runtime_error("Character motion: " + error);
        state.pose = std::move(pose);
        ++state.revision;
        state.sampledTime = sample;
    }
    const auto scale = asset.scale;
    const Mat4 transform = Mat4::Translate(position) * Mat4::RotateY(std::numbers::pi - yaw) *
                           Mat4::Scale({scale, scale * (crouched ? .78 : 1), scale}) *
                           Mat4::Translate(asset.foot * -1);
    for (size_t i = 0; i < asset.model->submeshes.size(); ++i) {
        const auto matrix =
            transform * state.pose->nodeWorldTransforms.at(asset.model->submeshes[i].node);
        if (!renderer.DrawMeshAsset(asset.path, matrix, asset.material, static_cast<int32>(i), -1,
                                    0, actor + 1, state.pose.get(), state.revision, state.pose))
            throw std::runtime_error("Character rendering: " + renderer.GetLastAssetError());
    }
}
} // namespace Frontier
