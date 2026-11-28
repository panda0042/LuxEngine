#pragma once

#include <LuxEngine/ECS/ECS.hpp>
#include <LuxEngine/Physics/PhysicsWorld.hpp>
#include <optional>
#include <span>
#include <unordered_map>

namespace LuxEngine {
class Scene;

class ScenePhysicsWorld final {
public:
    explicit ScenePhysicsWorld(bool planar);
    void Step(Scene& scene, std::span<const uint32> entities, double dt, uint32 index, uint32 count,
              const std::unordered_map<uint32, Vec3>& rootMotion,
              std::span<const uint32> characters = {});
    void Remove(uint32 entity);
    void RememberKinematicLocalPoses(const Scene& scene);
    std::unordered_map<uint32, TransformComponent>
    GetWorldPoses(const Scene& scene, std::span<const uint32> characters = {}) const;
    const std::vector<Collision3DEvent>& BeganContacts() const;
    const std::string& GetLastError() const;
    void SetPoseError(uint32 entity);
    bool IsGrounded(uint32 entity) const;
    bool Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit3D& hit,
                 uint32 ignoreEntity, bool includeTriggers) const;

private:
    void StepCharacters(Scene& scene, std::span<const uint32> characters, double dt, uint32 index,
                        uint32 count, const std::unordered_map<uint32, Vec3>& rootMotion,
                        const Vec3& gravity);
    bool planar_;
    PhysicsWorld world_;
    std::vector<uint32> active_;
    std::unordered_map<uint32, TransformComponent> kinematicTargets_;

    struct LocalPose {
        uint32 parent;
        Vec3 position;
        Vec3 rotation;
        Mat4 parentWorld;
    };

    struct LocalKinematicMotion {
        Vec3 startPosition;
        Vec3 targetPosition;
        Vec3 startRotation;
        Vec3 targetRotation;
        Mat4 parentWorld;
        Mat4 inverseParentWorld;

        Vec3 StepVelocity(const Vec3& worldPoint, double dt, uint32 index, uint32 count) const;
    };

    struct InheritedKinematicMotion {
        uint32 parent;
        std::optional<LocalKinematicMotion> localMotion;
    };

    std::unordered_map<uint32, LocalPose> previousKinematicLocals_;
    std::unordered_map<uint32, InheritedKinematicMotion> inheritedKinematicMotion_;
    std::string error_;
    std::string lastError_;
    std::string poseError_;
};
} // namespace LuxEngine
