#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Physics/PhysicsWorld.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void SmallBodies() {
    using namespace LuxEngine;
    for (const bool planar : {false, true})
        for (const double origin : {0., 1e9, 1e12})
            for (const int fps : {30, 60, 120})
                for (const uint32 substeps : {1u, 4u})
                    for (const double size : {.01, .019, .02, .1, 1.}) {
                        Scene scene;
                        auto settings = scene.GetSettings();
                        settings.overrideGravity = true;
                        settings.gravity = {0, -9.81};
                        settings.gravity3D = {0, -9.81, 0};
                        settings.physicsSubsteps = substeps;
                        Check(scene.SetSettings(settings), "small body settings");
                        auto floor = scene.CreateEntity("Floor");
                        floor.GetComponent<TransformComponent>().position = {origin, origin - .5,
                                                                             0};
                        auto box = scene.CreateEntity("Small box");
                        auto& transform = box.GetComponent<TransformComponent>();
                        transform.position = {origin + .125, origin + 3, 0};
                        transform.scale = {size, size, size};
                        if (planar) {
                            floor.AddComponent<BoxCollider2DComponent>().size = {20, 1};
                            box.AddComponent<BoxCollider2DComponent>();
                            box.AddComponent<Rigidbody2DComponent>();
                        } else {
                            floor.AddComponent<BoxCollider3DComponent>().size = {20, 1, 20};
                            box.AddComponent<BoxCollider3DComponent>();
                            box.AddComponent<Rigidbody3DComponent>();
                        }
                        std::string snapshot;
                        Check(scene.SerializeToString(snapshot), "small body scene can be saved");
                        bool contact = false;
                        for (int frame = 0; frame < fps * 2; ++frame) {
                            scene.OnUpdate(1. / fps);
                            contact |= planar ? !scene.GetCollisionEvents2D().empty()
                                              : !scene.GetCollisionEvents3D().empty();
                        }
                        const double height = transform.position.y - origin;
                        if (!contact || std::abs(height - size * .5) >= .04) {
                            std::cerr << "small body: planar=" << planar << " origin=" << origin
                                      << " fps=" << fps << " substeps=" << substeps
                                      << " size=" << size << " height=" << height << '\n';
                            throw std::runtime_error("small authored body lost its simulation");
                        }
                    }
    Scene invalid;
    auto box = invalid.CreateEntity("Collapsed collider");
    box.AddComponent<BoxCollider3DComponent>();
    box.GetComponent<TransformComponent>().scale = {};
    const auto before = Log::ReadAfter(0);
    const auto cursor = before.empty() ? 0 : before.back().sequence;
    invalid.OnUpdate(1. / 60);
    const auto warning = Log::ReadAfter(cursor);
    Check(warning.size() == 1 && warning.front().level == LogLevel::Warn &&
              warning.front().message.find("#1") != std::string::npos,
          "invalid world collider reports its entity instead of silently disappearing");
    invalid.OnUpdate(1. / 60);
    Check(Log::ReadAfter(warning.front().sequence).empty(),
          "unchanged invalid collider does not flood the editor console");
}

int Run(int, char**) {
    using namespace LuxEngine;
    SmallBodies();
    for (const bool planar : {false, true}) {
        PhysicsWorld world(planar);
        PhysicsBodyDescription floor;
        floor.entity = 1;
        floor.size = {20, 1, 20};
        floor.position = {0, -.5f, 0};
        PhysicsBodyDescription box;
        box.entity = 2;
        box.type = BodyType::Dynamic;
        box.position = {0, 3, planar ? 12.f : 0};
        std::array descriptions{floor, box};
        Check(world.Synchronize(descriptions), "Jolt body creation failed");
        bool contact = false;
        for (unsigned i = 0; i < 240; ++i) {
            world.Step(1.0 / 60, {0, -9.81f, 0});
            contact |= !world.BeganContacts().empty();
        }
        const auto state = world.GetState(2);
        Check(state && state->position.y > .45f && state->position.y < .55f,
              "Jolt box did not rest on the authored floor");
        Check(contact && state->grounded, "Jolt contact/grounding missing");
        if (planar)
            Check(std::abs(state->position.z) < .001f, "Plane2D escaped XY");
        const auto before = *state;
        world.Step(-1, {0, -9.81f, 0});
        Check(world.GetState(2)->position.y == before.position.y,
              "Invalid timestep changed a Jolt body");
        box.type = BodyType::Kinematic;
        box.position = {0, 3, 0};
        box.velocity = {2, 0, planar ? 4.f : 0};
        descriptions = {floor, box};
        Check(world.Synchronize(descriptions), "Kinematic synchronization failed");
        for (unsigned i = 0; i < 60; ++i)
            world.Step(1.0 / 60, {0, -9.81f, 0});
        const auto kinematic = *world.GetState(2);
        Check(std::abs(kinematic.position.x - 2) < .01f &&
                  std::abs(kinematic.position.y - 3) < .01f,
              "Kinematic Jolt motion was affected by gravity");
        if (planar)
            Check(std::abs(kinematic.position.z) < .001f, "Planar velocity escaped XY");
        box.type = BodyType::Dynamic;
        box.position = {0, 3, 0};
        box.velocity = {};
        floor.trigger = true;
        descriptions = {floor, box};
        Check(world.Synchronize(descriptions), "Trigger synchronization failed");
        bool trigger = false;
        for (unsigned i = 0; i < 90; ++i) {
            world.Step(1.0 / 60, {0, -9.81f, 0});
            for (const auto& event : world.BeganContacts())
                trigger |= event.isTrigger;
        }
        Check(trigger && world.GetState(2)->position.y < -2,
              "Jolt trigger blocked the falling body");
        box.velocity.x = std::numeric_limits<float>::quiet_NaN();
        descriptions = {floor, box};
        Check(!world.Synchronize(descriptions) && !world.GetState(2) &&
                  !world.GetLastError().empty(),
              "Invalid Jolt body was retained");
        world.Remove(1);
        Check(world.BodyCount() == 0 && world.BeganContacts().empty(),
              "Jolt removal retained stale contact data");
    }
    Scene scene;
    auto floor = scene.CreateEntity("Floor");
    floor.GetComponent<TransformComponent>().position = {0, -.5f, 0};
    floor.AddComponent<BoxCollider3DComponent>().size = {12, 1, 12};
    auto box = scene.CreateEntity("Box");
    box.GetComponent<TransformComponent>().position = {0, 4, 0};
    box.AddComponent<BoxCollider3DComponent>();
    box.AddComponent<Rigidbody3DComponent>();
    bool event = false;
    for (unsigned i = 0; i < 240; ++i) {
        scene.OnUpdate(1.0 / 60);
        event |= !scene.GetCollisionEvents3D().empty();
    }
    Check(event, "Scene did not expose 3D collision events");
    Check(scene.GetWorldTransform(box.GetId()).position.y > .45f,
          "Scene body fell through Jolt floor");
    std::string saved;
    Check(scene.SerializeToString(saved), "Jolt scene cannot be serialized");
    Scene loaded;
    Check(loaded.DeserializeFromString(saved), "Jolt scene cannot be reloaded");
    loaded.OnUpdate(1.0 / 60);
    const auto boxId = box.GetId();
    box.GetComponent<TransformComponent>().position.y = 3;
    for (unsigned i = 0; i < 120; ++i) {
        scene.OnUpdate(1.0 / 60);
        if (!scene.GetCollisionEvents3D().empty())
            break;
    }
    Check(!scene.GetCollisionEvents3D().empty(), "Scene did not produce a renewed 3D contact");
    Check(scene.DeserializeFromString(saved) && scene.GetCollisionEvents3D().empty(),
          "Scene reload retained old 3D contact events");
    Check(scene.IsAlive(boxId), "Scene reload lost the 3D box");
    Scene replacement;
    std::string restored;
    Check(scene.SerializeToString(restored) && replacement.DeserializeFromString(restored),
          "Scene state clone failed");
    Check(scene.ApplyState(replacement), "Jolt-backed scene state application failed");
    scene.OnUpdate(1.0 / 60);
    std::cout << "Jolt 2D/3D world and scene checks passed\n";
    return 0;
}
} // namespace
LUX_ENGINE_MAIN(Run)
