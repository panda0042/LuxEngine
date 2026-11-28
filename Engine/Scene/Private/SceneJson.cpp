#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <type_traits>
#include <unordered_set>

namespace LuxEngine {
namespace {
using Json = nlohmann::ordered_json;
constexpr size_t MaxSceneBytes = 64ull * 1024 * 1024;
constexpr size_t MaxEntities = 100000;

void RequireValidScene(bool valid) {
    if (!valid)
        throw std::runtime_error("Invalid scene JSON");
}

template <class T> Json ToJson(const T& value) {
    return value;
}

Json ToJson(const Vec2& value) {
    return Json::array({value.x, value.y});
}

Json ToJson(const Vec3& value) {
    return Json::array({value.x, value.y, value.z});
}

Json ToJson(const Vec4& value) {
    return Json::array({value.x, value.y, value.z, value.w});
}

Json ToJson(BodyType value) {
    return static_cast<int>(value);
}

Json ToJson(SceneReferenceMode value) {
    return static_cast<int>(value);
}

Json ToJson(Render3DMode value) {
    return static_cast<int>(value);
}

Json ToJson(const RayTracingSettings& rays) {
    return {{"shadows", rays.shadows},
            {"reflections", rays.reflections},
            {"refractions", rays.refractions},
            {"indirect_diffuse", rays.indirectDiffuse},
            {"max_bounces", rays.maxBounces},
            {"max_distance", rays.maxDistance},
            {"indirect_strength", rays.indirectStrength},
            {"restir_direct", rays.restirDirect},
            {"restir_indirect", rays.restirIndirect},
            {"water_reflections", rays.waterReflections},
            {"temporal_restir", rays.temporalRestir},
            {"denoiser", rays.denoiser}};
}

Json ToJson(const VolumetricFogSettings& fog) {
    Json volumes = Json::array();
    for (const auto& v : fog.volumes)
        volumes.push_back({{"center", v.center}, {"radius", v.radius}, {"density", v.density}});
    return {{"color", ToJson(fog.color)},
            {"density", fog.density},
            {"max_distance", fog.maxDistance},
            {"height_falloff", fog.heightFalloff},
            {"base_height", fog.baseHeight},
            {"anisotropy", fog.anisotropy},
            {"ray_traced_shadows", fog.rayTracedShadows},
            {"density_variation", fog.densityVariation},
            {"multiple_scattering", fog.multipleScattering},
            {"history_weight", fog.historyWeight},
            {"volumes", std::move(volumes)}};
}

Json ToJson(const std::vector<Keyframe>& keys) {
    Json result = Json::array();
    for (const auto& key : keys)
        result.push_back({{"time", key.time},
                          {"position", ToJson(key.position)},
                          {"rotation", ToJson(key.rotation)},
                          {"scale", ToJson(key.scale)}});
    return result;
}

template <class T> void ReadJsonValue(const Json& json, T& output) {
    if constexpr (std::is_same_v<T, bool>)
        RequireValidScene(json.is_boolean());
    else if constexpr (std::is_integral_v<T>) {
        RequireValidScene(json.is_number_integer());
        if (json.is_number_unsigned())
            RequireValidScene(json.template get<uint64>() <=
                              static_cast<uint64>(std::numeric_limits<T>::max()));
        else {
            const auto value = json.template get<int64>();
            if constexpr (std::is_unsigned_v<T>)
                RequireValidScene(value >= 0 &&
                                  static_cast<uint64>(value) <= std::numeric_limits<T>::max());
            else
                RequireValidScene(value >= std::numeric_limits<T>::min() &&
                                  value <= std::numeric_limits<T>::max());
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        RequireValidScene(json.is_number());
        const auto value = json.template get<double>();
        RequireValidScene(std::isfinite(value) && std::abs(value) <= std::numeric_limits<T>::max());
    } else
        RequireValidScene(json.is_string());
    output = json.template get<T>();
}

void ReadJsonValue(const Json& json, Vec2& value) {
    RequireValidScene(json.is_array() && json.size() == 2);
    ReadJsonValue(json[0], value.x);
    ReadJsonValue(json[1], value.y);
}

void ReadJsonValue(const Json& json, Vec3& value) {
    RequireValidScene(json.is_array() && json.size() == 3);
    ReadJsonValue(json[0], value.x);
    ReadJsonValue(json[1], value.y);
    ReadJsonValue(json[2], value.z);
}

void ReadJsonValue(const Json& json, Vec4& value) {
    RequireValidScene(json.is_array() && json.size() == 4);
    ReadJsonValue(json[0], value.x);
    ReadJsonValue(json[1], value.y);
    ReadJsonValue(json[2], value.z);
    ReadJsonValue(json[3], value.w);
}

void ReadJsonValue(const Json& json, BodyType& value) {
    int number;
    ReadJsonValue(json, number);
    RequireValidScene(number >= 0 && number <= 2);
    value = static_cast<BodyType>(number);
}

void ReadJsonValue(const Json& json, SceneReferenceMode& value) {
    int number;
    ReadJsonValue(json, number);
    RequireValidScene(number >= 0 && number <= 2);
    value = static_cast<SceneReferenceMode>(number);
}

void ReadJsonValue(const Json& json, Render3DMode& value) {
    int number;
    ReadJsonValue(json, number);
    RequireValidScene(number >= 0 && number <= 1);
    value = static_cast<Render3DMode>(number);
}

void ReadJsonValue(const Json& json, RayTracingSettings& rays) {
    ReadJsonValue(json.at("shadows"), rays.shadows);
    ReadJsonValue(json.at("reflections"), rays.reflections);
    ReadJsonValue(json.at("refractions"), rays.refractions);
    ReadJsonValue(json.at("indirect_diffuse"), rays.indirectDiffuse);
    ReadJsonValue(json.at("max_bounces"), rays.maxBounces);
    ReadJsonValue(json.at("max_distance"), rays.maxDistance);
    ReadJsonValue(json.at("indirect_strength"), rays.indirectStrength);
    if (json.contains("restir_direct"))
        ReadJsonValue(json.at("restir_direct"), rays.restirDirect);
    if (json.contains("restir_indirect"))
        ReadJsonValue(json.at("restir_indirect"), rays.restirIndirect);
    if (json.contains("water_reflections"))
        ReadJsonValue(json.at("water_reflections"), rays.waterReflections);
    if (json.contains("temporal_restir"))
        ReadJsonValue(json.at("temporal_restir"), rays.temporalRestir);
    if (json.contains("denoiser"))
        ReadJsonValue(json.at("denoiser"), rays.denoiser);
}

void ReadJsonValue(const Json& json, VolumetricFogSettings& fog) {
    ReadJsonValue(json.at("color"), fog.color);
    ReadJsonValue(json.at("density"), fog.density);
    ReadJsonValue(json.at("max_distance"), fog.maxDistance);
    ReadJsonValue(json.at("height_falloff"), fog.heightFalloff);
    ReadJsonValue(json.at("base_height"), fog.baseHeight);
    ReadJsonValue(json.at("anisotropy"), fog.anisotropy);
    ReadJsonValue(json.at("ray_traced_shadows"), fog.rayTracedShadows);
    if (json.contains("density_variation"))
        ReadJsonValue(json.at("density_variation"), fog.densityVariation);
    if (json.contains("multiple_scattering"))
        ReadJsonValue(json.at("multiple_scattering"), fog.multipleScattering);
    if (json.contains("history_weight"))
        ReadJsonValue(json.at("history_weight"), fog.historyWeight);
    if (json.contains("volumes")) {
        const auto& volumes = json.at("volumes");
        RequireValidScene(volumes.is_array() && volumes.size() <= 4);
        fog.volumes.clear();
        for (const auto& v : volumes) {
            LocalFogVolume local;
            const auto& center = v.at("center");
            RequireValidScene(center.is_array() && center.size() == 3);
            for (size_t axis = 0; axis < 3; ++axis)
                ReadJsonValue(center.at(axis), local.center[axis]);
            ReadJsonValue(v.at("radius"), local.radius);
            ReadJsonValue(v.at("density"), local.density);
            fog.volumes.push_back(local);
        }
    }
}

void ReadJsonValue(const Json& json, std::vector<uint8_t>& tiles) {
    RequireValidScene(json.is_array() && json.size() <= 256 * 256);
    tiles.clear();
    tiles.reserve(json.size());
    for (const auto& item : json) {
        uint8_t number;
        ReadJsonValue(item, number);
        RequireValidScene(number <= 5);
        tiles.push_back(number);
    }
}

void ReadJsonValue(const Json& json, std::vector<Keyframe>& keys) {
    RequireValidScene(json.is_array() && json.size() <= 4096);
    keys.clear();
    keys.reserve(json.size());
    double last = -1;
    for (const auto& item : json) {
        Keyframe key;
        ReadJsonValue(item.at("time"), key.time);
        RequireValidScene(key.time >= 0 && key.time > last);
        last = key.time;
        ReadJsonValue(item.at("position"), key.position);
        ReadJsonValue(item.at("rotation"), key.rotation);
        ReadJsonValue(item.at("scale"), key.scale);
        keys.push_back(key);
    }
}

void ReadJsonValue(const Json& json, std::vector<std::pair<uint32, uint32>>& map) {
    RequireValidScene(json.is_array() && json.size() <= MaxEntities);
    map.clear();
    map.reserve(json.size());
    for (const auto& pair : json) {
        RequireValidScene(pair.is_array() && pair.size() == 2);
        uint32 a, b;
        ReadJsonValue(pair[0], a);
        ReadJsonValue(pair[1], b);
        map.emplace_back(a, b);
    }
}

class SceneJsonValidator final : public nlohmann::json_sax<Json> {
public:
    bool null() override {
        return WithinDepth();
    }

    bool boolean(bool) override {
        return WithinDepth();
    }

    bool number_integer(number_integer_t) override {
        return WithinDepth();
    }

    bool number_unsigned(number_unsigned_t) override {
        return WithinDepth();
    }

    bool number_float(number_float_t, const string_t&) override {
        return WithinDepth();
    }

    bool string(string_t&) override {
        return WithinDepth();
    }

    bool binary(binary_t&) override {
        return false;
    }

    bool start_object(std::size_t) override {
        return StartContainer();
    }

    bool start_array(std::size_t) override {
        return StartContainer();
    }

    bool key(string_t& value) override {
        return WithinDepth() && keys_.back().insert(std::move(value)).second;
    }

    bool end_object() override {
        keys_.pop_back();
        return true;
    }

    bool end_array() override {
        keys_.pop_back();
        return true;
    }

    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
        return false;
    }

private:
    bool WithinDepth() const {
        return keys_.size() <= 64;
    }

    bool StartContainer() {
        if (!WithinDepth())
            return false;
        keys_.emplace_back();
        return true;
    }

    std::vector<std::unordered_set<std::string>> keys_;
};

Json ParseSceneJson(const std::string& data) {
    SceneJsonValidator validator;
    RequireValidScene(Json::sax_parse(data, &validator));
    return Json::parse(data);
}
} // namespace

bool Scene::SerializeToString(std::string& output) {
    try {
        if (!ValidateForSerialization())
            return false;
        Json root = {{"format", "LuxEngine.Scene"}, {"version", 1}};
        Json settings = Json::object();
        settings["clear_color"] = ToJson(settings_.clearColor);
        settings["exposure"] = ToJson(settings_.exposure);
        settings["gravity2d"] = ToJson(settings_.gravity);
        settings["gravity3d"] = ToJson(settings_.gravity3D);
        settings["override_gravity"] = ToJson(settings_.overrideGravity);
        settings["fixed_step"] = ToJson(settings_.fixedStep);
        settings["physics_substeps"] = ToJson(settings_.physicsSubsteps);
        Json render = Json::object();
        render["mode"] = ToJson(settings_.render3D.mode);
        render["sun_direction"] = ToJson(settings_.render3D.sun.direction);
        render["sun_color"] = ToJson(settings_.render3D.sun.color);
        render["sun_intensity"] = ToJson(settings_.render3D.sun.intensity);
        render["environment_color"] = ToJson(settings_.render3D.environmentColor);
        render["environment_intensity"] = ToJson(settings_.render3D.environmentIntensity);
        render["environment_rotation"] = ToJson(settings_.render3D.environmentRotation);
        render["environment"] = ToJson(settings_.render3D.environmentPath);
        render["image_based_lighting"] = ToJson(settings_.render3D.imageBasedLighting);
        render["skybox"] = ToJson(settings_.render3D.showSkybox);
        render["tone_mapping"] = ToJson(settings_.render3D.toneMapping);
        if (settings_.render3D.screenSpaceAntialiasing)
            render["screen_space_antialiasing"] =
                ToJson(settings_.render3D.screenSpaceAntialiasing);
        if (settings_.render3D.temporalAntialiasing)
            render["temporal_antialiasing"] = true;
        if (settings_.render3D.motionBlurShutter > 0)
            render["motion_blur_shutter"] = ToJson(settings_.render3D.motionBlurShutter);
        const Render3DSettings defaults;
        const auto& visual = settings_.render3D;
        if (visual.temporalDebugView != defaults.temporalDebugView)
            render["temporal_debug_view"] = static_cast<uint32>(visual.temporalDebugView);
        if (visual.temporalSharpness != defaults.temporalSharpness)
            render["temporal_sharpness"] = ToJson(visual.temporalSharpness);
        if (visual.gtaoRadius != defaults.gtaoRadius)
            render["gtao_radius"] = ToJson(visual.gtaoRadius);
        if (visual.contactShadowDistance != defaults.contactShadowDistance)
            render["contact_shadow_distance"] = ToJson(visual.contactShadowDistance);
        if (visual.shadowLightAngularRadius != defaults.shadowLightAngularRadius)
            render["shadow_light_angular_radius"] = ToJson(visual.shadowLightAngularRadius);
        if (visual.hierarchicalReflections != defaults.hierarchicalReflections)
            render["hierarchical_reflections"] = ToJson(visual.hierarchicalReflections);
        if (visual.bloomIntensity != defaults.bloomIntensity)
            render["bloom_intensity"] = ToJson(visual.bloomIntensity);
        if (visual.bloomThreshold != defaults.bloomThreshold)
            render["bloom_threshold"] = ToJson(visual.bloomThreshold);
        if (visual.physicallyBasedBloom != defaults.physicallyBasedBloom)
            render["physical_bloom"] = ToJson(visual.physicallyBasedBloom);
        if (visual.localExposure != defaults.localExposure)
            render["local_exposure"] = ToJson(visual.localExposure);
        if (visual.apertureBlades != defaults.apertureBlades)
            render["aperture_blades"] = ToJson(visual.apertureBlades);
        if (visual.autoExposure != defaults.autoExposure)
            render["auto_exposure"] = ToJson(visual.autoExposure);
        if (visual.depthOfFieldRadius != defaults.depthOfFieldRadius)
            render["depth_of_field_radius"] = ToJson(visual.depthOfFieldRadius);
        if (visual.metering != defaults.metering) {
            const auto& value = visual.metering;
            render["metering"] = {{"low_percentile", value.lowPercentile},
                                  {"high_percentile", value.highPercentile},
                                  {"min_ev", value.minEV},
                                  {"max_ev", value.maxEV},
                                  {"brighten_speed", value.brightenSpeed},
                                  {"darken_speed", value.darkenSpeed},
                                  {"compensation_ev", value.compensationEV}};
        }
        if (visual.colorGrade != defaults.colorGrade) {
            const auto& value = visual.colorGrade;
            render["color_grade"] = {{"temperature_kelvin", value.temperatureK},
                                     {"tint", value.tint},
                                     {"saturation", value.saturation},
                                     {"vibrance", value.vibrance},
                                     {"contrast", value.contrast},
                                     {"lift", value.lift},
                                     {"gamma", value.gamma},
                                     {"gain", value.gain},
                                     {"shadows", value.shadows},
                                     {"midtones", value.midtones},
                                     {"highlights", value.highlights}};
        }
        if (visual.bloomRadius != defaults.bloomRadius)
            render["bloom_radius"] = visual.bloomRadius;
        if (visual.bloomScatter != defaults.bloomScatter)
            render["bloom_scatter"] = visual.bloomScatter;
        if (!visual.postVolumes.empty()) {
            auto& volumes = render["post_volumes"] = Json::array();
            for (const auto& box : visual.postVolumes) {
                Json volume{{"global", box.global}, {"priority", box.priority},
                            {"center", box.center}, {"half_extent", box.halfExtent},
                            {"weight", box.weight}, {"blend_distance", box.blendDistance}};
                if (box.compensationEV)
                    volume["compensation_ev"] = *box.compensationEV;
                if (box.bloomIntensity)
                    volume["bloom_intensity"] = *box.bloomIntensity;
                if (box.localExposure)
                    volume["local_exposure"] = *box.localExposure;
                if (box.saturation)
                    volume["saturation"] = *box.saturation;
                if (box.temperatureK)
                    volume["temperature_kelvin"] = *box.temperatureK;
                if (box.focusDistance)
                    volume["focus_distance"] = *box.focusDistance;
                volumes.push_back(std::move(volume));
            }
        }
        if (visual.physicalCamera != defaults.physicalCamera) {
            const auto& camera = visual.physicalCamera;
            render["physical_camera"] = {{"exposure", camera.exposure},
                                         {"depth_of_field", camera.depthOfField},
                                         {"autofocus_speed", camera.autofocusSpeed},
                                         {"iso", camera.iso},
                                         {"shutter_seconds", camera.shutterSeconds},
                                         {"motion_blur", camera.motionBlur},
                                         {"f_number", camera.fNumber},
                                         {"sensor_height_mm", camera.sensorHeightMm},
                                         {"luminance_scale", camera.luminanceScale}};
        }
        if (visual.bloomFireflySuppression != defaults.bloomFireflySuppression)
            render["bloom_firefly_suppression"] = visual.bloomFireflySuppression;
        if (visual.colorGradingMix != defaults.colorGradingMix)
            render["color_grading_mix"] = visual.colorGradingMix;
        if (visual.secondaryColorGradingLut != defaults.secondaryColorGradingLut)
            render["secondary_color_grading_lut"] = visual.secondaryColorGradingLut;
        if (visual.textureLodBias != defaults.textureLodBias)
            render["texture_lod_bias"] = visual.textureLodBias;
        if (visual.postDebugView != defaults.postDebugView)
            render["post_debug_view"] = static_cast<uint32>(visual.postDebugView);
        if (visual.focusDistance != defaults.focusDistance)
            render["focus_distance"] = ToJson(visual.focusDistance);
        if (visual.colorGradingLut != defaults.colorGradingLut)
            render["color_grading_lut"] = ToJson(visual.colorGradingLut);
        if (visual.colorGradingStrength != defaults.colorGradingStrength)
            render["color_grading_strength"] = ToJson(visual.colorGradingStrength);
        if (visual.dynamicSkyLighting != defaults.dynamicSkyLighting)
            render["dynamic_sky_lighting"] = ToJson(visual.dynamicSkyLighting);
        if (visual.cloudShadowStrength != defaults.cloudShadowStrength)
            render["cloud_shadow_strength"] = ToJson(visual.cloudShadowStrength);
        if (visual.dynamicAtmosphere != defaults.dynamicAtmosphere)
            render["dynamic_atmosphere"] = ToJson(visual.dynamicAtmosphere);
        if (visual.atmosphereSolarIrradiance != defaults.atmosphereSolarIrradiance)
            render["atmosphere_solar_irradiance"] = visual.atmosphereSolarIrradiance;
        if (visual.aerosolDensity != defaults.aerosolDensity)
            render["aerosol_density"] = ToJson(visual.aerosolDensity);
        if (visual.vignette != defaults.vignette)
            render["vignette"] = visual.vignette;
        if (visual.filmGrain != defaults.filmGrain)
            render["film_grain"] = visual.filmGrain;
        if (visual.chromaticAberration != defaults.chromaticAberration)
            render["chromatic_aberration"] = visual.chromaticAberration;
        if (visual.lensDistortion != defaults.lensDistortion)
            render["lens_distortion"] = visual.lensDistortion;
        if (visual.lensFlareStrength != defaults.lensFlareStrength)
            render["lens_flare_strength"] = visual.lensFlareStrength;
        if (visual.cloudCoverage != defaults.cloudCoverage)
            render["cloud_coverage"] = ToJson(visual.cloudCoverage);
        if (visual.foliageInteractions != defaults.foliageInteractions) {
            for (const auto& interaction : visual.foliageInteractions)
                render["foliage_interactions"].push_back({{"center", ToJson(interaction.center)},
                                                          {"radius", interaction.radius},
                                                          {"strength", interaction.strength}});
        }
        if (visual.windVelocity.x != 0 || visual.windVelocity.y != 0)
            render["wind_velocity"] = ToJson(visual.windVelocity);
        if (visual.rainfall != defaults.rainfall)
            render["rainfall"] = visual.rainfall;
        if (visual.surfaceWetness != defaults.surfaceWetness)
            render["surface_wetness"] = visual.surfaceWetness;
        if (visual.weatherTime != defaults.weatherTime)
            render["weather_time"] = ToJson(visual.weatherTime);
        render["shadows"] = ToJson(settings_.render3D.shadows);
        render["shadow_resolution"] = ToJson(settings_.render3D.shadowResolution);
        render["shadow_range"] = ToJson(settings_.render3D.shadowRange);
        render["shadow_depth_bias"] = ToJson(settings_.render3D.shadowDepthBias);
        render["shadow_normal_bias"] = ToJson(settings_.render3D.shadowNormalBias);
        const auto rays = ToJson(settings_.render3D.rayTracing);
        const auto fog = ToJson(settings_.render3D.fog);
        if (rays != ToJson(RayTracingSettings{}))
            render["ray_tracing"] = rays;
        if (fog != ToJson(VolumetricFogSettings{}))
            render["volumetric_fog"] = fog;
        if (settings_.render3D.cascadedShadows)
            render["cascaded_shadows"] = true;
        if (settings_.render3D.virtualShadows)
            render["virtual_shadows"] = true;
        const auto& water = settings_.render3D.waterReflection;
        if (!water.environmentPath.empty() || water.rotation != 0 || water.intensity != 1 ||
            water.faceSize != WaterReflectionSettings{}.faceSize)
            render["water_reflection"] = {{"environment", water.environmentPath},
                                          {"rotation", water.rotation},
                                          {"intensity", water.intensity},
                                          {"face_size", water.faceSize}};
        const auto& cascades = settings_.render3D.radianceCascades;
        if (cascades.strength != 0 || cascades.spacing != RadianceCascadeSettings{}.spacing)
            render["radiance_cascades"] = {{"strength", cascades.strength},
                                           {"spacing", cascades.spacing}};
        settings["render3d"] = std::move(render);
        root["settings"] = std::move(settings);
        root["entities"] = Json::array();
        std::vector<uint32> ids(entities_.begin(), entities_.end());
        std::sort(ids.begin(), ids.end());
        const auto firstFreeId = ids.empty() ? 1u : ids.back() + 1;
        if (nextEntityId_ > firstFreeId && std::any_of(ids.begin(), ids.end(), [&](const auto id) {
                const auto* reference = TryGetComponent<SceneReferenceComponent>(id);
                return reference && !reference->suspended.empty();
            }))
            root["next_entity_id"] = nextEntityId_;
        for (auto id : ids) {
            Json components = Json::object();
            if (const auto* component = TryGetComponent<ScriptComponent>(id)) {
                std::string fields;
                RequireValidScene(SerializeScriptFields(component->fields, fields));
                components["script"] = {{"type", component->typeName},
                                        {"enabled", component->enabled},
                                        {"fields", Json::parse(fields)}};
            }
            if (const auto* component = TryGetComponent<TagComponent>(id)) {
                Json value = Json::object();
                value["name"] = ToJson(component->tag);
                components["tag"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<TransformComponent>(id)) {
                Json value = Json::object();
                value["position"] = ToJson(component->position);
                value["rotation"] = ToJson(component->rotation);
                value["scale"] = ToJson(component->scale);
                components["transform"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<SpriteRendererComponent>(id)) {
                Json value = Json::object();
                value["color"] = ToJson(component->color);
                value["texture"] = component->texturePath;
                value["frame"] = component->frame;
                value["columns"] = component->columns;
                value["rows"] = component->rows;
                components["sprite"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<MeshRendererComponent>(id)) {
                Json value = Json::object();
                value["albedo"] = ToJson(component->material.albedoColor);
                value["metallic"] = ToJson(component->material.metallic);
                value["roughness"] = ToJson(component->material.roughness);
                value["asset"] = ToJson(component->meshPath);
                value["model_submesh"] = component->modelSubmesh;
                value["base_color_texture"] = ToJson(component->material.baseColorTexture);
                value["alpha_mode"] = static_cast<int>(component->material.alphaMode);
                value["alpha_cutoff"] = ToJson(component->material.alphaCutoff);
                value["double_sided"] = component->material.doubleSided;
                if (component->material.gpuSkinning)
                    value["gpu_skinning"] = true;
                if (component->material.lodTransition != 0)
                    value["lod_transition"] = component->material.lodTransition;
                if (const auto& s = component->material.surface) {
                    Json block = Json::object();
                    block["model"] = static_cast<uint32>(s->model);
                    block["impostor_depth"] = s->impostorDepth;
                    block["clear_coat"] = ToJson(s->clearCoat);
                    block["coat_roughness"] = ToJson(s->coatRoughness);
                    block["anisotropy"] = ToJson(s->anisotropy);
                    block["sheen"] = ToJson(s->sheen);
                    block["transmission"] = ToJson(s->transmission);
                    block["ior"] = ToJson(s->indexOfRefraction);
                    block["detail_scale"] = ToJson(s->detailScale);
                    block["specular_tint"] = ToJson(s->specularTint);
                    block["thickness_texture"] = ToJson(s->thicknessTexture);
                    block["coat_normal"] = ToJson(s->coatNormalTexture);
                    block["detail_color"] = ToJson(s->detailColorTexture);
                    block["detail_normal"] = ToJson(s->detailNormalTexture);
                    block["detail_roughness"] = ToJson(s->detailRoughnessTexture);
                    value["principled_surface"] = std::move(block);
                }
                if (component->material.wettingResponse > 0)
                    value["wetting_response"] = component->material.wettingResponse;
                if (const auto& water = component->material.water)
                    value["water"] = {{"absorption", ToJson(water->absorption)},
                                      {"ior", water->indexOfRefraction},
                                      {"ripple_strength", water->rippleStrength}};
                if (component->material.windAmplitude > 0)
                    value["wind_amplitude"] = component->material.windAmplitude;
                if (component->material.subsurfaceRadius.Dot(component->material.subsurfaceRadius) >
                    0)
                    value["subsurface_radius"] = ToJson(component->material.subsurfaceRadius);
                if (component->material.subsurfaceThickness > 0)
                    value["subsurface_thickness"] = component->material.subsurfaceThickness;
                if (component->material.emissive.Dot(component->material.emissive) > 0)
                    value["emissive"] = ToJson(component->material.emissive);
                if (!component->material.heightTexture.empty())
                    value["height_texture"] = component->material.heightTexture;
                if (component->material.parallaxDepth != 0)
                    value["parallax_depth"] = component->material.parallaxDepth;
                if (const auto& layer = component->material.terrainLayer) {
                    value["terrain_layer"] = {{"base_color_texture", layer->baseColorTexture},
                                              {"normal_texture", layer->normalTexture},
                                              {"roughness_texture", layer->roughnessTexture},
                                              {"height_texture", layer->heightTexture},
                                              {"height_blend", layer->heightBlend}};
                    if (layer->palette)
                        value["terrain_layer"]["palette"] = true;
                }
                if (!component->material.emissiveTexture.empty())
                    value["emissive_texture"] = component->material.emissiveTexture;
                value["normal_texture"] = component->material.normalTexture;
                value["metallic_texture"] = component->material.metallicTexture;
                value["roughness_texture"] = component->material.roughnessTexture;
                value["occlusion_texture"] = component->material.occlusionTexture;
                value["normal_scale"] = component->material.normalScale;
                value["occlusion_strength"] = component->material.occlusionStrength;
                value["sampling"] = {
                    {"mipmaps", component->material.textureSampling.mipmaps},
                    {"address", static_cast<int>(component->material.textureSampling.addressMode)},
                    {"anisotropic", component->material.textureSampling.anisotropic}};
                components["mesh"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<ModelAnimationComponent>(id)) {
                components["model_animation"] = {
                    {"asset", component->modelPath},
                    {"node", component->nodeIndex},
                    {"clip", component->clip},
                    {"time", component->time},
                    {"speed", component->speed},
                    {"playing", component->playing},
                    {"loop", component->loop},
                    {"apply_node_transform", component->applyNodeTransform}};
            }
            if (const auto* component = TryGetComponent<CameraComponent>(id)) {
                Json value = Json::object();
                value["primary"] = ToJson(component->isPrimary);
                value["perspective"] = ToJson(component->isPerspective);
                value["fov"] = ToJson(component->fov);
                value["ortho_size"] = ToJson(component->orthoSize);
                value["near_clip"] = component->nearClip;
                value["far_clip"] = component->farClip;
                components["camera"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<Rigidbody2DComponent>(id)) {
                Json value = Json::object();
                value["type"] = ToJson(component->type);
                value["velocity"] = ToJson(component->velocity);
                value["mass"] = ToJson(component->mass);
                value["gravity_scale"] = ToJson(component->gravityScale);
                value["grounded"] = ToJson(component->isGrounded);
                components["rigidbody2d"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<BoxCollider2DComponent>(id)) {
                Json value = Json::object();
                value["size"] = ToJson(component->size);
                value["offset"] = ToJson(component->offset);
                value["trigger"] = ToJson(component->isTrigger);
                components["box_collider2d"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<Rigidbody3DComponent>(id)) {
                Json value = Json::object();
                value["type"] = ToJson(component->type);
                value["velocity"] = ToJson(component->velocity);
                value["mass"] = ToJson(component->mass);
                value["gravity_scale"] = ToJson(component->gravityScale);
                components["rigidbody3d"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<BoxCollider3DComponent>(id)) {
                Json value = Json::object();
                value["size"] = ToJson(component->size);
                value["offset"] = ToJson(component->offset);
                value["trigger"] = ToJson(component->isTrigger);
                components["box_collider3d"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<CapsuleCollider3DComponent>(id)) {
                Json value = Json::object();
                value["radius"] = ToJson(component->radius);
                value["half_height"] = ToJson(component->halfHeight);
                value["offset"] = ToJson(component->offset);
                value["trigger"] = ToJson(component->isTrigger);
                components["capsule_collider3d"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<CharacterController3DComponent>(id)) {
                Json value = Json::object();
                value["radius"] = ToJson(component->radius);
                value["half_height"] = ToJson(component->halfHeight);
                value["max_slope_radians"] = ToJson(component->maxSlopeRadians);
                value["step_height"] = ToJson(component->stepHeight);
                components["character_controller3d"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<TriangleMeshCollider3DComponent>(id)) {
                Json vertices = Json::array();
                for (const auto& vertex : component->mesh->vertices)
                    vertices.push_back(ToJson(vertex));
                components["triangle_mesh_collider3d"] = {{"vertices", std::move(vertices)},
                                                          {"indices", component->mesh->indices}};
            }
            if (const auto* component = TryGetComponent<TilemapComponent>(id)) {
                Json value = Json::object();
                value["width"] = ToJson(component->width);
                value["height"] = ToJson(component->height);
                value["tile_size"] = ToJson(component->tileSize);
                value["tiles"] = ToJson(component->tiles);
                components["tilemap"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<AnimationComponent>(id)) {
                Json value = Json::object();
                value["name"] = ToJson(component->clip.name);
                value["duration"] = ToJson(component->clip.duration);
                value["loop"] = ToJson(component->clip.loop);
                value["playing"] = ToJson(component->playing);
                value["keyframes"] = ToJson(component->clip.keyframes);
                value["asset"] = component->assetPath;
                value["speed"] = component->speed;
                value["root_motion"] = component->rootMotion;
                std::string clipData, error;
                RequireValidScene(component->clip.SerializeToString(clipData, error));
                value["clip_data"] = Json::parse(clipData);
                components["animation"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<AnimatorComponent>(id)) {
                Json parameters = Json::array();
                for (const auto& parameter : component->parameters)
                    parameters.push_back({{"name", parameter.name},
                                          {"type", int(parameter.type)},
                                          {"value", parameter.value}});
                components["animator"] = {{"controller", component->controllerPath},
                                          {"playing", component->playing},
                                          {"root_motion", component->rootMotion},
                                          {"parameters", parameters}};
            }
            if (const auto* component = TryGetComponent<HierarchyComponent>(id)) {
                Json value = Json::object();
                value["parent"] = ToJson(component->parent);
                value["group"] = ToJson(component->group);
                components["hierarchy"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<SceneReferenceComponent>(id)) {
                Json value = Json::object();
                value["path"] = ToJson(component->path);
                value["mode"] = ToJson(component->mode);
                value["baseline"] = ToJson(component->baseline);
                value["suspended"] = ToJson(component->suspended);
                value["stream_center"] = ToJson(component->streamCenter);
                value["load_radius"] = ToJson(component->streamLoadRadius);
                value["unload_radius"] = ToJson(component->streamUnloadRadius);
                value["loaded"] = ToJson(component->loaded);
                value["entity_map"] = ToJson(component->entityMap);
                components["scene_reference"] = std::move(value);
            }
            if (const auto* component = TryGetComponent<InstanceSourceComponent>(id)) {
                Json value = Json::object();
                value["root"] = ToJson(component->instanceRoot);
                value["entity"] = ToJson(component->sourceEntity);
                components["instance_source"] = std::move(value);
            }
            Json entity = {{"id", id}, {"components", std::move(components)}};
            if (const auto identity = referenceIdentities_.find(id);
                identity != referenceIdentities_.end())
                entity["reference_identity"] = identity->second;
            root["entities"].push_back(std::move(entity));
        }
        auto serialized = root.dump(2) + '\n';
        if (serialized.size() > MaxSceneBytes)
            return false;
        output = std::move(serialized);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Scene::DeserializeJson(const std::string& data) {
    try {
        const auto root = ParseSceneJson(data);
        RequireValidScene(root.is_object() && root.at("format") == "LuxEngine.Scene");
        uint32 version = 0;
        ReadJsonValue(root.at("version"), version);
        RequireValidScene(version == 1);
        Scene parsed;
        SceneSettings settings;
        const auto& source = root.at("settings");
        ReadJsonValue(source.at("clear_color"), settings.clearColor);
        ReadJsonValue(source.at("exposure"), settings.exposure);
        ReadJsonValue(source.at("gravity2d"), settings.gravity);
        ReadJsonValue(source.at("gravity3d"), settings.gravity3D);
        ReadJsonValue(source.at("override_gravity"), settings.overrideGravity);
        ReadJsonValue(source.at("fixed_step"), settings.fixedStep);
        ReadJsonValue(source.at("physics_substeps"), settings.physicsSubsteps);
        const auto& render = source.at("render3d");
        ReadJsonValue(render.at("mode"), settings.render3D.mode);
        ReadJsonValue(render.at("sun_direction"), settings.render3D.sun.direction);
        ReadJsonValue(render.at("sun_color"), settings.render3D.sun.color);
        ReadJsonValue(render.at("sun_intensity"), settings.render3D.sun.intensity);
        ReadJsonValue(render.at("environment_color"), settings.render3D.environmentColor);
        ReadJsonValue(render.at("environment_intensity"), settings.render3D.environmentIntensity);
        ReadJsonValue(render.at("environment_rotation"), settings.render3D.environmentRotation);
        ReadJsonValue(render.at("environment"), settings.render3D.environmentPath);
        ReadJsonValue(render.at("image_based_lighting"), settings.render3D.imageBasedLighting);
        ReadJsonValue(render.at("skybox"), settings.render3D.showSkybox);
        ReadJsonValue(render.at("tone_mapping"), settings.render3D.toneMapping);
        if (const auto value = render.find("screen_space_antialiasing"); value != render.end())
            ReadJsonValue(*value, settings.render3D.screenSpaceAntialiasing);
        if (const auto value = render.find("temporal_antialiasing"); value != render.end())
            ReadJsonValue(*value, settings.render3D.temporalAntialiasing);
        if (render.contains("temporal_debug_view")) {
            uint32 view = 0;
            ReadJsonValue(render.at("temporal_debug_view"), view);
            RequireValidScene(view <= static_cast<uint32>(TemporalDebugView::Transparency));
            settings.render3D.temporalDebugView = static_cast<TemporalDebugView>(view);
        }
        if (render.contains("temporal_sharpness"))
            ReadJsonValue(render.at("temporal_sharpness"), settings.render3D.temporalSharpness);
        if (render.contains("motion_blur_shutter"))
            ReadJsonValue(render.at("motion_blur_shutter"), settings.render3D.motionBlurShutter);
        if (render.contains("gtao_radius"))
            ReadJsonValue(render.at("gtao_radius"), settings.render3D.gtaoRadius);
        if (render.contains("contact_shadow_distance"))
            ReadJsonValue(render.at("contact_shadow_distance"),
                          settings.render3D.contactShadowDistance);
        if (render.contains("shadow_light_angular_radius"))
            ReadJsonValue(render.at("shadow_light_angular_radius"),
                          settings.render3D.shadowLightAngularRadius);
        if (render.contains("hierarchical_reflections"))
            ReadJsonValue(render.at("hierarchical_reflections"),
                          settings.render3D.hierarchicalReflections);
        if (render.contains("bloom_intensity"))
            ReadJsonValue(render.at("bloom_intensity"), settings.render3D.bloomIntensity);
        if (render.contains("bloom_threshold"))
            ReadJsonValue(render.at("bloom_threshold"), settings.render3D.bloomThreshold);
        if (render.contains("physical_bloom"))
            ReadJsonValue(render.at("physical_bloom"), settings.render3D.physicallyBasedBloom);
        if (render.contains("local_exposure"))
            ReadJsonValue(render.at("local_exposure"), settings.render3D.localExposure);
        if (render.contains("aperture_blades"))
            ReadJsonValue(render.at("aperture_blades"), settings.render3D.apertureBlades);
        if (render.contains("auto_exposure"))
            ReadJsonValue(render.at("auto_exposure"), settings.render3D.autoExposure);
        if (render.contains("depth_of_field_radius"))
            ReadJsonValue(render.at("depth_of_field_radius"), settings.render3D.depthOfFieldRadius);
        if (render.contains("metering")) {
            const auto& block = render.at("metering");
            RequireValidScene(block.is_object());
            auto& value = settings.render3D.metering;
            if (block.contains("low_percentile"))
                ReadJsonValue(block.at("low_percentile"), value.lowPercentile);
            if (block.contains("high_percentile"))
                ReadJsonValue(block.at("high_percentile"), value.highPercentile);
            if (block.contains("min_ev"))
                ReadJsonValue(block.at("min_ev"), value.minEV);
            if (block.contains("max_ev"))
                ReadJsonValue(block.at("max_ev"), value.maxEV);
            if (block.contains("brighten_speed"))
                ReadJsonValue(block.at("brighten_speed"), value.brightenSpeed);
            if (block.contains("darken_speed"))
                ReadJsonValue(block.at("darken_speed"), value.darkenSpeed);
            if (block.contains("compensation_ev"))
                ReadJsonValue(block.at("compensation_ev"), value.compensationEV);
        }
        if (render.contains("color_grade")) {
            const auto& block = render.at("color_grade");
            RequireValidScene(block.is_object());
            auto& value = settings.render3D.colorGrade;
            if (block.contains("temperature_kelvin"))
                ReadJsonValue(block.at("temperature_kelvin"), value.temperatureK);
            if (block.contains("tint"))
                ReadJsonValue(block.at("tint"), value.tint);
            if (block.contains("saturation"))
                ReadJsonValue(block.at("saturation"), value.saturation);
            if (block.contains("vibrance"))
                ReadJsonValue(block.at("vibrance"), value.vibrance);
            if (block.contains("contrast"))
                ReadJsonValue(block.at("contrast"), value.contrast);
            if (block.contains("lift")) {
                const auto& triplet = block.at("lift");
                RequireValidScene(triplet.is_array() && triplet.size() == 3);
                for (size_t i = 0; i < 3; ++i)
                    ReadJsonValue(triplet.at(i), value.lift[i]);
            }
            if (block.contains("gamma")) {
                const auto& triplet = block.at("gamma");
                RequireValidScene(triplet.is_array() && triplet.size() == 3);
                for (size_t i = 0; i < 3; ++i)
                    ReadJsonValue(triplet.at(i), value.gamma[i]);
            }
            if (block.contains("gain")) {
                const auto& triplet = block.at("gain");
                RequireValidScene(triplet.is_array() && triplet.size() == 3);
                for (size_t i = 0; i < 3; ++i)
                    ReadJsonValue(triplet.at(i), value.gain[i]);
            }
            if (block.contains("shadows")) {
                const auto& triplet = block.at("shadows");
                RequireValidScene(triplet.is_array() && triplet.size() == 3);
                for (size_t i = 0; i < 3; ++i)
                    ReadJsonValue(triplet.at(i), value.shadows[i]);
            }
            if (block.contains("midtones")) {
                const auto& triplet = block.at("midtones");
                RequireValidScene(triplet.is_array() && triplet.size() == 3);
                for (size_t i = 0; i < 3; ++i)
                    ReadJsonValue(triplet.at(i), value.midtones[i]);
            }
            if (block.contains("highlights")) {
                const auto& triplet = block.at("highlights");
                RequireValidScene(triplet.is_array() && triplet.size() == 3);
                for (size_t i = 0; i < 3; ++i)
                    ReadJsonValue(triplet.at(i), value.highlights[i]);
            }
        }
        if (render.contains("bloom_radius"))
            ReadJsonValue(render.at("bloom_radius"), settings.render3D.bloomRadius);
        if (render.contains("bloom_scatter"))
            ReadJsonValue(render.at("bloom_scatter"), settings.render3D.bloomScatter);
        if (render.contains("post_volumes")) {
            const auto& volumes = render.at("post_volumes");
            RequireValidScene(volumes.is_array() && volumes.size() <= 64);
            settings.render3D.postVolumes.clear();
            for (const auto& volume : volumes) {
                RequireValidScene(volume.is_object());
                PostProcessVolume box;
                if (volume.contains("global"))
                    ReadJsonValue(volume.at("global"), box.global);
                if (volume.contains("priority"))
                    ReadJsonValue(volume.at("priority"), box.priority);
                if (volume.contains("blend_distance"))
                    ReadJsonValue(volume.at("blend_distance"), box.blendDistance);
                if (volume.contains("weight"))
                    ReadJsonValue(volume.at("weight"), box.weight);
                if (volume.contains("center")) {
                    const auto& p = volume.at("center");
                    RequireValidScene(p.is_array() && p.size() == 3);
                    for (size_t i = 0; i < 3; ++i)
                        ReadJsonValue(p.at(i), box.center[i]);
                }
                if (volume.contains("half_extent")) {
                    const auto& p = volume.at("half_extent");
                    RequireValidScene(p.is_array() && p.size() == 3);
                    for (size_t i = 0; i < 3; ++i)
                        ReadJsonValue(p.at(i), box.halfExtent[i]);
                }
                if (volume.contains("compensation_ev"))
                    ReadJsonValue(volume.at("compensation_ev"), box.compensationEV.emplace());
                if (volume.contains("bloom_intensity"))
                    ReadJsonValue(volume.at("bloom_intensity"), box.bloomIntensity.emplace());
                if (volume.contains("local_exposure"))
                    ReadJsonValue(volume.at("local_exposure"), box.localExposure.emplace());
                if (volume.contains("saturation"))
                    ReadJsonValue(volume.at("saturation"), box.saturation.emplace());
                if (volume.contains("temperature_kelvin"))
                    ReadJsonValue(volume.at("temperature_kelvin"), box.temperatureK.emplace());
                if (volume.contains("focus_distance"))
                    ReadJsonValue(volume.at("focus_distance"), box.focusDistance.emplace());
                settings.render3D.postVolumes.push_back(std::move(box));
            }
        }
        if (render.contains("physical_camera")) {
            const auto& camera = render.at("physical_camera");
            RequireValidScene(camera.is_object());
            auto& physical = settings.render3D.physicalCamera;
            if (camera.contains("exposure"))
                ReadJsonValue(camera.at("exposure"), physical.exposure);
            if (camera.contains("depth_of_field"))
                ReadJsonValue(camera.at("depth_of_field"), physical.depthOfField);
            if (camera.contains("autofocus_speed"))
                ReadJsonValue(camera.at("autofocus_speed"), physical.autofocusSpeed);
            if (camera.contains("iso"))
                ReadJsonValue(camera.at("iso"), physical.iso);
            if (camera.contains("motion_blur"))
                ReadJsonValue(camera.at("motion_blur"), physical.motionBlur);
            if (camera.contains("shutter_seconds"))
                ReadJsonValue(camera.at("shutter_seconds"), physical.shutterSeconds);
            if (camera.contains("f_number"))
                ReadJsonValue(camera.at("f_number"), physical.fNumber);
            if (camera.contains("sensor_height_mm"))
                ReadJsonValue(camera.at("sensor_height_mm"), physical.sensorHeightMm);
            if (camera.contains("luminance_scale"))
                ReadJsonValue(camera.at("luminance_scale"), physical.luminanceScale);
        }
        if (render.contains("bloom_firefly_suppression"))
            ReadJsonValue(render.at("bloom_firefly_suppression"),
                          settings.render3D.bloomFireflySuppression);
        if (render.contains("color_grading_mix"))
            ReadJsonValue(render.at("color_grading_mix"), settings.render3D.colorGradingMix);
        if (render.contains("secondary_color_grading_lut"))
            ReadJsonValue(render.at("secondary_color_grading_lut"),
                          settings.render3D.secondaryColorGradingLut);
        if (render.contains("texture_lod_bias"))
            ReadJsonValue(render.at("texture_lod_bias"), settings.render3D.textureLodBias);
        if (render.contains("post_debug_view")) {
            uint32 mode = 0;
            ReadJsonValue(render.at("post_debug_view"), mode);
            settings.render3D.postDebugView = static_cast<PostDebugView>(mode);
        }
        if (render.contains("focus_distance"))
            ReadJsonValue(render.at("focus_distance"), settings.render3D.focusDistance);
        if (render.contains("color_grading_lut"))
            ReadJsonValue(render.at("color_grading_lut"), settings.render3D.colorGradingLut);
        if (render.contains("color_grading_strength"))
            ReadJsonValue(render.at("color_grading_strength"),
                          settings.render3D.colorGradingStrength);
        if (render.contains("dynamic_sky_lighting"))
            ReadJsonValue(render.at("dynamic_sky_lighting"), settings.render3D.dynamicSkyLighting);
        if (render.contains("cloud_shadow_strength"))
            ReadJsonValue(render.at("cloud_shadow_strength"),
                          settings.render3D.cloudShadowStrength);
        if (render.contains("dynamic_atmosphere"))
            ReadJsonValue(render.at("dynamic_atmosphere"), settings.render3D.dynamicAtmosphere);
        if (render.contains("atmosphere_solar_irradiance"))
            ReadJsonValue(render.at("atmosphere_solar_irradiance"),
                          settings.render3D.atmosphereSolarIrradiance);
        if (render.contains("aerosol_density"))
            ReadJsonValue(render.at("aerosol_density"), settings.render3D.aerosolDensity);
        if (render.contains("vignette"))
            ReadJsonValue(render.at("vignette"), settings.render3D.vignette);
        if (render.contains("film_grain"))
            ReadJsonValue(render.at("film_grain"), settings.render3D.filmGrain);
        if (render.contains("chromatic_aberration"))
            ReadJsonValue(render.at("chromatic_aberration"), settings.render3D.chromaticAberration);
        if (render.contains("lens_distortion"))
            ReadJsonValue(render.at("lens_distortion"), settings.render3D.lensDistortion);
        if (render.contains("lens_flare_strength"))
            ReadJsonValue(render.at("lens_flare_strength"), settings.render3D.lensFlareStrength);
        if (render.contains("cloud_coverage"))
            ReadJsonValue(render.at("cloud_coverage"), settings.render3D.cloudCoverage);
        if (render.contains("foliage_interactions")) {
            const auto& interactions = render.at("foliage_interactions");
            RequireValidScene(interactions.is_array() &&
                              interactions.size() <= settings.render3D.foliageInteractions.size());
            for (size_t i = 0; i < interactions.size(); ++i) {
                auto& destination = settings.render3D.foliageInteractions[i];
                ReadJsonValue(interactions.at(i).at("center"), destination.center);
                ReadJsonValue(interactions.at(i).at("radius"), destination.radius);
                ReadJsonValue(interactions.at(i).at("strength"), destination.strength);
            }
        }
        if (render.contains("wind_velocity"))
            ReadJsonValue(render.at("wind_velocity"), settings.render3D.windVelocity);
        if (render.contains("rainfall"))
            ReadJsonValue(render.at("rainfall"), settings.render3D.rainfall);
        if (render.contains("surface_wetness"))
            ReadJsonValue(render.at("surface_wetness"), settings.render3D.surfaceWetness);
        if (render.contains("weather_time"))
            ReadJsonValue(render.at("weather_time"), settings.render3D.weatherTime);
        ReadJsonValue(render.at("shadows"), settings.render3D.shadows);
        ReadJsonValue(render.at("shadow_resolution"), settings.render3D.shadowResolution);
        ReadJsonValue(render.at("shadow_range"), settings.render3D.shadowRange);
        ReadJsonValue(render.at("shadow_depth_bias"), settings.render3D.shadowDepthBias);
        ReadJsonValue(render.at("shadow_normal_bias"), settings.render3D.shadowNormalBias);
        if (render.contains("ray_tracing"))
            ReadJsonValue(render.at("ray_tracing"), settings.render3D.rayTracing);
        if (render.contains("volumetric_fog"))
            ReadJsonValue(render.at("volumetric_fog"), settings.render3D.fog);
        if (render.contains("cascaded_shadows"))
            ReadJsonValue(render.at("cascaded_shadows"), settings.render3D.cascadedShadows);
        if (render.contains("virtual_shadows"))
            ReadJsonValue(render.at("virtual_shadows"), settings.render3D.virtualShadows);
        if (render.contains("water_reflection")) {
            const auto& water = render.at("water_reflection");
            auto& reflection = settings.render3D.waterReflection;
            ReadJsonValue(water.at("environment"), reflection.environmentPath);
            ReadJsonValue(water.at("rotation"), reflection.rotation);
            ReadJsonValue(water.at("intensity"), reflection.intensity);
            if (water.contains("face_size"))
                ReadJsonValue(water.at("face_size"), reflection.faceSize);
        }
        if (render.contains("radiance_cascades")) {
            const auto& cascades = render.at("radiance_cascades");
            ReadJsonValue(cascades.at("strength"), settings.render3D.radianceCascades.strength);
            ReadJsonValue(cascades.at("spacing"), settings.render3D.radianceCascades.spacing);
        }
        RequireValidScene(parsed.SetSettings(settings));
        const auto& entities = root.at("entities");
        RequireValidScene(entities.is_array() && entities.size() <= MaxEntities);
        std::set<uint32> ids;
        for (const auto& item : entities) {
            uint32 id = 0;
            ReadJsonValue(item.at("id"), id);
            RequireValidScene(id > 0 && id < std::numeric_limits<uint32>::max() &&
                              ids.insert(id).second);
            parsed.nextEntityId_ = id;
            auto entity = parsed.CreateEntity();
            if (item.contains("reference_identity")) {
                uint64 identity = 0;
                ReadJsonValue(item.at("reference_identity"), identity);
                RequireValidScene(parsed.SetReferenceIdentity(id, identity));
            }
            entity.RemoveComponent<TagComponent>();
            entity.RemoveComponent<TransformComponent>();
            const auto& components = item.at("components");
            RequireValidScene(components.is_object());
            for (const auto& [name, value] : components.items()) {
                if (name == "script") {
                    ScriptComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("type"), component.typeName);
                    ReadJsonValue(value.at("enabled"), component.enabled);
                    if (value.contains("fields"))
                        RequireValidScene(
                            DeserializeScriptFields(value.at("fields").dump(), component.fields));
                    entity.AddComponent<ScriptComponent>(std::move(component));
                } else if (name == "tag") {
                    TagComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("name"), component.tag);
                    entity.AddComponent<TagComponent>(std::move(component));
                } else if (name == "transform") {
                    TransformComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("position"), component.position);
                    ReadJsonValue(value.at("rotation"), component.rotation);
                    ReadJsonValue(value.at("scale"), component.scale);
                    entity.AddComponent<TransformComponent>(std::move(component));
                } else if (name == "sprite") {
                    SpriteRendererComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("color"), component.color);
                    if (value.contains("texture"))
                        ReadJsonValue(value.at("texture"), component.texturePath);
                    if (value.contains("frame"))
                        ReadJsonValue(value.at("frame"), component.frame);
                    if (value.contains("columns"))
                        ReadJsonValue(value.at("columns"), component.columns);
                    if (value.contains("rows"))
                        ReadJsonValue(value.at("rows"), component.rows);
                    entity.AddComponent<SpriteRendererComponent>(std::move(component));
                } else if (name == "mesh") {
                    MeshRendererComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("albedo"), component.material.albedoColor);
                    ReadJsonValue(value.at("metallic"), component.material.metallic);
                    ReadJsonValue(value.at("roughness"), component.material.roughness);
                    ReadJsonValue(value.at("asset"), component.meshPath);
                    if (value.contains("model_submesh"))
                        ReadJsonValue(value.at("model_submesh"), component.modelSubmesh);
                    ReadJsonValue(value.at("base_color_texture"),
                                  component.material.baseColorTexture);
                    if (value.contains("alpha_mode")) {
                        int mode = 0;
                        ReadJsonValue(value.at("alpha_mode"), mode);
                        RequireValidScene(mode >= 0 && mode <= 2);
                        component.material.alphaMode = static_cast<MaterialAlphaMode>(mode);
                    }
                    if (value.contains("alpha_cutoff")) {
                        ReadJsonValue(value.at("alpha_cutoff"), component.material.alphaCutoff);
                        RequireValidScene(component.material.alphaCutoff >= 0 &&
                                          component.material.alphaCutoff <= 1);
                    }
                    if (value.contains("double_sided"))
                        ReadJsonValue(value.at("double_sided"), component.material.doubleSided);
                    if (value.contains("wetting_response"))
                        ReadJsonValue(value.at("wetting_response"),
                                      component.material.wettingResponse);
                    if (value.contains("water")) {
                        const auto& data = value.at("water");
                        auto& water = component.material.water.emplace();
                        ReadJsonValue(data.at("absorption"), water.absorption);
                        ReadJsonValue(data.at("ior"), water.indexOfRefraction);
                        if (data.contains("ripple_strength"))
                            ReadJsonValue(data.at("ripple_strength"), water.rippleStrength);
                    }
                    if (value.contains("gpu_skinning"))
                        ReadJsonValue(value.at("gpu_skinning"), component.material.gpuSkinning);
                    if (value.contains("lod_transition"))
                        ReadJsonValue(value.at("lod_transition"), component.material.lodTransition);
                    if (value.contains("principled_surface")) {
                        const auto& block = value.at("principled_surface");
                        RequireValidScene(block.is_object());
                        auto& s = component.material.surface.emplace();
                        if (block.contains("impostor_depth"))
                            ReadJsonValue(block.at("impostor_depth"), s.impostorDepth);
                        if (block.contains("model")) {
                            uint32 model = 0;
                            ReadJsonValue(block.at("model"), model);
                            RequireValidScene(model <=
                                              static_cast<uint32>(SurfaceModel::Character));
                            s.model = static_cast<SurfaceModel>(model);
                        }
                        if (block.contains("clear_coat"))
                            ReadJsonValue(block.at("clear_coat"), s.clearCoat);
                        if (block.contains("coat_roughness"))
                            ReadJsonValue(block.at("coat_roughness"), s.coatRoughness);
                        if (block.contains("anisotropy"))
                            ReadJsonValue(block.at("anisotropy"), s.anisotropy);
                        if (block.contains("sheen"))
                            ReadJsonValue(block.at("sheen"), s.sheen);
                        if (block.contains("transmission"))
                            ReadJsonValue(block.at("transmission"), s.transmission);
                        if (block.contains("ior"))
                            ReadJsonValue(block.at("ior"), s.indexOfRefraction);
                        if (block.contains("detail_scale"))
                            ReadJsonValue(block.at("detail_scale"), s.detailScale);
                        if (block.contains("specular_tint"))
                            ReadJsonValue(block.at("specular_tint"), s.specularTint);
                        if (block.contains("thickness_texture"))
                            ReadJsonValue(block.at("thickness_texture"), s.thicknessTexture);
                        if (block.contains("coat_normal"))
                            ReadJsonValue(block.at("coat_normal"), s.coatNormalTexture);
                        if (block.contains("detail_color"))
                            ReadJsonValue(block.at("detail_color"), s.detailColorTexture);
                        if (block.contains("detail_normal"))
                            ReadJsonValue(block.at("detail_normal"), s.detailNormalTexture);
                        if (block.contains("detail_roughness"))
                            ReadJsonValue(block.at("detail_roughness"), s.detailRoughnessTexture);
                    }
                    if (value.contains("wind_amplitude"))
                        ReadJsonValue(value.at("wind_amplitude"), component.material.windAmplitude);
                    if (value.contains("subsurface_radius"))
                        ReadJsonValue(value.at("subsurface_radius"),
                                      component.material.subsurfaceRadius);
                    if (value.contains("subsurface_thickness"))
                        ReadJsonValue(value.at("subsurface_thickness"),
                                      component.material.subsurfaceThickness);
                    if (value.contains("emissive"))
                        ReadJsonValue(value.at("emissive"), component.material.emissive);
                    if (value.contains("height_texture"))
                        ReadJsonValue(value.at("height_texture"), component.material.heightTexture);
                    if (value.contains("parallax_depth"))
                        ReadJsonValue(value.at("parallax_depth"), component.material.parallaxDepth);
                    if (value.contains("terrain_layer")) {
                        const auto& data = value.at("terrain_layer");
                        auto& layer = component.material.terrainLayer.emplace();
                        ReadJsonValue(data.at("base_color_texture"), layer.baseColorTexture);
                        if (data.contains("normal_texture"))
                            ReadJsonValue(data.at("normal_texture"), layer.normalTexture);
                        if (data.contains("roughness_texture"))
                            ReadJsonValue(data.at("roughness_texture"), layer.roughnessTexture);
                        if (data.contains("height_texture"))
                            ReadJsonValue(data.at("height_texture"), layer.heightTexture);
                        if (data.contains("palette"))
                            ReadJsonValue(data.at("palette"), layer.palette);
                        if (data.contains("height_blend"))
                            ReadJsonValue(data.at("height_blend"), layer.heightBlend);
                    }
                    if (value.contains("emissive_texture"))
                        ReadJsonValue(value.at("emissive_texture"),
                                      component.material.emissiveTexture);
                    if (value.contains("normal_texture"))
                        ReadJsonValue(value.at("normal_texture"), component.material.normalTexture);
                    if (value.contains("metallic_texture"))
                        ReadJsonValue(value.at("metallic_texture"),
                                      component.material.metallicTexture);
                    if (value.contains("roughness_texture"))
                        ReadJsonValue(value.at("roughness_texture"),
                                      component.material.roughnessTexture);
                    if (value.contains("occlusion_texture"))
                        ReadJsonValue(value.at("occlusion_texture"),
                                      component.material.occlusionTexture);
                    if (value.contains("normal_scale")) {
                        ReadJsonValue(value.at("normal_scale"), component.material.normalScale);
                        RequireValidScene(component.material.normalScale >= 0 &&
                                          component.material.normalScale <= 16);
                    }
                    if (value.contains("occlusion_strength")) {
                        ReadJsonValue(value.at("occlusion_strength"),
                                      component.material.occlusionStrength);
                        RequireValidScene(component.material.occlusionStrength >= 0 &&
                                          component.material.occlusionStrength <= 1);
                    }
                    if (value.contains("sampling")) {
                        const auto& sampling = value.at("sampling");
                        RequireValidScene(sampling.is_object());
                        ReadJsonValue(sampling.at("mipmaps"),
                                      component.material.textureSampling.mipmaps);
                        ReadJsonValue(sampling.at("anisotropic"),
                                      component.material.textureSampling.anisotropic);
                        int address = 0;
                        ReadJsonValue(sampling.at("address"), address);
                        RequireValidScene(address >= 0 && address <= 1);
                        component.material.textureSampling.addressMode =
                            static_cast<TextureAddressMode>(address);
                    }
                    entity.AddComponent<MeshRendererComponent>(std::move(component));
                } else if (name == "model_animation") {
                    ModelAnimationComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("asset"), component.modelPath);
                    ReadJsonValue(value.at("node"), component.nodeIndex);
                    ReadJsonValue(value.at("clip"), component.clip);
                    ReadJsonValue(value.at("time"), component.time);
                    ReadJsonValue(value.at("speed"), component.speed);
                    ReadJsonValue(value.at("playing"), component.playing);
                    ReadJsonValue(value.at("loop"), component.loop);
                    ReadJsonValue(value.at("apply_node_transform"), component.applyNodeTransform);
                    entity.AddComponent<ModelAnimationComponent>(std::move(component));
                } else if (name == "camera") {
                    CameraComponent component;
                    component.farClip = 1000.0;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("primary"), component.isPrimary);
                    ReadJsonValue(value.at("perspective"), component.isPerspective);
                    ReadJsonValue(value.at("fov"), component.fov);
                    ReadJsonValue(value.at("ortho_size"), component.orthoSize);
                    if (value.contains("near_clip"))
                        ReadJsonValue(value.at("near_clip"), component.nearClip);
                    if (value.contains("far_clip"))
                        ReadJsonValue(value.at("far_clip"), component.farClip);
                    entity.AddComponent<CameraComponent>(std::move(component));
                } else if (name == "rigidbody2d") {
                    Rigidbody2DComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("type"), component.type);
                    ReadJsonValue(value.at("velocity"), component.velocity);
                    ReadJsonValue(value.at("mass"), component.mass);
                    ReadJsonValue(value.at("gravity_scale"), component.gravityScale);
                    ReadJsonValue(value.at("grounded"), component.isGrounded);
                    entity.AddComponent<Rigidbody2DComponent>(std::move(component));
                } else if (name == "box_collider2d") {
                    BoxCollider2DComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("size"), component.size);
                    ReadJsonValue(value.at("offset"), component.offset);
                    ReadJsonValue(value.at("trigger"), component.isTrigger);
                    entity.AddComponent<BoxCollider2DComponent>(std::move(component));
                } else if (name == "rigidbody3d") {
                    Rigidbody3DComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("type"), component.type);
                    ReadJsonValue(value.at("velocity"), component.velocity);
                    ReadJsonValue(value.at("mass"), component.mass);
                    ReadJsonValue(value.at("gravity_scale"), component.gravityScale);
                    entity.AddComponent<Rigidbody3DComponent>(std::move(component));
                } else if (name == "box_collider3d") {
                    BoxCollider3DComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("size"), component.size);
                    ReadJsonValue(value.at("offset"), component.offset);
                    ReadJsonValue(value.at("trigger"), component.isTrigger);
                    entity.AddComponent<BoxCollider3DComponent>(std::move(component));
                } else if (name == "capsule_collider3d") {
                    CapsuleCollider3DComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("radius"), component.radius);
                    ReadJsonValue(value.at("half_height"), component.halfHeight);
                    ReadJsonValue(value.at("offset"), component.offset);
                    ReadJsonValue(value.at("trigger"), component.isTrigger);
                    entity.AddComponent<CapsuleCollider3DComponent>(std::move(component));
                } else if (name == "character_controller3d") {
                    CharacterController3DComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("radius"), component.radius);
                    ReadJsonValue(value.at("half_height"), component.halfHeight);
                    ReadJsonValue(value.at("max_slope_radians"), component.maxSlopeRadians);
                    ReadJsonValue(value.at("step_height"), component.stepHeight);
                    entity.AddComponent<CharacterController3DComponent>(std::move(component));
                } else if (name == "triangle_mesh_collider3d") {
                    const auto& vertices = value.at("vertices");
                    const auto& indices = value.at("indices");
                    RequireValidScene(vertices.is_array() && vertices.size() >= 3 &&
                                      vertices.size() <= PhysicsWorld::MaxTriangleMeshVertices &&
                                      indices.is_array() && indices.size() >= 3 &&
                                      indices.size() <= PhysicsWorld::MaxTriangleMeshIndices &&
                                      indices.size() % 3 == 0);
                    auto mesh = std::make_shared<TriangleMeshCollider3DData>();
                    mesh->vertices.resize(vertices.size());
                    mesh->indices.resize(indices.size());
                    for (size_t i = 0; i < vertices.size(); ++i)
                        ReadJsonValue(vertices[i], mesh->vertices[i]);
                    for (size_t i = 0; i < indices.size(); ++i)
                        ReadJsonValue(indices[i], mesh->indices[i]);
                    entity.AddComponent<TriangleMeshCollider3DComponent>(std::move(mesh));
                } else if (name == "tilemap") {
                    TilemapComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("width"), component.width);
                    ReadJsonValue(value.at("height"), component.height);
                    ReadJsonValue(value.at("tile_size"), component.tileSize);
                    ReadJsonValue(value.at("tiles"), component.tiles);
                    entity.AddComponent<TilemapComponent>(std::move(component));
                } else if (name == "animation") {
                    AnimationComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("name"), component.clip.name);
                    ReadJsonValue(value.at("duration"), component.clip.duration);
                    ReadJsonValue(value.at("loop"), component.clip.loop);
                    ReadJsonValue(value.at("playing"), component.playing);
                    ReadJsonValue(value.at("keyframes"), component.clip.keyframes);
                    if (value.contains("asset"))
                        ReadJsonValue(value.at("asset"), component.assetPath);
                    if (value.contains("speed"))
                        ReadJsonValue(value.at("speed"), component.speed);
                    if (value.contains("root_motion"))
                        ReadJsonValue(value.at("root_motion"), component.rootMotion);
                    if (value.contains("clip_data")) {
                        std::string error;
                        auto clip =
                            AnimationClip::DeserializeData(value.at("clip_data").dump(), error);
                        RequireValidScene(bool(clip));
                        component.clip = *clip;
                    }
                    entity.AddComponent<AnimationComponent>(std::move(component));
                } else if (name == "animator") {
                    AnimatorComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("controller"), component.controllerPath);
                    ReadJsonValue(value.at("playing"), component.playing);
                    ReadJsonValue(value.at("root_motion"), component.rootMotion);
                    const auto& parameters = value.at("parameters");
                    RequireValidScene(parameters.is_array() && parameters.size() <= 256);
                    for (const auto& p : parameters) {
                        AnimationParameter parameter;
                        int type = 0;
                        ReadJsonValue(p.at("name"), parameter.name);
                        ReadJsonValue(p.at("type"), type);
                        ReadJsonValue(p.at("value"), parameter.value);
                        parameter.type = static_cast<AnimationParameterType>(type);
                        component.parameters.push_back(std::move(parameter));
                    }
                    entity.AddComponent<AnimatorComponent>(std::move(component));
                } else if (name == "hierarchy") {
                    HierarchyComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("parent"), component.parent);
                    ReadJsonValue(value.at("group"), component.group);
                    entity.AddComponent<HierarchyComponent>(std::move(component));
                } else if (name == "scene_reference") {
                    SceneReferenceComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("path"), component.path);
                    ReadJsonValue(value.at("mode"), component.mode);
                    ReadJsonValue(value.at("baseline"), component.baseline);
                    ReadJsonValue(value.at("suspended"), component.suspended);
                    ReadJsonValue(value.at("stream_center"), component.streamCenter);
                    ReadJsonValue(value.at("load_radius"), component.streamLoadRadius);
                    ReadJsonValue(value.at("unload_radius"), component.streamUnloadRadius);
                    ReadJsonValue(value.at("loaded"), component.loaded);
                    ReadJsonValue(value.at("entity_map"), component.entityMap);
                    entity.AddComponent<SceneReferenceComponent>(std::move(component));
                } else if (name == "instance_source") {
                    InstanceSourceComponent component;
                    RequireValidScene(value.is_object());
                    ReadJsonValue(value.at("root"), component.instanceRoot);
                    ReadJsonValue(value.at("entity"), component.sourceEntity);
                    entity.AddComponent<InstanceSourceComponent>(std::move(component));
                } else
                    return false; // Unknown components must not disappear during the next save.
            }
        }
        parsed.nextEntityId_ = ids.empty() ? 1 : *ids.rbegin() + 1;
        if (root.contains("next_entity_id")) {
            uint32 nextId = 0;
            ReadJsonValue(root.at("next_entity_id"), nextId);
            RequireValidScene(nextId >= parsed.nextEntityId_);
            parsed.nextEntityId_ = nextId;
        }
        if (!parsed.ValidateForSerialization())
            return false;
        AdoptLoadedScene(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace LuxEngine
