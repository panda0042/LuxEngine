#include <LuxEngine/Animation/Animator.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
namespace LuxEngine {
namespace {
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

using Json = nlohmann::ordered_json;
constexpr size_t MaxAnimationFileBytes = 8 * 1024 * 1024;

void Require(bool value) {
    if (!value)
        throw std::runtime_error("애니메이션 데이터가 올바르지 않습니다.");
}

bool Finite(double v) {
    return std::isfinite(v);
}

Json Vector(Vec3 v) {
    return Json::array({v.x, v.y, v.z});
}

Vec3 Vector(const Json& v) {
    Require(v.is_array() && v.size() == 3);
    return {v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()};
}

Json Read(const std::string& path) {
    const auto size = std::filesystem::file_size(std::filesystem::u8path(path));
    Require(size <= MaxAnimationFileBytes);
    std::ifstream stream(std::filesystem::u8path(path));
    Json value;
    stream >> value;
    return value;
}

std::string FormatFile(const Json& value) {
    auto data = value.dump(2) + '\n';
    if (data.size() > MaxAnimationFileBytes)
        throw std::runtime_error("애니메이션 파일 크기 " + std::to_string(data.size()) +
                                 "바이트가 한도 " + std::to_string(MaxAnimationFileBytes) +
                                 "바이트를 넘었습니다.");
    return data;
}

void RebasePath(Json& value, const std::filesystem::path& destination,
                const std::filesystem::path& source = {}) {
    const auto& resource = value.get_ref<const std::string&>();
    if (resource.empty())
        return;
    auto file = std::filesystem::u8path(resource);
    if (file.is_relative() && !source.empty())
        file = source / file;
    if (file.is_absolute()) {
        std::error_code ec;
        const auto relative = std::filesystem::relative(file, destination, ec);
        value = Utf8(!ec && !relative.empty() ? relative : file);
    }
}

bool Write(const std::string& path, const std::string& data, std::string& error) {
    std::filesystem::path temporary;
    try {
        const auto file = std::filesystem::u8path(path);
        Require(!path.empty());
        if (!file.parent_path().empty())
            std::filesystem::create_directories(file.parent_path());
        static std::atomic<unsigned> sequence{};
        temporary = file;
        temporary += ".tmp-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(++sequence);
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(data.data(), static_cast<std::streamsize>(data.size()));
            stream.close();
            Require(bool(stream));
        }
#if defined(_WIN32)
        Require(MoveFileExW(temporary.c_str(), file.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
#else
        std::filesystem::rename(temporary, file);
#endif
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        std::error_code ignored;
        if (!temporary.empty())
            std::filesystem::remove(temporary, ignored);
        return false;
    }
}
} // namespace

bool AnimationClip::Validate(std::string& error) const {
    try {
        Require(Finite(duration) && duration > 0 && Finite(framesPerSecond) &&
                framesPerSecond >= 1 && framesPerSecond <= 1000);
        Require(int(wrap) >= 0 && int(wrap) <= 2 && tracks.size() <= 4096 &&
                events.size() <= 4096 && spriteColumns > 0 && spriteRows > 0 &&
                spriteColumns <= 4096 && spriteRows <= 4096);
        size_t count = keyframes.size();
        double previous = -1;
        for (const auto& key : keyframes) {
            Require(Finite(key.time) && key.time > previous && key.time <= 86400);
            previous = key.time;
            for (double v : {key.position.x, key.position.y, key.position.z, key.rotation.x,
                             key.rotation.y, key.rotation.z, key.scale.x, key.scale.y, key.scale.z})
                Require(Finite(v));
        }
        std::set<std::tuple<std::string, int, std::string>> bindings;
        for (const auto& track : tracks) {
            Require(int(track.property) >= 0 && int(track.property) <= 6 &&
                    bindings.emplace(track.target, int(track.property), track.field).second);
            count += track.keys.size();
            previous = -1;
            for (const auto& key : track.keys) {
                Require(Finite(key.time) && key.time > previous && key.time <= duration &&
                        int(key.interpolation) >= 0 && int(key.interpolation) <= 2);
                previous = key.time;
                for (auto values : {key.value, key.inTangent, key.outTangent})
                    for (double v : values)
                        Require(Finite(v));
            }
        }
        Require(count <= 100000);
        for (const auto& event : events)
            Require(Finite(event.time) && event.time >= 0 && event.time <= duration &&
                    !event.name.empty() && event.payload.size() <= 65536);
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool AnimationClip::SerializeToString(std::string& output, std::string& error) const {
    if (!Validate(error))
        return false;
    Json json = {{"format", "LuxAnimation"},
                 {"version", 1},
                 {"name", name},
                 {"duration", duration},
                 {"fps", framesPerSecond},
                 {"wrap", int(GetWrap())},
                 {"model", modelPath},
                 {"model_clip", modelClip},
                 {"root_motion_target", rootMotionTarget},
                 {"sprite_sheet", spriteSheet},
                 {"columns", spriteColumns},
                 {"rows", spriteRows},
                 {"tracks", Json::array()},
                 {"events", Json::array()},
                 {"legacy_keys", Json::array()}};
    for (const auto& track : tracks) {
        Json keys = Json::array();
        for (const auto& key : track.keys)
            keys.push_back({{"time", key.time},
                            {"value", key.value},
                            {"in", key.inTangent},
                            {"out", key.outTangent},
                            {"interpolation", int(key.interpolation)}});
        json["tracks"].push_back({{"target", track.target},
                                  {"property", int(track.property)},
                                  {"field", track.field},
                                  {"keys", std::move(keys)}});
    }
    for (const auto& event : events)
        json["events"].push_back(
            {{"time", event.time}, {"name", event.name}, {"payload", event.payload}});
    for (const auto& key : keyframes)
        json["legacy_keys"].push_back({{"time", key.time},
                                       {"position", Vector(key.position)},
                                       {"rotation", Vector(key.rotation)},
                                       {"scale", Vector(key.scale)}});
    output = json.dump();
    return true;
}

Ref<AnimationClip> AnimationClip::DeserializeFromString(const std::string& data,
                                                        std::string& error) {
    if (data.size() > MaxAnimationFileBytes) {
        error = "애니메이션 파일 크기가 8 MiB 한도를 넘었습니다.";
        return {};
    }
    return DeserializeData(data, error);
}

Ref<AnimationClip> AnimationClip::DeserializeData(const std::string& data, std::string& error) {
    try {
        const auto j = Json::parse(data);
        Require(j.at("format") == "LuxAnimation" && j.at("version") == 1);
        auto clip = CreateRef<AnimationClip>();
        clip->name = j.at("name");
        clip->duration = j.at("duration");
        clip->framesPerSecond = j.at("fps");
        clip->wrap = AnimationWrap(j.at("wrap").get<int>());
        clip->loop = clip->wrap != AnimationWrap::Once;
        clip->rootMotionTarget = j.value("root_motion_target", std::string{});
        clip->modelPath = j.value("model", std::string{});
        clip->modelClip = j.value("model_clip", -1);
        clip->spriteSheet = j.value("sprite_sheet", std::string{});
        clip->spriteColumns = j.value("columns", 1u);
        clip->spriteRows = j.value("rows", 1u);
        for (const auto& t : j.at("tracks")) {
            AnimationTrack track;
            track.target = t.at("target");
            track.property = AnimationProperty(t.at("property").get<int>());
            track.field = t.value("field", std::string{});
            Require(t.at("keys").size() <= 100000);
            for (const auto& k : t.at("keys")) {
                AnimationKey key;
                key.time = k.at("time");
                key.value = k.at("value").get<std::array<double, 4>>();
                key.inTangent = k.at("in").get<std::array<double, 4>>();
                key.outTangent = k.at("out").get<std::array<double, 4>>();
                key.interpolation = AnimationInterpolation(k.at("interpolation").get<int>());
                track.keys.push_back(key);
            }
            clip->tracks.push_back(std::move(track));
        }
        for (const auto& e : j.at("events"))
            clip->events.push_back({e.at("time"), e.at("name"), e.at("payload")});
        for (const auto& k : j.at("legacy_keys"))
            clip->keyframes.push_back({k.at("time"), Vector(k.at("position")),
                                       Vector(k.at("rotation")), Vector(k.at("scale"))});
        Require(clip->Validate(error));
        return clip;
    } catch (const std::exception& ex) {
        error = ex.what();
        return {};
    }
}

bool AnimationClip::SerializeForFile(const std::string& path, std::string& output,
                                     std::string& error) const try {
    Require(!path.empty());
    std::string value;
    if (!SerializeToString(value, error))
        return false;
    auto json = Json::parse(value);
    const auto destination = std::filesystem::absolute(std::filesystem::u8path(path)).parent_path();
    for (const auto key : {"model", "sprite_sheet"})
        RebasePath(json[key], destination);
    output = FormatFile(json);
    error.clear();
    return true;
} catch (const std::exception& ex) {

    error = ex.what();
    return false;
}

bool AnimationClip::Save(const std::string& path, std::string& error) const {
    std::string data;
    return SerializeForFile(path, data, error) && Write(path, data, error);
}

Ref<AnimationClip> AnimationClip::Load(const std::string& path, std::string& error) {
    try {
        auto result = DeserializeFromString(Read(path).dump(), error);
        if (result) {
            const auto base =
                std::filesystem::absolute(std::filesystem::u8path(path)).parent_path();
            for (auto* reference : {&result->modelPath, &result->spriteSheet})
                if (!reference->empty() && std::filesystem::u8path(*reference).is_relative())
                    *reference =
                        Utf8((base / std::filesystem::u8path(*reference)).lexically_normal());
        }
        return result;
    } catch (const std::exception& ex) {
        error = ex.what();
        return {};
    }
}

bool AnimatorController::Validate(std::string& error) const {
    try {
        Require(!layers.empty() && layers.size() <= 32 && parameters.size() <= 256);
        std::set<std::string> names;
        for (const auto& p : parameters)
            Require(!p.name.empty() && names.insert(p.name).second && Finite(p.value) &&
                    int(p.type) >= 0 && int(p.type) <= 3);
        for (const auto& layer : layers) {
            Require(Finite(layer.weight) && layer.weight >= 0 && layer.weight <= 1 &&
                    !layer.states.empty() && layer.states.size() <= 1024 &&
                    layer.transitions.size() <= 4096);
            std::set<std::string> states;
            for (const auto& state : layer.states) {
                Require(!state.name.empty() && states.insert(state.name).second &&
                        Finite(state.speed) && (!state.clip.empty() || !state.blendTree.empty()));
                double last = -INFINITY;
                for (const auto& point : state.blendTree) {
                    Require(names.contains(state.blendParameter) && Finite(point.threshold) &&
                            point.threshold > last && !point.clip.empty());
                    last = point.threshold;
                }
            }
            Require(states.contains(layer.entry));
            for (const auto& t : layer.transitions) {
                Require((t.from.empty() || states.contains(t.from)) && states.contains(t.to) &&
                        Finite(t.duration) && t.duration >= 0 && Finite(t.exitTime));
                for (const auto& c : t.conditions)
                    Require(names.contains(c.parameter) && Finite(c.value) &&
                            int(c.comparison) >= 0 && int(c.comparison) <= 4);
            }
        }
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool AnimatorController::SerializeForFile(const std::string& path, std::string& output,
                                          std::string& error) const try {
    Require(!path.empty());
    if (!Validate(error))
        return false;
    Json j = {{"format", "LuxAnimator"},
              {"version", 1},
              {"parameters", Json::array()},
              {"layers", Json::array()}};
    for (const auto& p : parameters)
        j["parameters"].push_back({{"name", p.name}, {"type", int(p.type)}, {"value", p.value}});
    for (const auto& layer : layers) {
        Json l = {{"name", layer.name},          {"entry", layer.entry}, {"weight", layer.weight},
                  {"additive", layer.additive},  {"mask", layer.mask},   {"states", Json::array()},
                  {"transitions", Json::array()}};
        for (const auto& s : layer.states) {
            Json tree = Json::array();
            for (const auto& p : s.blendTree)
                tree.push_back({{"threshold", p.threshold}, {"clip", p.clip}});
            l["states"].push_back({{"name", s.name},
                                   {"clip", s.clip},
                                   {"speed", s.speed},
                                   {"parameter", s.blendParameter},
                                   {"tree", tree},
                                   {"x", s.editorPosition.x},
                                   {"y", s.editorPosition.y}});
        }
        for (const auto& t : layer.transitions) {
            Json conditions = Json::array();
            for (const auto& c : t.conditions)
                conditions.push_back({{"parameter", c.parameter},
                                      {"comparison", int(c.comparison)},
                                      {"value", c.value}});
            l["transitions"].push_back({{"from", t.from},
                                        {"to", t.to},
                                        {"duration", t.duration},
                                        {"exit", t.exitTime},
                                        {"conditions", conditions}});
        }
        j["layers"].push_back(std::move(l));
    }
    const auto destination = std::filesystem::absolute(std::filesystem::u8path(path)).parent_path();
    const auto source =
        sourcePath.empty()
            ? std::filesystem::path{}
            : std::filesystem::absolute(std::filesystem::u8path(sourcePath)).parent_path();
    for (auto& layer : j["layers"])
        for (auto& state : layer["states"]) {
            RebasePath(state["clip"], destination, source);
            for (auto& point : state["tree"])
                RebasePath(point["clip"], destination, source);
        }
    output = FormatFile(j);
    error.clear();
    return true;
} catch (const std::exception& ex) {

    error = ex.what();
    return false;
}

bool AnimatorController::Save(const std::string& path, std::string& error) const {
    std::string data;
    return SerializeForFile(path, data, error) && Write(path, data, error);
}

Ref<AnimatorController> AnimatorController::Load(const std::string& path, std::string& error) {
    try {
        const auto j = Read(path);
        Require(j.at("format") == "LuxAnimator" && j.at("version") == 1);
        auto result = CreateRef<AnimatorController>();
        result->sourcePath = path;
        for (const auto& p : j.at("parameters"))
            result->parameters.push_back(
                {p.at("name"), AnimationParameterType(p.at("type").get<int>()), p.at("value")});
        for (const auto& l : j.at("layers")) {
            AnimatorLayer layer;
            layer.name = l.at("name");
            layer.entry = l.at("entry");
            layer.weight = l.at("weight");
            layer.additive = l.at("additive");
            layer.mask = l.at("mask").get<std::vector<std::string>>();
            for (const auto& s : l.at("states")) {
                AnimatorState state;
                state.name = s.at("name");
                state.clip = s.at("clip");
                state.speed = s.at("speed");
                state.blendParameter = s.at("parameter");
                state.editorPosition = {s.value("x", 0.0), s.value("y", 0.0)};
                for (const auto& p : s.at("tree"))
                    state.blendTree.push_back({p.at("threshold"), p.at("clip")});
                layer.states.push_back(std::move(state));
            }
            for (const auto& t : l.at("transitions")) {
                AnimatorTransition transition;
                transition.from = t.at("from");
                transition.to = t.at("to");
                transition.duration = t.at("duration");
                transition.exitTime = t.at("exit");
                for (const auto& c : t.at("conditions"))
                    transition.conditions.push_back(
                        {c.at("parameter"), AnimationComparison(c.at("comparison").get<int>()),
                         c.at("value")});
                layer.transitions.push_back(std::move(transition));
            }
            result->layers.push_back(std::move(layer));
        }
        Require(result->Validate(error));
        return result;
    } catch (const std::exception& ex) {
        error = ex.what();
        return {};
    }
}
} // namespace LuxEngine
