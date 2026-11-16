#include <LuxEngine/Animation/Animator.hpp>
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Physics/PhysicsWorld.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <cmath>
#include <filesystem>
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
        AssetsAndAnimator(root);
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
