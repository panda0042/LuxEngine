#include <LuxEngine/Animation/AnimationEngine.hpp>
#include <LuxEngine/Core/Log.hpp>
#include <algorithm>
#include <cmath>

namespace LuxEngine {

TransformComponent AnimationClip::Sample(double time, bool wrapTime) const {
    TransformComponent result;
    if (keyframes.empty())
        return result;
    if (keyframes.size() == 1) {
        result.position = keyframes[0].position;
        result.rotation = keyframes[0].rotation;
        result.scale = keyframes[0].scale;
        return result;
    }

    if (!std::isfinite(time))
        time = 0.0f;

    double clampedTime = AnimationTime(time, duration, wrapTime ? GetWrap() : AnimationWrap::Once);

    if (!std::isfinite(clampedTime))
        clampedTime = 0.0f;
    auto it = std::lower_bound(keyframes.begin(), keyframes.end(), clampedTime,
                               [](const Keyframe& k, double t) { return k.time < t; });
    if (it == keyframes.end()) {
        result.position = keyframes.back().position;
        result.rotation = keyframes.back().rotation;
        result.scale = keyframes.back().scale;
        return result;
    }
    if (it == keyframes.begin()) {
        result.position = keyframes[0].position;
        result.rotation = keyframes[0].rotation;
        result.scale = keyframes[0].scale;
        return result;
    }

    const auto& kf0 = *(it - 1);
    const auto& kf1 = *it;

    const double segmentDuration = kf1.time - kf0.time;
    const double rawFactor = segmentDuration > 0 ? (clampedTime - kf0.time) / segmentDuration : 0;
    const double factor = std::clamp(rawFactor, 0.0, 1.0);

    result.position = kf0.position + (kf1.position - kf0.position) * factor;
    const auto rotation = BlendAnimationValue(
        AnimationProperty::Rotation, {kf0.rotation.x, kf0.rotation.y, kf0.rotation.z, 0},
        {kf1.rotation.x, kf1.rotation.y, kf1.rotation.z, 0}, factor);
    result.rotation = {rotation[0], rotation[1], rotation[2]};
    result.scale = kf0.scale + (kf1.scale - kf0.scale) * factor;
    return result;
}

void AnimationEngine::Init() {
    Log::Info("AnimationEngine initialized.");
}

void AnimationEngine::Shutdown() {
    Log::Info("AnimationEngine shutdown.");
}

void AnimationEngine::UpdateClip(const AnimationClip& clip, double& currentTime, double dt,
                                 TransformComponent& outTransform) {
    if (!std::isfinite(currentTime))
        currentTime = 0.0f;
    if (std::isfinite(dt) && dt > 0.0f)
        currentTime += dt;
    if (clip.GetWrap() == AnimationWrap::Loop && clip.duration > 0 && std::isfinite(currentTime)) {
        currentTime = std::fmod(currentTime, clip.duration);
        if (currentTime < 0.0f)
            currentTime += clip.duration;
    }
    outTransform = clip.Sample(currentTime);
}

void AnimationClip::AddKeyframe(const Keyframe& kf) {
    if (!std::isfinite(kf.time) || kf.time < 0.0f)
        throw std::invalid_argument("Invalid keyframe time");
    auto it = std::lower_bound(keyframes.begin(), keyframes.end(), kf.time,
                               [](const Keyframe& k, double time) { return k.time < time; });
    if (it != keyframes.end() && it->time == kf.time)
        *it = kf;
    else
        keyframes.insert(it, kf);
}
} // namespace LuxEngine
