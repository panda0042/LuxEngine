#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <string_view>

namespace LuxEngine {
class Scene::AnimationBindings {
public:
    explicit AnimationBindings(Scene& scene, bool shared = true) : scene_(scene), shared_(shared) {}

    uint32 Resolve(uint32 owner, std::string_view path) {
        if (path.empty())
            return owner;
        if (shared_ && !indexed_) {
            indexed_ = true;
            const auto pool = scene_.pools_.find(typeid(HierarchyComponent));
            if (pool != scene_.pools_.end())
                for (const auto& [id, hierarchy] :
                     static_cast<const Pool<HierarchyComponent>&>(*pool->second).values) {
                    const auto* tag = scene_.TryGetComponent<TagComponent>(id);
                    if (!tag || !hierarchy.parent)
                        continue;
                    auto [child, added] = children_[hierarchy.parent].try_emplace(tag->tag, id);
                    if (!added)
                        child->second = 0;
                }
        }
        auto current = owner;
        while (!path.empty()) {
            const auto end = path.find('/');
            current = FindChild(current, path.substr(0, end));
            if (!current)
                return 0;
            if (end == std::string_view::npos)
                break;
            path.remove_prefix(end + 1);
        }
        return current;
    }

private:
    uint32 FindChild(uint32 parent, std::string_view name) const {
        if (shared_) {
            const auto children = children_.find(parent);
            if (children == children_.end())
                return 0;
            const auto match = children->second.find(name);
            return match != children->second.end() ? match->second : 0;
        }
        uint32 match = 0;
        for (const auto child : scene_.GetChildren(parent)) {
            const auto* tag = scene_.TryGetComponent<TagComponent>(child);
            if (tag && tag->tag == name) {
                if (match)
                    return 0;
                match = child;
            }
        }
        return match;
    }

    Scene& scene_;
    bool shared_;
    bool indexed_ = false;
    std::unordered_map<uint32, std::unordered_map<std::string_view, uint32>> children_;
};

namespace {
std::optional<std::array<double, 4>> ReadAnimatedValue(Scene& scene, uint32 id,
                                                       const AnimationSample& sample) {
    if (!id)
        return std::nullopt;
    using Value = std::array<double, 4>;
    const auto vector = [](const Vec3& v) { return Value{v.x, v.y, v.z, 0}; };
    if (const auto* transform = scene.TryGetComponent<TransformComponent>(id)) {
        if (sample.property == AnimationProperty::Position)
            return vector(transform->position);
        if (sample.property == AnimationProperty::Rotation)
            return vector(transform->rotation);
        if (sample.property == AnimationProperty::Scale)
            return vector(transform->scale);
    }
    const auto* mesh = scene.TryGetComponent<MeshRendererComponent>(id);
    if (sample.property == AnimationProperty::Color ||
        sample.property == AnimationProperty::Opacity) {
        const auto* sprite = scene.TryGetComponent<SpriteRendererComponent>(id);
        if (!mesh && !sprite)
            return std::nullopt;
        const auto& color = sprite ? sprite->color : mesh->material.albedoColor;
        if (sample.property == AnimationProperty::Opacity)
            return Value{color.w, 0, 0, 0};
        return Value{color.x, color.y, color.z, color.w};
    }
    if (sample.property == AnimationProperty::Float) {
        if (const auto* script = scene.TryGetComponent<ScriptComponent>(id)) {
            const auto field = script->fields.find(sample.field);
            if (field != script->fields.end())
                if (const auto* value = std::get_if<float>(&field->second))
                    return Value{*value, 0, 0, 0};
        }
        if (const auto* camera = scene.TryGetComponent<CameraComponent>(id)) {
            if (sample.field == "camera.fov")
                return Value{camera->fov, 0, 0, 0};
            if (sample.field == "camera.orthoSize")
                return Value{camera->orthoSize, 0, 0, 0};
            if (sample.field == "camera.nearClip")
                return Value{camera->nearClip, 0, 0, 0};
            if (sample.field == "camera.farClip")
                return Value{camera->farClip, 0, 0, 0};
        }
        if (mesh) {
            if (sample.field == "material.metallic")
                return Value{mesh->material.metallic, 0, 0, 0};
            if (sample.field == "material.roughness")
                return Value{mesh->material.roughness, 0, 0, 0};
        }
    }
    return std::nullopt;
}

void Fail(const std::string& error) {
    Log::Warn("애니메이션을 적용하지 못했습니다: " + error);
}
} // namespace

void Scene::ApplyAnimationFrame(uint32 owner, const AnimationFrame& frame, double dt,
                                bool rootMotion) {
    AnimationBindings bindings(*this, false);
    ApplyAnimationFrame(owner, frame, dt, rootMotion, bindings, nullptr);
}

void Scene::ApplyAnimationFrame(uint32 owner, const AnimationFrame& frame, double dt,
                                bool rootMotion, AnimationBindings& bindings,
                                RootMotionDeltas* physicsMotion) {
    if (!IsAlive(owner))
        return;
    for (auto id : GetComponentEntityIds<ModelAnimationComponent>()) {
        auto& model = GetComponent<ModelAnimationComponent>(id);
        if (model.poseOwner == owner)
            model.mixedPose.reset();
    }
    auto applyMotion = [&](Vec3 delta) {
        auto* transform = TryGetComponent<TransformComponent>(owner);
        if (!rootMotion || !transform || dt <= 0)
            return;
        auto* body2D = TryGetComponent<Rigidbody2DComponent>(owner);
        auto* body3D = TryGetComponent<Rigidbody3DComponent>(owner);
        const bool uses2D = body2D && HasComponent<BoxCollider2DComponent>(owner);
        const bool dynamic2D = uses2D && body2D->type == BodyType::Dynamic;
        const bool dynamic3D = !uses2D && body3D && body3D->type == BodyType::Dynamic &&
                               HasComponent<BoxCollider3DComponent>(owner);
        if (dynamic2D || dynamic3D) {
            const auto worldDelta = GetWorldMatrix(owner).TransformDirection(delta);
            if (physicsMotion)
                (*physicsMotion)[owner] = worldDelta;
            else if (dynamic2D)
                body2D->velocity = {worldDelta.x / dt, worldDelta.y / dt};
            else
                body3D->velocity = worldDelta * (1 / dt);
        } else
            transform->position += transform->GetTransform().TransformDirection(delta);
    };
    for (const auto& sample : frame.samples) {
        const uint32 target = bindings.Resolve(owner, sample.target);
        if (!target)
            continue;
        const auto& v = sample.value;
        auto* transform = TryGetComponent<TransformComponent>(target);
        if (sample.property == AnimationProperty::Position && transform) {
            const Vec3 position{v[0], v[1], v[2]};
            if (!rootMotion || (target != owner && !sample.rootMotionPosition))
                transform->position = position;
        } else if (sample.property == AnimationProperty::Rotation && transform)
            transform->rotation = {v[0], v[1], v[2]};
        else if (sample.property == AnimationProperty::Scale && transform)
            transform->scale = {v[0], v[1], v[2]};
        else if (sample.property == AnimationProperty::Color) {
            const Vec4 color{static_cast<float>(v[0]), static_cast<float>(v[1]),
                             static_cast<float>(v[2]), static_cast<float>(v[3])};
            if (auto* sprite = TryGetComponent<SpriteRendererComponent>(target))
                sprite->color = color;
            if (auto* mesh = TryGetComponent<MeshRendererComponent>(target))
                mesh->material.albedoColor = color;
        } else if (sample.property == AnimationProperty::Opacity) {
            const auto alpha = static_cast<float>(std::clamp(v[0], 0.0, 1.0));
            if (auto* sprite = TryGetComponent<SpriteRendererComponent>(target))
                sprite->color.w = alpha;
            if (auto* mesh = TryGetComponent<MeshRendererComponent>(target)) {
                mesh->material.albedoColor.w = alpha;
                mesh->material.alphaMode = MaterialAlphaMode::Blend;
            }
        } else if (sample.property == AnimationProperty::SpriteFrame) {
            if (auto* sprite = TryGetComponent<SpriteRendererComponent>(target)) {
                const auto& path =
                    sample.spriteSheet ? sample.spriteSheet->path : frame.spriteSheet;
                const auto columns =
                    sample.spriteSheet ? sample.spriteSheet->columns : frame.spriteColumns;
                const auto rows = sample.spriteSheet ? sample.spriteSheet->rows : frame.spriteRows;
                if (!path.empty())
                    sprite->texturePath = path;
                if (!path.empty() || columns != 1 || rows != 1) {
                    sprite->columns = columns;
                    sprite->rows = rows;
                }
                sprite->frame = static_cast<uint32>(
                    std::clamp(std::floor(v[0]), 0.0, double(sprite->columns) * sprite->rows - 1));
            }
        } else if (sample.property == AnimationProperty::Float) {
            if (auto* camera = TryGetComponent<CameraComponent>(target)) {
                if (sample.field == "camera.fov")
                    camera->fov = std::clamp(v[0], .01, 179.99);
                if (sample.field == "camera.orthoSize")
                    camera->orthoSize = std::max(.0001, v[0]);
                if (sample.field == "camera.nearClip") {
                    const double maximum = camera->farClip * .5;
                    if (maximum > 0 && std::isfinite(maximum))
                        camera->nearClip = std::clamp(v[0], std::min(.0001, maximum), maximum);
                }
                if (sample.field == "camera.farClip")
                    camera->farClip = std::max(camera->nearClip * 2, v[0]);
            }
            if (auto* mesh = TryGetComponent<MeshRendererComponent>(target)) {
                if (sample.field == "material.metallic")
                    mesh->material.metallic = static_cast<float>(std::clamp(v[0], 0., 1.));
                if (sample.field == "material.roughness")
                    mesh->material.roughness = static_cast<float>(std::clamp(v[0], 0., 1.));
            }
            if (auto* script = TryGetComponent<ScriptComponent>(target)) {
                auto at = script->fields.find(sample.field);
                if (at != script->fields.end() && std::holds_alternative<float>(at->second))
                    at->second = static_cast<float>(v[0]);
            }
        }
    }
    auto rootDelta = frame.rootDelta;
    if (rootMotion && rootDelta) {
        bool allBindingsMissing = !frame.childRootMotion.empty();
        for (const auto& sample : frame.childRootMotion) {
            const auto target = bindings.Resolve(owner, sample.target);
            if (!target || !HasComponent<TransformComponent>(target)) {
                if (sample.modelBinding)
                    allBindingsMissing = false;
                else
                    *rootDelta = *rootDelta - sample.delta;
                continue;
            }
            allBindingsMissing = false;
            if (target == owner)
                continue;
            auto delta = sample.delta;
            for (auto parent = GetParent(target); parent && parent != owner;
                 parent = GetParent(parent))
                if (const auto* transform = TryGetComponent<TransformComponent>(parent))
                    delta = transform->GetTransform().TransformDirection(delta);
            *rootDelta += delta - sample.delta;
        }
        if (allBindingsMissing && rootDelta->x == 0 && rootDelta->y == 0 && rootDelta->z == 0)
            rootDelta.reset();
    }
    if (frame.models.empty()) {
        if (rootDelta)
            applyMotion(*rootDelta);
        return;
    }
    const auto& path = frame.models.front().path;
    std::string error;
    auto model = ModelAsset::LoadFbx(ResolveAssetPath(path), error);
    if (!model) {
        Fail(error);
        return;
    }
    std::vector<ModelPose> poses;
    poses.reserve(frame.models.size());
    std::vector<const AnimationModelSample*> evaluated;
    std::vector<ModelPoseLayer> layers;
    for (const auto& sample : frame.models) {
        if (sample.path != path || sample.weight <= 0)
            continue;
        ModelPose pose;
        if (!model->Evaluate(sample.time, sample.clip, pose, error)) {
            Fail(error);
            return;
        }
        poses.push_back(std::move(pose));
        evaluated.push_back(&sample);
        layers.push_back(
            {&poses.back(), sample.weight, sample.layer, sample.additive, sample.mask});
    }
    std::vector<ModelPoseOverride> overrides;
    for (const auto& sample : frame.samples) {
        if (sample.target.empty() || int(sample.property) > 2)
            continue;
        std::optional<uint32> match;
        for (uint32 node = 0; node < model->nodes.size(); ++node) {
            std::string path = model->nodes[node].name;
            auto parent = model->nodes[node].parent;
            while (parent >= 0) {
                path = model->nodes[parent].name + "/" + path;
                parent = model->nodes[parent].parent;
            }
            if (path == sample.target || path.ends_with("/" + sample.target)) {
                if (match) {
                    match.reset();
                    break;
                }
                match = node;
            }
        }
        if (!match)
            continue;
        ModelPoseOverride value;
        value.node = *match;
        const Vec3 vector{sample.value[0], sample.value[1], sample.value[2]};
        if (sample.property == AnimationProperty::Position)
            value.position = vector;
        if (sample.property == AnimationProperty::Rotation)
            value.rotation = vector;
        if (sample.property == AnimationProperty::Scale)
            value.scale = vector;
        overrides.push_back(value);
    }

    if (rootMotion) {
        Vec3 delta = rootDelta.value_or(Vec3{});
        double total = 0;
        for (size_t index = 0; index < poses.size(); ++index) {
            const auto& sample = *evaluated[index];
            if (sample.weight <= 0 || sample.path != path)
                continue;
            uint32 root = model->submeshes.front().node;
            for (uint32 node = 0; node < model->nodes.size(); ++node)
                if ((!sample.rootMotionTarget.empty() &&
                     model->nodes[node].name == sample.rootMotionTarget) ||
                    (sample.rootMotionTarget.empty() && model->nodes[node].bone &&
                     (model->nodes[node].parent < 0 ||
                      !model->nodes[model->nodes[node].parent].bone))) {
                    root = node;
                    break;
                }
            if (!model->NodeMatchesMask(root, sample.mask))
                continue;
            if (!rootDelta) {
                ModelPose previous;
                if (!model->Evaluate(sample.previousTime, sample.clip, previous, error)) {
                    Fail(error);
                    return;
                }
                auto position = [&](const ModelPose& pose) {
                    const auto* m = pose.nodeWorldTransforms.at(root).elements;
                    return Vec3{m[12], m[13], m[14]};
                };
                Vec3 movement = position(poses[index]) - position(previous);
                if (sample.cycles != 0 && sample.clip >= 0) {
                    ModelPose start, end;
                    if (!model->Evaluate(0, sample.clip, start, error) ||
                        !model->Evaluate(model->animations[sample.clip].duration, sample.clip, end,
                                         error)) {
                        Fail(error);
                        return;
                    }
                    movement += (position(end) - position(start)) * sample.cycles;
                }
                delta += movement * sample.weight;
                total += sample.weight;
            }
            ModelPoseOverride extraction;
            extraction.node = root;
            extraction.position = model->nodes[root].localTransform.position;
            overrides.push_back(extraction);
        }
        if (!rootDelta && total > 1)
            delta = delta * (1 / total);
        if (rootDelta || total > 0)
            applyMotion(delta);
    }
    auto mixed = CreateRef<ModelPose>();
    if (!model->MixPoses(layers, *mixed, error, overrides)) {
        Fail(error);
        return;
    }
    std::vector<uint32> pending{owner};
    while (!pending.empty()) {
        auto id = pending.back();
        pending.pop_back();
        const auto children = GetChildren(id);
        pending.insert(pending.end(), children.begin(), children.end());
        auto* animation = TryGetComponent<ModelAnimationComponent>(id);
        if (!animation || ResolveAssetPath(animation->modelPath) != ResolveAssetPath(path) ||
            animation->nodeIndex >= mixed->nodeLocalTransforms.size())
            continue;
        animation->mixedPose = mixed;
        animation->poseOwner = owner;
        animation->playing = false;
        ++animation->poseRevision;
        if (animation->applyNodeTransform && id != owner) {
            const auto& node = mixed->nodeLocalTransforms[animation->nodeIndex];
            if (auto* transform = TryGetComponent<TransformComponent>(id)) {
                transform->position = node.position;
                transform->rotation = node.rotation;
                transform->scale = node.scale;
            }
        }
    }
}

void Scene::UpdateAnimations(double dt, bool emitEvents) {
    UpdateAnimations(dt, emitEvents, nullptr);
}

void Scene::UpdateAnimations(double dt, bool emitEvents, RootMotionDeltas* physicsMotion) {
    AnimationBindings bindings(*this);
    for (auto id : GetComponentEntityIds<ModelAnimationComponent>()) {
        auto& model = GetComponent<ModelAnimationComponent>(id);
        if (model.poseOwner && !HasComponent<AnimationComponent>(model.poseOwner) &&
            !HasComponent<AnimatorComponent>(model.poseOwner)) {
            model.mixedPose.reset();
            model.poseOwner = 0;
        }
    }
    for (auto id : GetComponentEntityIds<AnimationComponent>()) {
        auto* animation = TryGetComponent<AnimationComponent>(id);
        if (!animation)
            continue;
        animation->pendingEvents.clear();
        if (!animation->playing)
            continue;
        const auto path = ResolveAssetPath(animation->assetPath);
        if (animation->assetPath.empty()) {
            if (!animation->loadedPath.empty()) {
                animation->asset.reset();
                animation->loadedPath.clear();
            }
        } else if (path.empty() || !animation->asset || animation->loadedPath != path) {
            std::string error;
            auto clip = AnimationClip::Load(path, error);
            if (!clip) {
                animation->playing = false;
                Fail(error);
                continue;
            }
            animation->asset = std::move(clip);
            animation->loadedPath = path;
        }
        const auto& clip = animation->asset ? *animation->asset : animation->clip;
        const auto wrap = clip.GetWrap();
        AnimationPlayback playback{animation->currentTime, animation->speed, animation->playing};
        if (!animation->playbackStarted && playback.cursor == 0 && playback.speed < 0 &&
            wrap == AnimationWrap::Once && std::isfinite(dt) && dt >= 0 &&
            std::isfinite(clip.duration) && clip.duration > 0)
            playback.cursor = clip.duration;
        auto events = playback.Advance(dt, clip.duration, wrap, clip.events);
        const bool advanced = playback.cursor != playback.previousCursor;
        if (emitEvents && advanced && !animation->playbackStarted) {
            const auto start = AnimationTime(playback.previousCursor, clip.duration, wrap);
            std::vector<AnimationEvent> startingEvents;
            for (const auto& event : clip.events)
                if (event.time == start)
                    startingEvents.push_back(event);
            if (!startingEvents.empty()) {
                startingEvents.insert(startingEvents.end(), std::make_move_iterator(events.begin()),
                                      std::make_move_iterator(events.end()));
                events.swap(startingEvents);
            }
        }
        animation->playbackStarted |= advanced;
        animation->currentTime = playback.cursor;
        animation->playing = playback.playing;
        AnimationFrame frame;
        frame.rootDelta = clip.RootDelta(playback.previousCursor, playback.cursor);
        if (frame.rootDelta && !clip.rootMotionTarget.empty())
            frame.childRootMotion.push_back(
                {clip.rootMotionTarget, *frame.rootDelta, !clip.modelPath.empty()});
        frame.samples = clip.SampleTracks(playback.cursor);
        frame.spriteSheet = clip.spriteSheet;
        frame.spriteColumns = clip.spriteColumns;
        frame.spriteRows = clip.spriteRows;
        if (!clip.modelPath.empty()) {
            AnimationModelSample sample;
            sample.path = clip.modelPath;
            sample.clip = clip.modelClip;
            sample.time = playback.Time(clip.duration, wrap);
            sample.previousTime = AnimationTime(playback.previousCursor, clip.duration, wrap);
            sample.cycles = wrap == AnimationWrap::Loop
                                ? std::floor(playback.cursor / clip.duration) -
                                      std::floor(playback.previousCursor / clip.duration)
                                : 0;
            sample.rootMotionTarget = clip.rootMotionTarget;
            frame.models.push_back(sample);
        }
        ApplyAnimationFrame(id, frame, dt, animation->rootMotion, bindings, physicsMotion);
        if (emitEvents)
            animation->pendingEvents = std::move(events);
    }
    for (auto id : GetComponentEntityIds<AnimatorComponent>()) {
        auto* animation = TryGetComponent<AnimatorComponent>(id);
        if (!animation)
            continue;
        animation->pendingEvents.clear();
        if (!animation->playing)
            continue;
        const auto path = ResolveAssetPath(animation->controllerPath);
        if (!animation->player || animation->loadedPath != path) {
            std::string error;
            auto controller = AnimatorController::Load(path, error);
            auto player = CreateRef<AnimatorPlayer>();
            if (!controller || !player->SetController(controller, error)) {
                animation->playing = false;
                Fail(error);
                continue;
            }
            animation->player = std::move(player);
            animation->loadedPath = path;
            for (const auto& parameter : animation->parameters)
                animation->player->SetParameter(parameter.name, parameter.value);
            if (animation->rootMotion)
                ApplyAnimationFrame(id, animation->player->Update(0, false, {}, true), 0, true,
                                    bindings, nullptr);
        }
        animation->player->SetPlaying(animation->playing);
        auto frame = animation->player->Update(
            dt, emitEvents,
            [&](const auto& sample) {
                return ReadAnimatedValue(*this, bindings.Resolve(id, sample.target), sample);
            },
            animation->rootMotion);
        ApplyAnimationFrame(id, frame, dt, animation->rootMotion, bindings, physicsMotion);
        if (emitEvents)
            animation->pendingEvents = std::move(frame.events);
    }
}
} // namespace LuxEngine
