#include <LuxEngine/Physics/PhysicsWorld.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace LuxEngine;

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void Queries() {
    PhysicsWorld world;
    PhysicsBodyDescription near, far, sensor;
    near.entity = 1;
    near.position = {3, 0, 0};
    near.size = {2, 2, 2};
    far = near;
    far.entity = 2;
    far.position.x = 8;
    sensor.entity = 3;
    sensor.position.x = 1;
    sensor.size = {.5f, .5f, .5f};
    sensor.trigger = true;
    Check(world.Synchronize(std::array{near, far, sensor}), "ray fixtures synchronize");
    RaycastHit3D hit;
    Check(world.Raycast({}, {10, 0, 0}, 10, hit) && hit.entityId == 1 &&
              std::abs(hit.distance - 2) < .01f && hit.normal.x < -.99f,
          "nearest solid ray hit uses normalized world distance and surface normal");
    Check(world.Raycast({}, {1, 0, 0}, 10, hit, 0, true) && hit.entityId == 3 &&
              std::abs(hit.distance - .75f) < .01f,
          "rays optionally include sensors");
    Check(world.Raycast({}, {1, 0, 0}, 10, hit, 3, true) && hit.entityId == 1,
          "rays ignore the selected entity");
    Check(world.Raycast({3, 0, 0}, {0, 2, 0}, 5, hit) && hit.distance == 0 && hit.normal.y == -1 &&
              hit.point.x == 3,
          "inside rays use origin and opposite ray normal");
    Check(!world.Raycast({}, {1, 0, 0}, 1.5f, hit) && hit.entityId == 0 && hit.distance == 0 &&
              hit.normal.x == 0,
          "ray misses reset all hit data");
    for (float distance : {0.f, -1.f, std::numeric_limits<float>::infinity(), 100001.f})
        Check(!world.Raycast({}, {1, 0, 0}, distance, hit), "invalid ray lengths are rejected");
    Check(!world.Raycast({}, {}, 10, hit) &&
              !world.Raycast({}, {std::numeric_limits<float>::quiet_NaN(), 0, 0}, 10, hit),
          "invalid directions are rejected");
    near.rotation.z = ToRadians(45);
    Check(world.Synchronize(std::array{near, far, sensor}) &&
              world.Raycast({3, 4, 0}, {0, -1, 0}, 5, hit) && hit.entityId == 1 &&
              hit.distance > 2.5f && hit.distance < 2.7f && hit.normal.y > .6f,
          "queries follow runtime shape rotations");
    world.Remove(1);
    Check(world.Raycast({}, {1, 0, 0}, 10, hit) && hit.entityId == 2,
          "removed bodies immediately disappear from ray queries");
}

void LargeWorldQueries() {
    for (double base : {-1e12, 1e12}) {
        PhysicsWorld world;
        PhysicsBodyDescription body;
        body.entity = 1;
        body.position = {base + 3.125, base, base};
        body.size = {2, 2, 2};
        Check(world.Synchronize(std::array{body}), "distant bodies synchronize");
        RaycastHit3D hit;
        Check(world.Raycast({base, base, base}, {1, 0, 0}, 10, hit) && hit.entityId == 1 &&
                  std::abs(hit.distance - 2.125) < .001 &&
                  std::abs(hit.point.x - (base + 2.125)) < .001,
              "distant ray queries preserve sub-unit world positions");
        body.type = BodyType::Dynamic;
        body.velocity = {3, 0, 0};
        Check(world.Synchronize(std::array{body}), "distant dynamic body synchronizes");
        world.Step(1.0 / 60, {});
        const auto state = world.GetState(1);
        Check(state && std::abs(state->position.x - (base + 3.175)) < .001,
              "distant simulation preserves sub-unit displacement");
        body.position.x = 2e15;
        Check(!world.Synchronize(std::array{body}) && !world.GetState(1) &&
                  !world.GetLastError().empty(),
              "out-of-range world positions retain local diagnostics");
    }
}

void Diagnostics() {
    PhysicsWorld world;
    PhysicsBodyDescription first, second;
    first.entity = 1;
    first.velocity = {3, 0, 0};
    second.entity = 2;
    second.position.x = 4;
    Check(world.Synchronize(std::array{first, second}), "diagnostic fixtures synchronize");
    Check(world.GetState(1)->velocity.x == 0, "static body state never reports authored motion");
    Check(!world.Synchronize(std::array{first, first, second}) && !world.GetState(1) &&
              world.GetState(2) && world.GetLastError().find("duplicate") != std::string::npos,
          "duplicate IDs remove the ambiguous body while keeping valid bodies and reporting why");
    first.mass = std::numeric_limits<float>::quiet_NaN();
    Check(!world.Synchronize(std::array{first, second}) && !world.GetState(1) &&
              world.GetLastError().find("#1") != std::string::npos,
          "invalid input identifies its entity");
    first.mass = 1;
    Check(world.Synchronize(std::array{first, second}) && world.GetLastError().empty(),
          "valid synchronization recovers and clears diagnostics");
    world.Step(-1, {});
    Check(!world.GetLastError().empty(), "invalid world steps are diagnosed");
    world.Step(1.0 / 60, {});
    Check(world.GetLastError().empty(), "valid world steps clear old errors");
    Scene scene;
    auto body = scene.CreateEntity("Invalid scene body");
    body.AddComponent<BoxCollider3DComponent>();
    body.AddComponent<Rigidbody3DComponent>().mass = -1;
    scene.OnUpdate(1.0 / 60);
    Check(scene.GetLastPhysicsError3D().find("#1") != std::string::npos,
          "scene exposes invalid body diagnostics");
    body.GetComponent<Rigidbody3DComponent>().mass = 1;
    scene.OnUpdate(1.0 / 60);
    RaycastHit3D hit;
    Check(scene.GetLastPhysicsError3D().empty() &&
              scene.Raycast3D({0, 3, 0}, {0, -1, 0}, 10, hit) && hit.entityId == body.GetId(),
          "scene recovers its body and provides world queries");
    scene.DestroyEntity(body);
    Check(!scene.Raycast3D({0, 3, 0}, {0, -1, 0}, 10, hit),
          "scene entity destruction also removes its query target");
    auto parent = scene.CreateEntity("Scaled parent");
    parent.GetComponent<TransformComponent>().scale = {2, 1, 1};
    auto child = scene.CreateEntity("Sheared collider");
    child.GetComponent<TransformComponent>().rotation.z = ToRadians(45);
    child.AddComponent<BoxCollider3DComponent>();
    Check(scene.SetParent(child.GetId(), parent.GetId(), false),
          "sheared collider fixture parents");
    scene.OnUpdate(1.0 / 60);
    Check(scene.GetLastPhysicsError3D().find("sheared") != std::string::npos &&
              !scene.Raycast3D({0, 3, 0}, {0, -1, 0}, 10, hit),
          "unrepresentable sheared boxes are diagnosed instead of using an incorrect collider");
    parent.GetComponent<TransformComponent>().scale = {1, 1, 1};
    scene.OnUpdate(1.0 / 60);
    Check(scene.GetLastPhysicsError3D().empty() &&
              scene.Raycast3D({0, 3, 0}, {0, -1, 0}, 10, hit) && hit.entityId == child.GetId(),
          "fixing a sheared transform restores its collider");
    child.GetComponent<BoxCollider3DComponent>().size.x = -1;
    scene.OnUpdate(1.0 / 60);
    Check(!scene.GetLastPhysicsError3D().empty() &&
              !scene.Raycast3D({0, 3, 0}, {0, -1, 0}, 10, hit),
          "negative collider dimensions are rejected instead of being silently made positive");
    child.GetComponent<BoxCollider3DComponent>().size.x = 1;
    parent.GetComponent<TransformComponent>().scale.x = -1;
    scene.OnUpdate(1.0 / 60);
    Check(scene.GetLastPhysicsError3D().empty() && scene.Raycast3D({0, 3, 0}, {0, -1, 0}, 10, hit),
          "valid negative transform scales still support mirrored colliders");
    Scene planar;
    auto flat = planar.CreateEntity("Planar invalid collider");
    flat.AddComponent<BoxCollider2DComponent>().size.x = -1;
    flat.AddComponent<Rigidbody2DComponent>();
    planar.OnUpdate(1.0 / 60);
    Check(!planar.GetLastPhysicsError2D().empty(), "2D negative dimensions are diagnosed too");
    flat.GetComponent<BoxCollider2DComponent>().size.x = 1;
    flat.GetComponent<TransformComponent>().scale.x = -1;
    planar.OnUpdate(1.0 / 60);
    Check(planar.GetLastPhysicsError2D().empty() &&
              flat.GetComponent<Rigidbody2DComponent>().velocity.y < 0,
          "fixing a mirrored 2D collider restores simulation");
}

void MotionAndEdits() {
    {
        PhysicsWorld world;
        PhysicsBodyDescription body;
        body.entity = 1;
        body.type = BodyType::Dynamic;
        body.velocity = {100000, 100000, 100000};
        Check(world.Synchronize(std::array{body}), "bounded diagonal velocity is accepted");
        world.Step(1.0 / 60, {});
        const auto velocity = world.GetState(1)->velocity;
        Check(
            std::abs(velocity.x - 100000) < 1 && std::abs(velocity.y - 100000) < 1 &&
                std::abs(velocity.z - 100000) < 1,
            "accepted per-axis velocities must not be silently clamped by Jolt's magnitude limit");
    }
    for (bool planar : {false, true}) {
        PhysicsWorld world(planar);
        PhysicsBodyDescription wall, body;
        wall.entity = 1;
        wall.size = {.05f, 10, 10};
        body.entity = 2;
        body.type = BodyType::Dynamic;
        body.size = {.2f, .2f, .2f};
        body.position.x = -2;
        body.velocity.x = 600;
        Check(world.Synchronize(std::array{wall, body}), "CCD fixtures synchronize");
        world.Step(1.0 / 60, {});
        Check(world.GetState(2)->position.x < .1f, "CCD prevents a fast body crossing a thin wall");
        wall.trigger = true;
        body.type = BodyType::Kinematic;
        body.position.x = -.2f;
        body.velocity.x = 3;
        Check(world.Synchronize(std::array{wall, body}), "kinematic trigger fixture synchronizes");
        bool triggered = false;
        for (int i = 0; i < 12; ++i) {
            world.Step(1.0 / 60, {});
            for (const auto& contact : world.BeganContacts())
                triggered = triggered || contact.isTrigger;
        }
        Check(triggered, "kinematic actors activate authored static trigger volumes");
        body.type = BodyType::Dynamic;
        body.position = {-2, 0, 0};
        body.velocity = {};
        body.mass = 3;
        body.offset = {.3f, 0, 0};
        body.size = {2, 1, 1};
        Check(world.Synchronize(std::array{body}),
              "runtime type, mass, offset and size edits synchronize");
        RaycastHit3D hit;
        Check(world.Raycast({-5, 0, 0}, {1, 0, 0}, 5, hit) && std::abs(hit.distance - 2.3f) < .02f,
              "edited collider offsets and extents reach the physical query shape");
        world.Step(1.0 / 60, {0, -9.81f, 0});
        Check(world.GetState(2)->position.y < 0, "edited dynamic bodies continue simulating");
    }
}

void GameplayControl() {
    Scene scene;
    auto floor = scene.CreateEntity("Ground");
    floor.GetComponent<TransformComponent>().position.y = -.5f;
    floor.AddComponent<BoxCollider3DComponent>().size = {10, 1, 10};
    auto actor = scene.CreateEntity("Controlled body");
    actor.GetComponent<TransformComponent>().position.y = 2;
    actor.AddComponent<BoxCollider3DComponent>();
    actor.AddComponent<Rigidbody3DComponent>().mass = 2;
    for (int i = 0; i < 240; ++i)
        scene.OnUpdate(1.0 / 60);
    Check(scene.IsGrounded3D(actor.GetId()), "gameplay API reads physical support");
    Check(scene.SetVelocity3D(actor.GetId(), {}) && scene.AddImpulse3D(actor.GetId(), {0, 4, 0}),
          "dynamic body accepts velocity control and a central impulse");
    Vec3 velocity;
    Check(scene.GetVelocity3D(actor.GetId(), velocity) && std::abs(velocity.y - 2) < .0001f,
          "impulses respect authored body mass");
    const float before = scene.GetWorldTransform(actor.GetId()).position.y;
    scene.OnUpdate(1.0 / 60);
    Check(scene.GetWorldTransform(actor.GetId()).position.y > before,
          "the impulse survives scene synchronization and moves the physical body");
    actor.GetComponent<Rigidbody3DComponent>().type = BodyType::Static;
    Check(!scene.IsGrounded3D(actor.GetId()) && !scene.SetVelocity3D(actor.GetId(), {1, 0, 0}) &&
              !scene.AddImpulse3D(actor.GetId(), {1, 0, 0}),
          "static bodies reject movement and dynamic impulses");
    actor.GetComponent<Rigidbody3DComponent>().type = BodyType::Kinematic;
    Check(scene.SetVelocity3D(actor.GetId(), {2, 0, 0}) &&
              !scene.AddImpulse3D(actor.GetId(), {1, 0, 0}),
          "kinematic bodies accept velocity but reject dynamic impulses");
    Check(!scene.SetVelocity3D(actor.GetId(), {std::numeric_limits<float>::quiet_NaN(), 0, 0}),
          "invalid gameplay values do not corrupt body velocity");
    scene.DestroyEntity(actor);
    Check(!scene.GetVelocity3D(actor.GetId(), velocity) && velocity.x == 0 &&
              !scene.IsGrounded3D(actor.GetId()),
          "destroyed entities return empty gameplay state");
}

void KinematicTransformCarriesBox(uint32 substeps) {
    Scene scene;
    auto settings = scene.GetSettings();
    settings.physicsSubsteps = substeps;
    Check(scene.SetSettings(settings), "apply kinematic test substeps");
    auto platform = scene.CreateEntity("Kinematic platform");
    platform.AddComponent<BoxCollider3DComponent>().size = {12, 1, 12};
    platform.AddComponent<Rigidbody3DComponent>().type = BodyType::Kinematic;
    auto box = scene.CreateEntity("Box");
    box.AddComponent<BoxCollider3DComponent>();
    box.AddComponent<Rigidbody3DComponent>();
    box.GetComponent<TransformComponent>().position.y = 1.05;

    constexpr double dt = 1.0 / 60;
    for (int i = 0; i < 120; ++i)
        scene.OnUpdate(dt);
    Check(scene.IsGrounded3D(box.GetId()), "box is resting on the platform");
    auto& position = platform.GetComponent<TransformComponent>().position;
    for (int i = 0; i < 180; ++i) {
        position.x += dt;
        scene.OnUpdate(dt);
    }
    Check(std::abs(position.x - 3) < .01 && box.GetComponent<TransformComponent>().position.x > 1.5,
          "a transform-driven kinematic platform imparts motion to a contacting box");
    for (int i = 0; i < 60; ++i)
        scene.OnUpdate(dt);
    Check(std::abs(position.x - 3) < .01,
          "a served kinematic target does not leave residual velocity");
}

void CapacityAndReplacement() {
    PhysicsWorld world;
    std::vector<PhysicsBodyDescription> bodies(16384);
    for (size_t i = 0; i < bodies.size(); ++i) {
        bodies[i].entity = static_cast<uint32>(i + 1);
        bodies[i].position = {static_cast<float>(i % 128) * 2, static_cast<float>(i / 128) * 2, 0};
    }
    Check(world.Synchronize(bodies) && world.BodyCount() == bodies.size(),
          "the documented body capacity is available");
    for (auto& body : bodies)
        body.entity += 16384;
    Check(world.Synchronize(bodies) && world.BodyCount() == bodies.size(),
          "a full-capacity snapshot replacement releases old bodies before allocating new ones");
    auto extra = bodies.back();
    ++extra.entity;
    bodies.push_back(extra);
    Check(!world.Synchronize(bodies) && world.BodyCount() == 16384 &&
              world.GetLastError().find("limit") != std::string::npos,
          "capacity overflow is bounded and diagnosed without a native assertion");
    Check(world.Synchronize({}) && world.BodyCount() == 0 && world.GetLastError().empty(),
          "a large world can be cleared and reused");
}

void StableContactIdentity() {
    for (bool planar : {false, true}) {
        PhysicsWorld world(planar);
        PhysicsBodyDescription sensor, actor;
        sensor.entity = 1;
        sensor.size = {4, 4, 4};
        sensor.trigger = true;
        actor.entity = 2;
        actor.type = BodyType::Dynamic;
        Check(world.Synchronize(std::array{sensor, actor}), "stable contact fixtures synchronize");
        world.Step(1.0 / 60, {});
        Check(world.BeganContacts().size() == 1, "a new sensor overlap starts once");
        actor.mass = 3;
        Check(world.Synchronize(std::array{sensor, actor}), "live mass edit synchronizes");
        world.Step(1.0 / 60, {});
        Check(world.BeganContacts().empty() && !world.Contacts().empty(),
              "changing mass during an overlap must not restart the gameplay trigger");
        actor.size = {.5f, .5f, .5f};
        Check(world.Synchronize(std::array{sensor, actor}), "live shape edit synchronizes");
        world.Step(1.0 / 60, {});
        Check(world.BeganContacts().empty() && !world.Contacts().empty(),
              "changing a shape while still overlapping preserves logical contact identity");
        actor.position.x = 10;
        Check(world.Synchronize(std::array{sensor, actor}), "moving out synchronizes");
        world.Step(1.0 / 60, {});
        Check(world.Contacts().empty(), "leaving the volume ends the physical overlap");
        actor.position.x = 0;
        Check(world.Synchronize(std::array{sensor, actor}), "returning synchronizes");
        world.Step(1.0 / 60, {});
        Check(world.BeganContacts().size() == 1, "a genuine re-entry starts a new trigger");
    }
}

void PlanarColliderHierarchy() {
    Scene scene;
    auto settings = scene.GetSettings();
    settings.overrideGravity = true;
    settings.gravity = {};
    Check(scene.SetSettings(settings), "planar hierarchy settings");
    auto parent = scene.CreateEntity("Parent");
    auto child = scene.CreateEntity("Planar body");
    child.GetComponent<TransformComponent>().rotation.z = .53;
    Check(scene.SetParent(child.GetId(), parent.GetId(), false), "body attaches to parent");
    child.AddComponent<BoxCollider2DComponent>();
    auto& body = child.AddComponent<Rigidbody2DComponent>();
    body.type = BodyType::Dynamic;
    body.velocity = {4, 0};
    scene.OnUpdate(settings.fixedStep);
    Check(scene.GetWorldTransform(child.GetId()).position.x > 0 &&
              scene.GetLastPhysicsError2D().empty(),
          "an ordinary parent does not prevent planar motion");
    parent.GetComponent<TransformComponent>().scale = {2, 1, 1};
    body.isGrounded = true;
    scene.OnUpdate(settings.fixedStep);
    const auto& error = scene.GetLastPhysicsError2D();
    Check(error.find("#" + std::to_string(child.GetId())) != std::string::npos &&
              error.find("sheared") != std::string::npos && !body.isGrounded,
          "a sheared planar collider reports the responsible entity and clears contact state");
    parent.GetComponent<TransformComponent>().scale = {1, 1, 1};
    const auto before = scene.GetWorldTransform(child.GetId()).position.x;
    scene.OnUpdate(settings.fixedStep);
    Check(scene.GetWorldTransform(child.GetId()).position.x > before &&
              scene.GetLastPhysicsError2D().empty(),
          "correcting the parent transform restores planar motion");
    auto& local = child.GetComponent<TransformComponent>();
    local.position.z = 3;
    local.scale.z = 2;
    parent.GetComponent<TransformComponent>().scale.z = 0;
    const auto beforeFlattening = scene.GetWorldTransform(child.GetId());
    scene.OnUpdate(settings.fixedStep);
    const auto afterFlattening = scene.GetWorldTransform(child.GetId());
    Check(afterFlattening.position.x > beforeFlattening.position.x &&
              std::abs(afterFlattening.rotation.z - beforeFlattening.rotation.z) < 1e-5 &&
              local.position.z == 3 && local.scale.z == 2 && scene.GetLastPhysicsError2D().empty(),
          "a collapsed parent Z axis leaves planar motion, rotation and authored depth intact");
}

void MixedPhysicsHierarchy() {
    Scene scene;
    auto settings = scene.GetSettings();
    settings.physicsSubsteps = 8;
    Check(scene.SetSettings(settings), "mixed physics hierarchy settings");
    auto child = scene.CreateEntity("Planar child");
    child.GetComponent<TransformComponent>().position.x = 10;
    child.AddComponent<BoxCollider2DComponent>();
    child.AddComponent<BoxCollider3DComponent>();
    auto& planar = child.AddComponent<Rigidbody2DComponent>();
    planar.type = BodyType::Kinematic;
    planar.velocity = {3, 0};
    auto group = scene.CreateEntity("Group without a collider");
    auto parent = scene.CreateEntity("Spatial parent");
    parent.AddComponent<BoxCollider3DComponent>();
    auto& spatial = parent.AddComponent<Rigidbody3DComponent>();
    spatial.type = BodyType::Kinematic;
    spatial.velocity = {2, 0, 0};
    Check(scene.SetParent(child.GetId(), group.GetId(), false) &&
              scene.SetParent(group.GetId(), parent.GetId(), false),
          "mixed bodies are linked through a non-physics transform");

    for (int frame = 0; frame < 30; ++frame)
        scene.OnUpdate(settings.fixedStep);
    Check(std::abs(scene.GetWorldTransform(parent.GetId()).position.x - 1) < .01 &&
              std::abs(scene.GetWorldTransform(child.GetId()).position.x - 11.5) < .01 &&
              scene.GetLastPhysicsError2D().empty() && scene.GetLastPhysicsError3D().empty(),
          "mixed physics writes parent before child and gives the planar collider precedence");
    RaycastHit3D hit;
    Check(!scene.Raycast3D({11.5, 0, 2}, {0, 0, -1}, 4, hit),
          "a planar body does not also register a spatial collider");
}

void FlatColliderContact(bool parented) {
    Scene scene;
    auto settings = scene.GetSettings();
    settings.overrideGravity = true;
    settings.gravity = {};
    Check(scene.SetSettings(settings), "flat collider settings");
    auto wall = scene.CreateEntity("Vertical wall");
    wall.GetComponent<TransformComponent>().scale = {1, 1, 0};
    wall.GetComponent<TransformComponent>().rotation.z = ToRadians(90);
    wall.AddComponent<BoxCollider2DComponent>().size = {4, .25};
    if (parented) {
        auto parent = scene.CreateEntity("Parent");
        Check(scene.SetParent(wall.GetId(), parent.GetId(), false), "flat wall attaches to parent");
    }
    auto actor = scene.CreateEntity("Actor");
    actor.GetComponent<TransformComponent>().position = {0, 1, 0};
    actor.AddComponent<Rigidbody2DComponent>().type = BodyType::Dynamic;
    actor.AddComponent<BoxCollider2DComponent>().size = {.2f, .2f};
    scene.OnUpdate(settings.fixedStep);
    Check(scene.GetCollisionEvents2D().size() == 1 && scene.GetLastPhysicsError2D().empty(),
          "a rotated flat collider makes the same contact with or without a parent");
}
} // namespace

int main() {
    try {
        Queries();
        LargeWorldQueries();
        Diagnostics();
        MotionAndEdits();
        GameplayControl();
        for (const uint32 substeps : {1, 2, 4, 8})
            KinematicTransformCarriesBox(substeps);
        CapacityAndReplacement();
        StableContactIdentity();
        PlanarColliderHierarchy();
        MixedPhysicsHierarchy();
        FlatColliderContact(false);
        FlatColliderContact(true);
        std::cout << "Jolt queries, diagnostics, CCD, triggers and live body edits passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
