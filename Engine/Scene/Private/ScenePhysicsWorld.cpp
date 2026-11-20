#include "ScenePhysicsWorld.hpp"
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <cmath>

namespace LuxEngine {

ScenePhysicsWorld::ScenePhysicsWorld(bool planar) : planar_(planar), world_(planar) {}

void ScenePhysicsWorld::Remove(uint32 entity) {
    world_.Remove(entity);
    kinematicTargets_.erase(entity);
}

void ScenePhysicsWorld::Step(Scene& scene, double dt, uint32 index, uint32 count) {
    if (index == 0)
        kinematicTargets_.clear();
    error_.clear();
    std::vector<PhysicsBodyDescription> descriptions;
    active_.clear();
    for (const auto& entity : scene.GetEntitiesInHierarchyOrder()) {
        const auto id = entity.GetId();
        if (!scene.HasComponent<TransformComponent>(id))
            continue;
        const bool is2D = scene.HasComponent<BoxCollider2DComponent>(id) &&
                          (scene.HasComponent<Rigidbody2DComponent>(id) ||
                           !scene.HasComponent<BoxCollider3DComponent>(id));
        if (planar_ != is2D || (!planar_ && !scene.HasComponent<BoxCollider3DComponent>(id)))
            continue;
        if (!planar_) {
            const auto matrix = scene.GetWorldMatrix(id);
            const auto* m = matrix.elements;
            const Vec3 axes[] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
            const double lengths[] = {std::hypot(static_cast<double>(m[0]), m[1], m[2]),
                                      std::hypot(static_cast<double>(m[4]), m[5], m[6]),
                                      std::hypot(static_cast<double>(m[8]), m[9], m[10])};
            bool supported = true;
            for (int axis = 0; axis < 3; ++axis)
                supported = supported && std::isfinite(lengths[axis]) && lengths[axis] > 1e-8;
            if (supported)
                for (int a = 0; a < 3; ++a)
                    for (int b = a + 1; b < 3; ++b) {
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
            if (auto* rigid = scene.TryGetComponent<Rigidbody2DComponent>(id)) {
                body.type = rigid->type;
                body.velocity = {rigid->velocity.x, rigid->velocity.y, 0};
                body.gravityScale = rigid->gravityScale;
                body.mass = rigid->mass;
                rigid->isGrounded = false;
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
        descriptions.push_back(body);
        active_.push_back(id);
    }
    if (!world_.Synchronize(descriptions, dt) && error_.empty())
        error_ = world_.GetLastError();
    const auto& settings = scene.GetSettings();
    const auto gravity2D =
        settings.overrideGravity ? settings.gravity : PhysicsEngine::GetGravity2D();
    world_.Step(dt, planar_                    ? Vec3{gravity2D.x, gravity2D.y, 0}
                    : settings.overrideGravity ? settings.gravity3D
                                               : PhysicsEngine::GetGravity3D());
    if (error_.empty())
        error_ = world_.GetLastError();
    if (!error_.empty() && error_ != lastError_)
        Log::Warn(error_);
    lastError_ = error_;
    for (const auto id : active_) {
        const auto state = world_.GetState(id);
        if (!state)
            continue;
        if (planar_) {
            if (auto* rigid = scene.TryGetComponent<Rigidbody2DComponent>(id)) {
                rigid->velocity = {state->velocity.x, state->velocity.y};
                rigid->isGrounded = state->grounded;
            }
        } else if (auto* rigid = scene.TryGetComponent<Rigidbody3DComponent>(id))
            rigid->velocity = state->velocity;
    }
}

std::unordered_map<uint32, TransformComponent>
ScenePhysicsWorld::GetWorldPoses(const Scene& scene) const {
    std::unordered_map<uint32, TransformComponent> poses;
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
    return error_;
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
