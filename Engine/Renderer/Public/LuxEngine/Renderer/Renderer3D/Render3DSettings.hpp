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
    bool waterReflections = false;
    bool temporalRestir = true;
    bool denoiser = false;

    bool Enabled() const {
        return shadows || reflections || refractions || indirectDiffuse || restirDirect ||
               restirIndirect || waterReflections;
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

// Luminance is calibrated before exposure; the actual vertical FOV determines focal length.
struct PhysicalCameraSettings {
    bool exposure = false;
    bool depthOfField = false;
    float iso = 100;
    float shutterSeconds = 1.f / 125.f;
    float fNumber = 8;
    float sensorHeightMm = 24;
    float luminanceScale = 1; // cd/m² per unit of scene-linear HDR luminance.

    bool operator==(const PhysicalCameraSettings&) const = default;
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
    // Display-space edge filter in the HDR-to-SDR composite. Does not affect overlays.
    bool screenSpaceAntialiasing = false;
    // Jitter and depth-reproject the primary 3D view before tone mapping.
    bool temporalAntialiasing = false;
    // Bounded recovery of local luminance detail in the temporal resolve (0..1).
    float temporalSharpness = .3f;
    // Shutter duration as a fraction of the previous rendered frame interval (0 disables blur).
    float motionBlurShutter = 0.f;

    float gtaoRadius = 0;               // View-space search radius in metres.
    float contactShadowDistance = 0;    // Near-field sun occlusion in metres.
    float shadowLightAngularRadius = 0; // PCSS sun angular radius in radians.
    bool hierarchicalReflections = false;
    float vignette = 0;            // Optical falloff, 0..1.
    float filmGrain = 0;           // Display-space grain intensity, 0..1.
    float chromaticAberration = 0; // Maximum red/blue offset in pixels at 1080p.
    float lensDistortion = 0;      // Signed radial lens coefficient, -.35..+.35.
    float lensFlareStrength = 0;   // Additive HDR ghost and halo intensity, 0..2.
    float bloomIntensity = 0;
    float bloomThreshold = 1; // Scene-linear luminance.
    bool physicallyBasedBloom =
        false; // Scatter all scene-linear radiance; preserves a uniform field.
    bool autoExposure = false;
    float localExposure = 0;      // Edge-aware local exposure strength, 0..1 (up to 1.5 stops).
    uint32 apertureBlades = 0;    // Circular at zero, or 3..12 blades for polygonal bokeh.
    float depthOfFieldRadius = 0; // Maximum blur radius in pixels at 1080p.
    PhysicalCameraSettings physicalCamera;
    float focusDistance = 10;    // Metres.
    std::string colorGradingLut; // Display-referred 3D .cube file.
    float colorGradingStrength = 1;
    bool dynamicAtmosphere = false;
    float aerosolDensity = 1;
    float cloudCoverage = 0;  // Volumetric cover from clear (0) to overcast (1).
    Vec2 windVelocity{};      // World X/Z, metres per second.
    float surfaceWetness = 0; // Accumulated liquid film, 0 (dry) to 1 (saturated).
    double weatherTime = 0;   // Simulation seconds, supplied by the caller.
};

LUX_API bool ValidateRender3DSettings(const Render3DSettings& settings);
LUX_API bool IsDefaultRender3DSettings(const Render3DSettings& settings);

} // namespace LuxEngine
