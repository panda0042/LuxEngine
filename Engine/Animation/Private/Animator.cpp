#include <LuxEngine/Animation/Animator.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <stdexcept>
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
        if (!mask.empty() && std::none_of(mask.begin(), mask.end(), [&](const auto& path) {
                return sample.target == path || sample.target.starts_with(path + "/") ||
                       sample.target.ends_with("/" + path) ||
                       sample.target.find("/" + path + "/") != std::string::npos;
            }))
            continue;
        auto found = std::find_if(destination.samples.begin(), destination.samples.end(),
                                  [&](const auto& value) { return Key(value) == Key(sample); });
        if (found == destination.samples.end()) {
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
    if (!source.spriteSheet.empty() && weight >= .5) {
        destination.spriteSheet = source.spriteSheet;
        destination.spriteColumns = source.spriteColumns;
        destination.spriteRows = source.spriteRows;
    }
}
} // namespace

struct AnimatorPlayer::Impl {
    struct LayerState {
        size_t current = 0, previous = 0;
        AnimationPlayback playback, previousPlayback;
        double fade = 0, elapsed = 0;
        bool fresh = true;
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

    AnimationFrame Sample(const AnimatorState& state, const AnimationPlayback& playback) {
        AnimationFrame frame;
        std::vector<std::pair<Ref<AnimationClip>, double>> sources;
        if (state.blendTree.empty())
            sources.emplace_back(Clip(state.clip), 1);
        else {
            const double parameter = parameters[state.blendParameter];
            auto next =
                std::upper_bound(state.blendTree.begin(), state.blendTree.end(), parameter,
                                 [](double value, const auto& p) { return value < p.threshold; });
            if (next == state.blendTree.begin())
                sources.emplace_back(Clip(next->clip), 1);
            else if (next == state.blendTree.end())
                sources.emplace_back(Clip(state.blendTree.back().clip), 1);
            else {
                const auto& a = *std::prev(next);
                double t = (parameter - a.threshold) / (next->threshold - a.threshold);
                sources.emplace_back(Clip(a.clip), 1 - t);
                sources.emplace_back(Clip(next->clip), t);
            }
        }
        const auto reference = Primary(state);
        double accumulated = 0;
        for (const auto& [clip, weight] : sources) {
            if (!clip || weight <= 0)
                continue;
            const double cursor = playback.cursor * clip->duration / reference->duration;
            AnimationFrame part;
            part.rootDelta = clip->RootDelta(
                playback.previousCursor * clip->duration / reference->duration, cursor);
            part.samples = clip->SampleTracks(cursor);
            part.spriteSheet = clip->spriteSheet;
            part.spriteColumns = clip->spriteColumns;
            part.spriteRows = clip->spriteRows;
            if (!clip->modelPath.empty()) {
                AnimationModelSample model;
                model.path = clip->modelPath;
                model.clip = clip->modelClip;
                model.time = AnimationTime(cursor, clip->duration, clip->GetWrap());
                const double previous =
                    playback.previousCursor * clip->duration / reference->duration;
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
            runtime.playback.playing = true;
            next->layers.push_back(runtime);
            for (const auto& state : layer.states) {
                if (!state.clip.empty())
                    next->Clip(state.clip);
                for (const auto& point : state.blendTree)
                    next->Clip(point.clip);
            }
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
    runtime.playback = {0, 1, true};
    runtime.fade = fade;
    runtime.elapsed = 0;
    runtime.fresh = true;
    return true;
}

void AnimatorPlayer::Seek(double seconds, size_t layer) {
    if (layer < impl_->layers.size() && std::isfinite(seconds)) {
        auto& state = impl_->layers[layer];
        state.playback.cursor = seconds;
        state.playback.previousCursor = seconds;
        state.fade = 0;
        state.fresh = false;
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
        runtime.playback.speed = current.speed * impl_->speed;
        if (emitEvents && runtime.fresh && impl_->playing)
            for (const auto& event : clip->events)
                if (event.time == 0)
                    result.events.push_back(event);
        if (emitEvents)
            runtime.fresh = false;
        if (impl_->playing) {
            auto events =
                runtime.playback.Advance(dt, clip->duration, clip->GetWrap(), clip->events);
            if (emitEvents && layer.weight > 0)
                result.events.insert(result.events.end(), events.begin(), events.end());
        }
        auto frame = impl_->Sample(current, runtime.playback);
        if (runtime.fade > 0 && runtime.elapsed < runtime.fade) {
            if (impl_->playing) {
                runtime.elapsed += dt;
                auto old = impl_->Primary(layer.states[runtime.previous]);
                runtime.previousPlayback.speed =
                    layer.states[runtime.previous].speed * impl_->speed;
                runtime.previousPlayback.Advance(dt, old->duration, old->GetWrap(), {});
            }
            const double factor = std::clamp(runtime.elapsed / runtime.fade, 0.0, 1.0);
            auto previous = impl_->Sample(layer.states[runtime.previous], runtime.previousPlayback);
            // Preserve the weighted model pose contributors for skeleton-space mixing.
            for (auto& model : previous.models)
                model.weight *= 1 - factor;
            auto models = std::move(frame.models);
            Merge(previous, frame, factor);

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
