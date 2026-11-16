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
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
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
#include <set>
#include <stdexcept>
#include <tuple>

namespace LuxEngine {
namespace {
constexpr size_t MaxBodies = 16384;

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

JPH::Vec3 Vector(Vec3 v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

Vec3 Vector(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
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
} // namespace

struct PhysicsWorld::Impl final : JPH::ContactListener {
    struct Entry {
        JPH::BodyID body;
        PhysicsBodyDescription description;
        PhysicsBodyState state;
    };

    bool planar;
    Layers layers;
    BroadFilter broadFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    JPH::TempAllocatorImpl allocator{16 * 1024 * 1024};
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    std::map<uint32, Entry> entries;
    std::vector<Collision3DEvent> contacts, began;
    std::set<std::tuple<uint32, uint32, bool>> previousContacts;
    std::string error;

    explicit Impl(bool plane) : planar(plane) {
        system.Init(static_cast<JPH::uint>(MaxBodies), 0, 65536, 16384, layers, broadFilter,
                    pairFilter);
        system.SetContactListener(this);
    }

    ~Impl() override {
        system.SetContactListener(nullptr);
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

bool PhysicsWorld::Synchronize(std::span<const PhysicsBodyDescription> input) {
    impl_->error.clear();
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
        if (!id || !Valid(description.position, 1e15) || !Valid(description.rotation) ||
            !Valid(description.velocity) || !Valid(description.size, 10000) ||
            !Valid(description.offset) || description.size.x < .02f || description.size.y < .02f ||
            description.size.z < .02f || !std::isfinite(description.mass) ||
            description.mass < .001f || description.mass > 1000000 ||
            !std::isfinite(description.gravityScale) ||
            std::abs(description.gravityScale) > 10000 || description.type < BodyType::Static ||
            description.type > BodyType::Kinematic) {
            report(id, "invalid body type, transform, velocity, mass or collider dimensions");
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
            if (old.type != description.type || !Same(old.size, description.size) ||
                !Same(old.offset, description.offset) || old.mass != description.mass ||
                old.trigger != description.trigger) {
                impl_->Remove(id, true);
                found = impl_->entries.end();
            }
        }
        if (found == impl_->entries.end()) {
            const auto half = Vector(description.size * .5f);
            JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(
                half, std::min(.02f, .2f * std::min({half.GetX(), half.GetY(), half.GetZ()})));
            if (!Same(description.offset, {}))
                shape = new JPH::RotatedTranslatedShape(Vector(description.offset),
                                                        JPH::Quat::sIdentity(), shape);
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
            settings.mMaxLinearVelocity = 200000.f;
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
        if (!Same(entry.state.position, description.position) ||
            !Same(entry.state.rotation, description.rotation))
            bodies.SetPositionAndRotation(
                entry.body,
                JPH::RVec3(description.position.x, description.position.y, description.position.z),
                JPH::Quat::sEulerAngles(Vector(description.rotation)), JPH::EActivation::Activate);
        if (!Same(entry.state.velocity, description.velocity))
            bodies.SetLinearVelocity(entry.body, Vector(description.velocity));
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
    impl_->contacts.clear();
    impl_->began.clear();
    impl_->error.clear();
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
    const auto& bodies = impl_->system.GetBodyInterface();
    for (auto& [id, entry] : impl_->entries) {
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
    return impl_->error;
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

    struct Filter final : JPH::BodyFilter {
        uint32 ignored = 0;
        bool sensors = false;

        bool ShouldCollideLocked(const JPH::Body& body) const override {
            return body.GetUserData() != ignored && (sensors || !body.IsSensor());
        }
    } filter;

    filter.ignored = ignoreEntity;
    filter.sensors = includeTriggers;
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
