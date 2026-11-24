#include <LuxEngine/Renderer/Renderer3D/Render3DSettings.hpp>
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
           bounded(s.fog.anisotropy, -.9f, .9f) && Utf8(s.waterReflection.environmentPath) &&
           s.waterReflection.environmentPath.size() <= 4096 &&
           s.waterReflection.environmentPath.find('\0') == std::string::npos &&
           bounded(s.waterReflection.rotation, -36000, 36000) &&
           bounded(s.waterReflection.intensity, 0, 10000) &&
           (s.waterReflection.faceSize == 128 || s.waterReflection.faceSize == 256 ||
            s.waterReflection.faceSize == 512) &&
           bounded(s.radianceCascades.strength, 0, 1) &&
           bounded(s.radianceCascades.spacing, .5, 64);
}

bool IsDefaultRender3DSettings(const Render3DSettings& s) {
    const Render3DSettings d;
    const auto fields = [](const Render3DSettings& v) {
        return std::tie(
            v.mode, v.sun.direction.x, v.sun.direction.y, v.sun.direction.z, v.sun.color.x,
            v.sun.color.y, v.sun.color.z, v.sun.intensity, v.environmentColor.x,
            v.environmentColor.y, v.environmentColor.z, v.environmentIntensity,
            v.environmentRotation, v.environmentPath, v.imageBasedLighting, v.showSkybox,
            v.toneMapping, v.shadows, v.shadowResolution, v.shadowRange, v.shadowDepthBias,
            v.shadowNormalBias, v.rayTracing.shadows, v.rayTracing.reflections,
            v.rayTracing.refractions, v.rayTracing.indirectDiffuse, v.rayTracing.maxBounces,
            v.rayTracing.maxDistance, v.rayTracing.indirectStrength, v.fog.color.x, v.fog.color.y,
            v.fog.color.z, v.fog.density, v.fog.maxDistance, v.fog.heightFalloff, v.fog.baseHeight,
            v.fog.anisotropy, v.fog.rayTracedShadows, v.rayTracing.restirDirect,
            v.rayTracing.restirIndirect, v.virtualShadows, v.waterReflection.environmentPath,
            v.waterReflection.rotation, v.waterReflection.intensity, v.waterReflection.faceSize,
            v.radianceCascades.strength, v.radianceCascades.spacing, v.screenSpaceAntialiasing,
            v.temporalAntialiasing);
    };
    return fields(s) == fields(d);
}
} // namespace LuxEngine
