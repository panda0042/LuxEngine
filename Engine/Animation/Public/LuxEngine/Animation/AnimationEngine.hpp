#pragma once

#include <LuxEngine/Animation/AnimationData.hpp>
#include <LuxEngine/Core/Core.hpp>
#include <LuxEngine/Core/Math.hpp>
#include <LuxEngine/ECS/ECS.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace LuxEngine {

struct Keyframe {
    double time = 0.0f;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

class LUX_API AnimationClip {
public:
    std::string name;
    double duration = 1.0f;
    bool loop = true;
    std::vector<Keyframe> keyframes;

    AnimationWrap wrap = AnimationWrap::Loop;
    double framesPerSecond = 30;
    std::vector<AnimationTrack> tracks;
    std::vector<AnimationEvent> events;
    std::string modelPath;
    int32 modelClip = -1;
    std::string rootMotionTarget;
    std::optional<Vec3> RootDelta(double from, double to) const;
    std::string spriteSheet;
    uint32 spriteColumns = 1, spriteRows = 1;

    bool Validate(std::string& error) const;
    bool SerializeToString(std::string& output, std::string& error) const;
    static Ref<AnimationClip> DeserializeFromString(const std::string& data, std::string& error);
    bool Save(const std::string& path, std::string& error) const;
    static Ref<AnimationClip> Load(const std::string& path, std::string& error);
    std::vector<AnimationSample> SampleTracks(double time, bool wrapTime = true) const;
    AnimationWrap GetWrap() const;

    void AddKeyframe(const Keyframe& kf);

    TransformComponent Sample(double time, bool wrapTime = true) const;
};

struct AnimationComponent {
    AnimationClip clip;
    double currentTime = 0.0f;
    bool playing = true;
    std::string assetPath;
    Ref<AnimationClip> asset;
    std::string loadedPath;
    double speed = 1;
    bool rootMotion = false;
    std::vector<AnimationEvent> pendingEvents;
};

class LUX_API AnimationEngine {
public:
    static void Init();
    static void Shutdown();

    static void UpdateClip(const AnimationClip& clip, double& currentTime, double dt,
                           TransformComponent& outTransform);
};

} // namespace LuxEngine
