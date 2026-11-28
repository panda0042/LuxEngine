#include "ScenePhysicsWorld.hpp"
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace LuxEngine {
namespace {
bool FiniteMatrix(const Mat4& matrix) {
    return std::all_of(std::begin(matrix.elements), std::end(matrix.elements),
                       [](double value) { return std::isfinite(value); });
}

bool OrthogonalPhysicsAxes(const Mat4& matrix, int count) {
    const auto* m = matrix.elements;
    const Vec3 axes[] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
    const double lengths[] = {std::hypot(m[0], m[1], m[2]), std::hypot(m[4], m[5], m[6]),
                              std::hypot(m[8], m[9], m[10])};
    for (int axis = 0; axis < count; ++axis)
        if (!std::isfinite(lengths[axis]) || lengths[axis] <= 0)
            return false;
    for (int a = 0; a < count; ++a)
        for (int b = a + 1; b < count; ++b) {
            const double dot = (axes[a].x / lengths[a]) * (axes[b].x / lengths[b]) +
                               (axes[a].y / lengths[a]) * (axes[b].y / lengths[b]) +
                               (axes[a].z / lengths[a]) * (axes[b].z / lengths[b]);
            if (std::abs(dot) > 1e-4)
                return false;
        }
    return true;
}

bool UniformScale(const Vec3& scale) {
    const auto [minimum, maximum] =
        std::minmax({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)});
    return minimum > 0 && maximum - minimum <= maximum * 1e-6;
}

bool SameCoordinates(const Vec3& a, const Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
} // namespace

ScenePhysicsWorld::ScenePhysicsWorld(bool planar) : planar_(planar), world_(planar) {}

void ScenePhysicsWorld::Remove(uint32 entity) {
    world_.Remove(entity);
    if (!planar_)
        world_.RemoveCharacter(entity);
    kinematicTargets_.erase(entity);
    previousKinematicLocals_.erase(entity);
    inheritedKinematicMotion_.erase(entity);
}

void ScenePhysicsWorld::RememberKinematicLocalPoses(const Scene& scene) {
    previousKinematicLocals_.clear();
    for (const auto id : active_) {
        const auto* rigid = scene.TryGetComponent<Rigidbody3DComponent>(id);
        const auto parent = scene.GetParent(id);
        if (rigid && rigid->type == BodyType::Kinematic && parent) {
            const auto& local = scene.GetComponent<TransformComponent>(id);
            previousKinematicLocals_.emplace(id, LocalPose{parent, local.position, local.rotation,
                                                           scene.GetWorldMatrix(parent)});
        }
    }
}

void ScenePhysicsWorld::Step(Scene& scene, std::span<const uint32> entities, double dt,
                             uint32 index, uint32 count,
                             const std::unordered_map<uint32, Vec3>& rootMotion,
                             std::span<const uint32> characters) {
    if (index == 0) {
        kinematicTargets_.clear();
        inheritedKinematicMotion_.clear();
        if (characters.empty())
            previousKinematicLocals_.clear();
    }
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
        const auto* capsule =
            planar_ ? nullptr : scene.TryGetComponent<CapsuleCollider3DComponent>(id);
        const auto* box = planar_ ? nullptr : scene.TryGetComponent<BoxCollider3DComponent>(id);
        const auto* mesh =
            planar_ ? nullptr : scene.TryGetComponent<TriangleMeshCollider3DComponent>(id);
        const auto* rigid3D = planar_ ? nullptr : scene.TryGetComponent<Rigidbody3DComponent>(id);
        if ((capsule && box) || (mesh && (box || capsule))) {
            if (error_.empty())
                error_ = "Jolt physics entity #" + std::to_string(id) +
                         (mesh ? ": Triangle mesh and another 3D collider cannot coexist"
                               : ": Box and Capsule colliders cannot coexist");
            continue;
        }
        if (mesh && rigid3D && rigid3D->type != BodyType::Static) {
            if (error_.empty())
                error_ = "Jolt physics entity #" + std::to_string(id) +
                         ": Triangle mesh requires a static body";
            continue;
        }
        {
            const auto matrix = scene.GetWorldMatrix(id);
            if (!FiniteMatrix(matrix)) {
                if (error_.empty())
                    error_ = "Jolt physics entity #" + std::to_string(id) +
                             ": non-finite world transform";
                continue;
            }
            if (!OrthogonalPhysicsAxes(matrix, planar_ ? 2 : 3)) {
                if (error_.empty())
                    error_ = "Jolt physics entity #" + std::to_string(id) +
                             ": degenerate or sheared collider transform is unsupported";
                continue;
            }
        }
        TransformComponent transform;
        try {
            transform = scene.GetWorldTransform(id);
        } catch (const std::logic_error&) {
            if (error_.empty())
                error_ = "Jolt physics entity #" + std::to_string(id) + ": invalid world transform";
            continue;
        }
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
            if (mesh) {
                body.shape = ColliderShape3D::TriangleMesh;
                body.size = transform.scale;
                body.mesh = mesh->mesh;
            } else {
                const Vec3 scale{std::abs(transform.scale.x), std::abs(transform.scale.y),
                                 std::abs(transform.scale.z)};
                if (capsule) {
                    if (!UniformScale(scale)) {
                        if (error_.empty())
                            error_ = "Jolt physics entity #" + std::to_string(id) +
                                     ": Capsule requires uniform world scale";
                        continue;
                    }
                    body.shape = ColliderShape3D::Capsule;
                    body.radius = capsule->radius * scale.y;
                    body.halfHeight = capsule->halfHeight * scale.y;
                } else
                    body.size = {box->size.x * scale.x, box->size.y * scale.y,
                                 box->size.z * scale.z};
                const auto& offset = capsule ? capsule->offset : box->offset;
                body.offset = {offset.x * transform.scale.x, offset.y * transform.scale.y,
                               offset.z * transform.scale.z};
                body.trigger = capsule ? capsule->isTrigger : box->isTrigger;
            }
            if (rigid3D) {
                body.type = rigid3D->type;
                body.velocity = rigid3D->velocity;
                body.gravityScale = rigid3D->gravityScale;
                body.mass = rigid3D->mass;
            }
        }
        if (index == 0 && !characters.empty() && body.type == BodyType::Kinematic) {
            const auto previous = previousKinematicLocals_.find(id);
            if (previous != previousKinematicLocals_.end() &&
                previous->second.parent == scene.GetParent(id)) {
                const auto parent = previous->second.parent;
                const auto& local = scene.GetComponent<TransformComponent>(id);
                if (SameCoordinates(local.position, previous->second.position) &&
                    SameCoordinates(local.rotation, previous->second.rotation)) {
                    inheritedKinematicMotion_.emplace(id, InheritedKinematicMotion{parent, {}});
                } else if (!scene.GetParent(parent) &&
                           scene.GetPhysicsCollider(parent) == Scene::PhysicsCollider::None) {
                    const auto parentWorld = scene.GetWorldMatrix(parent);
                    const auto* current = parentWorld.elements;
                    const auto* last = previous->second.parentWorld.elements;
                    const Vec3 move{current[12] - last[12], 0, current[14] - last[14]};
                    const auto velocity = move * (1 / (dt * count));
                    if (FiniteMatrix(parentWorld) && std::equal(current, current + 12, last) &&
                        current[13] == last[13] && !SameCoordinates(move, {}) &&
                        std::hypot(velocity.x, velocity.z) <= PhysicsWorld::MaxLinearVelocity) {
                        inheritedKinematicMotion_.emplace(
                            id, InheritedKinematicMotion{parent, velocity});
                    }
                }
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
    world_.Step(dt, gravity, !characters.empty());
    if (error_.empty())
        error_ = world_.GetLastError();
    if (!characters.empty()) {
        try {
            StepCharacters(scene, characters, dt, count, rootMotion, gravity);
        } catch (...) {
            world_.RestoreKinematicVelocities();
            throw;
        }
        world_.RestoreKinematicVelocities();
    }
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

void ScenePhysicsWorld::StepCharacters(Scene& scene, std::span<const uint32> characters, double dt,
                                       uint32 count,
                                       const std::unordered_map<uint32, Vec3>& rootMotion,
                                       const Vec3& gravity) {
    std::vector<uint32> ancestors;
    for (const auto id : characters) {
        auto& motion = scene.characterMotions_[id];
        const double jump = std::exchange(motion.jumpSpeed, 0.0);
        motion.state.reset();
        const auto reject = [&](std::string_view reason) {
            motion.lastLocalPosition.reset();
            world_.RemoveCharacter(id);
            if (error_.empty())
                error_ = "Jolt character #" + std::to_string(id) + ": " + std::string(reason);
        };
        if (scene.GetPhysicsCollider(id) != Scene::PhysicsCollider::Character) {
            reject("cannot coexist with a rigid body or collider");
            continue;
        }
        const auto* local = scene.TryGetComponent<TransformComponent>(id);
        if (!local) {
            reject("requires a Transform component");
            continue;
        }
        const auto matrix = scene.GetWorldMatrix(id);
        if (!FiniteMatrix(matrix) || !OrthogonalPhysicsAxes(matrix, 3)) {
            reject("non-finite, degenerate or sheared world transform");
            continue;
        }
        TransformComponent pose;
        try {
            pose = scene.GetWorldTransform(id);
        } catch (const std::logic_error&) {
            reject("invalid world transform");
            continue;
        }
        if (!UniformScale(pose.scale)) {
            reject("requires uniform world scale");
            continue;
        }
        const double scale = std::abs(pose.scale.y);
        auto settings = scene.GetComponent<CharacterController3DComponent>(id);
        settings.radius *= scale;
        settings.halfHeight *= scale;
        settings.stepHeight *= scale;
        auto velocity = motion.horizontalVelocity;
        if (const auto animated = rootMotion.find(id); animated != rootMotion.end()) {
            velocity.x += animated->second.x / (dt * count);
            velocity.z += animated->second.z / (dt * count);
        }
        CharacterController3DState state;
        const auto directParent = scene.GetParent(id);
        const auto& last = motion.lastLocalPosition;
        const bool preserveVerticalVelocity = directParent && directParent == motion.lastParent &&
                                              last && SameCoordinates(local->position, *last);
        ancestors.clear();
        for (auto parent = directParent; parent; parent = scene.GetParent(parent))
            ancestors.push_back(parent);
        const auto supportMotionFilter = [this, &ancestors](uint32 platform, const Vec3& velocity) {
            if (std::ranges::find(ancestors, platform) != ancestors.end())
                return Vec3{};
            const auto inherited = inheritedKinematicMotion_.find(platform);
            if (inherited == inheritedKinematicMotion_.end() ||
                std::ranges::find(ancestors, inherited->second.parent) == ancestors.end())
                return velocity;
            const auto& shared = inherited->second.sharedVelocity;
            return shared ? velocity - *shared : Vec3{};
        };
        if (world_.MoveCharacter(id, settings, pose.position, velocity, jump, dt, gravity, state,
                                 supportMotionFilter, preserveVerticalVelocity))
            motion.state = state;
        else {
            motion.lastLocalPosition.reset();
            if (error_.empty())
                error_ = world_.GetLastError();
            world_.RemoveCharacter(id);
        }
    }
}

std::unordered_map<uint32, TransformComponent>
ScenePhysicsWorld::GetWorldPoses(const Scene& scene, std::span<const uint32> characters) const {
    std::unordered_map<uint32, TransformComponent> poses;
    poses.reserve(active_.size() + characters.size());
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
    for (const auto id : characters) {
        CharacterController3DState state;
        if (scene.GetCharacterState3D(id, state)) {
            auto transform = scene.GetWorldTransform(id);
            transform.position = state.position;
            poses.emplace(id, transform);
        }
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
