#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/SceneManager.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
using namespace LuxEngine;
namespace fs = std::filesystem;

void Check(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct Files {
    fs::path root = fs::temp_directory_path() /
                    ("lux-scene-manager-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Files() {
        fs::create_directories(root / "Scenes");
    }

    ~Files() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

uint32 AddSprite(Scene& scene, const std::string& name, float x) {
    auto entity = scene.CreateEntity(name);
    entity.GetComponent<TransformComponent>().position.x = x;
    entity.AddComponent<SpriteRendererComponent>();
    return entity.GetId();
}

void Save(Scene& scene, const fs::path& file) {
    std::string error;
    Check(SceneManager::Save(scene, file, {}, error), "save source: " + error);
    scene.SetSourcePath(Utf8(file));
}

uint32 Mapped(Scene& scene, uint32 root, uint32 source) {
    for (const auto& [sourceId, local] :
         scene.GetComponent<SceneReferenceComponent>(root).entityMap)
        if (sourceId == source)
            return local;
    return 0;
}

std::string Snapshot(Scene& scene) {
    std::string text;
    Check(scene.SerializeToString(text), "snapshot serializes");
    return text;
}

void TestLoadingAndCache(const fs::path& directory) {
    Scene source;
    const auto id = AddSprite(source, "가나다", 2);
    const auto file = directory / "Scenes/base.lux";
    Save(source, file);
    SceneManager manager;
    std::string error;
    Check(manager.Preload(file, error), "preload succeeds: " + error);
    Check(manager.IsPreloaded(file), "preloaded scene is tracked");
    auto first = manager.Load(file), second = manager.Load(file);
    Check(first && second && first.scene != second.scene, "loads own separate mutable scenes");
    first.scene->GetComponent<TagComponent>(id).tag = "edited";
    Check(second.scene->GetComponent<TagComponent>(id).tag == "가나다",
          "editing one load cannot mutate preload cache or another instance");
    AddSprite(source, "new version", 3);
    Save(source, file);
    Check(!manager.IsPreloaded(file), "a changed dependency invalidates a preloaded snapshot");
    Check(manager.Load(file).scene->GetEntityCount() == 2,
          "a stale preload cannot hide newly authored entities");
    Check(manager.Unload(file) && !manager.IsPreloaded(file), "unload releases cached scene");
    auto future = manager.LoadAsync(file);
    Check(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
          "async load completes");
    auto loaded = future.get();
    Check(loaded && loaded.scene->GetEntityCount() == 2, "async result contains a complete scene");
    std::future<SceneLoadResult> surviving;
    {
        SceneManager temporary;
        surviving = temporary.LoadAsync(file);
    }
    Check(surviving.get().scene != nullptr, "async job owns state after manager destruction");
    {
        struct RestoreDirectory {
            fs::path previous = fs::current_path();

            ~RestoreDirectory() {
                std::error_code error;
                fs::current_path(previous, error);
            }
        } restore;

        fs::current_path(directory);
        auto submitted = manager.LoadAsync("Scenes/base.lux");
        fs::current_path(directory / "Scenes");
        Check(submitted.get().scene != nullptr,
              "async relative paths remain anchored to the submission directory");
    }
    auto preload = manager.PreloadAsync(file);
    Check(static_cast<bool>(preload.get()) && manager.IsPreloaded(file),
          "asynchronous preload populates the same immutable cache");
    for (int iteration = 0; iteration < 12; ++iteration) {
        auto obsolete = manager.PreloadAsync(file);
        manager.ClearCache();
        Check(static_cast<bool>(obsolete.get()) && !manager.IsPreloaded(file),
              "a preload submitted before ClearCache cannot repopulate the cleared cache");
        obsolete = manager.PreloadAsync(file);
        manager.Unload(file);
        Check(static_cast<bool>(obsolete.get()) && !manager.IsPreloaded(file),
              "a preload submitted before Unload cannot resurrect that cached scene");
    }
    const auto origin = source.GetSourcePath(), state = Snapshot(source);
    fs::create_directories(directory / ".lux/autosave");
    Check(SceneManager::Save(source, directory / ".lux/autosave/base.lux", file, error) &&
              source.GetSourcePath() == origin && Snapshot(source) == state,
          "backup/autosave writes cannot change the live scene's path context or state");
    auto missing = manager.Load(directory / "missing.lux");
    Check(!missing && !missing.error.empty(), "missing file reports a useful failure");
    manager.ClearCache();
}

void TestInheritanceAndInstances(const fs::path& directory) {
    Scene source;
    const auto changed = AddSprite(source, "base", 1);
    const auto overridden = AddSprite(source, "custom", 2);
    const auto deleted = AddSprite(source, "deleted", 3);
    auto settings = source.GetSettings();
    settings.exposure = .7f;
    Check(source.SetSettings(settings), "source settings are valid");
    const auto file = directory / "Scenes/inherited.lux";
    Save(source, file);
    SceneManager manager;
    Scene target;
    std::string error;
    const auto root = manager.AddSceneInstance(target, file, SceneReferenceMode::Inherited, error);
    Check(root != 0, "inherited scene instantiates: " + error);
    const auto changedLocal = Mapped(target, root, changed);
    const auto customLocal = Mapped(target, root, overridden);
    const auto deletedLocal = Mapped(target, root, deleted);
    Check(changedLocal && customLocal && deletedLocal, "all imported entities are mapped");
    target.GetComponent<TransformComponent>(customLocal).position.x = 42;
    target.DestroyEntity(Entity(deletedLocal, &target));
    source.GetComponent<TransformComponent>(changed).position.x = 8;
    source.GetComponent<TransformComponent>(overridden).position.x = 9;
    const auto added = AddSprite(source, "new", 5);
    settings.exposure = 1.7f;
    Check(source.SetSettings(settings), "updated settings are valid");
    Save(source, file);
    Check(manager.RefreshInstances(target, error), "inheritance refresh succeeds: " + error);
    Check(target.GetComponent<TransformComponent>(changedLocal).position.x == 8,
          "untouched base entities receive source edits");
    Check(target.GetComponent<TransformComponent>(customLocal).position.x == 42,
          "entity override survives a source change");
    Check(Mapped(target, root, deleted) == 0, "local deletions remain deleted");
    Check(Mapped(target, root, added) != 0, "new source entities appear");
    Check(target.GetSettings().exposure == 1.7f, "untouched inherited settings receive changes");
    auto overriddenSettings = target.GetSettings();
    overriddenSettings.exposure = .3f;
    Check(target.SetSettings(overriddenSettings), "local settings override configures");
    settings.exposure = 2.5f;
    settings.clearColor.x = .7f;
    Check(source.SetSettings(settings), "source updates an independent settings field");
    Save(source, file);
    Check(manager.RefreshInstances(target, error) && target.GetSettings().exposure == .3f &&
              target.GetSettings().clearColor.x == .7f,
          "settings inherit field-by-field without overwriting local overrides");
    const auto group = target.CreateEntity("editor group").GetId();
    Check(target.SetParent(root, group, false), "an inherited root can be organized into a group");
    settings.clearColor.y = .6f;
    Check(source.SetSettings(settings), "grouped-base source setting updates");
    Save(source, file);
    Check(manager.RefreshInstances(target, error) && target.GetSettings().clearColor.y == .6f &&
              target.GetSettings().exposure == .3f,
          "organizing the inherited root into a group cannot disable settings inheritance");
    const auto nested = manager.AddSceneInstance(target, file, SceneReferenceMode::Nested, error);
    Check(nested && nested != root, "repeated instances have separate containers");
    Check(Mapped(target, nested, changed) != changedLocal, "instance entity IDs are independent");
    auto rootTransform = target.GetComponent<TransformComponent>(nested);
    rootTransform.position.x = 10;
    target.GetComponent<TransformComponent>(nested) = rootTransform;
    Check(target.GetWorldTransform(Mapped(target, nested, changed)).position.x == 18,
          "instance container transform affects its child scene");
    const auto before = Snapshot(target);
    fs::remove(file);
    Check(!manager.RefreshInstances(target, error) && Snapshot(target) == before,
          "missing dependency leaves the existing scene intact");
}

void TestNestedAndPortablePaths(const fs::path& directory) {
    SceneManager manager;
    std::string error;
    Scene leaf;
    const auto sprite = AddSprite(leaf, "leaf", 3);
    const auto leafPath = directory / "Scenes/leaf.lux";
    Save(leaf, leafPath);
    Scene middle;
    const auto inner =
        manager.AddSceneInstance(middle, leafPath, SceneReferenceMode::Nested, error);
    Check(inner != 0, "inner scene instantiates");
    const auto middlePath = directory / "Scenes/middle.lux";
    Save(middle, middlePath);
    Scene top;
    const auto outer = manager.AddSceneInstance(top, middlePath, SceneReferenceMode::Nested, error);
    Check(outer != 0 && top.GetEntityCount() == 3, "nested references compose recursively");
    const auto topPath = directory / "Scenes/top.lux";
    Save(top, topPath);
    leaf.GetComponent<TransformComponent>(sprite).position.x = 7;
    Save(leaf, leafPath);
    auto loaded = manager.Load(topPath);
    Check(loaded && loaded.scene->GetEntityCount() == 3, "nested file loads fully");
    bool receivedChange = false;
    for (auto entity : loaded.scene->GetEntities())
        if (const auto* tag = entity.TryGetComponent<TagComponent>(); tag && tag->tag == "leaf")
            receivedChange = entity.GetComponent<TransformComponent>().position.x == 7;
    Check(receivedChange, "nested dependency changes propagate through the outer scene");
    const auto relocated = directory / "relocated";
    fs::create_directories(relocated);
    fs::copy(directory / "Scenes", relocated / "Scenes", fs::copy_options::recursive);
    fs::remove_all(directory / "Scenes");
    loaded = manager.Load(relocated / "Scenes/top.lux");
    Check(loaded && loaded.scene->GetEntityCount() == 3,
          "a copied project resolves relative references in its new location");
    Scene savedAs;
    const auto reference = manager.AddSceneInstance(savedAs, relocated / "Scenes/leaf.lux",
                                                    SceneReferenceMode::Nested, error);
    Check(reference != 0, "Save As fixture instantiates");
    fs::create_directories(relocated / "Other");
    const auto other = relocated / "Other/copy.lux";
    Save(savedAs, other);
    Check(manager.Load(other).scene != nullptr, "Save As preserves source reference meaning");
    const auto original = Snapshot(savedAs);
    Check(!manager.AddSceneInstance(savedAs, other, SceneReferenceMode::Nested, error) &&
              Snapshot(savedAs) == original,
          "a direct or indirect reference cycle is rejected without mutation");
}

void TestStreamingAndUnload(const fs::path& directory) {
    Scene source;
    const auto sourceEntity = AddSprite(source, "streamed", 2);
    const auto file = directory / "Scenes/region.lux";
    Save(source, file);
    SceneManager manager;
    Scene scene;
    std::string error;
    const auto root = manager.AddSceneInstance(scene, file, SceneReferenceMode::Additive, error);
    Check(root != 0, "additive scene loads: " + error);
    const auto child = Mapped(scene, root, sourceEntity);
    scene.GetComponent<TransformComponent>(child).position.x = 17;
    Check(manager.ConfigureStreaming(scene, root, {0, 0}, 5, 8, error),
          "streaming region configures");
    bool changed = false;
    Check(manager.UpdateStreaming(scene, {20, 0}, error, &changed) && changed,
          "leaving unload radius releases scene entities");
    Check(scene.GetEntityCount() == 1 &&
              !scene.GetComponent<SceneReferenceComponent>(root).loaded &&
              !scene.GetComponent<SceneReferenceComponent>(root).suspended.empty(),
          "unload retains a small root and a recoverable authored state");
    const auto saved = directory / "Scenes/streamed.lux";
    Save(scene, saved);
    auto loaded = manager.Load(saved);
    Check(loaded && loaded.scene->GetEntityCount() == 1,
          "an unloaded region remains unloaded when its owning file opens");
    auto& restored = *loaded.scene;
    auto unchanged = Entity(root, &restored);
    Check(manager.UpdateStreaming(restored, {6, 0}, error, &changed) && !changed && unchanged,
          "unchanged streaming ticks keep the live scene and entity handles intact");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        Check(manager.UpdateStreaming(restored, {0, 0}, error, &changed),
              "entering region schedules/finishes its async load: " + error);
        if (restored.GetComponent<SceneReferenceComponent>(root).loaded)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    Check(restored.GetComponent<SceneReferenceComponent>(root).loaded,
          "streaming async load completes within a bounded deadline");
    Check(restored.GetEntityCount() == 2 &&
              restored.GetComponent<TransformComponent>(Mapped(restored, root, sourceEntity))
                      .position.x == 17,
          "reload restores the local instance override");
    unchanged = Entity(root, &restored);
    Check(manager.UpdateStreaming(restored, {6, 0}, error, &changed) && !changed && unchanged,
          "hysteresis retains a loaded region outside its load radius");
    const auto before = Snapshot(restored);
    Check(!manager.ConfigureStreaming(restored, root, {0, 0}, 9, 4, error) &&
              Snapshot(restored) == before,
          "invalid streaming configuration leaves the scene unchanged");
    Check(manager.UnloadInstance(restored, root, error) && restored.GetEntityCount() == 1,
          "additive instance can be unloaded explicitly");
    Check(manager.UpdateStreaming(restored, {0, 0}, error, &changed), "pending load can start");
    restored.DestroyEntity(Entity(root, &restored));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(manager.UpdateStreaming(restored, {0, 0}, error, &changed) && !changed &&
              restored.GetEntityCount() == 0,
          "a completed stale load cannot resurrect a deleted scene root");
}

void TestNestedStreaming(const fs::path& directory) {
    SceneManager manager;
    std::string error;
    Scene leaf;
    const auto sourceEntity = AddSprite(leaf, "nested streamed", 4);
    const auto leafPath = directory / "Scenes/stream-leaf.lux";
    Save(leaf, leafPath);
    Scene middle;
    const auto inner =
        manager.AddSceneInstance(middle, leafPath, SceneReferenceMode::Nested, error);
    Check(inner != 0, "nested streaming source instantiates");
    Check(manager.ConfigureStreaming(middle, inner, {0, 0}, 5, 8, error),
          "nested streaming configures");
    const auto sibling = AddSprite(middle, "sibling", 9);
    const auto middlePath = directory / "Scenes/stream-middle.lux";
    Save(middle, middlePath);
    Scene target;
    const auto outer =
        manager.AddSceneInstance(target, middlePath, SceneReferenceMode::Nested, error);
    Check(outer != 0, "outer streamed scene instantiates");
    const auto localInner = Mapped(target, outer, inner);
    const auto localSibling = Mapped(target, outer, sibling);
    const auto siblingHandle = Entity(localSibling, &target);
    const auto localLeaf = Mapped(target, localInner, sourceEntity);
    Check(localInner && localLeaf, "inner and outer provenance mappings coexist");
    target.GetComponent<TransformComponent>(localLeaf).position.x = 21;
    target.AddComponent<ScriptComponent>(localSibling, "Fixture.Sibling", true).fields["Link"] =
        ScriptEntityReference{localLeaf};
    target.AddComponent<ScriptComponent>(localLeaf, "Fixture.Leaf", true).fields["Link"] =
        ScriptEntityReference{localSibling};
    bool changed = false;
    Check(manager.UpdateStreaming(target, {20, 0}, error, &changed) && changed,
          "nested region unloads");
    Check(target.GetEntityCount() == 3 && siblingHandle.IsValid() &&
              Mapped(target, outer, sibling) == localSibling,
          "nested unload preserves the outer reference's sibling mapping and handle");
    Check(manager.RefreshInstances(target, error),
          "outer refresh can run while an inner region is unloaded");
    Check(!target.GetComponent<SceneReferenceComponent>(localInner).loaded &&
              !target.GetComponent<SceneReferenceComponent>(localInner).suspended.empty(),
          "outer refresh retains the inner region's unloaded state and suspended overrides");
    const auto nestedFile = directory / "Scenes/nested-streamed.lux";
    Save(target, nestedFile);
    auto reopened = manager.Load(nestedFile);
    Check(reopened && !reopened.scene->GetComponent<SceneReferenceComponent>(localInner).loaded &&
              reopened.scene->GetEntityCount() == 3,
          "opening a nested scene cannot silently discard an unloaded region's state");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        Check(manager.UpdateStreaming(target, {0, 0}, error, &changed),
              "nested region resumes asynchronously: " + error);
        if (target.GetComponent<SceneReferenceComponent>(localInner).loaded)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    Check(target.GetComponent<SceneReferenceComponent>(localInner).loaded &&
              target.GetComponent<TransformComponent>(Mapped(target, localInner, sourceEntity))
                      .position.x == 21,
          "nested unload/reload preserves source mappings and overrides");
    const auto restoredLeaf = Mapped(target, localInner, sourceEntity);
    const auto resolve = [&](uint32 id) {
        const auto& link = target.GetComponent<ScriptComponent>(id).fields.at("Link");
        return target.ResolveScriptEntityReference(std::get<ScriptEntityReference>(link));
    };
    Check(resolve(localSibling) == restoredLeaf && resolve(restoredLeaf) == localSibling,
          "nested streaming reconnects the sibling and streamed references");
    Check(manager.RefreshInstances(target, error),
          "outer refresh remains valid after nested reload: " + error);
    Check(target.GetEntityCount() == 4, "outer refresh does not duplicate reloaded inner entities");
}

void TestStreamingScriptReferences(const fs::path& directory) {
    Scene region;
    const auto source = region.CreateEntity("Referenced actor").GetId();
    region.AddComponent<ScriptComponent>(source, "Fixture.Region", true);
    const auto regionPath = directory / "Scenes/script-links-region.lux";
    Save(region, regionPath);

    Scene world;
    const auto actor = world.CreateEntity("Persistent actor").GetId();
    SceneManager manager;
    std::string error;
    const auto first =
        manager.AddSceneInstance(world, regionPath, SceneReferenceMode::Additive, error);
    const auto second =
        manager.AddSceneInstance(world, regionPath, SceneReferenceMode::Additive, error);
    Check(first && second, error);
    const auto firstId = Mapped(world, first, source);
    const auto secondId = Mapped(world, second, source);
    auto& links = world.AddComponent<ScriptComponent>(actor, "Fixture.Actor", true).fields;
    links = {{"First", ScriptEntityReference{firstId}},
             {"Second", ScriptEntityReference{secondId}}};
    for (const auto id : {firstId, secondId})
        world.GetComponent<ScriptComponent>(id).fields["Outside"] = ScriptEntityReference{actor};
    world.GetComponent<ScriptComponent>(firstId).fields["Peer"] = ScriptEntityReference{secondId};
    world.GetComponent<ScriptComponent>(secondId).fields["Peer"] = ScriptEntityReference{firstId};
    for (const auto root : {first, second})
        Check(manager.ConfigureStreaming(world, root, {0, 0}, 4, 8, error), error);

    bool changed = false;
    Check(manager.UpdateStreaming(world, {20, 0}, error, &changed) && changed, error);
    const auto saved = directory / "Scenes/script-links-world.lux";
    Save(world, saved);
    auto reopened = manager.Load(saved);
    Check(bool(reopened), reopened.error);
    auto& scene = *reopened.scene;
    const auto resolve = [&](uint32 owner, const char* name) {
        const auto& value = scene.GetComponent<ScriptComponent>(owner).fields.at(name);
        return scene.ResolveScriptEntityReference(std::get<ScriptEntityReference>(value));
    };
    Check(!resolve(actor, "First") && !resolve(actor, "Second"),
          "references to unloaded entities stay unresolved");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        Check(manager.UpdateStreaming(scene, {0, 0}, error, &changed), error);
        if (scene.GetComponent<SceneReferenceComponent>(first).loaded &&
            scene.GetComponent<SceneReferenceComponent>(second).loaded)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    const auto loadedFirst = Mapped(scene, first, source);
    const auto loadedSecond = Mapped(scene, second, source);
    Check(loadedFirst && loadedSecond && loadedFirst != loadedSecond &&
              resolve(actor, "First") == loadedFirst && resolve(actor, "Second") == loadedSecond,
          "references distinguish duplicate instances after save and streaming");
    Check(resolve(loadedFirst, "Outside") == actor && resolve(loadedSecond, "Outside") == actor,
          "streamed references restore links to the persistent scene");
    Check(resolve(loadedFirst, "Peer") == loadedSecond &&
              resolve(loadedSecond, "Peer") == loadedFirst,
          "references between independently streamed regions follow the restored entities");
}

void TestStreamingFailurePreservesScene(const fs::path& directory) {
    Scene source;
    const auto sourceId = source.CreateEntity("Region child").GetId();
    const auto path = directory / "Scenes/stream-failure.lux";
    Save(source, path);
    Scene world;
    SceneManager manager;
    std::string error;
    const auto root = manager.AddSceneInstance(world, path, SceneReferenceMode::Nested, error);
    Check(root && manager.ConfigureStreaming(world, root, {0, 0}, 4, 8, error) &&
              manager.UnloadInstance(world, root, error),
          error);
    auto& reference = world.GetComponent<SceneReferenceComponent>(root);
    const auto validSnapshot = reference.suspended;
    reference.suspended = "{";
    auto observer = world.CreateEntity("Outside observer");
    auto& field = observer.AddComponent<ScriptComponent>("Fixture.Observer", true).fields["Link"];
    field = ScriptEntityReference{root};
    const auto before = Snapshot(world);

    bool changed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (manager.UpdateStreaming(world, {0, 0}, error, &changed) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    Check(!error.empty() && !changed && Snapshot(world) == before,
          "a failed streaming restore preserves the serialized scene and reference identities");

    reference.suspended = validSnapshot;
    Check(manager.UpdateStreaming(world, {20, 0}, error, &changed) && !changed, error);
    const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        Check(manager.UpdateStreaming(world, {0, 0}, error, &changed), error);
        if (changed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < retryDeadline);
    const auto link = std::get<ScriptEntityReference>(field);
    Check(changed && Mapped(world, root, sourceId) && link.identity &&
              world.ResolveScriptEntityReference(link) == root,
          "the successful retry publishes the outside reference to the preserved region root");
}

void TestUnsupportedStreamedComponents(const fs::path& directory) {
    struct ExtensionState {
        int value;
    };

    Scene source;
    const auto sourceId = source.CreateEntity("Region child").GetId();
    const auto path = directory / "Scenes/stream-extension.lux";
    Save(source, path);
    Scene world;
    SceneManager manager;
    std::string error;
    const auto blocked = manager.AddSceneInstance(world, path, SceneReferenceMode::Nested, error);
    Check(blocked != 0, error);
    const auto healthy = manager.AddSceneInstance(world, path, SceneReferenceMode::Nested, error);
    Check(healthy != 0, error);
    const auto blockedChild = Mapped(world, blocked, sourceId);
    const auto healthyChild = Mapped(world, healthy, sourceId);
    world.AddComponent<ExtensionState>(blockedChild, ExtensionState{73});
    auto& blockedLink =
        world.AddComponent<ScriptComponent>(blockedChild, "Fixture.Blocked", true).fields["Peer"];
    blockedLink = ScriptEntityReference{healthyChild};
    world.AddComponent<ScriptComponent>(healthyChild, "Fixture.Healthy", true).fields["Peer"] =
        ScriptEntityReference{blockedChild};
    const auto outside = world.CreateEntity("Outside custom state").GetId();
    world.AddComponent<ExtensionState>(outside, ExtensionState{19});
    for (const auto root : {blocked, healthy})
        Check(manager.ConfigureStreaming(world, root, {0, 0}, 4, 8, error), error);

    Check(!manager.UnloadInstance(world, blocked, error) && !error.empty() &&
              world.IsAlive(blockedChild) &&
              world.GetComponent<ExtensionState>(blockedChild).value == 73 &&
              std::get<ScriptEntityReference>(blockedLink) == ScriptEntityReference{healthyChild},
          "a manual unload rejects removal of an entity with unsupported components");
    bool changed = false;
    Check(!manager.UpdateStreaming(world, {20, 0}, error, &changed) && !error.empty() && changed &&
              world.GetComponent<SceneReferenceComponent>(blocked).loaded &&
              !world.GetComponent<SceneReferenceComponent>(healthy).loaded &&
              !world.IsAlive(healthyChild) &&
              world.GetComponent<ExtensionState>(blockedChild).value == 73 &&
              world.GetComponent<ExtensionState>(outside).value == 19,
          "an unsupported component blocks only its own region and leaves the other state intact");
    Check(std::get<ScriptEntityReference>(blockedLink).identity &&
              !world.ResolveScriptEntityReference(std::get<ScriptEntityReference>(blockedLink)),
          "only the successful unload stabilizes the link to its unloaded child");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        Check(manager.UpdateStreaming(world, {0, 0}, error, &changed), error);
        if (changed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    const auto restored = Mapped(world, healthy, sourceId);
    Check(changed && restored &&
              world.ResolveScriptEntityReference(std::get<ScriptEntityReference>(blockedLink)) ==
                  restored &&
              world.ResolveScriptEntityReference(std::get<ScriptEntityReference>(
                  world.GetComponent<ScriptComponent>(restored).fields.at("Peer"))) == blockedChild,
          "cross-region links survive partial failure, unload and restore");
}

void TestCopyUnloadedNestedInstances(const fs::path& directory) {
    SceneManager manager;
    std::string error;
    Scene leaf;
    const auto actor = AddSprite(leaf, "streamed actor", 4);
    const auto leafPath = directory / "Scenes/copy-leaf.lux";
    Save(leaf, leafPath);

    Scene middle;
    const auto sibling = middle.CreateEntity("sibling").GetId();
    const auto inner =
        manager.AddSceneInstance(middle, leafPath, SceneReferenceMode::Nested, error);
    Check(inner && manager.ConfigureStreaming(middle, inner, {0, 0}, 4, 8, error), error);
    const auto original = Mapped(middle, inner, actor);
    middle.GetComponent<TransformComponent>(original).position.x = 21;
    middle.AddComponent<ScriptComponent>(original, "Fixture.Actor", true).fields["Link"] =
        ScriptEntityReference{sibling};
    middle.AddComponent<ScriptComponent>(sibling, "Fixture.Sibling", true).fields["Link"] =
        ScriptEntityReference{original};
    Check(manager.UnloadInstance(middle, inner, error), error);
    const auto middlePath = directory / "Scenes/copy-middle.lux";
    Save(middle, middlePath);
    auto reopenedMiddle = manager.Load(middlePath);
    Check(bool(reopenedMiddle), reopenedMiddle.error);
    const auto unrelated = reopenedMiddle.scene->CreateEntity("unrelated").GetId();
    Check(unrelated != original, "reopening leaves suspended entity IDs reserved");
    Save(*reopenedMiddle.scene, middlePath);

    Scene world;
    for (int i = 0; i < 20; ++i)
        world.CreateEntity("background");
    const auto first =
        manager.AddSceneInstance(world, middlePath, SceneReferenceMode::Nested, error);
    Check(first != 0, error);
    const auto second =
        manager.AddSceneInstance(world, middlePath, SceneReferenceMode::Nested, error);
    Check(second != 0, error);
    const auto firstInner = Mapped(world, first, inner);
    const auto secondInner = Mapped(world, second, inner);
    Check(firstInner != inner && secondInner != inner && firstInner != secondInner,
          "both saved roots are relocated");
    Check(manager.ConfigureStreaming(world, first, {0, 0}, 4, 8, error) &&
              manager.UnloadInstance(world, first, error),
          error);
    const auto worldPath = directory / "Scenes/copy-world.lux";
    Save(world, worldPath);
    auto reopened = manager.Load(worldPath);
    Check(bool(reopened), reopened.error);
    auto& scene = *reopened.scene;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        Check(manager.UpdateStreaming(scene, {0, 0}, error), error);
        const auto restoredInner = Mapped(scene, first, inner);
        if (restoredInner && Mapped(scene, restoredInner, actor) &&
            Mapped(scene, secondInner, actor))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    const auto resolve = [&](uint32 id) {
        const auto& field = scene.GetComponent<ScriptComponent>(id).fields.at("Link");
        return scene.ResolveScriptEntityReference(std::get<ScriptEntityReference>(field));
    };
    const auto verify = [&](uint32 outer, const char* operation) {
        const auto localInner = Mapped(scene, outer, inner);
        const auto localSibling = Mapped(scene, outer, sibling);
        Check(localInner && localSibling, "restored outer scene retains its live mappings");
        const auto localActor = Mapped(scene, localInner, actor);
        Check(localActor && Mapped(scene, outer, original) == localActor &&
                  scene.GetParent(localActor) == localInner,
              "restored actor belongs to the inner hierarchy and both instance maps");
        Check(scene.GetComponent<TransformComponent>(localActor).position.x == 21,
              "relocated actor restores its transform override");
        const auto actorLink = resolve(localActor);
        const auto siblingLink = resolve(localSibling);
        Check(actorLink == localSibling && siblingLink == localActor,
              std::string(operation) + " in instance " + std::to_string(outer) +
                  ": relocated actor " + std::to_string(localActor) + " links to " +
                  std::to_string(actorLink) + ", sibling " + std::to_string(localSibling) +
                  " links to " + std::to_string(siblingLink));
        return localActor;
    };
    Check(verify(first, "streaming") != verify(second, "streaming"),
          "duplicate copies restore distinct actors");
    Check(manager.RefreshInstances(scene, error), error);
    const auto firstActor = verify(first, "refresh");
    const auto secondActor = verify(second, "refresh");
    Check(firstActor != secondActor && scene.GetEntityCount() == 30,
          "refresh keeps both regions and does not duplicate restored actors");
}

void TestImportedSuspendedRegion(const fs::path& directory) {
    SceneManager manager;
    std::string error;
    Scene source;
    const auto actor = AddSprite(source, "imported actor", 4);
    const auto path = directory / "Scenes/imported-region.lux";
    Save(source, path);

    Scene origin;
    const auto sibling = origin.CreateEntity("sibling").GetId();
    const auto region = manager.AddSceneInstance(origin, path, SceneReferenceMode::Nested, error);
    Check(region != 0, error);
    const auto authoredActor = Mapped(origin, region, actor);
    origin.GetComponent<TransformComponent>(authoredActor).position.x = 21;
    origin.AddComponent<ScriptComponent>(sibling, "Fixture.Sibling", true).fields["Actor"] =
        ScriptEntityReference{authoredActor};
    origin.AddComponent<ScriptComponent>(authoredActor, "Fixture.Actor", true).fields["Sibling"] =
        ScriptEntityReference{sibling};
    Check(manager.ConfigureStreaming(origin, region, {0, 0}, 4, 8, error) &&
              manager.UnloadInstance(origin, region, error),
          error);

    Scene destination;
    auto scale = destination.CreateEntity("Scale");
    scale.GetComponent<TransformComponent>().scale = {2, 1, 1};
    auto rotate = destination.CreateEntity("Rotation");
    rotate.GetComponent<TransformComponent>().rotation.z = .65;
    Check(destination.SetParent(rotate.GetId(), scale.GetId(), false), "parent rotation");
    const auto first = destination.ImportEntities(origin, {}, rotate.GetId());
    const auto second = destination.ImportEntities(origin, {});
    Check(first.contains(region) && second.contains(region), "copy both dormant instances");
    const auto before = Snapshot(destination);
    auto& originReference = origin.GetComponent<SceneReferenceComponent>(region);
    const auto suspended = originReference.suspended;
    originReference.suspended = "{";
    Check(destination.ImportEntities(origin, {}).empty() && Snapshot(destination) == before,
          "a corrupt snapshot leaves the destination unchanged");
    originReference.suspended = suspended;
    const auto bad = manager.AddSceneInstance(destination, path, SceneReferenceMode::Nested, error);
    Check(bad && manager.ConfigureStreaming(destination, bad, {0, 0}, 4, 8, error) &&
              manager.UnloadInstance(destination, bad, error),
          error);
    auto& broken = destination.GetComponent<SceneReferenceComponent>(bad);
    broken.suspended = origin.GetComponent<SceneReferenceComponent>(region).suspended;

    bool sawFailure = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        sawFailure |= !manager.UpdateStreaming(destination, {0, 0}, error);
        if (sawFailure && Mapped(destination, first.at(region), actor) &&
            Mapped(destination, second.at(region), actor))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    Check(sawFailure && !destination.GetComponent<SceneReferenceComponent>(bad).loaded,
          "the broken region remains unloaded and reports failure");

    const auto resolve = [&](uint32 owner, const char* field) {
        const auto& value = destination.GetComponent<ScriptComponent>(owner).fields.at(field);
        return destination.ResolveScriptEntityReference(std::get<ScriptEntityReference>(value));
    };
    for (const auto* copy : {&first, &second}) {
        const auto localActor = Mapped(destination, copy->at(region), actor);
        Check(localActor && destination.GetParent(localActor) == copy->at(region) &&
                  destination.GetComponent<TransformComponent>(localActor).position.x == 21 &&
                  resolve(localActor, "Sibling") == copy->at(sibling) &&
                  resolve(copy->at(sibling), "Actor") == localActor,
              "healthy regions preserve their local pose and independent script links");
    }

    const auto underShear = first.at(region);
    Check(manager.UnloadInstance(destination, underShear, error) &&
              manager.RefreshInstance(destination, underShear, error),
          "restoring under an unchanged sheared parent preserves local transforms: " + error);
    Check(destination.GetParent(underShear) == rotate.GetId(), "restore keeps the live parent");

    destination.GetComponent<SceneReferenceComponent>(bad).path.clear();
    Check(manager.UnloadInstance(destination, underShear, error), error);
    const auto invalidPathDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool progressedWithPathError = false;
    do {
        bool changed = false;
        Check(!manager.UpdateStreaming(destination, {0, 0}, error, &changed) && !error.empty(),
              "invalid reference paths are reported");
        progressedWithPathError |= changed && Mapped(destination, underShear, actor) != 0;
        if (progressedWithPathError)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < invalidPathDeadline);
    Check(progressedWithPathError, "invalid paths cannot block a healthy region in the same tick");
}

void TestLiveRuntimeState(const fs::path& directory) {
    Scene target;
    auto ground = target.CreateEntity("ground");
    ground.GetComponent<TransformComponent>().position = {0, -1, 0};
    ground.GetComponent<TransformComponent>().scale = {10, 1, 1};
    ground.AddComponent<Rigidbody2DComponent>().type = BodyType::Static;
    ground.AddComponent<BoxCollider2DComponent>();
    auto body = target.CreateEntity("body");
    body.AddComponent<Rigidbody2DComponent>();
    body.AddComponent<BoxCollider2DComponent>();
    const auto bodyId = body.GetId();
    auto actor = target.CreateEntity("animated");
    const auto actorId = actor.GetId();
    auto& animation = actor.AddComponent<AnimationComponent>();
    animation.clip.duration = 10;
    animation.clip.AddKeyframe({0, {20, 5, 0}, {}, {1, 1, 1}});
    animation.clip.AddKeyframe({10, {30, 5, 0}, {}, {1, 1, 1}});
    for (int frame = 0; frame < 180; ++frame)
        target.OnUpdate(1.0 / 60);
    Check(target.GetComponent<Rigidbody2DComponent>(bodyId).isGrounded,
          "runtime body rests on its original contact before composition");
    Check(target.GetCollisionEvents2D().empty(), "settled contacts do not emit fresh begin events");
    const auto cursor = target.GetComponent<AnimationComponent>(actorId).currentTime;
    Scene chunk;
    AddSprite(chunk, "new chunk", 50);
    const auto file = directory / "Scenes/live-chunk.lux";
    Save(chunk, file);
    SceneManager manager;
    std::string error;
    const auto root = manager.AddSceneInstance(target, file, SceneReferenceMode::Additive, error);
    Check(root != 0, "live additive scene loads: " + error);
    Check(target.GetComponent<AnimationComponent>(actorId).currentTime == cursor &&
              target.GetComponent<Rigidbody2DComponent>(bodyId).isGrounded,
          "composition keeps unrelated animation cursors and grounded state");
    target.OnUpdate(1.0 / 60);
    Check(target.GetCollisionEvents2D().empty(),
          "adding an unrelated scene preserves existing native physics contacts");
    Check(target.GetComponent<AnimationComponent>(actorId).currentTime > cursor,
          "existing animation resumes rather than restarting after composition");
    Check(manager.UnloadInstance(target, root, error), "live scene unloads");
    target.OnUpdate(1.0 / 60);
    Check(target.GetCollisionEvents2D().empty(),
          "unloading an unrelated scene preserves existing native physics contacts");
}

std::string ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void TestCyclesAndFailurePreservation(const fs::path& directory) {
    Scene first, second;
    auto reference = first.CreateEntity("first reference");
    reference.AddComponent<SceneReferenceComponent>().path = "cycle-b.lux";
    reference = second.CreateEntity("second reference");
    reference.AddComponent<SceneReferenceComponent>().path = "cycle-a.lux";
    const auto a = directory / "Scenes/cycle-a.lux", b = directory / "Scenes/cycle-b.lux";
    Check(first.Serialize(Utf8(a)) && second.Serialize(Utf8(b)), "raw cyclic fixtures serialize");
    SceneManager manager;
    const auto failed = manager.Load(a);
    Check(!failed && !failed.error.empty(),
          "cross-file reference cycles fail without recursion overflow");
    const auto original = ReadFile(a);
    std::string error;
    Check(!SceneManager::Save(first, a, a, error) && ReadFile(a) == original,
          "a cyclic save cannot replace an existing scene file");
    Scene source;
    AddSprite(source, "valid", 1);
    const auto sourceFile = directory / "Scenes/valid.lux";
    Save(source, sourceFile);
    source.GetComponent<TagComponent>(1).tag = "unsaved changes";
    const auto fileBefore = ReadFile(sourceFile), originBefore = source.GetSourcePath();
    const auto stateBefore = Snapshot(source);
    Scene::InjectSaveFault(Scene::SaveFault::TempWriteFailure);
    const bool saved = SceneManager::Save(source, sourceFile, sourceFile, error);
    Scene::ClearSaveFaults();
    Check(!saved && ReadFile(sourceFile) == fileBefore && source.GetSourcePath() == originBefore &&
              Snapshot(source) == stateBefore,
          "failed composed saves preserve live state, path context and original file bytes");
    Scene target;
    const auto root =
        manager.AddSceneInstance(target, sourceFile, SceneReferenceMode::Nested, error);
    Check(root != 0, "failure-preservation target creates");
    const auto before = Snapshot(target);
    {
        std::ofstream broken(sourceFile, std::ios::binary | std::ios::trunc);
        broken << "not a scene";
    }
    Check(!manager.RefreshInstances(target, error) && Snapshot(target) == before,
          "a corrupt dependency cannot partially replace a live scene");
    Check(
        !manager.AddSceneInstance(target, sourceFile, static_cast<SceneReferenceMode>(42), error) &&
            Snapshot(target) == before,
        "invalid reference modes fail without changing authored data");
}

void TestReferenceLimits(const fs::path& directory) {
    Scene excessive;
    for (int index = 0; index < 129; ++index) {
        auto item = excessive.CreateEntity("unloaded reference");
        auto& reference = item.AddComponent<SceneReferenceComponent>();
        reference.path = "valid.lux";
        reference.loaded = false;
    }
    const auto oversized = directory / "Scenes/excessive.lux";
    Check(excessive.Serialize(Utf8(oversized)), "raw over-limit reference fixture serializes");
    SceneManager manager;
    Check(!manager.Load(oversized), "reference count limits also cover inactive/unloaded regions");
    std::string error;
    const auto rejected = directory / "Scenes/not-written.lux";
    Check(!SceneManager::Save(excessive, rejected, oversized, error) && !fs::exists(rejected),
          "the manager cannot save a scene that its reference limits would reject on open");
}

void TestSourceRemovalKeepsLocalChildren(const fs::path& directory) {
    Scene source;
    auto parent = source.CreateEntity("source parent");
    auto child = source.CreateEntity("source child");
    Check(source.SetParent(child.GetId(), parent.GetId(), false), "source hierarchy configures");
    const auto file = directory / "Scenes/removal.lux";
    Save(source, file);
    SceneManager manager;
    Scene target;
    std::string error;
    const auto root = manager.AddSceneInstance(target, file, SceneReferenceMode::Inherited, error);
    Check(root != 0, "removal fixture instantiates");
    const auto sourceParent = parent.GetId(), sourceChild = child.GetId();
    const auto parentLocal = Mapped(target, root, sourceParent),
               childLocal = Mapped(target, root, sourceChild);
    target.GetComponent<TagComponent>(childLocal).tag = "local override";
    auto local = target.CreateEntity("local child");
    const auto localId = local.GetId();
    local.GetComponent<TransformComponent>().position.x = 9;
    Check(target.SetParent(localId, parentLocal, false),
          "local child attaches to inherited parent");
    source.DestroyEntity(parent);
    Save(source, file);
    Check(manager.RefreshInstances(target, error), "source parent removal refreshes: " + error);
    Check(!target.IsAlive(parentLocal) && target.IsAlive(childLocal) && target.IsAlive(localId),
          "removed base parents do not destroy locally overridden or newly authored children");
    Check(target.GetParent(childLocal) == root && target.GetParent(localId) == root &&
              target.GetWorldTransform(localId).position.x == 9,
          "retained children reparent to the surviving container without moving");
}
} // namespace

int RunSceneManagerTests(int, char**) {
    try {
        {
            Files files;
            TestLoadingAndCache(files.root);
        }
        {
            Files files;
            TestInheritanceAndInstances(files.root);
        }
        {
            Files files;
            TestNestedAndPortablePaths(files.root);
        }
        {
            Files files;
            TestStreamingAndUnload(files.root);
        }
        {
            Files files;
            TestNestedStreaming(files.root);
            TestStreamingScriptReferences(files.root);
            TestStreamingFailurePreservesScene(files.root);
            TestUnsupportedStreamedComponents(files.root);
            TestCopyUnloadedNestedInstances(files.root);
            TestImportedSuspendedRegion(files.root);
        }
        {
            Files files;
            TestLiveRuntimeState(files.root);
        }
        {
            Files files;
            TestCyclesAndFailurePreservation(files.root);
        }
        {
            Files files;
            TestSourceRemovalKeepsLocalChildren(files.root);
        }
        {
            Files files;
            TestReferenceLimits(files.root);
        }
        std::cout << "Scene loading, cache, inheritance and nested-reference checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Scene manager check failed: " << error.what() << '\n';
        return 1;
    }
}

LUX_ENGINE_MAIN(RunSceneManagerTests)
