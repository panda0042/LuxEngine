#include "Swimmer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace LuxEngine;

std::string Utf8(const std::filesystem::path& file) {
    const auto path = file.u8string();
    return {reinterpret_cast<const char*>(path.data()), path.size()};
}

Vertex3D TransformVertex(const ModelVertex& vertex, const Mat4& transform) {
    return {transform.TransformPoint(vertex.position),
            transform.TransformDirection(vertex.normal).Normalized(), vertex.texCoords};
}
} // namespace

void Swimmer::Load(GpuDevice& gpu, const std::filesystem::path& file, bool recording) {
    gpu_ = &gpu;
    poseRate_ = recording ? 60 : 30;
    std::string error;
    model_ = ModelAsset::LoadFbx(Utf8(file), error);
    if (!model_ || !model_->Evaluate(0, -1, pose_, error))
        throw std::runtime_error("Could not load swimmer: " + error);

    constexpr std::array<std::string_view, 4> names{"idle", "walk", "swim_tread",
                                                    "swim_breaststroke"};
    for (size_t i = 0; i < names.size(); ++i) {
        const auto clip = std::ranges::find_if(model_->animations, [&](const ModelAnimation& c) {
            return (c.name == names[i] || c.name.ends_with("|" + std::string(names[i]))) &&
                   c.duration > 0;
        });
        if (clip == model_->animations.end())
            throw std::runtime_error("Missing swimmer animation: " + std::string(names[i]));
        clips_[i] = static_cast<int32>(clip - model_->animations.begin());
    }

    const auto texturePath = [&](int32 index) -> std::string {
        if (index < 0)
            return {};
        const auto& texture = model_->textures.at(static_cast<size_t>(index));
        const auto& name = texture.relativePath.empty() ? texture.path : texture.relativePath;
        const auto relative = std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t*>(name.data()), name.size()));
        const auto local = file.parent_path() / relative.filename();
        if (!std::filesystem::is_regular_file(local))
            throw std::runtime_error("Missing swimmer texture: " + Utf8(local));
        return Utf8(local);
    };
    for (const auto& source : model_->materials) {
        auto& material = materials_.emplace_back();
        material.alphaMode = MaterialAlphaMode::Opaque;
        material.albedoColor = source.baseColor;
        material.metallic = source.metallic;
        material.roughness = source.roughness;
        material.baseColorTexture = texturePath(source.baseColorTexture);
        material.normalTexture = texturePath(source.normalTexture);
        if (!material.baseColorTexture.empty())
            material.albedoColor = {1, 1, 1, 1};
        if (source.name == "해병대 오색 물결무늬" || source.name == "회녹색 능직") {
            material.normalTexture = Utf8(file.parent_path() / "weave.png");
            material.normalScale = .42f;
        }
    }

    const double infinity = std::numeric_limits<double>::infinity();
    Vec3 low{infinity, infinity, infinity}, high{-infinity, -infinity, -infinity};
    for (size_t i = 0; i < model_->submeshes.size(); ++i) {
        const auto& part = model_->submeshes[i];
        const auto& world = pose_.nodeWorldTransforms.at(part.node);
        if (part.material < 0 || part.vertices.empty() || !part.skinned)
            throw std::runtime_error("Swimmer mesh is missing a material or skin");
        const auto& first = part.vertices.front().weights;
        const bool rigid =
            first.size() == 1 && std::ranges::all_of(part.vertices, [&](const ModelVertex& v) {
                return v.weights.size() == 1 && v.weights[0].bone == first[0].bone &&
                       std::abs(v.weights[0].weight - 1) < 1e-5f;
            });
        const auto bone = rigid ? static_cast<int32>(part.bones.at(first[0].bone).node) : -1;
        auto found = std::ranges::find_if(batches_, [&](const Batch& b) {
            return b.material == part.material && b.bone == bone;
        });
        if (found == batches_.end())
            found = batches_.insert(batches_.end(), Batch{part.material, bone, {}, {}, {}, {}});
        const auto start = static_cast<uint32>(found->vertices.size());
        const auto& bind = rigid ? part.bones.at(first[0].bone).inverseBind : world;
        if (!rigid)
            found->parts.push_back({i, start});
        for (size_t j = 0; j < part.vertices.size(); ++j) {
            const auto& deformed = pose_.submeshes.at(i).at(j);
            found->vertices.push_back(TransformVertex(rigid ? part.vertices[j] : deformed, bind));
            const auto p = world.TransformPoint(deformed.position);
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
        for (const auto index : part.indices)
            found->indices.push_back(start + index);
    }
    if (!(high.y - low.y > .01))
        throw std::runtime_error("Swimmer has no usable standing height");
    feet_ = {(low.x + high.x) * .5, low.y, (low.z + high.z) * .5};
    scale_ = 1.55 / (high.y - low.y);
    for (auto& batch : batches_)
        if (!batch.mesh.Upload(&gpu, batch.vertices, batch.indices))
            throw std::runtime_error("Could not upload swimmer mesh: " + gpu.GetLastError());

    size_t dynamicBytes = 0;
    for (const auto& batch : batches_)
        if (batch.bone < 0)
            dynamicBytes +=
                batch.vertices.size() * sizeof(Vertex3D) + batch.indices.size() * sizeof(uint32);
    size_t remaining = 112 * 1024 * 1024;
    for (const auto motion : {Motion::Swim, Motion::Tread}) {
        const auto animation = static_cast<size_t>(motion);
        const auto clip = static_cast<size_t>(clips_[animation]);
        const double count = std::ceil(model_->animations[clip].duration * poseRate_);
        if (count <= 90 && count * double(dynamicBytes) <= double(remaining)) {
            frames_[animation].resize(static_cast<size_t>(count));
            remaining -= frames_[animation].size() * dynamicBytes;
        }
    }
}

void Swimmer::Advance(Motion motion, double dt, double playbackRate) {
    if (motion_ != motion) {
        if (activeFrame_) {
            std::string error;
            if (!model_->Evaluate(sampledPhase_, clips_[static_cast<size_t>(motion_)], previous_,
                                  error))
                throw std::runtime_error("Could not evaluate the previous swimmer pose: " + error);
        } else {
            previous_ = pose_;
        }
        activeFrame_ = nullptr;
        motion_ = motion;
        transition_ = 0;
        phase_ = 0;
        sampledPhase_ = -1;
    }
    const auto clip = static_cast<size_t>(clips_[static_cast<size_t>(motion_)]);
    phase_ = std::fmod(phase_ + dt * playbackRate, model_->animations[clip].duration);
    transition_ = std::min(1., transition_ + dt / .26);
}

void Swimmer::FillDeformed(Batch& batch) {
    for (const auto& part : batch.parts) {
        const auto& vertices = pose_.submeshes[part.submesh];
        const auto& world = pose_.nodeWorldTransforms[model_->submeshes[part.submesh].node];
        for (size_t i = 0; i < vertices.size(); ++i)
            batch.vertices[part.firstVertex + i] = TransformVertex(vertices[i], world);
    }
}

void Swimmer::UpdatePose() {
    const auto sample = static_cast<size_t>(std::floor(phase_ * poseRate_));
    const double time = double(sample) / poseRate_;
    if (time == sampledPhase_)
        return;
    sampledPhase_ = time;
    activeFrame_ = nullptr;
    std::string error;
    const auto motionIndex = static_cast<size_t>(motion_);
    const auto clip = clips_[motionIndex];
    auto& frames = frames_[motionIndex];
    if (transition_ >= 1 && sample < frames.size()) {
        auto& frame = frames[sample];
        if (frame.transforms.empty()) {
            if (!model_->Evaluate(time, clip, pose_, error))
                throw std::runtime_error("Could not evaluate swimmer animation: " + error);
            frame.meshes.resize(batches_.size());
            for (size_t i = 0; i < batches_.size(); ++i) {
                auto& batch = batches_[i];
                if (batch.bone >= 0)
                    continue;
                FillDeformed(batch);
                if (!frame.meshes[i].Upload(gpu_, batch.vertices, batch.indices))
                    throw std::runtime_error("Could not cache swimmer geometry");
            }
            frame.transforms = pose_.nodeWorldTransforms;
        }
        activeFrame_ = &frame;
        return;
    }
    if (transition_ < 1) {
        const double blend = transition_ * transition_ * (3 - 2 * transition_);
        layers_ = {{&previous_, 1 - blend, 0, false, {}}, {&target_, blend, 0, false, {}}};
        if (!model_->Evaluate(time, clip, target_, error) ||
            !model_->MixPoses(layers_, pose_, error))
            throw std::runtime_error("Could not blend swimmer animation: " + error);
    } else if (!model_->Evaluate(time, clip, pose_, error)) {
        throw std::runtime_error("Could not evaluate swimmer animation: " + error);
    }
    for (auto& batch : batches_)
        if (batch.bone < 0) {
            FillDeformed(batch);
            if (!batch.mesh.UpdateVertices(batch.vertices))
                throw std::runtime_error("Could not animate swimmer geometry");
        }
}

void Swimmer::Draw(Renderer3D::Context& renderer, Vec3 position, double yaw) {
    UpdatePose();
    const auto actor = Mat4::Translate(position) * Mat4::RotateY(yaw) *
                       Mat4::Scale({scale_, scale_, scale_}) * Mat4::Translate(feet_ * -1);
    const auto& transforms = activeFrame_ ? activeFrame_->transforms : pose_.nodeWorldTransforms;
    for (size_t i = 0; i < batches_.size(); ++i) {
        const auto& batch = batches_[i];
        const auto transform =
            batch.bone < 0 ? actor : actor * transforms[static_cast<size_t>(batch.bone)];
        const auto& mesh = activeFrame_ && batch.bone < 0 ? activeFrame_->meshes[i] : batch.mesh;
        renderer.DrawMesh(mesh, transform, materials_.at(static_cast<size_t>(batch.material)));
    }
}

void Swimmer::Release() {
    activeFrame_ = nullptr;
    for (auto& frames : frames_)
        frames.clear();
    batches_.clear();
    materials_.clear();
    model_.reset();
}
