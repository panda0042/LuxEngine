#pragma once

#include "World.hpp"
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <filesystem>
#include <map>
#include <tuple>

namespace Frontier {

class Characters {
public:
    void Load(const std::filesystem::path& assets);
    void Draw(LuxEngine::Renderer3D::Context& renderer, size_t actor, Vec3 position, double yaw,
              bool crouched, double speed, double clock);
    bool VerifyClips() const;

private:
    struct Character {
        std::string path;
        LuxEngine::Ref<LuxEngine::ModelAsset> model, clipSource;
        std::array<LuxEngine::int32, 2> clips;
        LuxEngine::Material3D material;
        Vec3 foot;
        double scale = 1;

        bool Evaluate(unsigned state, double time, LuxEngine::ModelPose& pose,
                      std::string& error) const;
    };

    struct Actor {
        LuxEngine::Ref<const LuxEngine::ModelPose> pose;
        unsigned state = 0;
        double previousClock = 0, phase = 0, sampledTime = -1;
        LuxEngine::uint64 revision = 0;
    };

    struct CachedPose {
        LuxEngine::Ref<const LuxEngine::ModelPose> pose;
        size_t bytes = 0;
        LuxEngine::uint64 lastUse = 0;
    };

    LuxEngine::Ref<const LuxEngine::ModelPose> SamplePose(size_t outfit, unsigned motion,
                                                          double time);
    std::array<Character, 3> characters_;
    std::array<Actor, 4> actors_;
    std::map<std::tuple<size_t, unsigned, double>, CachedPose> poses_;
    size_t poseBytes_ = 0;
    LuxEngine::uint64 poseClock_ = 0;
};
} // namespace Frontier
