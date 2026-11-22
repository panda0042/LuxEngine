#include "ScenePhysicsWorld.hpp"
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <cmath>

namespace LuxEngine {

ScenePhysicsWorld::ScenePhysicsWorld(bool planar) : planar_(planar), world_(planar) {}

void ScenePhysicsWorld::Remove(uint32 entity) {
    world_.Remove(entity);
    kinematicTargets_.erase(entity);
}

void ScenePhysicsWorld::Step(Scene& scene, std::span<const uint32> entities, double dt,
                             uint32 index, uint32 count,
                             const std::unordered_map<uint32, Vec3>& rootMotion) {
    if (index == 0)
        kinematicTargets_.clear();
    error_.clear();
    const auto& settings = scene.GetSettings();
    const auto gravity2D =
        settings.overrideGravity ? settings.gravity : PhysicsEngine::GetGravity2D();
    const auto gravity = planar_                    ? Vec3{gravity2D.x, gravity2D.y, 0}
                         : settings.overrideGravity ? settings.gravity3D
                                                    : PhysicsEngine::GetGravity3D();
    std::vector<PhysicsBodyDescription> descriptions;
    descriptions.reserve(entities.size());

    struct AppliedMotion {
        Vec3 drive;
        Vec3 expectedVelocity;
    };

    std::unordered_map<uint32, AppliedMotion> animationVelocities;
    active_.clear();
    for (const auto id : entities) {
        auto* rigid2D = planar_ ? scene.TryGetComponent<Rigidbody2DComponent>(id) : nullptr;
        if (rigid2D)
            rigid2D->isGrounded = false;
        {
            const auto matrix = scene.GetWorldMatrix(id);
            const auto* m = matrix.elements;
            const Vec3 axes[] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
            const double lengths[] = {std::hypot(static_cast<double>(m[0]), m[1], m[2]),
                                      std::hypot(static_cast<double>(m[4]), m[5], m[6]),
                                      std::hypot(static_cast<double>(m[8]), m[9], m[10])};
            const int axisCount = planar_ ? 2 : 3;
            bool supported = true;
            for (int axis = 0; axis < axisCount; ++axis)
                supported = supported && std::isfinite(lengths[axis]) && lengths[axis] > 1e-8;
            if (supported)
                for (int a = 0; a < axisCount; ++a)
                    for (int b = a + 1; b < axisCount; ++b) {
                        const double dot = static_cast<double>(axes[a].x) * axes[b].x +
                                           static_cast<double>(axes[a].y) * axes[b].y +
                                           static_cast<double>(axes[a].z) * axes[b].z;
                        supported = supported && std::abs(dot / (lengths[a] * lengths[b])) <= 1e-4;
                    }
            if (!supported) {
                if (error_.empty())
                    error_ = "Jolt physics entity #" + std::to_string(id) +
                             ": degenerate or sheared Box transform is unsupported";
                continue;
            }
        }
        const auto transform = scene.GetWorldTransform(id);
        PhysicsBodyDescription body;
        body.entity = id;
        body.position = transform.position;
        body.rotation = transform.rotation;
        if (planar_) {
            const auto& collider = scene.GetComponent<BoxCollider2DComponent>(id);
            body.size = {collider.size.x * std::abs(transform.scale.x),
                         collider.size.y * std::abs(transform.scale.y), .2f};
            body.offset = {collider.offset.x * transform.scale.x,
                           collider.offset.y * transform.scale.y, 0};
            body.trigger = collider.isTrigger;
            if (rigid2D) {
                body.type = rigid2D->type;
                body.velocity = {rigid2D->velocity.x, rigid2D->velocity.y, 0};
                body.gravityScale = rigid2D->gravityScale;
                body.mass = rigid2D->mass;
            }
        } else {
            const auto& collider = scene.GetComponent<BoxCollider3DComponent>(id);
            body.size = {collider.size.x * std::abs(transform.scale.x),
                         collider.size.y * std::abs(transform.scale.y),
                         collider.size.z * std::abs(transform.scale.z)};
            body.offset = {collider.offset.x * transform.scale.x,
                           collider.offset.y * transform.scale.y,
                           collider.offset.z * transform.scale.z};
            body.trigger = collider.isTrigger;
            if (const auto* rigid = scene.TryGetComponent<Rigidbody3DComponent>(id)) {
                body.type = rigid->type;
                body.velocity = rigid->velocity;
                body.gravityScale = rigid->gravityScale;
                body.mass = rigid->mass;
            }
        }
        if (body.type == BodyType::Kinematic && count > 1) {
            if (const auto previous = world_.GetState(id)) {
                const auto positionChange = body.position - previous->position;
                const auto rotationChange = body.rotation - previous->rotation;
                if (index == 0 && (positionChange.Dot(positionChange) > 0 ||
                                   rotationChange.Dot(rotationChange) > 0))
                    kinematicTargets_.emplace(id, transform);
                if (const auto target = kinematicTargets_.find(id);
                    target != kinematicTargets_.end()) {
                    const double fraction = 1.0 / (count - index);
                    body.position = previous->position +
                                    (target->second.position - previous->position) * fraction;
                    body.rotation =
                        SlerpRotation(previous->rotation, target->second.rotation, fraction);
                }
            }
        }
        if (const auto motion = rootMotion.find(id);
            body.type == BodyType::Dynamic && motion != rootMotion.end()) {
            auto velocity = motion->second * (1 / (dt * count));
            if (planar_)
                velocity.z = 0;
            body.velocity += velocity;
            animationVelocities.emplace(
                id, AppliedMotion{velocity, body.velocity + gravity * (body.gravityScale * dt)});
        }
        descriptions.push_back(body);
        active_.push_back(id);
    }
    if (!world_.Synchronize(descriptions, dt) && error_.empty())
        error_ = world_.GetLastError();
    world_.Step(dt, gravity);
    if (error_.empty())
        error_ = world_.GetLastError();
    if (!error_.empty() && error_ != lastError_)
        Log::Warn(error_);
    lastError_ = error_;
    if (!animationVelocities.empty()) {
        const auto constrain = [&](uint32 entity, Vec3 towardContact) {
            const auto motion = animationVelocities.find(entity);
            if (motion == animationVelocities.end())
                return;
            auto& applied = motion->second;
            const double toward = applied.drive.Dot(towardContact);
            if (const auto state = world_.GetState(entity); toward > 0 && state) {
                const double stopped = std::clamp(
                    (applied.expectedVelocity - state->velocity).Dot(towardContact), 0.0, toward);
                applied.drive = applied.drive - towardContact * stopped;
                applied.expectedVelocity = applied.expectedVelocity - towardContact * stopped;
            }
        };
        for (const auto& contact : world_.Contacts()) {
            if (contact.isTrigger)
                continue;
            constrain(contact.entityA, contact.normal);
            constrain(contact.entityB, contact.normal * -1);
        }
    }
    for (const auto id : active_) {
        const auto state = world_.GetState(id);
        if (!state)
            continue;
        auto velocity = state->velocity;
        if (const auto motion = animationVelocities.find(id); motion != animationVelocities.end()) {
            const auto& applied = motion->second;
            const double lengthSquared = applied.drive.Dot(applied.drive);
            if (lengthSquared > 0) {
                const auto undrivenVelocity = applied.expectedVelocity - applied.drive;
                const double remaining = std::clamp(
                    (velocity - undrivenVelocity).Dot(applied.drive) / lengthSquared, 0.0, 1.0);
                velocity = velocity - applied.drive * remaining;
            }
        }
        if (planar_) {
            if (auto* rigid = scene.TryGetComponent<Rigidbody2DComponent>(id)) {
                rigid->velocity = {velocity.x, velocity.y};
                rigid->isGrounded = state->grounded;
            }
        } else if (auto* rigid = scene.TryGetComponent<Rigidbody3DComponent>(id))
            rigid->velocity = velocity;
    }
}

std::unordered_map<uint32, TransformComponent>
ScenePhysicsWorld::GetWorldPoses(const Scene& scene) const {
    std::unordered_map<uint32, TransformComponent> poses;
    poses.reserve(active_.size());
    for (const auto id : active_) {
        const auto state = world_.GetState(id);
        if (!state || !scene.IsAlive(id))
            continue;
        auto transform = scene.GetWorldTransform(id);
        if (planar_) {
            transform.position.x = state->position.x;
            transform.position.y = state->position.y;
            transform.rotation.z = state->rotation.z;
        } else {
            transform.position = state->position;
            transform.rotation = state->rotation;
        }
        poses.emplace(id, transform);
    }
    return poses;
}

const std::vector<Collision3DEvent>& ScenePhysicsWorld::BeganContacts() const {
    return world_.BeganContacts();
}

const std::string& ScenePhysicsWorld::GetLastError() const {
    return error_.empty() ? poseError_ : error_;
}

void ScenePhysicsWorld::SetPoseError(uint32 entity) {
    std::string next;
    if (entity) {
        next = "Jolt physics entity #" + std::to_string(entity) +
               ": simulated pose cannot be applied to the scene hierarchy";
        if (next != poseError_)
            Log::Warn(next);
    }
    poseError_ = std::move(next);
}

bool ScenePhysicsWorld::IsGrounded(uint32 entity) const {
    const auto state = world_.GetState(entity);
    return state && state->grounded;
}

bool ScenePhysicsWorld::Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit3D& hit,
                                uint32 ignoreEntity, bool includeTriggers) const {
    return world_.Raycast(origin, direction, maxDistance, hit, ignoreEntity, includeTriggers);
}

} // namespace LuxEngine
