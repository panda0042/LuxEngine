#include <LuxEngine/Renderer/Renderer3D/Render3DSettings.hpp>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace LuxEngine {
namespace {
bool Utf8(const std::string& text) {
    for (size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80)
            continue;
        const int count = first >= 0xc2 && first <= 0xdf   ? 1
                          : first >= 0xe0 && first <= 0xef ? 2
                          : first >= 0xf0 && first <= 0xf4 ? 3
                                                           : -1;
        if (count < 0 || text.size() - i < static_cast<size_t>(count))
            return false;
        uint32 code = first & ((1u << (6 - count)) - 1);
        for (int n = 0; n < count; ++n) {
            const auto c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (c & 0x3f);
        }
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
            (count == 3 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
} // namespace

bool ValidateRender3DSettings(const Render3DSettings& s) {
    const auto bounded = [](double v, double low, double high) {
        return std::isfinite(v) && v >= low && v <= high;
    };
    const auto triplet = [&](const std::array<float, 3>& a, double lo, double hi) {
        return bounded(a[0], lo, hi) && bounded(a[1], lo, hi) && bounded(a[2], lo, hi);
    };
    const auto optional = [&](const std::optional<float>& value, float lo, float hi) {
        return !value || bounded(*value, lo, hi);
    };
    if (s.postVolumes.size() > 64)
        return false;
    for (const auto& box : s.postVolumes) {
        for (unsigned axis = 0; axis < 3; ++axis)
            if (!std::isfinite(box.center[axis]) || !bounded(box.halfExtent[axis], 0, 1e12))
                return false;
        if (!bounded(box.weight, 0, 1) || !bounded(box.blendDistance, 0, 1e9) ||
            !optional(box.compensationEV, -16, 16) || !optional(box.bloomIntensity, 0, 2) ||
            !optional(box.localExposure, 0, 1) || !optional(box.saturation, 0, 2) ||
            !optional(box.temperatureK, 2500, 25000) || !optional(box.focusDistance, .001f, 1e8f))
            return false;
    }
    const auto color = [&](Vec3 v) {
        return bounded(v.x, 0, 65504) && bounded(v.y, 0, 65504) && bounded(v.z, 0, 65504);
    };
    return (s.mode == Render3DMode::Legacy || s.mode == Render3DMode::Pbr) &&
           bounded(s.sun.direction.x, -1e4f, 1e4f) && bounded(s.sun.direction.y, -1e4f, 1e4f) &&
           bounded(s.sun.direction.z, -1e4f, 1e4f) &&
           s.sun.direction.Dot(s.sun.direction) > 1e-10f && color(s.sun.color) &&
           bounded(s.sun.intensity, 0, 10000) && color(s.environmentColor) &&
           bounded(s.environmentIntensity, 0, 10000) &&
           bounded(s.environmentRotation, -36000, 36000) && Utf8(s.environmentPath) &&
           s.environmentPath.size() <= 4096 && s.environmentPath.find('\0') == std::string::npos &&
           (s.shadowResolution == 1024 || s.shadowResolution == 2048 ||
            s.shadowResolution == 4096) &&
           bounded(s.shadowRange, 1, 10000) && bounded(s.shadowDepthBias, 0, .1f) &&
           bounded(s.shadowNormalBias, 0, 10) && s.rayTracing.maxBounces >= 1 &&
           s.rayTracing.maxBounces <= 4 && bounded(s.rayTracing.maxDistance, .1, 100000) &&
           bounded(s.rayTracing.indirectStrength, 0, 1) && bounded(s.fog.color.x, 0, 1) &&
           bounded(s.fog.color.y, 0, 1) && bounded(s.fog.color.z, 0, 1) &&
           bounded(s.fog.density, 0, 10) && bounded(s.fog.maxDistance, .1, 100000) &&
           bounded(s.fog.heightFalloff, 0, 10) && std::isfinite(s.fog.baseHeight) &&
           bounded(s.fog.anisotropy, -.9f, .9f) && bounded(s.fog.densityVariation, 0, 1) &&
           bounded(s.fog.multipleScattering, 0, 1) && bounded(s.fog.historyWeight, 0, .98f) &&
           s.fog.volumes.size() <= 4 &&
           std::all_of(s.fog.volumes.begin(), s.fog.volumes.end(),
                       [&](const LocalFogVolume& v) {
                           return std::all_of(v.center.begin(), v.center.end(),
                                              [](double x) { return std::isfinite(x); }) &&
                                  bounded(v.radius, .01, 100000) && bounded(v.density, 0, 10);
                       }) &&
           Utf8(s.waterReflection.environmentPath) &&
           s.waterReflection.environmentPath.size() <= 4096 &&
           s.waterReflection.environmentPath.find('\0') == std::string::npos &&
           bounded(s.waterReflection.rotation, -36000, 36000) &&
           bounded(s.waterReflection.intensity, 0, 10000) &&
           (s.waterReflection.faceSize == 128 || s.waterReflection.faceSize == 256 ||
            s.waterReflection.faceSize == 512) &&
           bounded(s.radianceCascades.strength, 0, 1) &&
           bounded(s.radianceCascades.spacing, .5, 64) && bounded(s.gtaoRadius, 0, 100) &&
           s.temporalDebugView <= TemporalDebugView::Transparency &&
           bounded(s.temporalSharpness, 0, 1) && bounded(s.motionBlurShutter, 0, 1) &&
           bounded(s.vignette, 0, 1) && bounded(s.filmGrain, 0, 1) &&
           bounded(s.chromaticAberration, 0, 10) && bounded(s.lensDistortion, -0.35f, 0.35f) &&
           bounded(s.lensFlareStrength, 0, 2) && bounded(s.contactShadowDistance, 0, 100) &&
           bounded(s.shadowLightAngularRadius, 0, .25) && bounded(s.bloomIntensity, 0, 2) &&
           bounded(s.bloomThreshold, 0, 65504) && bounded(s.localExposure, 0, 1) &&
           s.postDebugView <= PostDebugView::VegetationLod && bounded(s.textureLodBias, -4, 4) &&
           bounded(s.metering.lowPercentile, 0, 1) && bounded(s.metering.highPercentile, 0, 1) &&
           s.metering.highPercentile - s.metering.lowPercentile >= .001f &&
           bounded(s.metering.minEV, -20, 20) && bounded(s.metering.maxEV, s.metering.minEV, 20) &&
           bounded(s.metering.brightenSpeed, .01, 100) &&
           bounded(s.metering.darkenSpeed, .01, 100) &&
           bounded(s.metering.compensationEV, -16, 16) && bounded(s.bloomRadius, .25, 4) &&
           bounded(s.bloomScatter, 0, 1) && bounded(s.physicalCamera.autofocusSpeed, 0, 100) &&
           bounded(s.colorGrade.temperatureK, 2500, 25000) && bounded(s.colorGrade.tint, -1, 1) &&
           bounded(s.colorGrade.saturation, 0, 2) && bounded(s.colorGrade.vibrance, -1, 1) &&
           bounded(s.colorGrade.contrast, .25, 4) && triplet(s.colorGrade.lift, -.5, .5) &&
           triplet(s.colorGrade.gamma, .1, 4) && triplet(s.colorGrade.gain, 0, 4) &&
           triplet(s.colorGrade.shadows, -4, 4) && triplet(s.colorGrade.midtones, -4, 4) &&
           triplet(s.colorGrade.highlights, -4, 4) && bounded(s.physicalCamera.iso, 1, 409600) &&
           bounded(s.physicalCamera.shutterSeconds, 1.0 / 65536, 60) &&
           bounded(s.physicalCamera.fNumber, .5, 128) &&
           bounded(s.physicalCamera.sensorHeightMm, 1, 100) &&
           bounded(s.physicalCamera.luminanceScale, .00001, 1e7) &&
           (s.apertureBlades == 0 || (s.apertureBlades >= 3 && s.apertureBlades <= 12)) &&
           bounded(s.depthOfFieldRadius, 0, 16) && bounded(s.focusDistance, .001, 1e8) &&
           bounded(s.colorGradingMix, 0, 1) && bounded(s.bloomFireflySuppression, 0, 1) &&
           Utf8(s.secondaryColorGradingLut) && s.secondaryColorGradingLut.size() <= 4096 &&
           s.secondaryColorGradingLut.find('\0') == std::string::npos &&
           bounded(s.colorGradingStrength, 0, 1) && Utf8(s.colorGradingLut) &&
           s.colorGradingLut.size() <= 4096 && s.colorGradingLut.find('\0') == std::string::npos &&
           bounded(s.atmosphereSolarIrradiance, 0, 10000) && bounded(s.aerosolDensity, 0, 10) &&
           bounded(s.cloudCoverage, 0, 1) && bounded(s.cloudShadowStrength, 0, 1) &&
           std::all_of(s.foliageInteractions.begin(), s.foliageInteractions.end(),
                       [&](const auto& a) {
                           return std::isfinite(a.center.x) && std::isfinite(a.center.y) &&
                                  bounded(a.radius, 0, 100) && bounded(a.strength, 0, 1);
                       }) &&
           bounded(s.windVelocity.x, -150, 150) && bounded(s.windVelocity.y, -150, 150) &&
           bounded(s.rainfall, 0, 1) && bounded(s.surfaceWetness, 0, 1) &&
           std::isfinite(s.weatherTime);
}

void ApplyPostProcessVolumes(Render3DSettings& settings, Vec3 cameraPosition) {
    if (settings.postVolumes.empty())
        return;
    if (settings.postVolumes.size() > 64)
        throw std::invalid_argument("Too many postprocess volumes");
    const std::array position{cameraPosition.x, cameraPosition.y, cameraPosition.z};
    std::array<size_t, 64> order{};
    size_t count = 0;
    for (size_t i = 0; i < settings.postVolumes.size(); ++i)
        order[count++] = i;
    // Scene order is the tie breaker. No allocation or persistent state is needed per camera.
    std::sort(order.begin(), order.begin() + count, [&](size_t a, size_t b) {
        const auto pa = settings.postVolumes[a].priority, pb = settings.postVolumes[b].priority;
        return pa == pb ? a < b : pa < pb;
    });
    for (size_t i = 0; i < count; ++i) {
        const auto& box = settings.postVolumes[order[i]];
        float weight = box.weight;
        if (!box.global) {
            double d2 = 0;
            for (size_t axis = 0; axis < 3; ++axis) {
                const double d = std::max(0., std::abs(position[axis] - box.center[axis]) -
                                                  box.halfExtent[axis]);
                d2 += d * d;
            }
            if (d2 > 0) {
                if (box.blendDistance <= 0)
                    continue;
                const double fraction = std::clamp(1. - std::sqrt(d2) / box.blendDistance, 0., 1.);
                weight *= float(fraction * fraction * (3. - 2. * fraction));
            }
        }
        const auto blend = [&](float& out, const std::optional<float>& overrideValue) {
            if (overrideValue)
                out = std::lerp(out, *overrideValue, weight);
        };
        blend(settings.metering.compensationEV, box.compensationEV);
        blend(settings.bloomIntensity, box.bloomIntensity);
        blend(settings.localExposure, box.localExposure);
        blend(settings.colorGrade.saturation, box.saturation);
        blend(settings.colorGrade.temperatureK, box.temperatureK);
        blend(settings.focusDistance, box.focusDistance);
    }
}

bool IsDefaultRender3DSettings(const Render3DSettings& s) {
    const Render3DSettings d;
    const auto fields = [](const Render3DSettings& v) {
        return std::tie(
            v.mode, v.sun.direction.x, v.sun.direction.y, v.sun.direction.z, v.sun.color.x,
            v.sun.color.y, v.sun.color.z, v.sun.intensity, v.environmentColor.x,
            v.environmentColor.y, v.environmentColor.z, v.environmentIntensity,
            v.environmentRotation, v.environmentPath, v.imageBasedLighting, v.showSkybox,
            v.cascadedShadows, v.toneMapping, v.shadows, v.shadowResolution, v.shadowRange,
            v.shadowDepthBias, v.shadowNormalBias, v.rayTracing.shadows, v.rayTracing.reflections,
            v.rayTracing.refractions, v.rayTracing.indirectDiffuse, v.rayTracing.maxBounces,
            v.rayTracing.maxDistance, v.rayTracing.indirectStrength, v.fog.color.x, v.fog.color.y,
            v.fog.color.z, v.fog.density, v.fog.maxDistance, v.fog.heightFalloff, v.fog.baseHeight,
            v.fog.anisotropy, v.fog.rayTracedShadows, v.fog.densityVariation,
            v.fog.multipleScattering, v.fog.historyWeight, v.fog.volumes, v.rayTracing.restirDirect,
            v.rayTracing.restirIndirect, v.virtualShadows, v.waterReflection.environmentPath,
            v.waterReflection.rotation, v.waterReflection.intensity, v.waterReflection.faceSize,
            v.radianceCascades.strength, v.radianceCascades.spacing, v.screenSpaceAntialiasing,
            v.temporalAntialiasing, v.temporalDebugView, v.temporalSharpness, v.motionBlurShutter,
            v.rayTracing.waterReflections, v.rayTracing.temporalRestir, v.rayTracing.denoiser,
            v.vignette, v.filmGrain, v.chromaticAberration, v.lensDistortion, v.lensFlareStrength,
            v.gtaoRadius, v.contactShadowDistance, v.shadowLightAngularRadius,
            v.hierarchicalReflections, v.bloomIntensity, v.bloomThreshold, v.physicallyBasedBloom,
            v.textureLodBias, v.postDebugView, v.postVolumes, v.metering, v.colorGrade,
            v.bloomScatter, v.bloomRadius, v.localExposure, v.apertureBlades, v.physicalCamera,
            v.autoExposure, v.depthOfFieldRadius, v.focusDistance, v.colorGradingLut,
            v.colorGradingStrength, v.secondaryColorGradingLut, v.colorGradingMix,
            v.bloomFireflySuppression, v.dynamicAtmosphere, v.dynamicSkyLighting,
            v.cloudShadowStrength, v.atmosphereSolarIrradiance, v.aerosolDensity, v.cloudCoverage,
            v.rainfall, v.surfaceWetness, v.weatherTime, v.foliageInteractions, v.windVelocity.x,
            v.windVelocity.y);
    };
    return fields(s) == fields(d);
}
} // namespace LuxEngine
