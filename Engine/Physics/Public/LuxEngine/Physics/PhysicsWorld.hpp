#pragma once

#include <LuxEngine/Physics/PhysicsEngine.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace LuxEngine {

struct PhysicsBodyDescription {
    uint32 entity = 0;
    BodyType type = BodyType::Static;
    Vec3 position;
    Vec3 rotation;
    Vec3 size{1, 1, 1};
    Vec3 offset;
    Vec3 velocity;
    float mass = 1;
    float gravityScale = 1;
    bool trigger = false;
};

struct PhysicsBodyState {
    Vec3 position;
    Vec3 rotation;
    Vec3 velocity;
    bool grounded = false;
};

struct RaycastHit3D {
    uint32 entityId = 0;
    Vec3 point{};
    Vec3 normal{};
    float distance = 0;
};

struct Collision3DEvent {
    uint32 entityA = 0;
    uint32 entityB = 0;
    Vec3 normal;
    Vec3 point;
    bool isTrigger = false;
};

// Scene-independent Jolt world. Planar worlds constrain motion to XY and rotation to Z.
class LUX_API PhysicsWorld final {
public:
    static constexpr double MinBodyExtent = 1e-6;
    explicit PhysicsWorld(bool planar = false);
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    // Descriptors are a complete world snapshot. Missing/invalid bodies are removed;
    // valid descriptors still synchronize, and false reports any invalid input.
    bool Synchronize(std::span<const PhysicsBodyDescription> bodies);
    const std::string& GetLastError() const;
    void Remove(uint32 entity);
    void Step(double dt, Vec3 gravity);
    std::optional<PhysicsBodyState> GetState(uint32 entity) const;
    const std::vector<Collision3DEvent>& Contacts() const;
    const std::vector<Collision3DEvent>& BeganContacts() const;
    size_t BodyCount() const;
    bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit3D& hit,
                 uint32 ignoreEntity = 0, bool includeTriggers = false) const;

private:
    friend class ScenePhysicsWorld;
    bool Synchronize(std::span<const PhysicsBodyDescription> bodies, double step);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace LuxEngine
