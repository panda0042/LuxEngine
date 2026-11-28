#include "Game.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <stdexcept>

namespace Frontier {
namespace {
using namespace LuxEngine;

constexpr Vec2 TruckSpawn{10, 1.85};
constexpr Vec3 TruckSize{1.88, .70, 4.5};
constexpr double PlayerClearance = .25;
constexpr double PlayerHalfHeight = .55;
constexpr double PlayerCenterHeight = PlayerClearance + PlayerHalfHeight;
constexpr double DockTop = LakeLevel + .37;
constexpr Vec3 DockSize{11.6, .16, 3.2};
constexpr Vec3 DockCenter{Dock.x + 5.4, DockTop - DockSize.y * .5, Dock.y};
constexpr std::array WeatherProfiles{
    std::array{0.f, .0015f, 0.f},  std::array{.31f, .002f, 0.f},  std::array{.72f, .003f, .04f},
    std::array{.58f, .034f, .03f}, std::array{.67f, .006f, .62f}, std::array{.84f, .016f, 1.f},
    std::array{.91f, .019f, 1.f},  std::array{.61f, .009f, .34f}, std::array{.96f, .039f, .67f},
    std::array{.27f, .004f, .01f},
};
constexpr std::array<std::string_view, size_t(Weather::Count)> WeatherLabels{
    "Clear",    "Cloud",   "Overcast", "Fog",      "Light rain",
    "Downpour", "Thunder", "Snow",     "Blizzard", "Wind",
};

double Length(Vec3 delta) {
    return std::sqrt(delta.Dot(delta));
}

Vec3 Forward(double angle) {
    return {std::sin(angle), 0, -std::cos(angle)};
}

Vec3 Side(double angle) {
    return {std::cos(angle), 0, std::sin(angle)};
}

Vec2 Limit(Vec2 position) {
    return {std::clamp(position.x, -WorldHalf + 1., WorldHalf - 1.),
            std::clamp(position.y, -WorldHalf + 1., WorldHalf - 1.)};
}

Entity AddBox(Scene& scene, const char* name, Vec3 center, Vec3 size, BodyType type, float mass) {
    auto entity = scene.CreateEntity(name);
    entity.GetComponent<TransformComponent>().position = center;
    auto& body = entity.AddComponent<Rigidbody3DComponent>();
    body.type = type;
    body.mass = mass;
    entity.AddComponent<BoxCollider3DComponent>().size = size;
    return entity;
}

Vec3 Position(const Entity& entity) {
    return entity.GetComponent<TransformComponent>().position;
}

double Floor(Vec2 point, unsigned seed) {
    if (std::abs(point.x - DockCenter.x) <= DockSize.x * .5 &&
        std::abs(point.y - DockCenter.z) < DockSize.z * .5)
        return DockTop;
    return Height(point, seed);
}
} // namespace

Game::Game(unsigned seed, double daySeconds) : seed_(seed), daySeconds_(daySeconds) {}

void Game::Start(World& world) {
    fixed_.clear();
    terrain_.clear();
    terrainCenters_.reset();
    scene_ = std::make_unique<Scene>();
    ResetTruck(OnGround(TruckSpawn, seed_, .85), 0);
    StartAt(Spawn, false);
    ResetPhysics(world);
    AddBox(*scene_, "Dock deck", DockCenter, DockSize, BodyType::Static, 1);
}

void Game::ResetTruck(Vec3 position, double heading) {
    if (truck_)
        scene_->DestroyEntity(truck_);
    truck_ = AddBox(*scene_, "County pickup", position, TruckSize, BodyType::Dynamic,
                    float(VehicleMass));
    auto& truckPose = truck_.GetComponent<TransformComponent>();
    truckPose.rotation = {0, -heading, 0};
    previousTruck_ = truckPose;
    truckYaw_ = heading;
    if (driving_)
        cameraReady_ = false;
}

void Game::ResetPhysics(World& world) {
    for (auto entity : fixed_)
        scene_->DestroyEntity(entity);
    fixed_.clear();
    obstacles_ = world.NearbyObstacles(Horizontal(Player()), 320);
    for (const auto& obstacle : obstacles_)
        fixed_.push_back(
            AddBox(*scene_, "Building", obstacle.center, obstacle.size, BodyType::Static, 1));
    collisionChunk_ = ChunkAt(Horizontal(Player()));
    collisionRevision_ = world.GetStats().built;
}

void Game::UpdateTerrain() {
    const std::array centers{ChunkAt(Horizontal(Player())), ChunkAt(Horizontal(Truck()))};
    if (terrainCenters_ == centers)
        return;
    const auto nearby = [centers](ChunkKey key) {
        return std::ranges::any_of(centers, [key](ChunkKey center) {
            return std::abs(key.x - center.x) <= 1 && std::abs(key.z - center.z) <= 1;
        });
    };
    for (auto it = terrain_.begin(); it != terrain_.end();) {
        if (nearby(it->first)) {
            ++it;
            continue;
        }
        scene_->DestroyEntity(it->second);
        it = terrain_.erase(it);
    }
    for (const auto center : centers)
        for (int z = center.z - 1; z <= center.z + 1; ++z)
            for (int x = center.x - 1; x <= center.x + 1; ++x) {
                const ChunkKey key{x, z};
                const auto origin = ChunkOrigin(key);
                if (origin.x < -WorldHalf || origin.x >= WorldHalf || origin.y < -WorldHalf ||
                    origin.y >= WorldHalf || terrain_.contains(key))
                    continue;
                auto entity = scene_->CreateEntity("Terrain collision chunk");
                entity.GetComponent<TransformComponent>().position = {origin.x, 0, origin.y};
                entity.AddComponent<TriangleMeshCollider3DComponent>().mesh =
                    TerrainCollision(key, seed_);
                terrain_.emplace(key, entity);
            }
    terrainCenters_ = centers;
}

bool Game::HasTerrainAt(Vec2 position) const {
    const auto found = terrain_.find(ChunkAt(position));
    const auto height = Height(position, seed_);
    RaycastHit3D hit;
    return found != terrain_.end() &&
           scene_->Raycast3D({position.x, height + 2, position.y}, {0, -1, 0}, 4, hit) &&
           hit.entityId == found->second.GetId() && std::abs(hit.point.y - height) <= .001;
}

void Game::RemoveWalker() {
    if (walker_)
        scene_->DestroyEntity(walker_);
    walker_ = {};
}

void Game::StartAt(Vec2 place, bool drive) {
    RemoveWalker();
    driving_ = drive;
    place = Limit(place);
    player_ = {place.x, Floor(place, seed_) + .02, place.y};
    playerVelocity_ = {};
    swimming_ = false;
    previousLook_ = {yaw_, pitch_};
    cameraReady_ = false;
    if (drive)
        ResetTruck(player_ + Vec3{0, .82, 0}, 1.49);
}

void Game::Face(Vec2 target) {
    const auto direction = target - Horizontal(Player());
    yaw_ = std::atan2(direction.x, -direction.y);
    previousLook_ = {yaw_, pitch_};
    cameraReady_ = false;
}

void Game::Step(const Controls& input, World& world, double dt) {
    if (paused_)
        return;
    previousTruck_ = truck_.GetComponent<TransformComponent>();
    previousLook_ = {yaw_, pitch_};
    elapsed_ += dt;
    hour_ = std::fmod(hour_ + dt * (24 / daySeconds_), 24.);
    AdvanceWeather(dt);
    if (input.weather)
        SetWeather(Weather((unsigned(weather_) + 1) % unsigned(Weather::Count)));
    if (input.camera) {
        firstPerson_ = !firstPerson_;
        cameraReady_ = false;
    }
    if (collisionRevision_ != world.GetStats().built ||
        collisionChunk_ != ChunkAt(Horizontal(Player())))
        ResetPhysics(world);
    if (input.interact)
        Interact(world);
    constexpr double radiansPerPixel = .0026;
    yaw_ = std::remainder(yaw_ + input.mouseX * radiansPerPixel, 2 * std::numbers::pi);
    pitch_ = std::clamp(pitch_ - input.mouseY * radiansPerPixel, -.9, .72);
    if (driving_)
        Drive(input, dt);
    else {
        MovePlayer(input, dt);
        // The parked vehicle still needs its suspension and water response.
        Drive({}, dt);
    }
    UpdateTerrain();
    scene_->OnUpdate(dt);
    if (!scene_->GetLastPhysicsError3D().empty())
        throw std::runtime_error(scene_->GetLastPhysicsError3D());
    if (walker_)
        ReadWalker(dt);
    const auto& truckPose = truck_.GetComponent<TransformComponent>();
    const auto forward = truckPose.GetTransform().TransformDirection({0, 0, -1});
    if (std::hypot(forward.x, forward.z) > .001)
        truckYaw_ = std::atan2(forward.x, -forward.z);
    UpdatePeople(dt);
}

bool Game::BlockedByBuilding(Vec2 from, Vec2 to, double feet) const {
    const Vec2 halfPath = (to - from) * .5;
    const Vec2 middle = from + halfPath;
    const double pathX = std::abs(halfPath.x), pathZ = std::abs(halfPath.y);
    for (const auto& obstacle : obstacles_) {
        if (feet >= obstacle.center.y + obstacle.size.y * .5)
            continue;
        const Vec2 offset = middle - Horizontal(obstacle.center);
        const double halfX = obstacle.size.x * .5 + PlayerClearance;
        const double halfZ = obstacle.size.z * .5 + PlayerClearance;
        if (std::abs(offset.x) < halfX + pathX && std::abs(offset.y) < halfZ + pathZ &&
            std::abs(offset.x * halfPath.y - offset.y * halfPath.x) <=
                halfX * pathZ + halfZ * pathX)
            return true;
    }
    return false;
}

void Game::MovePlayer(const Controls& input, double dt) {
    if (input.crouch)
        crouched_ = !crouched_;
    const Vec2 here = Horizontal(player_);
    const double terrain = Floor(here, seed_);
    const bool wasSwimming = swimming_;
    swimming_ = InLake(here) && terrain < LakeLevel - .52;
    const bool jog = input.sprint && !crouched_ && !swimming_;
    const double speed = swimming_ ? 1.45 : (crouched_ ? .95 : (jog ? 3.25 : 1.72));
    Vec3 wish = Forward(yaw_) * input.throttle + Side(yaw_) * input.side;
    if (Length(wish) > 1)
        wish = wish.Normalized();
    const Vec2 delta = Horizontal(wish * (speed * dt));
    if (!swimming_) {
        if (!walker_) {
            if (wasSwimming)
                player_.y = std::max(player_.y, terrain + .02);
            walker_ = scene_->CreateEntity("County walker");
            walker_.GetComponent<TransformComponent>().position =
                player_ + Vec3{0, PlayerCenterHeight, 0};
            auto& character = walker_.AddComponent<CharacterController3DComponent>();
            character.radius = PlayerClearance;
            character.halfHeight = PlayerHalfHeight;
        }
        const Vec2 velocity = (Limit(here + delta) - here) * (1 / dt);
        if (!scene_->SetCharacterMotion3D(walker_.GetId(), {velocity.x, 0, velocity.y},
                                          input.jump ? 4.75 : 0))
            throw std::runtime_error("Could not move the county walker");
        return;
    }
    RemoveWalker();
    const auto truck = Truck();
    const auto truckSide = Side(truckYaw_), truckForward = Forward(truckYaw_);
    const auto blocked = [&](Vec2 next) {
        const Vec3 offset{next.x - truck.x, 0, next.y - truck.z};
        return BlockedByBuilding(next, next, player_.y) ||
               (std::abs(offset.Dot(truckSide)) < TruckSize.x * .5 + PlayerClearance &&
                std::abs(offset.Dot(truckForward)) < TruckSize.z * .5 + PlayerClearance);
    };
    auto next = Limit(here + Vec2{delta.x, 0});
    if (blocked(next))
        next.x = here.x;
    next = Limit(next + Vec2{0, delta.y});
    if (blocked(next))
        next.y = here.y;
    playerVelocity_ = (next - here) * (1 / dt);
    player_.y += std::clamp(LakeLevel - .75 - player_.y, -dt * 2, dt * 2);
    player_.x = next.x;
    player_.z = next.y;
}

void Game::ReadWalker(double dt) {
    CharacterController3DState state;
    if (!scene_->GetCharacterState3D(walker_.GetId(), state))
        throw std::runtime_error("Could not read the county walker");
    const auto horizontal = Horizontal(state.position);
    const auto bound = Limit(horizontal);
    playerVelocity_ = (bound - Horizontal(player_)) * (1 / dt);
    player_ = {bound.x, state.position.y - PlayerCenterHeight, bound.y};
    if (bound.x != horizontal.x || bound.y != horizontal.y)
        walker_.GetComponent<TransformComponent>().position = {bound.x, state.position.y, bound.y};
}

void Game::Drive(const Controls& input, double dt) {
    Vec3 velocity;
    scene_->GetVelocity3D(truck_.GetId(), velocity);
    const Vec3 p = Truck();
    const Vec2 horizontal = Horizontal(p);
    const bool water = InLake(horizontal) && Height(horizontal, seed_) < LakeLevel - .6;
    const double ground = Height(horizontal, seed_);
    if (horizontal.x <= -WorldHalf || horizontal.x >= WorldHalf || horizontal.y <= -WorldHalf ||
        horizontal.y >= WorldHalf || p.y < std::min(LakeLevel, ground) - 4) {
        ResetTruck(OnGround(Limit(horizontal), seed_, .9), truckYaw_);
        return;
    }

    const double speed = velocity.Dot(Forward(truckYaw_));
    const bool road = RoadDistance(horizontal) < 4.5;
    const double grip = (road ? 1. : .75) * (1 - .33 * wetness_);
    const double vehicleSteer = input.side;
    const double signedTurn = std::clamp(speed * .29, -.8, .95);
    const double turn = vehicleSteer * signedTurn * grip * dt;
    const double heading = std::remainder(truckYaw_ + turn, 2 * std::numbers::pi);
    if (turn != 0) {
        auto& rotation = truck_.GetComponent<TransformComponent>().rotation;
        rotation = AdditiveRotation({0, -turn, 0}, {}, rotation, 1);
        yaw_ = std::remainder(yaw_ + turn, 2 * std::numbers::pi);
    }
    const Vec3 forward = Forward(heading), side = Side(heading);
    double suspension = 0;
    for (double axle : {-1.65, 1.65})
        for (double tire : {-.77, .77}) {
            const auto contact = p + forward * axle + side * tire;
            const double compression = Height(Horizontal(contact), seed_) + .98 - p.y;
            if (compression > 0)
                suspension += std::clamp(compression * 66 - velocity.y * 10, 0., 39.) * .25;
        }
    if (water)
        suspension += std::clamp((LakeLevel - .27 - p.y) * 28 - velocity.y * 8, 0., 31.);

    const bool traction = suspension > 2.5 && !water;
    const double thrust =
        std::abs(speed) > (input.throttle < 0 ? 7 : 26) && speed * input.throttle > 0
            ? 0.
            : input.throttle * 7.4 * grip;
    const double resistance = water ? 2.6 : (input.sprint && driving_ ? 6.5 : (road ? .28 : .32));
    Vec3 acceleration = forward * (-speed * resistance + (traction ? thrust : 0));
    acceleration += side * (-velocity.Dot(side) * (traction ? 6 * grip : 1.5));
    acceleration.y = suspension;
    if (!scene_->AddImpulse3D(truck_.GetId(), acceleration * (VehicleMass * dt)))
        throw std::runtime_error("Could not apply truck forces");
}

void Game::AdvanceWeather(double dt) {
    if (elapsed_ > 0 && unsigned(elapsed_ / 165) != unsigned((elapsed_ - dt) / 165))
        SetWeather(Weather((seed_ + unsigned(elapsed_ / 165) * 3) % unsigned(Weather::Count)));
    const auto [cloud, fog, wet] = WeatherProfiles[unsigned(weather_)];
    const double fade = 1 - std::exp(-dt * .32);
    cloud_ += float((cloud - cloud_) * fade);
    fog_ += float((fog - fog_) * fade);
    wetness_ += (wet - wetness_) * (1 - std::exp(-dt * .13));
    const double lightningPhase = std::fmod(elapsed_ + 2.4, 7.8);
    flash_ = weather_ == Weather::Thunder ? float(std::max(0., 1. - lightningPhase * 9.)) : 0;
}

void Game::SetWeather(Weather weather) {
    weather_ = weather;
}

std::string_view Game::WeatherName() const {
    return WeatherLabels[unsigned(weather_)];
}

void Game::UpdatePeople(double dt) {
    for (size_t i = 0; i < pedestrians_.size(); ++i) {
        const double t = elapsed_ * (.073 + double(i) * .037);
        const double shelter = 1 - wetness_ * .73;
        const std::array centers{Vec2{25, 9}, Vec2{-17, -10}, Vec2{19, 27}};
        const auto next = centers[i] + Vec2{std::sin(t) * 10, std::cos(t) * 2} * shelter;
        peopleVelocity_[i] = (next - pedestrians_[i]) * (1 / dt);
        pedestrians_[i] = next;
    }
}

void Game::Interact(const World& world) {
    if (driving_) {
        Vec3 velocity;
        if (!scene_->GetVelocity3D(truck_.GetId(), velocity) || Length(velocity) >= 3.2)
            return;
        const auto truck = Truck();
        const auto from = Horizontal(truck);
        for (const double side : {2.1, -2.1}) {
            const auto exit = Horizontal(truck + Side(truckYaw_) * side);
            if (std::abs(exit.x) > WorldHalf - 1 || std::abs(exit.y) > WorldHalf - 1 ||
                !world.HasChunk(ChunkAt(exit)))
                continue;
            const double feet = Floor(exit, seed_);
            const double truckFloor = truck.y - .82;
            if (std::abs(feet - truckFloor) > .85 || (InLake(exit) && feet < LakeLevel - .52) ||
                BlockedByBuilding(from, exit, std::min(feet, truckFloor)))
                continue;
            StartAt(exit, false);
            break;
        }
        return;
    }
    const auto truck = Truck();
    if (Distance(Horizontal(Player()), Horizontal(truck)) < 3.8 &&
        std::abs(player_.y - truck.y) < 2) {
        RemoveWalker();
        driving_ = true;
        cameraReady_ = false;
    }
}

Vec3 Game::Player() const {
    return driving_ ? Truck() : player_;
}

Vec3 Game::Truck() const {
    return Position(truck_);
}

Vec2 Game::Velocity() const {
    Vec3 velocity;
    if (driving_ && scene_->GetVelocity3D(truck_.GetId(), velocity))
        return Horizontal(velocity);
    return playerVelocity_;
}

Vec3 Game::Look() const {
    return Forward(yaw_) * std::cos(pitch_) + Vec3{0, std::sin(pitch_), 0};
}

bool Game::ShowPlayer() const {
    return !driving_ && !firstPerson_;
}

double Game::VehicleCameraClearance(Vec3 eye, Vec3 offset, Vec3 side) const {
    constexpr double radius = .24, padding = .21;
    const std::array probes{Vec3{}, side * radius, side * -radius, Vec3{0, radius, 0},
                            Vec3{0, -radius, 0}};
    double clearance = 1;
    for (const auto probe : probes) {
        const auto direction = offset + probe;
        const double distance = Length(direction);
        RaycastHit3D hit;
        if (distance > 1e-6 &&
            scene_->Raycast3D(eye, direction, float(distance), hit, truck_.GetId()))
            clearance = std::min(clearance, std::max(0., (hit.distance - padding) / distance));
    }
    return clearance;
}

Game::RenderView Game::GetRenderView(double dt, double alpha) {
    if (paused_)
        alpha = 1;
    auto truckPose = truck_.GetComponent<TransformComponent>();
    if (alpha < 1) {
        truckPose.position =
            previousTruck_.position + (truckPose.position - previousTruck_.position) * alpha;
        truckPose.rotation =
            alpha == 0 ? previousTruck_.rotation
                       : SlerpRotation(previousTruck_.rotation, truckPose.rotation, alpha);
    }
    double yaw = yaw_, pitch = pitch_;
    if (driving_ && alpha < 1) {
        const double delta = std::remainder(yaw_ - previousLook_.x, 2 * std::numbers::pi);
        yaw = previousLook_.x + delta * alpha;
        pitch = std::lerp(previousLook_.y, pitch_, alpha);
    }
    const bool vehicleChase = driving_ && !firstPerson_;
    if (vehicleChase) {
        if (!cameraReady_) {
            vehicleCameraLook_ = {yaw, pitch};
        } else if (!paused_) {
            const double blend = -std::expm1(-dt * 12);
            const double tau = 2 * std::numbers::pi;
            const double turn = std::remainder(yaw - vehicleCameraLook_.x, tau);
            vehicleCameraLook_.x = std::remainder(vehicleCameraLook_.x + turn * blend, tau);
            vehicleCameraLook_.y = std::lerp(vehicleCameraLook_.y, pitch, blend);
        }
        yaw = vehicleCameraLook_.x;
        pitch = vehicleCameraLook_.y;
    }
    const Vec3 look = Forward(yaw) * std::cos(pitch) + Vec3{0, std::sin(pitch), 0};
    const auto anchor = driving_ ? truckPose.position : player_;
    const Vec3 eye = anchor + Vec3{0, driving_ ? .83 : (crouched_ ? .94 : 1.58), 0};
    if (vehicleChase) {
        auto desired = eye - look * 7.6 + Vec3{0, 1.55, 0};
        desired.y = std::max(desired.y, Floor(Horizontal(desired), seed_) + .21);
        const auto offset = desired - eye;
        const auto side = Side(yaw);
        const double clearance = VehicleCameraClearance(eye, offset, side);
        if (!cameraReady_ || clearance < vehicleCameraArm_)
            vehicleCameraArm_ = clearance;
        else if (!paused_)
            vehicleCameraArm_ = std::lerp(vehicleCameraArm_, clearance, -std::expm1(-dt * 7));

        camera_ = eye + offset * vehicleCameraArm_;
        const double floor = Floor(Horizontal(camera_), seed_) + .21;
        if (camera_.y < floor) {
            camera_.y = floor;
            const auto lifted = camera_ - eye;
            camera_ = eye + lifted * VehicleCameraClearance(eye, lifted, side);
        }
        cameraReady_ = true;
        return {truckPose, camera_, eye + look * 3.7};
    }

    Vec3 desired = eye;
    if (!firstPerson_) {
        desired = eye - look * 4.45 + Vec3{0, .55, 0};
        const Vec3 delta = desired - eye;
        RaycastHit3D hit;
        const double length = Length(delta);
        if (length > .1 && scene_->Raycast3D(eye, delta, float(length), hit, truck_.GetId()))
            desired = eye + delta * (std::max(.1, double(hit.distance) - .2) / length);
    }
    if (!cameraReady_ || firstPerson_)
        camera_ = desired;
    else if (!paused_)
        camera_ += (desired - camera_) * (1 - std::exp(-dt * 15));
    cameraReady_ = true;
    camera_.y = std::max(camera_.y, Floor(Horizontal(camera_), seed_) + .21);
    return {truckPose, camera_, eye + look * (firstPerson_ ? 10 : 3.7)};
}

std::array<Vec3, 3> Game::Pedestrians() const {
    return {OnGround(pedestrians_[0], seed_), OnGround(pedestrians_[1], seed_),
            OnGround(pedestrians_[2], seed_)};
}

} // namespace Frontier
