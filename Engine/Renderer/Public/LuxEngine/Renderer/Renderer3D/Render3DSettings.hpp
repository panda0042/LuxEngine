#pragma once

#include <LuxEngine/Core/Math.hpp>
#include <array>
#include <optional>
#include <string>
#include <vector>

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

struct LocalFogVolume {
    std::array<double, 3> center{}; // World coordinates in metres.
    float radius = 10;
    float density = .05f; // Additional extinction per metre at the centre.
    bool operator==(const LocalFogVolume&) const = default;
};

struct VolumetricFogSettings {
    Vec3 color{.8, .86, .95}; // Single-scattering albedo in linear RGB.
    float density = 0;        // Global extinction per world unit at baseHeight.
    float maxDistance = 100;
    float heightFalloff = .1f;
    double baseHeight = 0;
    float anisotropy = .2f;
    bool rayTracedShadows = false;
    float densityVariation = 0;          // Fractional world-space density modulation.
    float multipleScattering = 0;        // Isotropic approximation in optically thick media.
    float historyWeight = 0;             // Reproject the previous viewport's integrated froxels.
    std::vector<LocalFogVolume> volumes; // Up to four soft spherical density volumes.
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

// Percentiles trim the dark/bright ends of a scene-linear log luminance histogram.
enum class PostDebugView : uint32 {
    Off,
    ExposureHistogram,
    HdrLuminance,
    TextureMip,
    BaseColor,
    WorldNormal,
    GeometricNormal,
    Roughness,
    Metallic,
    MaterialAO,
    GTAO,
    BentNormal,
    DirectLighting,
    IndirectLighting,
    IBL,
    Emissive,
    SceneDepth,
    LinearDepth,
    DepthPyramid,
    ShadowMap,
    ShadowCascade,
    VirtualShadowPage,
    ContactShadows,
    CloudShadows,
    RestirReservoir,
    RayNoise,
    DenoisedLighting,
    SsrConfidence,
    VolumetricFog,
    WaterDepth,
    CloudDensity,
    WaterReflection,
    TerrainWeights,
    VegetationLod
};

struct ExposureMeteringSettings {
    float lowPercentile = .10f, highPercentile = .90f;
    float minEV = -4, maxEV = 4; // Automatic adjustment relative to the manual camera.
    float brightenSpeed = 1.3f, darkenSpeed = 3.f; // Exponential convergence rates per second.
    float compensationEV = 0; // Intentional photographic offset, after eye adaptation.
    bool operator==(const ExposureMeteringSettings&) const = default;
};

struct SceneColorGrade {
    float temperatureK = 6500, tint = 0, saturation = 1, vibrance = 0, contrast = 1;
    std::array<float, 3> lift{}, gamma{1, 1, 1}, gain{1, 1, 1};
    std::array<float, 3> shadows{}, midtones{}, highlights{}; // RGB offsets in stops.
    bool operator==(const SceneColorGrade&) const = default;
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

    // A nonzero response rate follows the median scene depth at the screen centre.
    float autofocusSpeed = 0;

    // Use shutterSeconds and elapsed render time for motion integration.
    bool motionBlur = false;

    bool operator==(const PhysicalCameraSettings&) const = default;
};

// Blend only authored overrides. Inside a local axis-aligned box it has full weight;
// blendDistance fades outward in world metres. Higher priorities are applied last.
struct PostProcessVolume {
    bool global = false;
    int priority = 0;
    std::array<double, 3> center{}, halfExtent{1, 1, 1};
    float blendDistance = 1, weight = 1;
    std::optional<float> compensationEV, bloomIntensity, localExposure, saturation, temperatureK,
        focusDistance;
    bool operator==(const PostProcessVolume&) const = default;
};

struct FoliageInteraction {
    Vec2 center{};
    float radius = 0, strength = 0;

    bool operator==(const FoliageInteraction& other) const {
        return center.x == other.center.x && center.y == other.center.y && radius == other.radius &&
               strength == other.strength;
    }
};

enum class TemporalDebugView : uint32 {
    Off,
    Velocity,
    Reactive,
    HistoryWeight,
    Rejection,
    Transparency
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
    bool cascadedShadows =
        false; // Four stabilized directional shadow splits with view-depth blending.
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
    TemporalDebugView temporalDebugView = TemporalDebugView::Off;

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
    ExposureMeteringSettings metering;
    PostDebugView postDebugView = PostDebugView::Off;
    float textureLodBias = 0; // Mipmap levels; positive selects coarser material samples.
    SceneColorGrade colorGrade;
    std::vector<PostProcessVolume> postVolumes;
    float bloomFireflySuppression =
        0;                        // Optional normalized Karis weighting in the first reduction.
    float bloomScatter = .68f;    // Fraction passed to the next broader octave.
    float bloomRadius = 1;        // Tent width in low-resolution texels.
    float localExposure = 0;      // Edge-aware local exposure strength, 0..1 (up to 1.5 stops).
    uint32 apertureBlades = 0;    // Circular at zero, or 3..12 blades for polygonal bokeh.
    float depthOfFieldRadius = 0; // Maximum blur radius in pixels at 1080p.
    PhysicalCameraSettings physicalCamera;
    float focusDistance = 10;    // Metres.
    std::string colorGradingLut; // Display-referred 3D .cube file.
    float colorGradingStrength = 1;
    std::string secondaryColorGradingLut; // An empty slot uses the ungraded display color.
    float colorGradingMix = 0;            // Crossfade from primary to secondary LUT.
    bool dynamicAtmosphere = false;
    // Capture the procedural sky for SH diffuse and roughness-filtered reflections.
    bool dynamicSkyLighting = false;
    float cloudShadowStrength = 0;
    // Top-of-atmosphere neutral solar irradiance in scene units. Zero follows the sun light.
    float atmosphereSolarIrradiance = 0;
    float aerosolDensity = 1;
    float cloudCoverage = 0; // Volumetric cover from clear (0) to overcast (1).
    std::array<FoliageInteraction, 2>
        foliageInteractions{}; // Up to two world-space XZ contact regions.
    Vec2 windVelocity{};       // World X/Z, metres per second.
    float rainfall = 0;        // Instantaneous precipitation, normalized to a heavy downpour.
    float surfaceWetness = 0;  // Accumulated liquid film, 0 (dry) to 1 (saturated).
    double weatherTime = 0;    // Simulation seconds, supplied by the caller.
};

void LUX_API ApplyPostProcessVolumes(Render3DSettings& settings, Vec3 cameraPosition);
LUX_API bool ValidateRender3DSettings(const Render3DSettings& settings);
LUX_API bool IsDefaultRender3DSettings(const Render3DSettings& settings);

} // namespace LuxEngine
