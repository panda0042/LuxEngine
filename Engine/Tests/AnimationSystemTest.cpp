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

void MutedEvents(const std::filesystem::path& root) {
    auto clip = Clip(0);
    clip.events = {{0, "Start", ""}, {.1, "Later", ""}};
    std::string error;
    const auto path = (root / "event-boundary.luxanim").string();
    Check(clip.Save(path, error), error.c_str());
    for (const double weight : {0., .000001, 1.})
        for (const double dt : {0., 1. / 60, .2})
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
        FixedStepRootMotion();
        AnimatorEventSources(root);
        AnimatedCameraClipRange();
        AnimatorLayerBoundaries(root);
        AssetsAndAnimator(root);
        MutedEvents(root);
        SkeletalBlend();
        World64();
        std::filesystem::remove_all(root);
        std::cout << "Animation curves/assets/events/animator/scene/world64 passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
