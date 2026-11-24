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
    if (point.x >= Dock.x - .4 && point.x <= Dock.x + 11.2 && std::abs(point.y - Dock.y) < 1.6)
        return LakeLevel + .37;
    return Height(point, seed);
}
} // namespace

Game::Game(unsigned seed, double daySeconds) : seed_(seed), daySeconds_(daySeconds) {}

void Game::Start(World& world) {
    scene_ = std::make_unique<Scene>();
    truck_ = AddBox(*scene_, "County pickup", OnGround(TruckSpawn, seed_, .85), {1.88, .70, 4.5},
                    BodyType::Dynamic, float(VehicleMass));
    ResetPhysics(world);
    StartAt(Spawn, false);
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

void Game::StartAt(Vec2 place, bool drive) {
    driving_ = drive;
    player_ = OnGround(Limit(place), seed_, .02);
    playerVelocity_ = {};
    up_ = 0;
    cameraReady_ = false;
    if (drive) {
        truck_.GetComponent<TransformComponent>().position = player_ + Vec3{0, .82, 0};
        scene_->SetVelocity3D(truck_.GetId(), {});
        truckYaw_ = 1.49;
    }
}

void Game::Face(Vec2 target) {
    const auto direction = target - Horizontal(Player());
    yaw_ = std::atan2(direction.x, -direction.y);
    cameraReady_ = false;
}

void Game::Step(const Controls& input, World& world, double dt) {
    if (paused_)
        return;
    elapsed_ += dt;
    hour_ = std::fmod(hour_ + dt * (24 / daySeconds_), 24.);
    AdvanceWeather(dt);
    if (input.weather)
        SetWeather(Weather((unsigned(weather_) + 1) % unsigned(Weather::Count)));
    if (input.camera)
        firstPerson_ = !firstPerson_;
    if (collisionRevision_ != world.GetStats().built ||
        collisionChunk_ != ChunkAt(Horizontal(Player())))
        ResetPhysics(world);
    if (input.interact)
        Interact();
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
    scene_->OnUpdate(dt);
    if (!scene_->GetLastPhysicsError3D().empty())
        throw std::runtime_error(scene_->GetLastPhysicsError3D());
    UpdatePeople(dt);
}

void Game::MovePlayer(const Controls& input, double dt) {
    if (input.crouch)
        crouched_ = !crouched_;
    const Vec2 here = Horizontal(player_);
    const double terrain = Floor(here, seed_);
    swimming_ = InLake(here) && terrain < LakeLevel - .52;
    const bool jog = input.sprint && !crouched_ && !swimming_;
    const double speed = swimming_ ? 1.45 : (crouched_ ? .95 : (jog ? 3.25 : 1.72));
    Vec3 wish = Forward(yaw_) * input.throttle + Side(yaw_) * input.side;
    if (Length(wish) > 1)
        wish = wish.Normalized();
    const Vec2 delta = Horizontal(wish * (speed * dt));
    auto blocked = [&](Vec2 next) {
        for (const auto& obstacle : obstacles_) {
            if (player_.y >= obstacle.center.y + obstacle.size.y / 2)
                continue;
            if (std::abs(next.x - obstacle.center.x) < obstacle.size.x * .5 + .25 &&
                std::abs(next.y - obstacle.center.z) < obstacle.size.z * .5 + .25)
                return true;
        }
        const auto p = Truck();
        return !driving_ && std::abs(next.x - p.x) < 1.18 && std::abs(next.y - p.z) < 2.5;
    };
    auto next = Limit(here + Vec2{delta.x, 0});
    if (blocked(next))
        next.x = here.x;
    next = Limit(next + Vec2{0, delta.y});
    if (blocked(next))
        next.y = here.y;
    playerVelocity_ = (next - here) * (1 / dt);
    const double ground = Floor(next, seed_);
    if (swimming_) {
        player_.y += std::clamp(LakeLevel - .75 - player_.y, -dt * 2, dt * 2);
        up_ = 0;
    } else {
        if (input.jump && player_.y < terrain + .04)
            up_ = 4.75;
        up_ -= 13. * dt;
        player_.y += up_ * dt;
        if (player_.y < ground) {
            player_.y = ground;
            up_ = 0;
        }
    }
    player_.x = next.x;
    player_.z = next.y;
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
        truck_.GetComponent<TransformComponent>().position = OnGround(Limit(horizontal), seed_, .9);
        scene_->SetVelocity3D(truck_.GetId(), {});
        return;
    }

    const double speed = velocity.Dot(Forward(truckYaw_));
    const bool road = RoadDistance(horizontal) < 4.5;
    const double grip = (road ? 1. : .75) * (1 - .33 * wetness_);
    const double vehicleSteer = input.side;
    const double signedTurn = std::clamp(speed * .29, -.8, .95);
    const double turn = vehicleSteer * signedTurn * grip * dt;
    truckYaw_ = std::remainder(truckYaw_ + turn, 2 * std::numbers::pi);
    if (driving_)
        yaw_ = std::remainder(yaw_ + turn, 2 * std::numbers::pi);
    const Vec3 forward = Forward(truckYaw_), side = Side(truckYaw_);
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

void Game::Interact() {
    if (driving_) {
        if (Distance(Velocity(), {}) < 3.2)
            StartAt(Horizontal(Truck() + Side(truckYaw_) * 2.1), false);
        return;
    }
    const auto truck = Truck();
    if (Distance(Horizontal(Player()), Horizontal(truck)) < 3.8 &&
        std::abs(player_.y - truck.y) < 2) {
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

Vec3 Game::CameraTarget() const {
    const auto at = Player() + Vec3{0, driving_ ? .83 : (crouched_ ? .94 : 1.58), 0};
    return firstPerson_ ? at + Look() * 10 : at + Look() * 3.7;
}

bool Game::ShowPlayer() const {
    return !driving_ && !firstPerson_;
}

Vec3 Game::Camera(double dt) {
    const Vec3 eye = Player() + Vec3{0, driving_ ? .83 : (crouched_ ? .94 : 1.58), 0};
    Vec3 desired = eye;
    if (!firstPerson_) {
        desired = eye - Look() * (driving_ ? 7.6 : 4.45) + Vec3{0, driving_ ? 1.55 : .55, 0};
        const Vec3 delta = desired - eye;
        RaycastHit3D hit;
        const double length = Length(delta);
        if (length > .1 && scene_->Raycast3D(eye, delta, float(length), hit, truck_.GetId()))
            desired = eye + delta * (std::max(.1, double(hit.distance) - .2) / length);
    }
    if (!cameraReady_ || firstPerson_)
        camera_ = desired;
    else
        camera_ += (desired - camera_) * (1 - std::exp(-dt * 15));
    cameraReady_ = true;
    camera_.y = std::max(camera_.y, Floor(Horizontal(camera_), seed_) + .21);
    return camera_;
}

std::array<Vec3, 3> Game::Pedestrians() const {
    return {OnGround(pedestrians_[0], seed_), OnGround(pedestrians_[1], seed_),
            OnGround(pedestrians_[2], seed_)};
}

} // namespace Frontier
