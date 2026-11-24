#include <LuxEngine/Animation/AnimationEngine.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace LuxEngine {
namespace {
using Value = std::array<double, 4>;

bool Finite(const Value& value) {
    return std::all_of(value.begin(), value.end(), [](double x) { return std::isfinite(x); });
}

const AnimationTrack* FindRootMotionTrack(const AnimationClip& clip) {
    const auto track = std::find_if(clip.tracks.begin(), clip.tracks.end(), [&](const auto& value) {
        return value.target == clip.rootMotionTarget &&
               value.property == AnimationProperty::Position && !value.keys.empty();
    });
    return track == clip.tracks.end() ? nullptr : &*track;
}
} // namespace

std::array<double, 4> BlendAnimationValue(AnimationProperty property, const Value& a,
                                          const Value& b, double weight) {
    weight = std::clamp(weight, 0.0, 1.0);
    if (property != AnimationProperty::Rotation) {
        Value result;
        for (size_t i = 0; i < 4; ++i)
            result[i] = std::lerp(a[i], b[i], weight);
        return result;
    }
    const auto value = SlerpRotation({a[0], a[1], a[2]}, {b[0], b[1], b[2]}, weight);
    return {value.x, value.y, value.z, 0};
}

double AnimationTime(double time, double duration, AnimationWrap wrap) {
    if (!std::isfinite(time) || !std::isfinite(duration) || duration <= 0)
        return 0;
    if (wrap == AnimationWrap::Once)
        return std::clamp(time, 0.0, duration);
    const double period = duration * (wrap == AnimationWrap::PingPong ? 2 : 1);
    double value = std::fmod(time, period);
    if (value < 0)
        value += period;
    return value > duration ? period - value : value;
}

double AnimationPlayback::Time(double duration, AnimationWrap wrap) const {
    return AnimationTime(cursor, duration, wrap);
}

std::vector<AnimationEvent> AnimationPlayback::Advance(double dt, double duration,
                                                       AnimationWrap wrap,
                                                       const std::vector<AnimationEvent>& events) {
    previousCursor = cursor;
    std::vector<std::pair<double, AnimationEvent>> crossed;
    if (!playing || !std::isfinite(dt) || dt <= 0 || !std::isfinite(speed) ||
        !std::isfinite(cursor) || !std::isfinite(duration) || duration <= 0)
        return {};
    const double previous = cursor, next = cursor + dt * speed;
    if (!std::isfinite(next) || next == previous)
        return {};
    const double low = std::min(previous, next), high = std::max(previous, next);
    const double span = high - low;
    auto emit = [&](double at, const AnimationEvent& event) {
        if ((next > previous && at > 0 && at <= span) || (next < previous && at >= 0 && at < span))
            crossed.emplace_back(at, event);
    };
    if (wrap == AnimationWrap::Once) {
        for (const auto& event : events)
            if (event.time >= low && event.time <= high)
                emit(event.time - low, event);
    } else if (!events.empty()) {
        const double period = duration * (wrap == AnimationWrap::PingPong ? 2 : 1);
        // Bound catch-up after a suspended application; ordinary playback never drops events.
        double phase = std::fmod(low, period);
        if (phase < 0)
            phase += period;
        const double cycles = std::floor(span / period + phase / period);
        if (!std::isfinite(cycles) || cycles > 4096)
            throw std::runtime_error("애니메이션 이벤트 순회 범위가 너무 큽니다.");
        const auto count = static_cast<unsigned>(cycles);
        for (unsigned cycle = 0; cycle <= count; ++cycle)
            for (const auto& event : events) {
                const double start = cycle ? cycle * period - phase : -phase;
                emit(start + event.time, event);
                if (wrap == AnimationWrap::PingPong && event.time > 0 && event.time < duration)
                    emit(start + period - event.time, event);
            }
        if (wrap == AnimationWrap::Loop && next < previous && phase == 0)
            for (const auto& event : events)
                if (event.time == duration)
                    emit(0, event);
    }
    std::stable_sort(crossed.begin(), crossed.end(), [&](const auto& a, const auto& b) {
        return next >= previous ? a.first < b.first : a.first > b.first;
    });
    cursor = wrap == AnimationWrap::Once ? std::clamp(next, 0.0, duration) : next;
    if (wrap == AnimationWrap::Once &&
        ((speed > 0 && next >= duration) || (speed < 0 && next <= 0)))
        playing = false;
    std::vector<AnimationEvent> result;
    for (auto& event : crossed)
        result.push_back(std::move(event.second));
    return result;
}

void AnimationTrack::SetKey(AnimationKey key) {
    if (!std::isfinite(key.time) || key.time < 0 || !Finite(key.value) || !Finite(key.inTangent) ||
        !Finite(key.outTangent))
        throw std::invalid_argument("애니메이션 키 값이 올바르지 않습니다.");
    auto at = std::lower_bound(keys.begin(), keys.end(), key.time,
                               [](const auto& a, double time) { return a.time < time; });
    if (at != keys.end() && at->time == key.time)
        *at = key;
    else
        keys.insert(at, key);
}

std::array<double, 4> AnimationTrack::Sample(double time) const {
    if (keys.empty())
        return {};
    auto next = std::upper_bound(keys.begin(), keys.end(), time,
                                 [](double t, const auto& key) { return t < key.time; });
    if (next == keys.begin())
        return next->value;
    const auto& a = *std::prev(next);
    if (next == keys.end() || a.interpolation == AnimationInterpolation::Step ||
        property == AnimationProperty::SpriteFrame)
        return a.value;
    const auto& b = *next;
    const double duration = b.time - a.time, t = (time - a.time) / duration;
    if (a.interpolation == AnimationInterpolation::Linear)
        return BlendAnimationValue(property, a.value, b.value, t);
    if (property == AnimationProperty::Rotation) {
        const auto c1 =
            AdditiveRotation({a.value[0], a.value[1], a.value[2]}, {},
                             {a.outTangent[0] * duration / 3, a.outTangent[1] * duration / 3,
                              a.outTangent[2] * duration / 3},
                             1);
        const auto c2 =
            AdditiveRotation({b.value[0], b.value[1], b.value[2]}, {},
                             {b.inTangent[0] * -duration / 3, b.inTangent[1] * -duration / 3,
                              b.inTangent[2] * -duration / 3},
                             1);
        const Value first{c1.x, c1.y, c1.z, 0}, second{c2.x, c2.y, c2.z, 0};
        const auto ab = BlendAnimationValue(property, a.value, first, t),
                   bc = BlendAnimationValue(property, first, second, t),
                   cd = BlendAnimationValue(property, second, b.value, t);
        return BlendAnimationValue(property, BlendAnimationValue(property, ab, bc, t),
                                   BlendAnimationValue(property, bc, cd, t), t);
    }
    Value result;
    for (size_t i = 0; i < 4; ++i)
        result[i] = (2 * t * t * t - 3 * t * t + 1) * a.value[i] +
                    (t * t * t - 2 * t * t + t) * duration * a.outTangent[i] +
                    (-2 * t * t * t + 3 * t * t) * b.value[i] +
                    (t * t * t - t * t) * duration * b.inTangent[i];
    return result;
}

AnimationWrap AnimationClip::GetWrap() const {
    return loop ? wrap : AnimationWrap::Once;
}

std::vector<AnimationSample> AnimationClip::SampleTracks(double time, bool wrapTime) const {
    std::vector<AnimationSample> result;
    const auto* root = rootMotionTarget.empty() ? nullptr : FindRootMotionTrack(*this);
    time = AnimationTime(time, duration, wrapTime ? GetWrap() : AnimationWrap::Once);
    for (const auto& track : tracks)
        if (!track.keys.empty())
            result.push_back({track.target,
                              track.property,
                              track.field,
                              track.Sample(time),
                              {},
                              &track == root});
    if (tracks.empty() && !keyframes.empty()) {
        const auto value = Sample(time, false);
        result.push_back({{},
                          AnimationProperty::Position,
                          {},
                          {value.position.x, value.position.y, value.position.z, 0}});
        result.push_back({{},
                          AnimationProperty::Rotation,
                          {},
                          {value.rotation.x, value.rotation.y, value.rotation.z, 0}});
        result.push_back(
            {{}, AnimationProperty::Scale, {}, {value.scale.x, value.scale.y, value.scale.z, 0}});
    }
    return result;
}

std::optional<Vec3> AnimationClip::RootDelta(double from, double to) const {
    const auto* track = FindRootMotionTrack(*this);
    if (!track && (keyframes.empty() || !rootMotionTarget.empty()))
        return {};
    auto position = [&](double time, bool endpoint) {
        if (track) {
            auto value = track->Sample(endpoint ? time : AnimationTime(time, duration, GetWrap()));
            return Vec3{value[0], value[1], value[2]};
        }
        AnimationClip copy = *this;
        copy.loop = false;
        return copy.Sample(endpoint ? time : AnimationTime(time, duration, GetWrap())).position;
    };
    Vec3 delta = position(to, false) - position(from, false);
    if (GetWrap() == AnimationWrap::Loop)
        delta += (position(duration, true) - position(0, true)) *
                 (std::floor(to / duration) - std::floor(from / duration));
    return delta;
}
} // namespace LuxEngine
