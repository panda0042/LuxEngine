#include <LuxEngine/Animation/Animator.hpp>
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Physics/PhysicsWorld.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace LuxEngine;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

bool Near(double a, double b, double epsilon = 1e-8) {
    return std::abs(a - b) < epsilon;
}

AnimationTrack Track(AnimationProperty property, double a, double b, double duration = 1) {
    AnimationTrack track;
    track.property = property;
    track.SetKey({0, {a, 0, 0, 0}});
    track.SetKey({duration, {b, 0, 0, 0}});
    return track;
}

AnimationClip Clip(double x) {
    AnimationClip clip;
    clip.name = "Clip";
    clip.tracks.push_back(Track(AnimationProperty::Position, x, x));
    return clip;
}

void Curves() {
    auto track = Track(AnimationProperty::Position, 0, 10);
    Check(Near(track.Sample(.25)[0], 2.5), "linear channel");
    const auto extreme = Track(AnimationProperty::Position, -1e308, 1e308);
    Check(extreme.Sample(.5)[0] == 0 && std::isfinite(extreme.Sample(.75)[0]),
          "linear interpolation has finite results between extreme finite keys");
    AnimationClip legacyExtreme;
    legacyExtreme.AddKeyframe({0, {-1e308, 0, 0}});
    legacyExtreme.AddKeyframe({1, {1e308, 0, 0}});
    Check(legacyExtreme.Sample(.5).position.x == 0,
          "legacy keyframes also interpolate extreme finite positions");
    Scene animated;
    auto entity = animated.CreateEntity("Extreme animated position");
    auto& component = entity.AddComponent<AnimationComponent>();
    component.clip.tracks = {extreme};
    animated.OnUpdate(.5);
    std::string serialized;
    Check(entity.GetComponent<TransformComponent>().position.x == 0 &&
              animated.SerializeToString(serialized),
          "a scene stays saveable after sampling extreme finite position keys");
    track.keys.front().interpolation = AnimationInterpolation::Step;
    Check(track.Sample(.9)[0] == 0, "step channel");
    track.keys.front().interpolation = AnimationInterpolation::Cubic;
    Check(Near(track.Sample(.25)[0], 1.5625), "cubic tangents");
    track.keys.front().outTangent[0] = 10;
    track.keys.back().inTangent[0] = 10;
    Check(Near(track.Sample(.25)[0], 2.5), "editable cubic tangents");
    AnimationTrack rotation = Track(AnimationProperty::Rotation, ToRadians(350), ToRadians(10));
    auto middle = rotation.Sample(.5);
    Check(std::abs(std::sin(middle[0])) < 1e-8, "quaternion shortest rotation");
    AnimationClip endpoints;
    endpoints.tracks = {Track(AnimationProperty::Position, 0, 10)};
    Check(Near(endpoints.SampleTracks(1, false).at(0).value[0], 10),
          "scrubbing evaluates the last key without wrapping");
    AnimationClip motion;
    motion.tracks = {Track(AnimationProperty::Position, 0, 10)};
    Check(motion.RootDelta(.9, 1.1) && Near(motion.RootDelta(.9, 1.1)->x, 2),
          "root motion remains continuous across loops");
    auto cubicRotation = Track(AnimationProperty::Rotation, 0, ToRadians(90));
    cubicRotation.keys[0].interpolation = AnimationInterpolation::Cubic;
    Check(Near(cubicRotation.Sample(.25)[0], ToRadians(14.0625)),
          "spherical cubic rotation tangents");
    Check(Near(AnimationTime(-.25, 1, AnimationWrap::Loop), .75), "reverse wrapping");
    Check(Near(AnimationTime(1.25, 1, AnimationWrap::PingPong), .75), "ping pong");
    AnimationClip legacy;
    legacy.AddKeyframe({0, {0, 0, 0}});
    legacy.AddKeyframe({.00005, {10, 0, 0}});
    Check(Near(legacy.Sample(.000025).position.x, 5) && Near(legacy.Sample(.00005).position.x, 10),
          "closely spaced legacy keys interpolate and reach the endpoint");
    legacy.wrap = AnimationWrap::PingPong;
    legacy.AddKeyframe({1, {20, 0, 0}});
    double cursor = .8;
    TransformComponent pose;
    AnimationEngine::UpdateClip(legacy, cursor, .3, pose);
    Check(Near(pose.position.x, legacy.Sample(1.1).position.x),
          "legacy update helper preserves the ping-pong return leg");
    AnimationPlayback playback{0, 1, true};
    std::vector<AnimationEvent> events{{.25, "footstep", "left"}, {.75, "footstep", "right"}};
    Check(playback.Advance(2, 1, AnimationWrap::Loop, events).size() == 4,
          "events across multiple loops");
    playback.speed = -1;
    Check(playback.Advance(1, 1, AnimationWrap::Loop, events).size() == 2, "reverse events");
    const std::vector<AnimationEvent> endpointsAtBoundary{{0, "start", ""}, {1, "end", ""}};
    playback = {.25, -1, true};
    const auto reverseBoundary = playback.Advance(.25, 1, AnimationWrap::Loop, endpointsAtBoundary);
    Check(reverseBoundary.size() == 2 && reverseBoundary[0].name == "start" &&
              reverseBoundary[1].name == "end" &&
              playback.Advance(.25, 1, AnimationWrap::Loop, endpointsAtBoundary).empty(),
          "reverse landing on a loop boundary emits both endpoint events exactly once");
    playback = {0, 1, true};
    Check(playback.Advance(2, 1, AnimationWrap::PingPong, events).size() == 4,
          "ping pong events in both directions");
    playback = {0, 1, true};
    Check(playback.Advance(2, 1, AnimationWrap::Once, events).size() == 2 && !playback.playing &&
              playback.cursor == 1,
          "once stops at endpoint");
    playback = {0x1p54, 1, true};
    const std::vector<AnimationEvent> marker{{.5, "marker", ""}};
    Check(playback.Advance(.01, 1, AnimationWrap::Loop, marker).empty(),
          "a timeline increment smaller than double precision returns without iterating");
    Check(playback.Advance(4, 1, AnimationWrap::Loop, marker).size() == 4,
          "event traversal advances even when the absolute cycle index cannot increase by one");
    playback.speed = -1;
    Check(playback.Advance(4, 1, AnimationWrap::PingPong, marker).size() == 4,
          "large reverse ping-pong traversal retains events at distinct relative offsets");
}

void AnimationMotion(const std::filesystem::path& root) {
    std::string error;
    AnimationClip moving;
    moving.tracks = {Track(AnimationProperty::Position, 0, 10)};
    const auto path = (root / "moving.luxanim").string();
    Check(moving.Save(path, error), error.c_str());
    auto controller = CreateRef<AnimatorController>();
    AnimatorLayer layer;
    layer.entry = "Move";
    layer.states = {{"Move", path}, {"Other", path}};
    controller->layers = {layer};
    AnimatorPlayer player;
    Check(player.SetController(controller, error), error.c_str());
    player.Update(.1);
    Check(player.Play("Other", .5), "crossfade starts");
    Check(Near(player.Update(.1).rootDelta.value_or(Vec3{}).x, 1), "crossfade emits motion");
    player.SetPlaying(false);
    for (int i = 0; i < 2; ++i)
        Check(Near(player.Update(.1).rootDelta.value_or(Vec3{}).x, 0) && Near(player.GetTime(), .1),
              "paused animator and its outgoing state emit no root motion");
    player.SetPlaying(true);
    Check(Near(player.Update(.1).rootDelta.value_or(Vec3{}).x, 1),
          "resuming emits only new motion");

    Scene scene;
    auto entity = scene.CreateEntity("clip binding");
    auto& animation = entity.AddComponent<AnimationComponent>();
    animation.assetPath = path;
    animation.clip.tracks = {Track(AnimationProperty::Position, 42, 42)};
    scene.UpdateAnimations(.1);
    Check(Near(entity.GetComponent<TransformComponent>().position.x, 1),
          "external animation is active before unlinking");
    animation.assetPath.clear();
    scene.UpdateAnimations(.1);
    Check(Near(entity.GetComponent<TransformComponent>().position.x, 42) && !animation.asset &&
              animation.loadedPath.empty(),
          "unlinking a clip returns to the embedded animation");
}

void AnimationFileRoundTrip(const std::filesystem::path& root) {
    std::string error;
    AnimationClip clip;
    clip.name = "Original";
    const auto clipPath = (root / "round-trip.luxanim").string();
    Check(clip.Save(clipPath, error), error.c_str());
    const auto read = [&] {
        std::ifstream input(clipPath, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto originalBytes = read();
    clip.name = "Oversized";
    clip.events.assign(130, {.5, "Marker", std::string(65536, 'x')});
    Check(!clip.Save(clipPath, error) && error.find("8388608") != std::string::npos,
          "oversized clip is rejected by the on-disk reader's budget");
    const auto original = AnimationClip::Load(clipPath, error);
    Check(original && original->name == "Original" && read() == originalBytes,
          "saving an oversized clip leaves the existing asset readable");
    const auto scenePath = (root / "embedded-clip.lux").string();
    Scene authored;
    auto entity = authored.CreateEntity("Embedded clip");
    auto& embedded = entity.AddComponent<AnimationComponent>().clip;
    embedded = *original;
    Check(authored.Serialize(scenePath), "small embedded clip initially saves");
    embedded = std::move(clip);
    Scene reopened;
    Check(authored.Serialize(scenePath) && reopened.Deserialize(scenePath),
          "an embedded clip over 8 MiB can be reopened within the scene size limit");
    const auto& restored = reopened.GetComponent<AnimationComponent>(entity.GetId()).clip;
    Check(restored.events.size() == embedded.events.size() &&
              restored.events.back().payload == embedded.events.back().payload,
          "large embedded event payloads survive a scene round trip");

    auto controller = CreateRef<AnimatorController>();
    controller->parameters = {{"speed", AnimationParameterType::Float, 0}};
    AnimatorLayer layer;
    layer.entry = "Move";
    layer.states = {{"Move", "", 1, "speed", {{0, clipPath}}}};
    controller->layers = {layer};
    const auto controllerPath = (root / "round-trip.luxanimator").string();
    Check(controller->Save(controllerPath, error), error.c_str());
    controller = AnimatorController::Load(controllerPath, error);
    Check(bool(controller), error.c_str());
    const auto relocated = (root / "elsewhere/round-trip.luxanimator").string();
    for (const auto& destination : {controllerPath, relocated}) {
        Check(controller->Save(destination, error), error.c_str());
        auto saved = AnimatorController::Load(destination, error);
        AnimatorPlayer player;
        Check(saved && saved->layers.front().states.front().clip.empty() &&
                  player.SetController(saved, error),
              "saving a loaded blend tree preserves its empty primary clip and playable weights");
    }
}

void ReverseOneShotFromSavedScene(const std::filesystem::path& root) {
    AnimationClip clip;
    clip.duration = 1;
    clip.loop = false;
    clip.tracks = {Track(AnimationProperty::Position, 0, 10)};
    clip.events = {{.875, "reverse-marker", ""}};
    std::string error;
    const auto clipPath = (root / "reverse-once.luxanim").string();
    Check(clip.Save(clipPath, error), error.c_str());
    const auto scenePath = (root / "reverse-once.lux").string();
    for (const bool external : {false, true}) {
        Scene authored, runtime;
        auto entity = authored.CreateEntity("Reverse once");
        auto& animation = entity.AddComponent<AnimationComponent>();
        animation.speed = -1;
        if (external)
            animation.assetPath = clipPath;
        else
            animation.clip = clip;
        Check(authored.Serialize(scenePath) && runtime.Deserialize(scenePath),
              "reverse clip survives a scene round trip");
        auto saved = runtime.GetEntities().front();
        auto& restored = saved.GetComponent<AnimationComponent>();
        auto& transform = saved.GetComponent<TransformComponent>();
        runtime.UpdateAnimations(.25);
        Check(restored.playing && Near(restored.currentTime, .75) &&
                  Near(transform.position.x, 7.5) && restored.pendingEvents.size() == 1 &&
                  restored.pendingEvents.front().name == "reverse-marker",
              "a saved reverse one-shot starts at the end and crosses reverse events");
        runtime.UpdateAnimations(.75);
        runtime.UpdateAnimations(.25);
        Check(!restored.playing && Near(restored.currentTime, 0) && Near(transform.position.x, 0) &&
                  restored.pendingEvents.empty(),
              "the reverse one-shot stops at the first key and remains stopped");

        Scene positioned;
        Check(positioned.Deserialize(scenePath), "a second playback scene opens");
        auto sought = positioned.GetEntities().front();
        sought.GetComponent<AnimationComponent>().currentTime = .5;
        positioned.UpdateAnimations(.25);
        Check(Near(sought.GetComponent<AnimationComponent>().currentTime, .25),
              "reverse playback respects a manually positioned start time");
    }
}

void CrossfadeFromChildRootMotion(const std::filesystem::path& root) {
    AnimationClip walk, pose;
    walk.rootMotionTarget = "Motion";
    auto motion = Track(AnimationProperty::Position, 0, 2);
    motion.target = walk.rootMotionTarget;
    walk.tracks = {motion};
    auto normal = Track(AnimationProperty::Position, 7, 7);
    normal.target = motion.target;
    pose.tracks = {normal};
    const auto walkPath = (root / "crossfade-walk.luxanim").string();
    const auto posePath = (root / "crossfade-pose.luxanim").string();
    const auto controllerPath = (root / "crossfade-root.luxanimator").string();
    std::string error;
    Check(walk.Save(walkPath, error) && pose.Save(posePath, error), "crossfade assets save");
    AnimatorController controller;
    controller.parameters = {{"TakePose", AnimationParameterType::Trigger, 0}};
    AnimatorLayer layer;
    layer.entry = "Walk";
    layer.states = {{"Walk", walkPath}, {"Pose", posePath}};
    layer.transitions = {{"Walk", "Pose", .5, -1, {{"TakePose", AnimationComparison::Set, 0}}}};
    controller.layers = {layer};
    Check(controller.Save(controllerPath, error), error.c_str());

    for (const bool rootMotion : {false, true}) {
        Scene scene;
        auto owner = scene.CreateEntity("Owner");
        auto child = scene.CreateEntity("Motion");
        auto& transform = child.GetComponent<TransformComponent>();
        transform.position.x = 4;
        Check(scene.SetParent(child.GetId(), owner.GetId(), false), "root motion child is bound");
        auto& animator = owner.AddComponent<AnimatorComponent>();
        animator.controllerPath = controllerPath;
        animator.rootMotion = rootMotion;
        scene.UpdateAnimations(.125);
        Check(animator.player && animator.player->Trigger("TakePose"),
              "author triggers transition");
        Check(Near(transform.position.x, rootMotion ? 4 : .25),
              "the source pose uses the locally applied root motion mode");
        scene.UpdateAnimations(.25);
        Check(Near(transform.position.x, rootMotion ? 5.5 : 3.875),
              "halfway through the crossfade the ordinary pose is already blended");
        scene.UpdateAnimations(.25);
        Check(Near(transform.position.x, 7), "the final crossfade frame reaches the new pose");
        scene.UpdateAnimations(.01);
        Check(Near(transform.position.x, 7), "the first full frame has no pose jump");
    }
}

void RotatedChildRootMotion(const std::filesystem::path& root) {
    AnimationClip clip;
    clip.loop = false;
    clip.rootMotionTarget = "Rig/Motion";
    auto track = Track(AnimationProperty::Position, 0, 2);
    track.target = clip.rootMotionTarget;
    clip.tracks = {track};
    std::string error;
    const auto clipPath = (root / "rotated-root.luxanim").string();
    const auto controllerPath = (root / "rotated-root.luxanimator").string();
    Check(clip.Save(clipPath, error), error.c_str());
    AnimatorController controller;
    AnimatorLayer layer;
    layer.entry = "Move";
    layer.states = {{"Move", clipPath}};
    controller.layers = {layer};
    Check(controller.Save(controllerPath, error), error.c_str());

    for (const bool useAnimator : {false, true}) {
        for (const bool extract : {false, true}) {
            Scene scene;
            auto actor = scene.CreateEntity("Actor");
            auto rig = scene.CreateEntity("Rig");
            auto motion = scene.CreateEntity("Motion");
            auto& transform = rig.GetComponent<TransformComponent>();
            transform.rotation.z = ToRadians(90);
            transform.scale = {2, 3, 1};
            Check(scene.SetParent(rig.GetId(), actor.GetId(), false) &&
                      scene.SetParent(motion.GetId(), rig.GetId(), false),
                  "a rotated and scaled intermediate parent is bound");
            if (useAnimator) {
                auto& animator = actor.AddComponent<AnimatorComponent>();
                animator.controllerPath = controllerPath;
                animator.rootMotion = extract;
            } else {
                auto& animation = actor.AddComponent<AnimationComponent>();
                animation.clip = clip;
                animation.rootMotion = extract;
            }
            scene.UpdateAnimations(.5);
            const auto child = scene.GetWorldTransform(motion.GetId()).position;
            const auto owner = actor.GetComponent<TransformComponent>().position;
            Check(Near(child.x, 0) && Near(child.y, 2) && Near(owner.x, 0) &&
                      Near(owner.y, extract ? 2 : 0),
                  "extracting child motion preserves the authored world direction and scale");
            scene.DestroyEntity(motion);
            scene.UpdateAnimations(.25);
            const auto afterRemoval = actor.GetComponent<TransformComponent>().position;
            Check(Near(afterRemoval.x, owner.x) && Near(afterRemoval.y, owner.y),
                  "a missing child binding cannot move the owner");
        }
    }
}

void ClipStartEvents(const std::filesystem::path& root) {
    AnimationClip clip;
    clip.duration = 1;
    clip.loop = false;
    clip.events = {{0, "start", ""}, {.5, "midpoint", ""}, {1, "end", ""}};
    std::string error;
    const auto path = (root / "clip-start-events.luxanim").string();
    Check(clip.Save(path, error), error.c_str());
    for (const double speed : {1., -1.}) {
        Scene scene;
        auto actor = scene.CreateEntity("Actor");
        auto& animation = actor.AddComponent<AnimationComponent>();
        animation.assetPath = path;
        animation.speed = speed;
        scene.UpdateAnimations(0, false);
        scene.UpdateAnimations(.25);
        Check(animation.pendingEvents.size() == 1 &&
                  animation.pendingEvents.front().name == (speed > 0 ? "start" : "end"),
              "the first actual update emits the event at the starting endpoint");
        scene.UpdateAnimations(.25);
        Check(animation.pendingEvents.size() == 1 &&
                  animation.pendingEvents.front().name == "midpoint",
              "subsequent updates still deliver crossed events without a duplicate start");
    }
}

void ChildRootMotion(const std::filesystem::path& root) {
    AnimationClip clip;
    clip.loop = false;
    clip.rootMotionTarget = "Motion";
    auto motion = Track(AnimationProperty::Position, 4, 6);
    motion.target = clip.rootMotionTarget;
    auto pose = Track(AnimationProperty::Position, 0, 8);
    pose.target = "Pose";
    clip.tracks = {motion, pose};
    std::string error;
    const auto clipPath = (root / "child-root.luxanim").string();
    const auto controllerPath = (root / "child-root.luxanimator").string();
    Check(clip.Save(clipPath, error), error.c_str());
    AnimatorController controller;
    AnimatorLayer layer;
    layer.entry = "Move";
    layer.states = {{"Move", clipPath}};
    controller.layers = {layer};
    Check(controller.Save(controllerPath, error), error.c_str());

    for (const bool useAnimator : {false, true}) {
        for (const bool extractMotion : {false, true}) {
            Scene scene;
            auto owner = scene.CreateEntity("Owner");
            auto child = scene.CreateEntity("Motion");
            auto other = scene.CreateEntity("Pose");
            Check(scene.SetParent(child.GetId(), owner.GetId(), false) &&
                      scene.SetParent(other.GetId(), owner.GetId(), false),
                  "animation child bindings resolve");
            child.GetComponent<TransformComponent>().position.x = 4;
            if (useAnimator) {
                auto& animator = owner.AddComponent<AnimatorComponent>();
                animator.controllerPath = controllerPath;
                animator.rootMotion = extractMotion;
            } else {
                auto& animation = owner.AddComponent<AnimationComponent>();
                animation.assetPath = clipPath;
                animation.rootMotion = extractMotion;
            }
            for (const double elapsed : {.5, 1.}) {
                scene.UpdateAnimations(.5);
                const double displacement = elapsed * 2;
                Check(Near(scene.GetWorldTransform(owner.GetId()).position.x,
                           extractMotion ? displacement : 0) &&
                          Near(child.GetComponent<TransformComponent>().position.x,
                               extractMotion ? 4 : 4 + displacement) &&
                          Near(scene.GetWorldTransform(child.GetId()).position.x, 4 + displacement),
                      "child root position contributes to displacement exactly once");
                Check(Near(other.GetComponent<TransformComponent>().position.x, elapsed * 8),
                      "root extraction preserves other position tracks");
            }
        }
    }
}

void RootMotionWithoutCollider() {
    for (const bool planar : {false, true}) {
        Scene scene;
        auto owner = scene.CreateEntity("Unsimulated body");
        if (planar)
            owner.AddComponent<Rigidbody2DComponent>().type = BodyType::Dynamic;
        else
            owner.AddComponent<Rigidbody3DComponent>().type = BodyType::Dynamic;
        auto& animation = owner.AddComponent<AnimationComponent>();
        animation.rootMotion = true;
        animation.clip.loop = false;
        animation.clip.duration = .25;
        animation.clip.tracks = {Track(AnimationProperty::Position, 0, 2, .25)};
        scene.OnUpdate(.25);
        const auto position = [&] { return scene.GetWorldTransform(owner.GetId()).position.x; };
        const auto velocity = [&] {
            return planar ? owner.GetComponent<Rigidbody2DComponent>().velocity.x
                          : owner.GetComponent<Rigidbody3DComponent>().velocity.x;
        };
        Check(!animation.playing && Near(position(), 2) && Near(velocity(), 0),
              "root motion applies to the transform without storing unsimulated body velocity");
        if (planar)
            owner.AddComponent<BoxCollider2DComponent>();
        else
            owner.AddComponent<BoxCollider3DComponent>();
        scene.OnUpdate(.25);
        Check(Near(position(), 2, .001) && Near(velocity(), 0),
              "adding a collider after the clip ends does not replay root motion");
    }
}

void FixedStepRootMotion() {
    const auto simulate = [](bool planar, bool split) {
        Scene scene;
        auto settings = scene.GetSettings();
        settings.fixedStep = .02f;
        settings.overrideGravity = true;
        settings.gravity = {};
        settings.gravity3D = {};
        Check(scene.SetSettings(settings), "root motion physics settings");
        auto entity = scene.CreateEntity("animated body");
        if (planar) {
            entity.AddComponent<BoxCollider2DComponent>();
            entity.AddComponent<Rigidbody2DComponent>().type = BodyType::Dynamic;
        } else {
            entity.AddComponent<BoxCollider3DComponent>();
            entity.AddComponent<Rigidbody3DComponent>().type = BodyType::Dynamic;
        }
        auto& animation = entity.AddComponent<AnimationComponent>();
        animation.rootMotion = true;
        const double step = settings.fixedStep;
        auto& track =
            animation.clip.tracks.emplace_back(Track(AnimationProperty::Position, 0, 1, step / 2));
        track.SetKey({step, {1, 0, 0, 0}});
        if (split) {
            scene.OnUpdate(step / 2);
            Check(Near(entity.GetComponent<TransformComponent>().position.x, 0),
                  "half a physics step has not simulated");
            settings.exposure = 2;
            Check(scene.SetSettings(settings), "visual settings can change between physics steps");
            scene.OnUpdate(step / 2);
        } else
            scene.OnUpdate(step);
        Check(scene.GetLastPhysicsError3D().empty() && scene.GetLastPhysicsError2D().empty(),
              "root motion physics remains valid");
        return entity.GetComponent<TransformComponent>().position.x;
    };
    for (const bool planar : {false, true})
        Check(Near(simulate(planar, false), 1, .001) && Near(simulate(planar, true), 1, .001),
              "root motion survives render frames shorter than the physics step");

    for (const bool planar : {false, true}) {
        for (const bool uneven : {false, true}) {
            for (const bool paused : {false, true}) {
                Scene scene;
                auto settings = scene.GetSettings();
                settings.fixedStep = .02f;
                settings.physicsSubsteps = 2;
                settings.overrideGravity = true;
                settings.gravity = {};
                settings.gravity3D = {};
                Check(scene.SetSettings(settings), "fixed-step animation settings");
                const double step = settings.fixedStep;
                auto entity = scene.CreateEntity("Dynamic animation");
                if (planar) {
                    entity.AddComponent<BoxCollider2DComponent>();
                    auto& body = entity.AddComponent<Rigidbody2DComponent>();
                    body.type = BodyType::Dynamic;
                    body.velocity.x = 2;
                } else {
                    entity.AddComponent<BoxCollider3DComponent>();
                    auto& body = entity.AddComponent<Rigidbody3DComponent>();
                    body.type = BodyType::Dynamic;
                    body.velocity.x = 2;
                }
                auto& animation = entity.AddComponent<AnimationComponent>();
                animation.rootMotion = true;
                animation.clip.duration = paused ? 2 * step : step;
                animation.clip.loop = paused;
                const double first = uneven ? step * 1.5 : step;
                animation.clip.tracks = {
                    Track(AnimationProperty::Position, 0, 1, paused ? first : step)};
                scene.OnUpdate(first);
                if (paused)
                    animation.playing = false;
                scene.OnUpdate(2 * step - first);
                const auto& transform = entity.GetComponent<TransformComponent>();
                Check(Near(transform.position.x, 1 + 4 * step, .001),
                      "ending or pausing root motion preserves only the intended displacement");
                scene.OnUpdate(step);
                Check(Near(transform.position.x, 1 + 6 * step, .001),
                      "ordinary Rigidbody velocity survives the end of root motion");
            }
        }
    }

    Scene blocked;
    auto settings = blocked.GetSettings();
    settings.fixedStep = .02f;
    settings.overrideGravity = true;
    settings.gravity3D = {};
    Check(blocked.SetSettings(settings), "wall fixture physics settings");
    auto wall = blocked.CreateEntity("Wall");
    wall.GetComponent<TransformComponent>().position.x = 1.4;
    wall.AddComponent<BoxCollider3DComponent>();
    auto moving = blocked.CreateEntity("Animated body");
    moving.AddComponent<BoxCollider3DComponent>();
    auto& body = moving.AddComponent<Rigidbody3DComponent>();
    body.type = BodyType::Dynamic;
    auto& animation = moving.AddComponent<AnimationComponent>();
    animation.rootMotion = true;
    animation.clip.duration = settings.fixedStep;
    animation.clip.loop = false;
    animation.clip.tracks = {Track(AnimationProperty::Position, 0, 1, settings.fixedStep)};
    blocked.OnUpdate(settings.fixedStep);
    const auto contactX = moving.GetComponent<TransformComponent>().position.x;
    Check(contactX > 0 && contactX < .5 && std::abs(body.velocity.x) < .01,
          "Jolt blocks animation motion at a wall without creating a reverse velocity");
    blocked.OnUpdate(settings.fixedStep);
    Check(Near(moving.GetComponent<TransformComponent>().position.x, contactX, .01),
          "a wall contact does not push the body backward when animation stops");

    for (const bool planar : {false, true}) {
        Scene grounded;
        settings.gravity = {0, -10};
        settings.gravity3D = {0, -10, 0};
        Check(grounded.SetSettings(settings), "floor fixture physics settings");
        auto floor = grounded.CreateEntity("Floor");
        floor.GetComponent<TransformComponent>().position.y = -1;
        auto actor = grounded.CreateEntity("Grounded body");
        if (planar) {
            floor.AddComponent<BoxCollider2DComponent>().size = {20, 1};
            actor.AddComponent<BoxCollider2DComponent>();
            actor.AddComponent<Rigidbody2DComponent>().type = BodyType::Dynamic;
        } else {
            floor.AddComponent<BoxCollider3DComponent>().size = {20, 1, 10};
            actor.AddComponent<BoxCollider3DComponent>();
            actor.AddComponent<Rigidbody3DComponent>().type = BodyType::Dynamic;
        }
        for (int frame = 0; frame < 10; ++frame)
            grounded.OnUpdate(settings.fixedStep);
        auto& motion = actor.AddComponent<AnimationComponent>();
        motion = animation;
        motion.playing = true;
        motion.currentTime = 0;
        grounded.OnUpdate(settings.fixedStep);
        const double velocity = planar ? actor.GetComponent<Rigidbody2DComponent>().velocity.x
                                       : actor.GetComponent<Rigidbody3DComponent>().velocity.x;
        Check(actor.GetComponent<TransformComponent>().position.x > .9 && std::abs(velocity) < .002,
              "ground friction cannot turn animation velocity into a backward impulse");
    }
}

void AnimatorEventSources(const std::filesystem::path& root) {
    std::string error;
    auto first = Clip(0);
    first.loop = false;
    first.events = {{.25, "first", ""}, {1, "reverse entry", ""}};
    auto second = Clip(10);
    second.duration = 2;
    second.loop = false;
    second.events = {{.5, "second", ""}, {1.2, "second later", ""}};
    const auto a = (root / "event-source-a.luxanim").string();
    const auto b = (root / "event-source-b.luxanim").string();
    Check(first.Save(a, error) && second.Save(b, error), error.c_str());
    auto controller = CreateRef<AnimatorController>();
    controller->parameters = {{"blend", AnimationParameterType::Float, 1}};
    AnimatorLayer layer;
    layer.entry = "Tree";
    layer.states = {{"Tree", "", 1, "blend", {{0, a}, {1, b}}}, {"Reverse", a, -1}};
    controller->layers = {layer};
    AnimatorPlayer player;
    Check(player.SetController(controller, error), error.c_str());
    auto frame = player.Update(.3);
    Check(frame.events.size() == 1 && frame.events.front().name == "second",
          "the dominant clip emits events on its normalized timeline");
    Check(player.SetParameter("blend", 0) && player.Update(.3).events.empty(),
          "switching blend dominance does not replay events already passed");
    Check(player.SetParameter("blend", 1) && player.Play("Tree"), "tree restarts");
    frame = player.Update(.65);
    Check(frame.events.size() == 2 && frame.events.back().name == "second later",
          "all markers crossed on the selected clip are ordered");
    Check(player.Play("Tree") && player.SetSpeed(-1) && Near(player.GetTime(), 1),
          "negative speed set before first update also starts at the endpoint");
    player.Update(.25);
    Check(Near(player.GetTime(), .75) && player.SetSpeed(1) && Near(player.GetTime(), .75),
          "changing direction during playback preserves the current time");
    Check(player.Play("Reverse") && Near(player.GetTime(), 1),
          "one-shot reverse playback starts at the endpoint");
    frame = player.Update(.25);
    Check(Near(player.GetTime(), .75) && frame.events.size() == 1 &&
              frame.events.front().name == "reverse entry",
          "reverse playback advances without a seek and emits its starting marker");
    controller->layers.front().entry = "Reverse";
    Check(player.SetController(controller, error) && Near(player.GetTime(), 1),
          "a reverse entry state starts at the endpoint on controller load");
}

void AnimatedCameraClipRange() {
    Scene scene;
    auto entity = scene.CreateEntity("Short-range camera");
    auto& camera = entity.AddComponent<CameraComponent>();
    camera.nearClip = .00001;
    camera.farClip = .00005;
    AnimationFrame frame;
    frame.samples.push_back({{}, AnimationProperty::Float, "camera.nearClip", {.1, 0, 0, 0}});
    scene.ApplyAnimationFrame(entity.GetId(), frame, 0);
    Check(camera.nearClip > 0 && Near(camera.nearClip, camera.farClip * .5),
          "near clip animation preserves valid bounds for a short-range camera");
}

void AssetsAndAnimator(const std::filesystem::path& root) {
    std::string error;
    auto idle = Clip(0), run = Clip(10);
    run.events = {{.1, "run", ""}};
    const auto a = (root / "idle.luxanim").string(), b = (root / "run.luxanim").string();
    Check(idle.Save(a, error) && run.Save(b, error), error.c_str());
    auto reopened = AnimationClip::Load(a, error);
    Check(reopened && reopened->tracks.size() == 1, "independent clip reload");
    auto controller = CreateRef<AnimatorController>();
    controller->parameters = {{"speed", AnimationParameterType::Float, 0},
                              {"jump", AnimationParameterType::Trigger, 0}};
    AnimatorLayer layer;
    layer.entry = "Idle";
    layer.states = {{"Idle", a, 1}, {"Run", b, 1}};
    layer.transitions.push_back(
        {"Idle", "Run", .2, -1, {{"speed", AnimationComparison::Greater, .5}}});
    controller->layers = {layer};
    const auto path = (root / "character.luxanimator").string();
    Check(controller->Save(path, error), error.c_str());
    controller = AnimatorController::Load(path, error);
    Check(bool(controller), error.c_str());
    AnimatorPlayer player;
    Check(player.SetController(controller, error), error.c_str());
    Check(player.GetState() == "Idle", "entry state");
    Check(player.SetParameter("speed", 1), "typed parameter");
    auto pose = player.Update(.1);
    Check(player.GetState() == "Run" && Near(pose.samples.at(0).value[0], 5), "crossfade halfway");
    Check(pose.events.size() == 1, "incoming state event once");
    pose = player.Update(.1);
    Check(Near(pose.samples.at(0).value[0], 10), "crossfade completes");
    player.Seek(.5);
    Check(player.Update(0, false).events.empty(), "scrubbing never dispatches events");
    player.SetSpeed(-1);
    player.Update(.1);
    Check(Near(player.GetTime(), .4), "reverse animator playback");
    auto tree = CreateRef<AnimatorController>();
    tree->parameters = {{"speed", AnimationParameterType::Float, 1}};
    layer.transitions.clear();
    layer.states = {{"Blend", {}, 1, "speed", {{0, a}, {2, b}}}};
    layer.entry = "Blend";
    tree->layers = {layer};
    Check(player.SetController(tree, error), error.c_str());
    Check(Near(player.Update(0).samples.at(0).value[0], 5), "one dimensional blend tree");
    auto upper = layer;
    upper.name = "Upper";
    upper.weight = .5;
    upper.mask = {"Arm"};
    tree->layers.push_back(upper);
    Check(player.SetController(tree, error), error.c_str());
    Check(Near(player.Update(0).samples.at(0).value[0], 5), "layer mask excludes unbound root");
    controller->layers.front().transitions = {
        {"Idle", "Run", 0, -1, {{"jump", AnimationComparison::Set, 0}}}};
    Check(player.SetController(controller, error), error.c_str());
    Check(player.Trigger("jump"), "trigger parameter");
    player.Update(.01);
    Check(player.GetState() == "Run" && player.GetParameter("jump") == 0,
          "trigger consumed by transition");
    Scene source;
    auto entity = source.CreateEntity("Animated");
    auto& animator = entity.AddComponent<AnimatorComponent>();
    animator.controllerPath = path;
    animator.parameters = {{"speed", AnimationParameterType::Float, 1}};
    std::string data;
    Check(source.SerializeToString(data), "scene persists animator binding");
    Scene runtime;
    Check(runtime.DeserializeFromString(data), "scene reloads animator binding");
    runtime.OnUpdate(.1);
    Check(source.GetComponent<TransformComponent>(entity.GetId()).position.x == 0,
          "runtime clone preserves authored transform");
    Check(runtime.GetComponent<TransformComponent>(entity.GetId()).position.x > 0,
          "runtime applies controller pose");
    auto child = runtime.CreateEntity("Child");
    runtime.SetParent(child.GetId(), entity.GetId());
    child.AddComponent<SpriteRendererComponent>();
    AnimationFrame frame;
    frame.samples = {{"Child", AnimationProperty::Color, {}, {1, .5, .25, .75}},
                     {"Child", AnimationProperty::Opacity, {}, {.2, 0, 0, 0}},
                     {"Child", AnimationProperty::SpriteFrame, {}, {3, 0, 0, 0}}};
    frame.spriteColumns = 2;
    frame.spriteRows = 2;
    runtime.ApplyAnimationFrame(entity.GetId(), frame, 0);
    Check(child.GetComponent<SpriteRendererComponent>().frame == 3 &&
              std::abs(child.GetComponent<SpriteRendererComponent>().color.w - .2) < 1e-6,
          "descendant color alpha and sprite bindings");

    auto sparse = AnimationClip{};
    auto spriteClip = Clip(120);
    spriteClip.tracks.push_back(Track(AnimationProperty::SpriteFrame, 6, 6));
    spriteClip.spriteSheet = "sheet.png";
    spriteClip.spriteColumns = 4;
    spriteClip.spriteRows = 2;
    Check(sparse.Save(a, error) && spriteClip.Save(b, error), error.c_str());
    controller->layers.front().transitions = {
        {"Idle", "Run", 1, -1, {{"speed", AnimationComparison::Greater, .5}}}};
    Check(controller->Save(path, error), error.c_str());
    Scene sparseScene;
    auto sparseActor = sparseScene.CreateEntity("Sparse animator");
    sparseActor.GetComponent<TransformComponent>().position.x = 100;
    auto& sprite = sparseActor.AddComponent<SpriteRendererComponent>();
    sprite.texturePath = "authored.png";
    sparseActor.AddComponent<AnimatorComponent>().controllerPath = path;
    sparseScene.UpdateAnimations(0);
    auto& boundAnimator = sparseActor.GetComponent<AnimatorComponent>().player;
    Check(boundAnimator && boundAnimator->SetParameter("speed", 1), "sparse animator starts");
    sparseScene.UpdateAnimations(.3);
    Check(Near(sparseActor.GetComponent<TransformComponent>().position.x, 106) &&
              sprite.texturePath == "authored.png",
          "incoming-only channels blend from the scene and leave the previous sprite intact");
    sparseScene.UpdateAnimations(.3);
    Check(Near(sparseActor.GetComponent<TransformComponent>().position.x, 112) &&
              sprite.texturePath.ends_with("sheet.png") && sprite.frame == 6 &&
              sprite.columns == 4 && sprite.rows == 2,
          "crossfade holds the captured origin and switches sprite frame and sheet together");
}

void AnimatorLayerBoundaries(const std::filesystem::path& root) {
    std::string error;
    AnimationClip clip;
    clip.loop = false;
    clip.tracks = {Track(AnimationProperty::Position, 0, 10)};
    const auto motion = (root / "once-motion.luxanim").string();
    Check(clip.Save(motion, error), error.c_str());
    auto controller = CreateRef<AnimatorController>();
    AnimatorLayer layer;
    layer.entry = "Move";
    layer.states = {{"Move", motion}};
    layer.mask = {"Arm"};
    controller->layers = {layer};
    AnimatorPlayer player;
    Check(player.SetController(controller, error), error.c_str());
    auto masked = player.Update(.5);
    Check(masked.samples.empty() && !masked.rootDelta, "excluded root track emits no motion");
    controller->layers[0].mask.clear();
    Check(player.SetController(controller, error), error.c_str());
    Check(Near(player.Update(.5).rootDelta.value_or(Vec3{}).x, 5),
          "unrestricted root track emits its full motion");
    player.Update(.6);
    Check(Near(player.GetTime(), 1), "once clip reaches its endpoint");
    player.SetPlaying(false);
    player.Seek(.25);
    Check(Near(player.Update(.25).rootDelta.value_or(Vec3{}).x, 0) && Near(player.GetTime(), .25),
          "seeking a stopped layer cannot advance a paused player");
    player.SetPlaying(true);
    Check(Near(player.Update(.25).rootDelta.value_or(Vec3{}).x, 2.5) && Near(player.GetTime(), .5),
          "seeking into a completed once clip makes it playable again");

    Scene scene;
    auto owner = scene.CreateEntity();
    controller->layers.clear();
    for (const auto& name : {std::string("Left"), std::string("Right")}) {
        auto child = scene.CreateEntity(name);
        Check(scene.SetParent(child.GetId(), owner.GetId(), false), "sprite child is parented");
        child.AddComponent<SpriteRendererComponent>();
        AnimationClip sprite;
        sprite.spriteSheet = name + ".png";
        sprite.spriteColumns = name == "Left" ? 2 : 4;
        auto track = Track(AnimationProperty::SpriteFrame, 1, 1);
        track.target = name;
        sprite.tracks = {track};
        const auto file = (root / (name + ".luxanim")).string();
        Check(sprite.Save(file, error), error.c_str());
        layer.name = name;
        layer.mask = {name};
        layer.states = {{"Move", file}};
        controller->layers.push_back(layer);
    }
    Check(player.SetController(controller, error), error.c_str());
    scene.ApplyAnimationFrame(owner.GetId(), player.Update(.1), .1);
    for (const auto id : scene.GetChildren(owner.GetId())) {
        const auto& name = scene.GetComponent<TagComponent>(id).tag;
        const auto& sprite = scene.GetComponent<SpriteRendererComponent>(id);
        Check(sprite.texturePath.ends_with(name + ".png") && sprite.frame == 1 &&
                  sprite.columns == (name == "Left" ? 2u : 4u),
              "each masked sprite binding applies its own frame and sheet layout");
    }
}

void TypedDefaultsAndExitTimes(const std::filesystem::path& root) {
    std::string error;
    for (const auto wrap : {AnimationWrap::Once, AnimationWrap::Loop, AnimationWrap::PingPong}) {
        auto clip = Clip(0);
        clip.wrap = wrap;
        clip.loop = wrap != AnimationWrap::Once;
        const auto path = (root / "exit-time.luxanim").string();
        Check(clip.Save(path, error), error.c_str());
        auto controller = CreateRef<AnimatorController>();
        controller->parameters = {{"Lives", AnimationParameterType::Integer, 2.75},
                                  {"Enabled", AnimationParameterType::Boolean, 7},
                                  {"Fire", AnimationParameterType::Trigger, -1}};
        AnimatorLayer layer;
        layer.entry = "Start";
        layer.states = {{"Start", path}, {"Done", path}};
        layer.transitions = {{"Start",
                              "Done",
                              0,
                              .5,
                              {{"Lives", AnimationComparison::Equal, 2},
                               {"Enabled", AnimationComparison::Equal, 1}}}};
        controller->layers = {layer};
        for (const double speed : {1., -1.}) {
            controller->layers.front().states.front().speed = speed;
            AnimatorPlayer player;
            Check(player.SetController(controller, error), error.c_str());
            Check(player.GetParameter("Lives") == 2 && player.GetParameter("Enabled") == 1 &&
                      player.GetParameter("Fire") == 1,
                  "controller defaults are normalized like explicitly set values");
            player.Update(.25);
            player.Update(.25);
            Check(player.GetState() == "Start", "Exit Time must not fire before the threshold");
            player.Update(.125);
            Check(player.GetState() == "Done",
                  "Exit Time advances in either direction for each wrap mode");
        }
    }
}

void MutedEvents(const std::filesystem::path& root) {
    auto clip = Clip(0);
    clip.events = {{0, "Start", ""}, {.1, "Later", ""}};
    std::string error;
    const auto path = (root / "event-boundary.luxanim").string();
    Check(clip.Save(path, error), error.c_str());
    for (const double weight : {0., .000001, 1.})
        for (const double dt : {0., 1. / 60, .1, .2})
            for (const bool emit : {false, true}) {
                auto controller = CreateRef<AnimatorController>();
                AnimatorLayer layer;
                layer.weight = weight;
                layer.entry = "Boundary";
                layer.states.push_back({"Boundary", path});
                controller->layers.push_back(layer);
                AnimatorPlayer player;
                Check(player.SetController(controller, error), error.c_str());
                const auto frame = player.Update(dt, emit);
                const size_t expected = emit && weight > 0 ? (dt >= .1 ? 2u : 1u) : 0u;
                Check(frame.events.size() == expected,
                      "start and interval events obey the same layer weight and emit policy");
                Check(player.Update(0, emit).events.empty(), "start event is not repeated");
                if (!emit) {
                    const auto resumed = player.Update(0, true);
                    const size_t expectedStart = dt == 0 && weight > 0 ? 1 : 0;
                    Check(resumed.events.size() == expectedStart &&
                              (resumed.events.empty() || resumed.events.front().name == "Start"),
                          "silent progress cannot replay an event when emission resumes");
                }
            }
}

void MutedFastLoop() {
    Scene scene;
    auto& animation = scene.CreateEntity("Fast animation").AddComponent<AnimationComponent>();
    animation.clip.duration = .001;
    animation.clip.framesPerSecond = 1000;
    animation.clip.events = {{.0005, "Marker", ""}};
    animation.speed = 100;
    scene.UpdateAnimations(.05, false);
    Check(Near(animation.currentTime, 5) && animation.pendingEvents.empty(),
          "disabled events do not traverse loop repetitions");
}

void FadeToLowerLayer(const std::filesystem::path& root) {
    std::string error;
    const auto lower = (root / "fade-lower.luxanim").string();
    const auto upper = (root / "fade-upper.luxanim").string();
    const auto unkeyed = (root / "fade-unkeyed.luxanim").string();
    Check(Clip(0).Save(lower, error) && Clip(10).Save(upper, error) &&
              AnimationClip{}.Save(unkeyed, error),
          error.c_str());
    AnimatorLayer base;
    base.entry = "Ground";
    base.states = {{"Ground", lower}};
    AnimatorLayer overlay;
    overlay.entry = "Raised";
    overlay.states = {{"Raised", upper}, {"Unkeyed", unkeyed}};
    auto controller = CreateRef<AnimatorController>();
    controller->layers = {base, overlay};
    AnimatorPlayer player;
    Check(player.SetController(controller, error), error.c_str());
    const auto position = [&](double dt) {
        const auto frame = player.Update(dt);
        Check(frame.samples.size() == 1, "both layers resolve to one position");
        return frame.samples.front().value[0];
    };
    Check(Near(position(0), 10) && player.Play("Unkeyed", 1, 1), "start the upper layer fade");
    Check(Near(position(.5), 5) && Near(position(.5), 0) && Near(position(.01), 0),
          "an outgoing track fades to the lower layer and ends without a jump");
}

void FbxReverseOnce() {
    std::string error;
    const auto model = ModelAsset::LoadFbx(LUX_FBX_ANIMATED_FIXTURE, error);
    Check(model && !model->animations.empty(), "animated fixture has a clip");
    const auto duration = model->animations.front().duration;
    Scene scene;
    auto& modelPlayback =
        scene.CreateEntity("Reversed FBX").AddComponent<ModelAnimationComponent>();
    modelPlayback.modelPath = LUX_FBX_ANIMATED_FIXTURE;
    modelPlayback.clip = 0;
    modelPlayback.speed = -1;
    modelPlayback.playing = true;
    modelPlayback.loop = false;
    scene.OnUpdate(duration / 4);
    Check(modelPlayback.playing && Near(modelPlayback.time, duration * .75),
          "reverse one-shot FBX starts at the end");
    scene.OnUpdate(duration * .75);
    Check(!modelPlayback.playing && Near(modelPlayback.time, 0),
          "reverse one-shot FBX stops at zero");
    scene.OnUpdate(duration / 4);
    Check(!modelPlayback.playing && Near(modelPlayback.time, 0),
          "the finished reverse clip does not restart during an update");
}

void ExplicitFbxRoot(const std::filesystem::path& root) {
    std::ifstream source(LUX_FBX_ANIMATED_FIXTURE, std::ios::binary);
    std::string model(std::istreambuf_iterator<char>(source), {});
    const std::string boneConnection = "C:\"OP\",6003,3001,\"Lcl Translation\"";
    const auto connection = model.find(boneConnection);
    Check(connection != std::string::npos,
          "the authored FBX contains the expected animation channel");
    model.replace(connection, boneConnection.size(), "C:\"OP\",6003,2001,\"Lcl Translation\"");
    const auto path = (root / "mesh-root-motion.fbx").string();
    std::ofstream(path, std::ios::binary) << model;
    for (const bool valid : {false, true}) {
        Scene scene;
        auto owner = scene.CreateEntity("Root motion");
        auto& animation = owner.AddComponent<AnimationComponent>();
        animation.clip.loop = false;
        animation.clip.modelPath = path;
        animation.clip.modelClip = 0;
        animation.clip.rootMotionTarget = valid ? "Triangle" : "MisspelledRoot";
        animation.rootMotion = true;
        scene.UpdateAnimations(.5);
        Check(Near(owner.GetComponent<TransformComponent>().position.x, valid ? 1 : 0),
              "only a valid explicit FBX root can transfer its motion to the owner");
    }
}

void SkeletalBlend() {
    std::string error;
    auto model = ModelAsset::LoadFbx(LUX_FBX_ANIMATED_FIXTURE, error);
    Check(bool(model), error.c_str());
    ModelPose a, b, mixed;
    Check(model->Evaluate(0, 0, a, error) && model->Evaluate(.5, 0, b, error), error.c_str());
    Check(model->MixPoses({{&b, 1}}, mixed, error), error.c_str());
    for (size_t n = 0; n < model->nodes.size(); ++n)
        Check((mixed.nodeLocalTransforms[n].position - b.nodeLocalTransforms[n].position)
                      .Dot(mixed.nodeLocalTransforms[n].position -
                           b.nodeLocalTransforms[n].position) < 1e-8,
              "skeletal endpoint pose");
    Check(model->MixPoses({{&a, .5}, {&b, .5}}, mixed, error), error.c_str());
    for (const auto& mesh : mixed.submeshes)
        for (const auto& vertex : mesh)
            Check(std::isfinite(vertex.position.x) && std::isfinite(vertex.normal.x),
                  "blended skin vertices remain finite");
}

void World64() {
    static_assert(std::is_same_v<decltype(Vec3{}.x), double>);
    static_assert(sizeof(Mat4::elements) == 16 * sizeof(double));
    const double base = 1e12;
    const Vec3 a{base + .125, base + .25, base + .5};
    Check(Near((a - Vec3{base, base, base}).x, .125), "world vector preserves subunit offsets");
    const auto matrix = Mat4::Translate(a);
    Check(Near((matrix.InverseAffine() * matrix).elements[12], 0), "double matrix inverse");
    Scene scene;
    auto parent = scene.CreateEntity("Parent"), child = scene.CreateEntity("Child");
    parent.GetComponent<TransformComponent>().position = a;
    child.GetComponent<TransformComponent>().position = {.125, 0, 0};
    Check(scene.SetParent(child.GetId(), parent.GetId(), false), "large world hierarchy");
    Check(Near(scene.GetWorldTransform(child.GetId()).position.x - base, .25),
          "hierarchy translation precision");
    std::string data;
    Check(scene.SerializeToString(data), "large scene save");
    Scene restored;
    Check(restored.DeserializeFromString(data), "large scene load");
    Check(restored.GetWorldTransform(child.GetId()).position.x ==
              scene.GetWorldTransform(child.GetId()).position.x,
          "JSON preserves 64 bit coordinate");
    PhysicsWorld physics;
    PhysicsBodyDescription body;
    body.entity = 1;
    body.position = {base + .125, 0, 0};
    body.type = BodyType::Dynamic;
    body.velocity = {.125, 0, 0};
    Check(physics.Synchronize(std::span(&body, 1)), "Jolt accepts double world positions");
    physics.Step(.125, {});
    auto state = physics.GetState(1);
    Check(state && Near(state->position.x - base, .140625, .001),
          "Jolt keeps fractional motion at a trillion units");
}
} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path() / "lux-animation-focused-tests";
        std::filesystem::create_directories(root);
        Curves();
        AnimationMotion(root);
        AnimationFileRoundTrip(root);
        ReverseOneShotFromSavedScene(root);
        CrossfadeFromChildRootMotion(root);
        RotatedChildRootMotion(root);
        ClipStartEvents(root);
        ChildRootMotion(root);
        RootMotionWithoutCollider();
        FixedStepRootMotion();
        AnimatorEventSources(root);
        AnimatedCameraClipRange();
        AnimatorLayerBoundaries(root);
        AssetsAndAnimator(root);
        MutedEvents(root);
        MutedFastLoop();
        TypedDefaultsAndExitTimes(root);
        FadeToLowerLayer(root);
        SkeletalBlend();
        FbxReverseOnce();
        ExplicitFbxRoot(root);
        World64();
        std::filesystem::remove_all(root);
        std::cout << "Animation curves/assets/events/animator/scene/world64 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
