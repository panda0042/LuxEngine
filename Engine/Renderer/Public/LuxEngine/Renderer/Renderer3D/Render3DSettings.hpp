#pragma once

#include <LuxEngine/Core/Math.hpp>
#include <string>

namespace LuxEngine {

// Legacy is accepted for source and scene-file compatibility. Scene rendering always uses PBR.
enum class Render3DMode { Legacy, Pbr };

struct DirectionalLight {
    Vec3 direction{0.0f, -1.0f, 0.0f};
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

struct RayTracingSettings {
    // Raster shadows remain available through Render3DSettings::shadows.
    bool shadows = false;
    bool reflections = false;
    bool refractions = false;
    bool indirectDiffuse = false;
    uint32 maxBounces = 2;
    float maxDistance = 100.0f;
    float indirectStrength = .35f;
    bool restirDirect = false;
    bool restirIndirect = false;

    bool Enabled() const {
        return shadows || reflections || refractions || indirectDiffuse || restirDirect ||
               restirIndirect;
    }
};

struct VolumetricFogSettings {
    Vec3 color{.8, .86, .95}; // Single-scattering albedo in linear RGB.
    float density = 0;        // Extinction per world unit at baseHeight. Zero disables the pass.
    float maxDistance = 100;
    float heightFalloff = .1f;
    double baseHeight = 0;
    float anisotropy = .2f;
    bool rayTracedShadows = false;
};

struct WaterReflectionSettings {
    std::string environmentPath;
    float rotation = 0; // Degrees about the vertical axis.
    float intensity = 1;
    uint32 faceSize = 128; // Pixels per reflection cube face, either 128, 256 or 512.
};

struct RadianceCascadeSettings {
    float strength = 0; // Zero disables the cascades.
    float spacing = 2;  // Finest probe spacing, in world units.
};

struct Render3DSettings {
    Render3DMode mode = Render3DMode::Pbr;
    DirectionalLight sun{{.5f, -1.0f, .5f}, {1.0f, .98f, .95f}, 1.0f};
    Vec3 environmentColor{.15f, .15f, .15f};
    float environmentIntensity = 1.0f;
    float environmentRotation = 0.0f;
    std::string environmentPath;
    bool imageBasedLighting = true;
    bool showSkybox = false;
    bool toneMapping = true;
    bool shadows = true;
    uint32 shadowResolution = 2048;
    float shadowRange = 50.0f;
    float shadowDepthBias = .0005f;
    float shadowNormalBias = .02f;
    RayTracingSettings rayTracing;
    VolumetricFogSettings fog;
    bool virtualShadows = false;
    WaterReflectionSettings waterReflection;
    RadianceCascadeSettings radianceCascades;
};

LUX_API bool ValidateRender3DSettings(const Render3DSettings& settings);
LUX_API bool IsDefaultRender3DSettings(const Render3DSettings& settings);

} // namespace LuxEngine
