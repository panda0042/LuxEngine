#include <LuxEngine/Platform/Input.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <LuxEngine/Scene/SceneManager.hpp>
#include <LuxEngine/Scripting/ScriptRuntime.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace LuxEngine;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void CheckRuntime(bool success, const ScriptRuntime& runtime) {
    if (!success)
        throw std::runtime_error(runtime.GetLastError());
}

void TestStreamingAndAnimatedFields(const ScriptRuntimeConfig& config,
                                    const std::filesystem::path& directory) {
    Scene region;
    region.CreateEntity("Region script").AddComponent<ScriptComponent>() = {
        "ScriptTests.LiveFields", true};
    const auto path = directory / "region.lux";
    Check(region.Serialize(path.string()), "Stream region did not save");
    Scene scene;
    SceneManager manager;
    std::string error;
    const auto root = manager.AddSceneInstance(scene, path, SceneReferenceMode::Additive, error);
    Check(root && manager.ConfigureStreaming(scene, root, {0, 0}, 5, 8, error),
          "Stream region could not be configured");

    auto actor = scene.CreateEntity("Persistent script");
    const auto actorId = actor.GetId();

    struct RuntimeState {
        int value;
    };

    actor.AddComponent<RuntimeState>(42);
    auto& fields = actor.AddComponent<ScriptComponent>("ScriptTests.LiveFields", true).fields;
    fields = {{"Amount", 3.0f}, {"Other", 7.0f}};
    auto& clip = actor.AddComponent<AnimationComponent>().clip;
    AnimationTrack track;
    track.property = AnimationProperty::Float;
    track.field = "Amount";
    track.SetKey({0, {3, 0, 0, 0}});
    track.SetKey({1, {13, 0, 0, 0}});
    clip.tracks.push_back(track);

    ScriptRuntime runtime;
    CheckRuntime(runtime.Initialize(scene, config), runtime);
    CheckRuntime(runtime.Update(.1), runtime);
    scene.OnUpdate(.1);
    CheckRuntime(runtime.Update(.1), runtime);
    const auto live = actor.GetComponent<TransformComponent>();
    Check(live.position.x == 2 && live.position.y == 1 && live.position.z == 0 &&
              live.scale.x == 4 && live.scale.y == 9,
          "Animating a C# field must retain the instance and unrelated field values");

    fields.erase("Amount");
    CheckRuntime(runtime.Update(.1), runtime);
    Check(actor.GetComponent<TransformComponent>().scale.x == 2,
          "Removing an authored value restores the instance's field default");

    const auto& initialMap = scene.GetComponent<SceneReferenceComponent>(root).entityMap;
    Check(initialMap.size() == 1 &&
              scene.GetComponent<TransformComponent>(initialMap.front().second).position.y == 1,
          "The original region script starts before unloading");
    const auto lifetime = scene.Lifetime();
    bool changed = false;
    Check(manager.UpdateStreaming(scene, {20, 0}, error, &changed) && changed &&
              !lifetime.expired() && actor.IsValid(),
          "Unloading must preserve handles outside the region");
    CheckRuntime(runtime.Update(.1), runtime);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        Check(manager.UpdateStreaming(scene, {0, 0}, error, &changed),
              "Stream region could not reload");
        if (changed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    Check(changed && !lifetime.expired() && actor.IsValid(), "Streaming load replaced the scene");
    CheckRuntime(runtime.Update(.1), runtime);
    const auto persistent = actor.GetComponent<TransformComponent>().position;
    Check(persistent.x == 5 && persistent.y == 1 && persistent.z == 0 &&
              actor.GetComponent<RuntimeState>().value == 42,
          "Persistent script must continue running across streaming");
    const auto& map = scene.GetComponent<SceneReferenceComponent>(root).entityMap;
    Check(map.size() == 1 && map.front().second != actorId,
          "Reloaded region has a distinct script entity");
    const auto streamed = scene.GetComponent<TransformComponent>(map.front().second).position;
    Check(streamed.x == 1 && streamed.y == 2,
          "A newly streamed script must start in the existing runtime");
}

void TestScriptAnimatorPlay(const ScriptRuntimeConfig& config, const std::string& controller) {
    Scene scene;
    auto rejected = scene.CreateEntity("Rejected state");
    auto control = scene.CreateEntity("Paused control");
    auto accepted = scene.CreateEntity("Accepted state");
    for (auto actor : {rejected, control, accepted}) {
        actor.AddComponent<AnimatorComponent>().controllerPath = controller;
        actor.AddComponent<ScriptComponent>() = {"ScriptTests.AnimatorPlayProbe", true};
    }
    rejected.GetComponent<ScriptComponent>().fields["State"] = std::string("No such state");
    accepted.GetComponent<ScriptComponent>().fields["State"] = std::string("Run");
    ScriptRuntime runtime;
    CheckRuntime(runtime.Initialize(scene, config), runtime);
    CheckRuntime(runtime.PrepareInstances(), runtime);
    Check(rejected.GetComponent<TransformComponent>().position.z == -1 &&
              accepted.GetComponent<TransformComponent>().position.z == 1,
          "C# Play returns false for an invalid state and true for a valid state");
    scene.UpdateAnimations(.25);
    const auto& invalid = rejected.GetComponent<AnimatorComponent>();
    const auto& paused = control.GetComponent<AnimatorComponent>();
    const auto& running = accepted.GetComponent<AnimatorComponent>();
    Check(!invalid.playing && invalid.player->GetTime() == 0 && !paused.playing &&
              paused.player->GetTime() == 0 && running.playing && running.player->GetTime() == .25,
          "a rejected Play must preserve the previously paused Animator");
}

void TestControllerSwitchBeforeScriptUpdate(const ScriptRuntimeConfig& config,
                                            const std::string& controller,
                                            const std::filesystem::path& directory) {
    std::string error;
    auto replacement = AnimatorController::Load(controller, error);
    Check(bool(replacement), "load the controller fixture");
    auto& layer = replacement->layers.front();
    layer.states[1].name = "NewMove";
    layer.transitions.clear();
    const auto next = (directory / "replacement.luxanimator").string();
    Check(replacement->Save(next, error), error.c_str());
    Scene scene;
    auto actor = scene.CreateEntity("Controller switch");
    auto& animator = actor.AddComponent<AnimatorComponent>();
    animator.controllerPath = controller;
    actor.AddComponent<ScriptComponent>() = {"ScriptTests.ControllerSwitchProbe", true};
    scene.UpdateAnimations(.1);
    Check(animator.player && animator.player->GetState() == "Idle",
          "original controller is active");
    ScriptRuntime runtime;
    CheckRuntime(runtime.Initialize(scene, config), runtime);
    animator.controllerPath = next;
    CheckRuntime(runtime.Update(.1), runtime);
    Check(actor.GetComponent<TransformComponent>().position.z == 1 &&
              animator.player->GetState() == "NewMove",
          "C# Play uses the newly assigned controller before the scene's next animation update");
    scene.UpdateAnimations(.1);
    Check(animator.player->GetState() == "NewMove",
          "the subsequent animation update keeps the state selected from C#");
}

void TestScriptAnimationQueues(const ScriptRuntimeConfig& config, const std::string& controller) {
    Scene scene;
    auto plain = scene.CreateEntity("Clip only");
    auto paused = scene.CreateEntity("Clip with paused Animator");
    auto active = scene.CreateEntity("Clip with active Animator");
    for (auto actor : {plain, paused, active}) {
        auto& clip = actor.AddComponent<AnimationComponent>().clip;
        clip.loop = false;
        clip.events = {{0, "clip", ""}, {.125, "clip", ""}};
        actor.AddComponent<ScriptComponent>() = {"ScriptTests.AnimationQueueProbe", true};
    }
    auto& pausedAnimator = paused.AddComponent<AnimatorComponent>();
    pausedAnimator.controllerPath = controller;
    pausedAnimator.playing = false;
    active.AddComponent<AnimatorComponent>().controllerPath = controller;

    ScriptRuntime runtime;
    CheckRuntime(runtime.Initialize(scene, config), runtime);
    scene.UpdateAnimations(.25);
    CheckRuntime(runtime.Update(.25), runtime);
    const auto count = [](Entity actor) {
        return actor.GetComponent<TransformComponent>().position;
    };
    Check(count(plain).x == 2 && count(plain).y == 0 && count(paused).x == 2 &&
              count(paused).y == 0 && count(active).x == 2 && count(active).y == 1 &&
              active.GetComponent<AnimationComponent>().pendingEvents.empty() &&
              active.GetComponent<AnimatorComponent>().pendingEvents.empty(),
          "C# drains both animation queues even when an Animator is present");
}

void TestFirstGame(const char* assemblyPath, const char* scenePath) {
    Scene scene;
    Check(scene.Deserialize(scenePath), "First game scene did not load");
    const auto scripts = scene.GetComponentEntityIds<ScriptComponent>();
    Check(scripts.size() == 1, "First game scene must contain one script");
    const auto player = scripts.front();
    const auto& fields = scene.GetComponent<ScriptComponent>(player).fields;
    const auto beacon =
        scene.ResolveScriptEntityReference(std::get<ScriptEntityReference>(fields.at("Beacon")));
    const auto timer =
        scene.ResolveScriptEntityReference(std::get<ScriptEntityReference>(fields.at("Timer")));
    auto& playerTransform = scene.GetComponent<TransformComponent>(player);
    const auto& beaconTransform = scene.GetComponent<TransformComponent>(beacon);
    const auto& playerSprite = scene.GetComponent<SpriteRendererComponent>(player);
    const auto& beaconSprite = scene.GetComponent<SpriteRendererComponent>(beacon);
    const auto& bar = scene.GetComponent<TransformComponent>(timer);

    ScriptRuntime runtime;
    ScriptRuntimeConfig config;
    config.assemblyPath = assemblyPath;
    CheckRuntime(runtime.Initialize(scene, config), runtime);
    CheckRuntime(runtime.PrepareInstances(), runtime);
    const auto fullWidth = bar.scale.x;
    const auto startX = playerTransform.position.x;
    Input::SetExternalKey(LuxKey::D, true);
    const bool moved = runtime.Update(.125);
    Input::SetExternalKey(LuxKey::D, false);
    CheckRuntime(moved, runtime);
    Check(playerTransform.position.x > startX && bar.scale.x < fullWidth,
          "First game input or countdown did not reach the scene");

    const auto beaconX = beaconTransform.position.x;
    for (int i = 0; i < 3; ++i) {
        playerTransform.position = beaconTransform.position;
        CheckRuntime(runtime.Update(.125), runtime);
        if (i == 0)
            Check(beaconTransform.position.x != beaconX, "First game beacon did not move");
    }
    Check(playerSprite.color.y == 1 && beaconSprite.color.w == 0,
          "First game win did not update the sprite colors");

    Input::SetExternalKey(LuxKey::Space, true);
    const bool restarted = runtime.Update(.125);
    Input::SetExternalKey(LuxKey::Space, false);
    CheckRuntime(restarted, runtime);
    Check(playerTransform.position.x == startX && bar.scale.x == fullWidth &&
              beaconSprite.color.w == 1,
          "First game restart did not restore the round");
    for (int frame = 0; frame < 80; ++frame)
        CheckRuntime(runtime.Update(.125), runtime);
    Check(playerSprite.color.x == 1 && bar.scale.x == 0,
          "First game timeout did not update the sprite and timer");
}

} // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 4, "Script test assembly, sample assembly and sample scene are required");
        Scene scene;
        auto entity = scene.CreateEntity("C# regression");
        entity.AddComponent<ScriptComponent>() = {"ScriptTests.Mover", true};
        std::string serialized;
        Check(scene.SerializeToString(serialized), "Script component serialization failed");
        Scene restored;
        Check(restored.DeserializeFromString(serialized), "Script component restore failed");
        Check(restored.GetComponentEntityIds<ScriptComponent>().size() == 1,
              "Script component was lost during serialization");

        struct CustomComponent {
            int value;
        };

        entity.AddComponent<CustomComponent>(42);
        Check(entity.GetComponent<CustomComponent>().value == 42,
              "Custom ECS types were restricted");
        scene.CreateEntity("Unassigned script").AddComponent<ScriptComponent>();
        ScriptRuntime runtime;
        ScriptRuntimeConfig config;
        config.assemblyPath = argv[1];
        CheckRuntime(runtime.Initialize(scene, config), runtime);
        CheckRuntime(runtime.Update(.25), runtime);
        auto& transform = entity.GetComponent<TransformComponent>();
        Check(std::abs(transform.position.x - .25f) < .0001f && transform.position.y == 10,
              "Managed lifecycle or native transform bridge failed");
        bool accepted = true;
        std::thread worker([&] { accepted = runtime.Update(.25); });
        worker.join();
        Check(!accepted && transform.position.x == .25f, "Off-thread scene access was accepted");
        entity.GetComponent<ScriptComponent>().enabled = false;
        CheckRuntime(runtime.Update(.25), runtime);
        Check(transform.position.z == 1, "Disabled script was not destroyed");
        entity.GetComponent<ScriptComponent>() = {"ScriptTests.Throwing", true};
        Check(!runtime.Update(.25) &&
                  runtime.GetLastError().find("managed regression exception") != std::string::npos,
              "Managed exception was not contained and reported");
        runtime.Shutdown();
        Check(!runtime.IsRunning(), "Script world was not stopped");
        entity.GetComponent<ScriptComponent>() = {"ScriptTests.Mover", true};
        Check(runtime.Initialize(scene, config) && runtime.Update(.25),
              "Script world did not reload");
        scene.DestroyEntity(entity);
        Check(runtime.Update(.25), "Destroyed entity was retained by a script");
        runtime.Shutdown();
        {
            Scene temporary;
            Check(runtime.Initialize(temporary, config), "Temporary world failed to start");
        }
        Check(!runtime.Update(.25) && !runtime.IsRunning(),
              "Expired scene lifetime was not rejected");
        Scene authored;
        auto target = authored.CreateEntity("Target");
        auto scripted = authored.CreateEntity("Fields");
        ScriptComponent component{"ScriptTests.Fields", true};
        component.fields = {
            {"Health", int32{7}},      {"Speed", 3.5f},
            {"Enabled", true},         {"Title", std::string("필드 회귀")},
            {"Offset", Vec3{1, 2, 3}}, {"Tint", Vec4{1, .5f, .25f, 1}},
            {"window", Vec2{8, 9}},    {"Target", ScriptEntityReference{target.GetId()}}};
        scripted.AddComponent<ScriptComponent>(component);
        std::string fieldScene;
        Check(authored.SerializeToString(fieldScene), "Script fields did not serialize");
        Scene restoredFields;
        Check(restoredFields.DeserializeFromString(fieldScene),
              "Script fields did not deserialize");
        CheckRuntime(runtime.Initialize(restoredFields, config), runtime);
        std::vector<ScriptFieldDescription> descriptions;
        CheckRuntime(runtime.DescribeFields("ScriptTests.Fields", descriptions), runtime);
        Check(descriptions.size() == 8, "Public or attributed private fields were not reflected");
        std::vector<ScriptFieldDescription> single;
        CheckRuntime(runtime.DescribeFields("ScriptTests.MetadataOnce", single), runtime);
        Check(single.size() == 1 && std::get<std::string>(single.front().defaultValue) == "x",
              "Metadata inspection must construct the script only once");
        Check(!runtime.DescribeFields("ScriptTests.LargeMetadata", single) &&
                  runtime.GetLastError().find("65536-byte UTF-8 limit") != std::string::npos &&
                  single.size() == 1 && std::get<std::string>(single.front().defaultValue) == "x",
              "Oversized metadata must report its limit and preserve prior metadata");
        Check(!runtime.DescribeFields("ScriptTests.UnsupportedField", descriptions) &&
                  runtime.GetLastError().find("Unsupported serialized script field") !=
                      std::string::npos,
              "Unsupported field type was not rejected");
        CheckRuntime(runtime.Update(0), runtime);
        const auto fieldTransform =
            restoredFields.GetComponent<TransformComponent>(scripted.GetId());
        Check(fieldTransform.position.x == 7 && fieldTransform.position.y == 3.5f &&
                  fieldTransform.position.z == static_cast<float>(target.GetId()),
              "Authored fields or entity reference were not applied before creation");
        runtime.Shutdown();
        Scene imported;
        imported.CreateEntity("Existing");
        const auto mapping = imported.ImportEntities(authored, {});
        Check(mapping.size() == 2 &&
                  std::get<ScriptEntityReference>(
                      imported.GetComponent<ScriptComponent>(mapping.at(scripted.GetId()))
                          .fields.at("Target"))
                          .id == mapping.at(target.GetId()),
              "Imported script entity references were not remapped");
        auto& importedFields =
            imported.GetComponent<ScriptComponent>(mapping.at(scripted.GetId())).fields;
        importedFields["Health"] = std::string("wrong type");
        CheckRuntime(runtime.Initialize(imported, config), runtime);
        Check(!runtime.Update(0) &&
                  runtime.GetLastError().find("type changed") != std::string::npos,
              "A mismatched authored field type reached OnCreate");
        runtime.Shutdown();
        ScriptFieldMap invalidFields;
        Check(!DeserializeScriptFields("{\"bad\":{\"type\":4294967296,\"value\":true}}",
                                       invalidFields),
              "Overflowed field kind was accepted");
        std::string invalidOutput;
        Check(!SerializeScriptFields({{"bad", std::numeric_limits<float>::infinity()}},
                                     invalidOutput),
              "Non-finite authored value was accepted");
        {
            const auto root = std::filesystem::temp_directory_path() / "lux-animation-script-tests";
            std::filesystem::create_directories(root);
            std::string error;
            AnimationClip clip;
            clip.events = {{.1, "step", "right"}};
            const auto clipPath = (root / "move.luxanim").string();
            Check(clip.Save(clipPath, error), error.c_str());
            AnimatorController controller;
            controller.parameters = {{"speed", AnimationParameterType::Float, 0},
                                     {"jump", AnimationParameterType::Trigger, 0}};
            AnimatorLayer layer;
            layer.entry = "Idle";
            layer.states = {{"Idle", clipPath, 1}, {"Run", clipPath, 1}};
            layer.transitions = {{"Idle", "Run", 0, -1, {{"jump", AnimationComparison::Set, 0}}}};
            controller.layers = {layer};
            const auto controllerPath = (root / "move.luxanimator").string();
            Check(controller.Save(controllerPath, error), error.c_str());
            Scene animated;
            auto character = animated.CreateEntity("C# animation");
            character.GetComponent<TransformComponent>().position.x = 1e12 + .125;
            character.AddComponent<AnimatorComponent>().controllerPath = controllerPath;
            character.AddComponent<ScriptComponent>() = {"ScriptTests.AnimationDriver", true};
            CheckRuntime(runtime.Initialize(animated, config), runtime);
            CheckRuntime(runtime.Update(0), runtime);
            Check(character.GetComponent<TransformComponent>().position.x == 1e12 + .25,
                  "Managed double transform roundtrip");
            animated.OnUpdate(.2);
            CheckRuntime(runtime.Update(0), runtime);
            const auto& animator = character.GetComponent<AnimatorComponent>();
            Check(animator.player && animator.player->GetState() == "Run" &&
                      animator.player->GetParameter("speed") == 2,
                  "Managed Animator controls state and parameters");
            Check(character.GetComponent<TransformComponent>().scale.z == 2,
                  "Managed animation event callback");
            runtime.Shutdown();
            TestScriptAnimatorPlay(config, controllerPath);
            TestControllerSwitchBeforeScriptUpdate(config, controllerPath, root);
            TestScriptAnimationQueues(config, controllerPath);
            TestStreamingAndAnimatedFields(config, root);
            std::filesystem::remove_all(root);
        }
        TestFirstGame(argv[2], argv[3]);
        config.assemblyPath += ".missing";
        Check(!runtime.Initialize(scene, config), "Missing assembly was accepted");
        std::cout
            << "C# lifecycle, transform, exception, lifetime and serialization checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
