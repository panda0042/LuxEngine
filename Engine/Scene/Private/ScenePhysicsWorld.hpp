#pragma once

#include <LuxEngine/ECS/ECS.hpp>
#include <LuxEngine/Physics/PhysicsWorld.hpp>
#include <span>
#include <unordered_map>

namespace LuxEngine {
class Scene;

class ScenePhysicsWorld final {
public:
    explicit ScenePhysicsWorld(bool planar);
    void Step(Scene& scene, std::span<const uint32> entities, double dt, uint32 index, uint32 count,
              const std::unordered_map<uint32, Vec3>& rootMotion);
    void Remove(uint32 entity);
    std::unordered_map<uint32, TransformComponent> GetWorldPoses(const Scene& scene) const;
    const std::vector<Collision3DEvent>& BeganContacts() const;
    const std::string& GetLastError() const;
    void SetPoseError(uint32 entity);
    bool IsGrounded(uint32 entity) const;
    bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit3D& hit,
                 uint32 ignoreEntity, bool includeTriggers) const;

private:
    bool planar_;
    PhysicsWorld world_;
    std::vector<uint32> active_;
    std::unordered_map<uint32, TransformComponent> kinematicTargets_;
    std::string error_;
    std::string lastError_;
    std::string poseError_;
};
} // namespace LuxEngine
