// Jolt requires its platform definitions before every other Jolt header.
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#ifndef JPH_DOUBLE_PRECISION
#error "LuxEngine requires Jolt built with DOUBLE_PRECISION=ON"
#endif
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Physics/PhysicsWorld.hpp>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>

namespace LuxEngine {
namespace {
constexpr size_t MaxBodies = 16384;
constexpr double MaxBodyExtent = 10000;

void JoltTrace(const char* format, ...) {
    char buffer[2048];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    try {
        Log::Warn(buffer);
    } catch (...) {
    }
}
#ifdef JPH_ENABLE_ASSERTS
bool JoltAssertion(const char* expression, const char* message, const char*, JPH::uint) {
    try {
        Log::Error(std::string("Jolt 내부 검증 실패: ") + expression +
                   (message ? std::string(" / ") + message : std::string{}));
    } catch (...) {
    }
    return true;
}
#endif

struct JoltRuntime {
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = JoltTrace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = JoltAssertion;
#endif
        JPH::Factory::sInstance = new JPH::Factory;
        JPH::RegisterTypes();
    }

    ~JoltRuntime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

void EnsureRuntime() {
    static JoltRuntime runtime;
}

bool Valid(Vec3 value, double limit = 100000.0) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::abs(value.x) <= limit && std::abs(value.y) <= limit && std::abs(value.z) <= limit;
}

bool Same(Vec3 a, Vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool ValidCapsuleDimensions(double radius, double halfHeight) {
    constexpr double minExtent = PhysicsWorld::MinBodyExtent;
    return radius >= minExtent && halfHeight >= minExtent &&
           radius + halfHeight <= MaxBodyExtent * .5;
}

bool ValidColliderDimensions(const PhysicsBodyDescription& body) {
    constexpr double minExtent = PhysicsWorld::MinBodyExtent;
    if (body.shape == ColliderShape3D::Capsule)
        return ValidCapsuleDimensions(body.radius, body.halfHeight);
    if (body.shape == ColliderShape3D::TriangleMesh)
        return body.mesh && Valid(body.size, MaxBodyExtent) && body.size.x != 0 &&
               body.size.y != 0 && body.size.z != 0;
    return Valid(body.size, MaxBodyExtent) && body.size.x >= minExtent &&
           body.size.y >= minExtent && body.size.z >= minExtent;
}

JPH::Vec3 Vector(Vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

Vec3 Vector(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

bool SameColliderDimensions(const PhysicsBodyDescription& first,
                            const PhysicsBodyDescription& second) {
    if (first.shape != second.shape)
        return false;
    if (first.shape == ColliderShape3D::Capsule)
        return static_cast<float>(first.radius) == static_cast<float>(second.radius) &&
               static_cast<float>(first.halfHeight) == static_cast<float>(second.halfHeight);
    if (first.shape == ColliderShape3D::TriangleMesh)
        return first.mesh == second.mesh && Same(first.size, second.size);
    return Vector(first.size * .5) == Vector(second.size * .5);
}

struct MeshBounds {
    Vec3 minimum{MaxBodyExtent, MaxBodyExtent, MaxBodyExtent};
    Vec3 maximum{-MaxBodyExtent, -MaxBodyExtent, -MaxBodyExtent};

    bool Include(const Vec3& vertex) {
        if (!Valid(vertex, MaxBodyExtent))
            return false;
        minimum = {std::min(minimum.x, vertex.x), std::min(minimum.y, vertex.y),
                   std::min(minimum.z, vertex.z)};
        maximum = {std::max(maximum.x, vertex.x), std::max(maximum.y, vertex.y),
                   std::max(maximum.z, vertex.z)};
        return maximum.x - minimum.x <= MaxBodyExtent && maximum.y - minimum.y <= MaxBodyExtent &&
               maximum.z - minimum.z <= MaxBodyExtent;
    }
};

JPH::RefConst<JPH::Shape> CreateTriangleMesh(const TriangleMeshCollider3DData& mesh,
                                             const Vec3& scale, std::string& error) {
    if (!PhysicsWorld::ValidTriangleMesh(mesh)) {
        error = "invalid triangle mesh geometry";
        return {};
    }
    JPH::MeshShapeSettings settings;
    settings.mTriangleVertices.reserve(mesh.vertices.size());
    settings.mIndexedTriangles.reserve(mesh.indices.size() / 3);
    MeshBounds bounds;
    for (const Vec3& vertex : mesh.vertices) {
        const Vec3 scaled{vertex.x * scale.x, vertex.y * scale.y, vertex.z * scale.z};
        if (!bounds.Include(scaled)) {
            error = "triangle mesh exceeds scaled local bounds";
            return {};
        }
        settings.mTriangleVertices.emplace_back(static_cast<float>(scaled.x),
                                                static_cast<float>(scaled.y),
                                                static_cast<float>(scaled.z));
    }
    const bool mirrored = (scale.x < 0) ^ (scale.y < 0) ^ (scale.z < 0);
    for (size_t index = 0; index < mesh.indices.size(); index += 3) {
        const uint32 a = mesh.indices[index];
        const uint32 b = mesh.indices[index + 1];
        const uint32 c = mesh.indices[index + 2];
        settings.mIndexedTriangles.emplace_back(a, mirrored ? c : b, mirrored ? b : c);
    }
    const auto result = settings.Create();
    if (!result.IsValid()) {
        error = result.HasError() ? result.GetError().c_str() : "triangle mesh construction failed";
        return {};
    }
    return result.Get();
}

struct Layers final : JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override {
        return 2;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(layer == 0 ? 0 : 1);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == JPH::BroadPhaseLayer(0) ? "Static" : "Moving";
    }
#endif
};

struct BroadFilter final : JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer != 0 || broad == JPH::BroadPhaseLayer(1);
    }
};

struct PairFilter final : JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a != 0 || b != 0;
    }
};

struct BodyQueryFilter final : JPH::BodyFilter {
    uint32 ignored;
    bool sensors;

    BodyQueryFilter(uint32 ignoredEntity, bool includeSensors)
        : ignored(ignoredEntity), sensors(includeSensors) {}

    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return body.GetUserData() != ignored && (sensors || !body.IsSensor());
    }
};
} // namespace

struct PhysicsWorld::Impl final : JPH::ContactListener {
    struct Entry {
        JPH::BodyID body;
        PhysicsBodyDescription description;
        PhysicsBodyState state;
        bool movingToTarget = false;
    };

    struct CharacterEntry {
        JPH::Ref<JPH::CharacterVirtual> character;
        float radius = 0;
        float halfHeight = 0;
        float maxSlope = 0;
        float verticalVelocity = 0;
    };

    bool planar;
    Layers layers;
    BroadFilter broadFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl allocator{16 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    std::map<uint32, Entry> entries;
    std::map<uint32, CharacterEntry> characters;
    std::vector<Collision3DEvent> contacts, began;
    std::set<std::tuple<uint32, uint32, bool>> previousContacts;
    std::string error;
    std::string characterError;

    explicit Impl(bool plane) : planar(plane) {
        system.Init(static_cast<JPH::uint>(MaxBodies), 0, 65536, 16384, layers, broadFilter,
                    pairFilter);
        system.SetContactListener(this);
    }

    ~Impl() override {
        system.SetContactListener(nullptr);
        characters.clear();
        while (!entries.empty())
            Remove(entries.begin()->first);
    }

    void Remove(uint32 id, bool preserveContactIdentity = false) {
        const auto it = entries.find(id);
        if (it == entries.end())
            return;
        auto& bodies = system.GetBodyInterface();
        bodies.RemoveBody(it->second.body);
        bodies.DestroyBody(it->second.body);
        entries.erase(it);
        const auto involves = [id](const auto& event) {
            return event.entityA == id || event.entityB == id;
        };
        std::erase_if(contacts, involves);
        std::erase_if(began, involves);
        if (!preserveContactIdentity)
            std::erase_if(previousContacts, [id](const auto& pair) {
                return std::get<0>(pair) == id || std::get<1>(pair) == id;
            });
    }

    void RestoreKinematicVelocity(Entry& entry) {
        if (!entry.movingToTarget)
            return;
        auto& bodies = system.GetBodyInterface();
        bodies.SetLinearAndAngularVelocity(entry.body, Vector(entry.description.velocity),
                                           JPH::Vec3::sZero());
        entry.state.velocity = Vector(bodies.GetLinearVelocity(entry.body));
        entry.movingToTarget = false;
    }

    void Contact(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& manifold) {
        Collision3DEvent event;
        event.entityA = static_cast<uint32>(a.GetUserData());
        event.entityB = static_cast<uint32>(b.GetUserData());
        event.normal = Vector(manifold.mWorldSpaceNormal);
        const auto point = manifold.GetWorldSpaceContactPointOn1(0);
        event.point = {static_cast<double>(point.GetX()), static_cast<double>(point.GetY()),
                       static_cast<double>(point.GetZ())};
        event.isTrigger = a.IsSensor() || b.IsSensor();
        contacts.push_back(event);
    }

    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m,
                        JPH::ContactSettings&) override {
        Contact(a, b, m);
    }

    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold& m,
                            JPH::ContactSettings&) override {
        Contact(a, b, m);
    }
};

PhysicsWorld::PhysicsWorld(bool planar) {
    EnsureRuntime();
    impl_ = std::make_unique<Impl>(planar);
}

PhysicsWorld::~PhysicsWorld() = default;

bool PhysicsWorld::ValidTriangleMesh(const TriangleMeshCollider3DData& mesh) {
    if (mesh.vertices.size() < 3 || mesh.vertices.size() > MaxTriangleMeshVertices ||
        mesh.indices.empty() || mesh.indices.size() > MaxTriangleMeshIndices ||
        mesh.indices.size() % 3 != 0)
        return false;
    MeshBounds bounds;
    for (const Vec3& vertex : mesh.vertices)
        if (!bounds.Include(vertex))
            return false;
    for (size_t index = 0; index < mesh.indices.size(); index += 3) {
        const uint32 a = mesh.indices[index];
        const uint32 b = mesh.indices[index + 1];
        const uint32 c = mesh.indices[index + 2];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size())
            return false;
        const auto base = Vector(mesh.vertices[a]);
        if ((Vector(mesh.vertices[b]) - base).Cross(Vector(mesh.vertices[c]) - base).IsNearZero())
            return false;
    }
    return true;
}

bool PhysicsWorld::Synchronize(std::span<const PhysicsBodyDescription> input) {
    return Synchronize(input, 0);
}

bool PhysicsWorld::Synchronize(std::span<const PhysicsBodyDescription> input, double step) {
    impl_->error.clear();
    impl_->characterError.clear();
    bool valid = true;
    const auto report = [&](uint32 id, const char* reason) {
        valid = false;
        if (impl_->error.empty())
            impl_->error = "Jolt physics entity #" + std::to_string(id) + ": " + reason;
    };
    std::map<uint32, PhysicsBodyDescription> incoming;
    std::set<uint32> seen;
    for (const auto& description : input) {
        const auto id = description.entity;
        if (!seen.insert(id).second) {
            incoming.erase(id);
            report(id, "duplicate entity ID");
            continue;
        }
        const auto shape = description.shape;
        if (shape != ColliderShape3D::Box && shape != ColliderShape3D::Capsule &&
            shape != ColliderShape3D::TriangleMesh) {
            report(id, "invalid collider shape");
            continue;
        }
        if (impl_->planar && shape == ColliderShape3D::Capsule) {
            report(id, "capsule collider requires a 3D physics world");
            continue;
        }
        if (shape == ColliderShape3D::TriangleMesh &&
            (impl_->planar || description.type != BodyType::Static || description.trigger)) {
            report(id, "triangle mesh collider requires a non-trigger static 3D body");
            continue;
        }
        if (!id || !Valid(description.position, 1e15) || !Valid(description.rotation) ||
            !Valid(description.velocity, MaxLinearVelocity) || !Valid(description.offset) ||
            !ValidColliderDimensions(description) || !std::isfinite(description.mass) ||
            description.mass < .001f || description.mass > 1000000 ||
            !std::isfinite(description.gravityScale) ||
            std::abs(description.gravityScale) > 10000 || description.type < BodyType::Static ||
            description.type > BodyType::Kinematic) {
            report(id, "invalid body type, transform, velocity, mass or collider dimensions "
                       "(0.000001~10000)");
            continue;
        }
        incoming.emplace(id, description);
    }
    while (incoming.size() > MaxBodies) {
        const auto last = std::prev(incoming.end());
        report(last->first, "16384-body world limit exceeded");
        incoming.erase(last);
    }
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
        const auto id = (it++)->first;
        if (!incoming.contains(id))
            impl_->Remove(id);
    }
    auto& bodies = impl_->system.GetBodyInterface();
    for (auto [id, description] : incoming) {
        if (description.type == BodyType::Static)
            description.velocity = {};
        if (impl_->planar) {
            description.position.z = 0;
            description.velocity.z = 0;
            description.rotation.x = description.rotation.y = 0;
        }
        auto found = impl_->entries.find(id);
        if (found != impl_->entries.end()) {
            const auto& old = found->second.description;
            if (old.type != description.type || !SameColliderDimensions(old, description) ||
                Vector(old.offset) != Vector(description.offset) || old.mass != description.mass ||
                old.trigger != description.trigger) {
                impl_->Remove(id, true);
                found = impl_->entries.end();
            }
        }
        if (found == impl_->entries.end()) {
            JPH::RefConst<JPH::Shape> shape;
            if (description.shape == ColliderShape3D::Capsule) {
                shape = new JPH::CapsuleShape(static_cast<float>(description.halfHeight),
                                              static_cast<float>(description.radius));
            } else if (description.shape == ColliderShape3D::TriangleMesh) {
                std::string error;
                shape = CreateTriangleMesh(*description.mesh, description.size, error);
                if (!shape) {
                    report(id, error.c_str());
                    continue;
                }
            } else {
                const auto half = Vector(description.size * .5f);
                shape = new JPH::BoxShape(
                    half, std::min(.02f, .2f * std::min({half.GetX(), half.GetY(), half.GetZ()})));
            }
            const auto offset = Vector(description.offset);
            if (offset != JPH::Vec3::sZero())
                shape = new JPH::RotatedTranslatedShape(offset, JPH::Quat::sIdentity(), shape);
            const auto motion = description.type == BodyType::Static ? JPH::EMotionType::Static
                                : description.type == BodyType::Dynamic
                                    ? JPH::EMotionType::Dynamic
                                    : JPH::EMotionType::Kinematic;
            JPH::BodyCreationSettings settings(
                shape,
                JPH::RVec3(description.position.x, description.position.y, description.position.z),
                JPH::Quat::sEulerAngles(Vector(description.rotation)), motion,
                motion == JPH::EMotionType::Static ? 0 : 1);
            settings.mUserData = id;
            settings.mIsSensor = description.trigger;
            settings.mCollideKinematicVsNonDynamic = motion == JPH::EMotionType::Kinematic;
            settings.mAllowedDOFs =
                impl_->planar ? JPH::EAllowedDOFs::Plane2D : JPH::EAllowedDOFs::All;
            settings.mLinearVelocity = Vector(description.velocity);
            settings.mGravityFactor = description.gravityScale;
            settings.mLinearDamping = settings.mAngularDamping = 0;
            settings.mMaxLinearVelocity = MaxLinearVelocity;
            if (motion == JPH::EMotionType::Dynamic && !description.trigger)
                settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            settings.mAllowSleeping = false;
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = description.mass;
            auto* body = bodies.CreateBody(settings);
            if (!body) {
                report(id, "body allocation failed");
                continue;
            }
            bodies.AddBody(body->GetID(), JPH::EActivation::Activate);
            found = impl_->entries
                        .emplace(id, Impl::Entry{body->GetID(),
                                                 description,
                                                 {description.position, description.rotation,
                                                  description.velocity, false}})
                        .first;
        }
        auto& entry = found->second;
        const bool poseChanged = !Same(entry.state.position, description.position) ||
                                 !Same(entry.state.rotation, description.rotation);
        const bool hadKinematicTarget = entry.movingToTarget;
        if (poseChanged || hadKinematicTarget) {
            const auto position =
                JPH::RVec3(description.position.x, description.position.y, description.position.z);
            const auto rotation = JPH::Quat::sEulerAngles(Vector(description.rotation));
            if (description.type == BodyType::Kinematic && step > 0) {
                bodies.MoveKinematic(entry.body, position, rotation, static_cast<float>(step));
                entry.movingToTarget = true;
            } else {
                bodies.SetPositionAndRotation(entry.body, position, rotation,
                                              JPH::EActivation::Activate);
                entry.state.position = description.position;
                entry.state.rotation = description.rotation;
                entry.movingToTarget = false;
            }
        }
        if (!entry.movingToTarget &&
            (hadKinematicTarget || !Same(entry.state.velocity, description.velocity))) {
            if (hadKinematicTarget)
                bodies.SetLinearAndAngularVelocity(entry.body, Vector(description.velocity),
                                                   JPH::Vec3::sZero());
            else
                bodies.SetLinearVelocity(entry.body, Vector(description.velocity));
            entry.state.velocity = description.velocity;
        }
        if (description.type != BodyType::Static)
            bodies.SetGravityFactor(entry.body, description.gravityScale);
        entry.description = description;
    }
    return valid;
}

void PhysicsWorld::Remove(uint32 entity) {
    impl_->Remove(entity);
}

void PhysicsWorld::Step(double dt, Vec3 gravity) {
    Step(dt, gravity, false);
}

void PhysicsWorld::Step(double dt, Vec3 gravity, bool keepKinematicTargetVelocity) {
    impl_->contacts.clear();
    impl_->began.clear();
    impl_->error.clear();
    impl_->characterError.clear();
    if (!std::isfinite(dt) || dt <= 0 || dt > .25 || !Valid(gravity)) {
        impl_->error = "Invalid Jolt time step or gravity";
        return;
    }
    impl_->system.SetGravity(Vector(gravity));
    if (impl_->system.Update(static_cast<float>(dt), 1, &impl_->allocator, &impl_->jobs) !=
        JPH::EPhysicsUpdateError::None) {
        impl_->error = "Jolt 물리 시뮬레이션 버퍼가 부족합니다.";
        throw std::runtime_error(impl_->error);
    }
    std::set<std::tuple<uint32, uint32, bool>> currentContacts;
    for (const auto& contact : impl_->contacts) {
        const auto pair = std::tuple{std::min(contact.entityA, contact.entityB),
                                     std::max(contact.entityA, contact.entityB), contact.isTrigger};
        if (currentContacts.insert(pair).second && !impl_->previousContacts.contains(pair))
            impl_->began.push_back(contact);
    }
    impl_->previousContacts = std::move(currentContacts);
    auto& bodies = impl_->system.GetBodyInterface();
    for (auto& [id, entry] : impl_->entries) {
        if (!keepKinematicTargetVelocity)
            impl_->RestoreKinematicVelocity(entry);
        const auto p = bodies.GetPosition(entry.body);
        entry.state = {{static_cast<double>(p.GetX()), static_cast<double>(p.GetY()),
                        static_cast<double>(p.GetZ())},
                       Vector(bodies.GetRotation(entry.body).GetEulerAngles()),
                       Vector(bodies.GetLinearVelocity(entry.body)),
                       false};
    }
    for (const auto& contact : impl_->contacts) {
        if (contact.isTrigger)
            continue;
        if (const auto a = impl_->entries.find(contact.entityA);
            contact.normal.y < -.5f && a != impl_->entries.end() &&
            a->second.description.type != BodyType::Static)
            a->second.state.grounded = true;
        if (const auto b = impl_->entries.find(contact.entityB);
            contact.normal.y > .5f && b != impl_->entries.end() &&
            b->second.description.type != BodyType::Static)
            b->second.state.grounded = true;
    }
}

void PhysicsWorld::RestoreKinematicVelocities() {
    for (auto& [id, entry] : impl_->entries)
        impl_->RestoreKinematicVelocity(entry);
}

bool PhysicsWorld::MoveCharacter(uint32 entity, const CharacterController3DComponent& settings,
                                 Vec3 position, Vec3 horizontalVelocity, double jumpSpeed,
                                 double dt, Vec3 gravity, CharacterController3DState& state) {
    return MoveCharacter(entity, settings, position, horizontalVelocity, jumpSpeed, dt, gravity,
                         state, {}, false);
}

bool PhysicsWorld::MoveCharacter(
    uint32 entity, const CharacterController3DComponent& settings, Vec3 position,
    Vec3 horizontalVelocity, double jumpSpeed, double dt, Vec3 gravity,
    CharacterController3DState& state,
    const std::function<Vec3(uint32, const Vec3&)>& supportMotionFilter,
    bool preserveVerticalVelocity) {
    state = {};
    impl_->characterError.clear();
    const auto fail = [&](const char* reason) {
        impl_->characterError = "Jolt character #" + std::to_string(entity) + ": " + reason;
        return false;
    };
    if (!entity || impl_->planar)
        return fail("character requires a valid entity in a 3D physics world");
    if (!ValidCapsuleDimensions(settings.radius, settings.halfHeight) ||
        !(settings.maxSlopeRadians >= 0 && settings.maxSlopeRadians < std::numbers::pi / 2) ||
        !(settings.stepHeight >= 0 && settings.stepHeight <= MaxBodyExtent))
        return fail("invalid capsule dimensions, slope or step height");
    if (!Valid(position, 1e15) || !Valid(horizontalVelocity, MaxLinearVelocity) ||
        !Valid(gravity) || !(jumpSpeed >= 0 && jumpSpeed <= MaxLinearVelocity) ||
        !(dt >= 1e-6 && dt <= .25))
        return fail("invalid position, velocity, jump, gravity or time step");
    if (impl_->characters.size() >= MaxBodies && !impl_->characters.contains(entity))
        return fail("16384-character world limit exceeded");

    auto& entry = impl_->characters[entity];
    const auto radius = static_cast<float>(settings.radius);
    const auto halfHeight = static_cast<float>(settings.halfHeight);
    // Jolt treats cosines near one as disabling the slope limit.
    const auto maxSlope = std::clamp(static_cast<float>(settings.maxSlopeRadians),
                                     std::acos(std::nextafter(0.9999f, 0.0f)),
                                     std::nextafter(std::numbers::pi_v<float> * .5f, 0.0f));
    const JPH::RVec3 start(position.x, position.y, position.z);
    const auto movingLayer = JPH::ObjectLayer(1);
    const auto broadFilter = impl_->system.GetDefaultBroadPhaseLayerFilter(movingLayer);
    const auto layerFilter = impl_->system.GetDefaultLayerFilter(movingLayer);
    const BodyQueryFilter bodyFilter(entity, false);

    const bool newShape =
        !entry.character || entry.radius != radius || entry.halfHeight != halfHeight;
    bool refresh = jumpSpeed > 0;
    if (newShape) {
        JPH::CharacterVirtualSettings nativeSettings;
        nativeSettings.mShape = new JPH::CapsuleShape(halfHeight, radius);
        nativeSettings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), halfHeight);
        nativeSettings.mMaxSlopeAngle = maxSlope;
        nativeSettings.mCharacterPadding = std::min(nativeSettings.mCharacterPadding, radius * .2f);
        nativeSettings.mPredictiveContactDistance =
            std::min(nativeSettings.mPredictiveContactDistance, radius * .5f);
        nativeSettings.mCollisionTolerance =
            std::min(nativeSettings.mCollisionTolerance, radius * .01f);
        nativeSettings.mMinTimeRemaining = .5e-6f;
        entry.character = new JPH::CharacterVirtual(&nativeSettings, start, JPH::Quat::sIdentity(),
                                                    entity, &impl_->system);
        entry.radius = radius;
        entry.halfHeight = halfHeight;
        entry.maxSlope = maxSlope;
        entry.verticalVelocity = 0;
        refresh = true;
    }
    auto& character = *entry.character;
    const auto samePositionAxis = [](double current, double requested) {
        const double magnitude = std::abs(current);
        const double ulp =
            std::nextafter(magnitude, std::numeric_limits<double>::infinity()) - magnitude;
        return std::abs(current - requested) <= std::max(1e-7, 4 * ulp);
    };
    const auto current = character.GetPosition();
    if (!samePositionAxis(current.GetX(), start.GetX()) ||
        !samePositionAxis(current.GetY(), start.GetY()) ||
        !samePositionAxis(current.GetZ(), start.GetZ())) {
        character.SetPosition(start);
        if (!preserveVerticalVelocity)
            entry.verticalVelocity = 0;
        refresh = true;
    }
    if (entry.maxSlope != maxSlope) {
        character.SetMaxSlopeAngle(maxSlope);
        entry.maxSlope = maxSlope;
        refresh = true;
    }
    const auto groundBody = character.GetGroundBodyID();
    const bool removedGround =
        !groundBody.IsInvalid() && !impl_->system.GetBodyInterface().IsAdded(groundBody);
    if (refresh || removedGround)
        character.RefreshContacts(broadFilter, layerFilter, bodyFilter, {}, impl_->allocator);
    else
        character.UpdateGroundVelocity();

    const bool grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    const bool jumping = grounded && jumpSpeed > 0;
    const auto step = static_cast<float>(dt);
    const auto oldPosition = character.GetPosition();
    auto velocity = Vector(horizontalVelocity);
    velocity.SetY(grounded ? static_cast<float>(jumpSpeed) : entry.verticalVelocity);
    const auto support = character.GetGroundBodyID();
    if (grounded) {
        auto supportVelocity = character.GetGroundVelocity();
        if (!support.IsInvalid() && supportMotionFilter) {
            const auto supportEntity =
                static_cast<uint32>(impl_->system.GetBodyInterface().GetUserData(support));
            const auto platform = impl_->entries.find(supportEntity);
            if (platform != impl_->entries.end() && platform->second.body == support &&
                platform->second.movingToTarget)
                supportVelocity =
                    Vector(supportMotionFilter(supportEntity, Vector(supportVelocity)));
        }
        velocity += supportVelocity;
    }
    velocity += step * Vector(gravity);
    character.SetLinearVelocity(velocity);

    JPH::CharacterVirtual::ExtendedUpdateSettings movement;
    const auto stepHeight = static_cast<float>(settings.stepHeight);
    movement.mWalkStairsStepUp = {0, grounded && !jumping ? stepHeight : 0, 0};
    movement.mStickToFloorStepDown = {0, jumping ? 0 : -stepHeight, 0};
    character.ExtendedUpdate(step, Vector(gravity), movement, broadFilter, layerFilter, bodyFilter,
                             {}, impl_->allocator);

    const auto end = character.GetPosition();
    const auto delta = end - oldPosition;
    state.position = {end.GetX(), end.GetY(), end.GetZ()};
    state.velocity = {delta.GetX() / dt, delta.GetY() / dt, delta.GetZ() / dt};
    state.grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    if (state.grounded)
        state.groundNormal = Vector(character.GetGroundNormal());
    entry.verticalVelocity =
        state.grounded ? 0
                       : static_cast<float>(std::clamp(state.velocity.y, -double(MaxLinearVelocity),
                                                       double(MaxLinearVelocity)));
    return true;
}

void PhysicsWorld::RemoveCharacter(uint32 entity) {
    impl_->characters.erase(entity);
}

std::optional<PhysicsBodyState> PhysicsWorld::GetState(uint32 entity) const {
    const auto it = impl_->entries.find(entity);
    return it == impl_->entries.end() ? std::nullopt : std::optional(it->second.state);
}

const std::vector<Collision3DEvent>& PhysicsWorld::Contacts() const {
    return impl_->contacts;
}

const std::vector<Collision3DEvent>& PhysicsWorld::BeganContacts() const {
    return impl_->began;
}

const std::string& PhysicsWorld::GetLastError() const {
    return impl_->error.empty() ? impl_->characterError : impl_->error;
}

bool PhysicsWorld::Raycast(Vec3 origin, Vec3 direction, float maxDistance, RaycastHit3D& hit,
                           uint32 ignoreEntity, bool includeTriggers) const {
    hit = {};
    const double length =
        std::hypot(static_cast<double>(direction.x), static_cast<double>(direction.y),
                   static_cast<double>(direction.z));
    if (!Valid(origin, 1e15) || !std::isfinite(length) || length < 1e-12 ||
        !std::isfinite(maxDistance) || maxDistance <= 0 || maxDistance > 100000)
        return false;
    const Vec3 unit{static_cast<float>(direction.x / length),
                    static_cast<float>(direction.y / length),
                    static_cast<float>(direction.z / length)};

    const BodyQueryFilter filter(ignoreEntity, includeTriggers);
    const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), Vector(unit * maxDistance));
    JPH::RayCastResult result;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, filter))
        return false;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
    if (!lock.Succeeded())
        return false;
    hit.entityId = static_cast<uint32>(lock.GetBody().GetUserData());
    hit.distance = result.mFraction * maxDistance;
    hit.point = origin + unit * hit.distance;
    hit.normal = result.mFraction == 0
                     ? unit * -1.f
                     : Vector(lock.GetBody().GetWorldSpaceSurfaceNormal(
                           result.mSubShapeID2, ray.GetPointOnRay(result.mFraction)));
    return true;
}

size_t PhysicsWorld::BodyCount() const {
    return impl_->entries.size();
}

} // namespace LuxEngine
