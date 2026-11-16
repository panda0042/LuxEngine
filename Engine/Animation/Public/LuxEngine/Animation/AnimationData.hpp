#pragma once

#include <LuxEngine/Core/Core.hpp>
#include <array>
#include <string>
#include <vector>

namespace LuxEngine {
enum class AnimationWrap { Once, Loop, PingPong };
enum class AnimationInterpolation { Step, Linear, Cubic };
enum class AnimationProperty { Position, Rotation, Scale, Color, Opacity, SpriteFrame, Float };

struct AnimationKey {
    double time = 0;
    std::array<double, 4> value{};
    std::array<double, 4> inTangent{};
    std::array<double, 4> outTangent{};
    AnimationInterpolation interpolation = AnimationInterpolation::Linear;
};

struct AnimationTrack {
    // Empty targets the owning entity. Paths bind descendants by tag, separated by '/'.
    std::string target{};
    AnimationProperty property = AnimationProperty::Position;
    std::string field{};
    std::vector<AnimationKey> keys{};
    LUX_API void SetKey(AnimationKey key);
    LUX_API std::array<double, 4> Sample(double time) const;
};

struct AnimationEvent {
    double time = 0;
    std::string name{};
    std::string payload{};
};

struct AnimationSample {
    std::string target{};
    AnimationProperty property;
    std::string field{};
    std::array<double, 4> value;
};

struct AnimationPlayback {
    double cursor = 0; // Unwrapped clock, retained for reverse playback and event traversal.
    double speed = 1;
    bool playing = false;
    double previousCursor = 0;
    LUX_API double Time(double duration, AnimationWrap wrap) const;
    LUX_API std::vector<AnimationEvent> Advance(double dt, double duration, AnimationWrap wrap,
                                                const std::vector<AnimationEvent>& events);
};

LUX_API double AnimationTime(double time, double duration, AnimationWrap wrap);
LUX_API std::array<double, 4> BlendAnimationValue(AnimationProperty property,
                                                  const std::array<double, 4>& a,
                                                  const std::array<double, 4>& b, double weight);
} // namespace LuxEngine
