#pragma once

#include "World.hpp"
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <array>
#include <map>
#include <optional>
#include <string_view>

namespace Frontier {

enum class Weather : unsigned {
    Clear,
    Cloud,
    Overcast,
    Fog,
    Rain,
    Downpour,
    Thunder,
    Snow,
    Blizzard,
    Wind,
    Count
};

struct Controls {
    double throttle = 0, side = 0, mouseX = 0, mouseY = 0;
    bool sprint = false, jump = false, crouch = false;
    bool interact = false, camera = false, weather = false;
};

class Game {
public:
    static constexpr double VehicleMass = 1450;

    struct RenderView {
        LuxEngine::TransformComponent truck;
        Vec3 cameraPosition, cameraTarget;
    };

    explicit Game(unsigned seed, double daySeconds);
    void Start(World& world);
    void Step(const Controls& controls, World& world, double dt);
    void SetWeather(Weather weather);
    void StartAt(Vec2 place, bool driving);
    void Face(Vec2 target);

    Vec3 Player() const;
    Vec3 Truck() const;
    Vec2 Velocity() const;
    RenderView GetRenderView(double dt, double alpha);
    Vec3 Look() const;
    bool ShowPlayer() const;
    bool HasTerrainAt(Vec2 position) const;

    bool Driving() const {
        return driving_;
    }

    double PlayerYaw() const {
        return yaw_;
    }

    double Hour() const {
        return hour_;
    }

    double Wetness() const {
        return wetness_;
    }

    float CloudCover() const {
        return cloud_;
    }

    float Fog() const {
        return fog_;
    }

    float StormFlash() const {
        return flash_;
    }

    std::string_view WeatherName() const;

    unsigned Bodies() const {
        return unsigned(fixed_.size() + terrain_.size() + 2);
    }

    Vec2 PedestrianVelocity(size_t actor) const {
        return peopleVelocity_.at(actor);
    }

    bool Crouched() const {
        return crouched_;
    }

    bool Paused() const {
        return paused_;
    }

    void TogglePause() {
        paused_ = !paused_;
    }

    std::array<Vec3, 3> Pedestrians() const;

private:
    void ResetPhysics(World& world);
    void UpdateTerrain();
    void ResetTruck(Vec3 position, double heading);
    void RemoveWalker();
    bool BlockedByBuilding(Vec2 from, Vec2 to, double feet) const;
    void MovePlayer(const Controls& input, double dt);
    void ReadWalker(double dt);
    void Drive(const Controls& input, double dt);
    void AdvanceWeather(double dt);
    void UpdatePeople(double dt);
    void Interact(const World& world);
    double VehicleCameraClearance(Vec3 eye, Vec3 offset, Vec3 side) const;
    unsigned seed_;
    double daySeconds_;
    std::unique_ptr<LuxEngine::Scene> scene_;
    LuxEngine::Entity truck_;
    LuxEngine::Entity walker_;
    std::vector<LuxEngine::Entity> fixed_;
    std::map<ChunkKey, LuxEngine::Entity> terrain_;
    std::optional<std::array<ChunkKey, 2>> terrainCenters_;
    std::vector<Obstacle> obstacles_;
    unsigned collisionRevision_ = 0;
    ChunkKey collisionChunk_;
    std::array<Vec2, 3> pedestrians_{{{24, 9}, {-13, -10}, {17, 25}}};
    std::array<Vec2, 3> peopleVelocity_{};
    Vec2 playerVelocity_{};
    Vec3 player_{}, camera_{};
    double yaw_ = 1.49, pitch_ = -.12, truckYaw_ = 0;
    LuxEngine::TransformComponent previousTruck_;
    Vec2 previousLook_{yaw_, pitch_};
    Vec2 vehicleCameraLook_{};
    double vehicleCameraArm_ = 1;
    double elapsed_ = 0, hour_ = 12, wetness_ = 0;
    float cloud_ = 0, fog_ = .0015f, flash_ = 0;
    Weather weather_ = Weather::Clear;
    bool firstPerson_ = false, driving_ = false, crouched_ = false, swimming_ = false,
         paused_ = false;
    bool cameraReady_ = false;
};
} // namespace Frontier
