#include <LuxEngine/Animation/Animator.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace LuxEngine {
namespace {
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

using Binding = std::tuple<std::string, AnimationProperty, std::string>;

Binding Key(const AnimationSample& sample) {
    return {sample.target, sample.property, sample.field};
}

bool MaskIncludes(const std::vector<std::string>& mask, std::string_view target) {
    return mask.empty() || std::any_of(mask.begin(), mask.end(), [&](const auto& path) {
               return target == path || target.starts_with(path + "/") ||
                      target.ends_with("/" + path) ||
                      target.find("/" + path + "/") != std::string_view::npos;
           });
}

void Merge(AnimationFrame& destination, const AnimationFrame& source, double weight,
           bool additive = false, const std::vector<std::string>& mask = {}) {
    if (weight <= 0)
        return;
    if (source.rootDelta) {
        if (!destination.rootDelta)
            destination.rootDelta = *source.rootDelta * weight;
        else if (additive)
            *destination.rootDelta += *source.rootDelta * weight;
        else
            *destination.rootDelta += (*source.rootDelta - *destination.rootDelta) * weight;
    }
    for (const auto& sample : source.samples) {
        const bool sprite = sample.property == AnimationProperty::SpriteFrame;
        if (!MaskIncludes(mask, sample.target) || (sprite && weight < .5))
            continue;
        auto found = std::find_if(destination.samples.begin(), destination.samples.end(),
                                  [&](const auto& value) { return Key(value) == Key(sample); });
        if (found != destination.samples.end())
            found->rootMotionPosition |= sample.rootMotionPosition;
        if (sprite) {
            auto value = sample;
            if (!value.spriteSheet)
                value.spriteSheet = AnimationSpriteSheet{source.spriteSheet, source.spriteColumns,
                                                         source.spriteRows};
            destination.spriteSheet = value.spriteSheet->path;
            destination.spriteColumns = value.spriteSheet->columns;
            destination.spriteRows = value.spriteSheet->rows;
            if (found == destination.samples.end())
                destination.samples.push_back(std::move(value));
            else
                *found = std::move(value);
        } else if (found == destination.samples.end()) {
            auto value = sample;
            if (weight < 1) {
                std::array<double, 4> identity{};
                if (sample.property == AnimationProperty::Scale)
                    identity = {1, 1, 1, 0};
                value.value = BlendAnimationValue(sample.property, identity, sample.value, weight);
            }
            destination.samples.push_back(std::move(value));
        } else if (additive) {
            if (sample.property == AnimationProperty::Scale) {
                for (size_t i = 0; i < 3; ++i)
                    found->value[i] *= 1 + (sample.value[i] - 1) * weight;
            } else if (sample.property == AnimationProperty::Rotation) {
                const auto r =
                    AdditiveRotation({found->value[0], found->value[1], found->value[2]}, {},
                                     {sample.value[0], sample.value[1], sample.value[2]}, weight);
                found->value = {r.x, r.y, r.z, 0};
            } else {
                for (size_t i = 0; i < 4; ++i)
                    found->value[i] += sample.value[i] * weight;
            }
        } else
            found->value = BlendAnimationValue(sample.property, found->value, sample.value, weight);
    }
    for (auto model : source.models) {
        model.weight *= weight;
        destination.models.push_back(std::move(model));
    }
}
} // namespace

struct AnimatorPlayer::Impl {
    struct LayerState {
        size_t current = 0, previous = 0;
        AnimationPlayback playback, previousPlayback;
        double fade = 0, elapsed = 0;
        std::map<Binding, std::array<double, 4>> fadeOrigins;
        bool fresh = true;
        bool positioned = false;
        bool interrupted = false;
    };

    Ref<AnimatorController> controller;
    std::vector<LayerState> layers;
    std::map<std::string, double> parameters;
    std::map<std::string, Ref<AnimationClip>> clips;
    bool playing = true;
    double speed = 1;

    Ref<AnimationClip> Clip(const std::string& path) {
        if (path.empty())
            return {};
        if (auto at = clips.find(path); at != clips.end())
            return at->second;
        auto file = std::filesystem::u8path(path);
        if (file.is_relative())
            file = std::filesystem::u8path(controller->sourcePath).parent_path() / file;
        std::string error;
        auto clip = AnimationClip::Load(Utf8(file), error);
        if (!clip)
            throw std::runtime_error(error);
        if (!clip->modelPath.empty())
            clip->modelPath = Utf8(
                (file.parent_path() / std::filesystem::u8path(clip->modelPath)).lexically_normal());
        if (!clip->spriteSheet.empty())
            clip->spriteSheet =
                Utf8((file.parent_path() / std::filesystem::u8path(clip->spriteSheet))
                         .lexically_normal());
        clips[path] = clip;
        return clip;
    }

    Ref<AnimationClip> Primary(const AnimatorState& state) {
        return Clip(state.blendTree.empty() ? state.clip : state.blendTree.front().clip);
    }

    using Sources = std::vector<std::pair<Ref<AnimationClip>, double>>;

    Sources BlendSources(const AnimatorState& state) {
        if (state.blendTree.empty())
            return {{Clip(state.clip), 1}};
        const double parameter = parameters.at(state.blendParameter);
        const auto next = std::upper_bound(
            state.blendTree.begin(), state.blendTree.end(), parameter,
            [](double value, const auto& point) { return value < point.threshold; });
        if (next == state.blendTree.begin())
            return {{Clip(next->clip), 1}};
        if (next == state.blendTree.end())
            return {{Clip(state.blendTree.back().clip), 1}};
        const auto& previous = *std::prev(next);
        const double t = (parameter - previous.threshold) / (next->threshold - previous.threshold);
        return {{Clip(previous.clip), 1 - t}, {Clip(next->clip), t}};
    }

    AnimationPlayback StartPlayback(const AnimatorState& state) {
        const auto clip = Primary(state);
        const double playbackSpeed = state.speed * speed;
        const double start =
            playbackSpeed < 0 && clip->GetWrap() == AnimationWrap::Once ? clip->duration : 0;
        return {start, playbackSpeed, true, start};
    }

    AnimationFrame Sample(const Sources& sources, const AnimationPlayback& playback,
                          double referenceDuration, const std::vector<std::string>& mask,
                          bool extractRootMotion) {
        AnimationFrame frame;
        double accumulated = 0;
        for (const auto& [clip, weight] : sources) {
            if (!clip || weight <= 0)
                continue;
            const double cursor = playback.cursor * clip->duration / referenceDuration;
            AnimationFrame part;
            if (MaskIncludes(mask, clip->rootMotionTarget))
                if (const auto delta = clip->RootDelta(
                        playback.previousCursor * clip->duration / referenceDuration, cursor))
                    frame.rootDelta = frame.rootDelta.value_or(Vec3{}) + *delta * weight;
            part.samples = clip->SampleTracks(cursor);
            if (extractRootMotion)
                std::erase_if(part.samples, [](const AnimationSample& sample) {
                    return sample.rootMotionPosition;
                });
            part.spriteSheet = clip->spriteSheet;
            part.spriteColumns = clip->spriteColumns;
            part.spriteRows = clip->spriteRows;
            if (!clip->modelPath.empty()) {
                AnimationModelSample model;
                model.path = clip->modelPath;
                model.clip = clip->modelClip;
                model.time = AnimationTime(cursor, clip->duration, clip->GetWrap());
                const double previous =
                    playback.previousCursor * clip->duration / referenceDuration;
                model.previousTime = AnimationTime(previous, clip->duration, clip->GetWrap());
                model.cycles = clip->GetWrap() == AnimationWrap::Loop
                                   ? std::floor(cursor / clip->duration) -
                                         std::floor(previous / clip->duration)
                                   : 0;
                model.rootMotionTarget = clip->rootMotionTarget;
                part.models.push_back(model);
            }
            // Model weights are absolute, unlike successive normalized property interpolation.
            auto models = std::move(part.models);
            Merge(frame, part, weight / (accumulated + weight));
            for (auto& model : models) {
                model.weight = weight;
                frame.models.push_back(std::move(model));
            }
            accumulated += weight;
        }
        return frame;
    }

    bool Matches(const AnimatorTransition& transition) const {
        return std::all_of(transition.conditions.begin(), transition.conditions.end(),
                           [&](const auto& condition) {
                               const auto value = parameters.at(condition.parameter);
                               switch (condition.comparison) {
                               case AnimationComparison::Greater:
                                   return value > condition.value;
                               case AnimationComparison::Less:
                                   return value < condition.value;
                               case AnimationComparison::Equal:
                                   return value == condition.value;
                               case AnimationComparison::NotEqual:
                                   return value != condition.value;
                               case AnimationComparison::Set:
                                   return value != 0;
                               }
                               return false;
                           });
    }
};

AnimatorPlayer::AnimatorPlayer() : impl_(std::make_unique<Impl>()) {}

AnimatorPlayer::~AnimatorPlayer() = default;

AnimatorPlayer::AnimatorPlayer(const AnimatorPlayer& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

AnimatorPlayer& AnimatorPlayer::operator=(const AnimatorPlayer& other) {
    if (this != &other)
        impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

bool AnimatorPlayer::SetController(Ref<AnimatorController> controller, std::string& error) {
    if (!controller || !controller->Validate(error))
        return false;
    auto next = std::make_unique<Impl>();
    next->controller = std::move(controller);
    try {
        for (const auto& parameter : next->controller->parameters)
            next->parameters[parameter.name] = parameter.value;
        for (const auto& layer : next->controller->layers) {
            Impl::LayerState runtime;
            runtime.current = static_cast<size_t>(
                std::find_if(layer.states.begin(), layer.states.end(),
                             [&](const auto& state) { return state.name == layer.entry; }) -
                layer.states.begin());
            runtime.previous = runtime.current;
            for (const auto& state : layer.states) {
                if (!state.clip.empty())
                    next->Clip(state.clip);
                for (const auto& point : state.blendTree)
                    next->Clip(point.clip);
            }
            runtime.playback = next->StartPlayback(layer.states[runtime.current]);
            next->layers.push_back(runtime);
        }
        impl_ = std::move(next);
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool AnimatorPlayer::SetParameter(const std::string& name, double value) {
    if (!impl_->controller || !std::isfinite(value))
        return false;
    auto parameter =
        std::find_if(impl_->controller->parameters.begin(), impl_->controller->parameters.end(),
                     [&](const auto& p) { return p.name == name; });
    if (parameter == impl_->controller->parameters.end())
        return false;
    if (parameter->type == AnimationParameterType::Boolean ||
        parameter->type == AnimationParameterType::Trigger)
        value = value != 0 ? 1 : 0;
    if (parameter->type == AnimationParameterType::Integer)
        value = std::trunc(value);
    impl_->parameters[name] = value;
    return true;
}

bool AnimatorPlayer::Trigger(const std::string& name) {
    if (!impl_->controller)
        return false;
    for (const auto& parameter : impl_->controller->parameters)
        if (parameter.name == name && parameter.type == AnimationParameterType::Trigger)
            return SetParameter(name, 1);
    return false;
}

double AnimatorPlayer::GetParameter(const std::string& name) const {
    auto at = impl_->parameters.find(name);
    return at == impl_->parameters.end() ? 0 : at->second;
}

bool AnimatorPlayer::Play(const std::string& name, double fade, size_t layer) {
    if (!impl_->controller || layer >= impl_->layers.size() || !std::isfinite(fade) || fade < 0)
        return false;
    const auto& definition = impl_->controller->layers[layer];
    auto found = std::find_if(definition.states.begin(), definition.states.end(),
                              [&](const auto& state) { return state.name == name; });
    if (found == definition.states.end())
        return false;
    auto& runtime = impl_->layers[layer];
    runtime.previous = runtime.current;
    runtime.previousPlayback = runtime.playback;
    runtime.current = static_cast<size_t>(found - definition.states.begin());
    runtime.playback = impl_->StartPlayback(*found);
    runtime.interrupted = runtime.fade > 0 && runtime.elapsed < runtime.fade;
    runtime.fade = fade;
    runtime.fadeOrigins.clear();
    runtime.elapsed = 0;
    runtime.fresh = true;
    runtime.positioned = false;
    return true;
}

void AnimatorPlayer::Seek(double seconds, size_t layer) {
    if (layer < impl_->layers.size() && std::isfinite(seconds)) {
        auto& state = impl_->layers[layer];
        state.playback.cursor = seconds;
        state.playback.previousCursor = seconds;
        if (seconds >= 0 && seconds <= GetDuration(layer))
            state.playback.playing = true;
        state.fade = 0;
        state.fadeOrigins.clear();
        state.fresh = false;
        state.positioned = true;
    }
}

std::string AnimatorPlayer::GetState(size_t layer) const {
    return impl_->controller && layer < impl_->layers.size()
               ? impl_->controller->layers[layer].states[impl_->layers[layer].current].name
               : std::string{};
}

double AnimatorPlayer::GetTime(size_t layer) const {
    return layer < impl_->layers.size() ? impl_->layers[layer].playback.cursor : 0;
}

void AnimatorPlayer::SetPlaying(bool value) {
    impl_->playing = value;
}

bool AnimatorPlayer::IsPlaying() const {
    return impl_->playing;
}

bool AnimatorPlayer::SetSpeed(double value) {
    if (!std::isfinite(value))
        return false;
    impl_->speed = value;
    for (size_t index = 0; index < impl_->layers.size(); ++index) {
        auto& runtime = impl_->layers[index];
        if (!runtime.positioned)
            runtime.playback =
                impl_->StartPlayback(impl_->controller->layers[index].states[runtime.current]);
    }
    return true;
}

double AnimatorPlayer::GetSpeed() const {
    return impl_->speed;
}

double AnimatorPlayer::GetDuration(size_t layer) const {
    return impl_->controller && layer < impl_->layers.size()
               ? impl_
                     ->Primary(
                         impl_->controller->layers[layer].states[impl_->layers[layer].current])
                     ->duration
               : 0;
}

AnimationFrame AnimatorPlayer::Update(double dt, bool emitEvents) {
    return Update(dt, emitEvents, {}, false);
}

AnimationFrame AnimatorPlayer::Update(double dt, bool emitEvents,
                                      const PropertyReader& readProperty, bool extractRootMotion) {
    AnimationFrame result;
    if (!impl_->controller || !std::isfinite(dt) || dt < 0)
        return result;
    std::vector<std::string> consumed;
    for (size_t layerIndex = 0; layerIndex < impl_->layers.size(); ++layerIndex) {
        const auto& layer = impl_->controller->layers[layerIndex];
        auto& runtime = impl_->layers[layerIndex];
        if (impl_->playing && dt > 0) {
            const auto& state = layer.states[runtime.current];
            const auto clip = impl_->Primary(state);
            for (const auto& transition : layer.transitions) {
                if ((!transition.from.empty() && transition.from != state.name) ||
                    transition.to == state.name || !impl_->Matches(transition))
                    continue;
                if (transition.exitTime >= 0 &&
                    runtime.playback.cursor / clip->duration < transition.exitTime)
                    continue;
                Play(transition.to, transition.duration, layerIndex);
                for (const auto& condition : transition.conditions)
                    for (const auto& parameter : impl_->controller->parameters)
                        if (parameter.name == condition.parameter &&
                            parameter.type == AnimationParameterType::Trigger)
                            consumed.push_back(parameter.name);
                break;
            }
        }
        const auto& current = layer.states[runtime.current];
        const auto clip = impl_->Primary(current);
        const auto sources = impl_->BlendSources(current);
        runtime.playback.speed = current.speed * impl_->speed;
        if (impl_->playing) {
            runtime.playback.Advance(dt, clip->duration, clip->GetWrap(), {});
            runtime.positioned |= runtime.playback.previousCursor != runtime.playback.cursor;
            if (emitEvents) {
                if (layer.weight > 0) {
                    const auto& eventClip = *std::max_element(sources.rbegin(), sources.rend(),
                                                              [](const auto& a, const auto& b) {
                                                                  return a.second < b.second;
                                                              })
                                                 ->first;
                    const double ratio = eventClip.duration / clip->duration;
                    const double from = runtime.playback.previousCursor * ratio;
                    const double to = runtime.playback.cursor * ratio;
                    if (runtime.fresh) {
                        const double initial =
                            AnimationTime(from, eventClip.duration, eventClip.GetWrap());
                        for (const auto& event : eventClip.events)
                            if (event.time == initial)
                                result.events.push_back(event);
                    }
                    AnimationPlayback eventPlayback{from, to - from, true};
                    auto events = eventPlayback.Advance(1, eventClip.duration, eventClip.GetWrap(),
                                                        eventClip.events);
                    result.events.insert(result.events.end(), events.begin(), events.end());
                }
                runtime.fresh = false;
            }
        } else
            runtime.playback.previousCursor = runtime.playback.cursor;
        auto frame =
            impl_->Sample(sources, runtime.playback, clip->duration, layer.mask, extractRootMotion);
        if (runtime.fade > 0 && runtime.elapsed < runtime.fade) {
            if (impl_->playing) {
                runtime.elapsed += dt;
                auto old = impl_->Primary(layer.states[runtime.previous]);
                runtime.previousPlayback.speed =
                    layer.states[runtime.previous].speed * impl_->speed;
                runtime.previousPlayback.Advance(dt, old->duration, old->GetWrap(), {});
            } else
                runtime.previousPlayback.previousCursor = runtime.previousPlayback.cursor;
            const double factor = std::clamp(runtime.elapsed / runtime.fade, 0.0, 1.0);
            const auto& previousState = layer.states[runtime.previous];
            auto previous = impl_->Sample(
                impl_->BlendSources(previousState), runtime.previousPlayback,
                impl_->Primary(previousState)->duration, layer.mask, extractRootMotion);
            // Preserve the weighted model pose contributors for skeleton-space mixing.
            for (auto& model : previous.models)
                model.weight *= 1 - factor;
            auto models = std::move(frame.models);
            for (const auto& sample : frame.samples) {
                if (sample.property == AnimationProperty::SpriteFrame)
                    continue;
                const auto binding = Key(sample);
                const auto outgoing =
                    std::find_if(previous.samples.begin(), previous.samples.end(),
                                 [&](const auto& value) { return Key(value) == binding; });
                if (outgoing != previous.samples.end() && !runtime.interrupted)
                    continue;
                const auto fallback =
                    outgoing == previous.samples.end() ? sample.value : outgoing->value;
                auto [origin, added] = runtime.fadeOrigins.try_emplace(binding, fallback);
                if (added && readProperty)
                    if (auto currentValue = readProperty(sample))
                        origin->second = *currentValue;
                if (outgoing != previous.samples.end()) {
                    outgoing->value = origin->second;
                } else {
                    auto startingSample = sample;
                    startingSample.value = origin->second;
                    previous.samples.push_back(std::move(startingSample));
                }
            }
            auto motion = std::optional<Vec3>{};
            if (previous.rootDelta || frame.rootDelta)
                motion = previous.rootDelta.value_or(Vec3{}) * (1 - factor) +
                         frame.rootDelta.value_or(Vec3{}) * factor;
            Merge(previous, frame, factor);
            previous.rootDelta = motion;
            if (factor >= 1)
                runtime.fadeOrigins.clear();

            for (auto& model : models) {
                model.weight *= factor;
                previous.models.push_back(std::move(model));
            }
            frame = std::move(previous);
        }
        for (auto& model : frame.models) {
            model.layer = layerIndex;
            model.additive = layer.additive;
            model.mask = layer.mask;
        }
        Merge(result, frame, layer.weight, layer.additive, layer.mask);
    }
    for (const auto& name : consumed)
        impl_->parameters[name] = 0;
    return result;
}

AnimatorComponent::AnimatorComponent() = default;
AnimatorComponent::~AnimatorComponent() = default;
AnimatorComponent::AnimatorComponent(AnimatorComponent&&) noexcept = default;
AnimatorComponent& AnimatorComponent::operator=(AnimatorComponent&&) noexcept = default;

AnimatorComponent::AnimatorComponent(const AnimatorComponent& other)
    : controllerPath(other.controllerPath), playing(other.playing), rootMotion(other.rootMotion),
      parameters(other.parameters) {}

AnimatorComponent& AnimatorComponent::operator=(const AnimatorComponent& other) {
    if (this != &other) {
        AnimatorComponent copy(other);
        *this = std::move(copy);
    }
    return *this;
}
} // namespace LuxEngine
