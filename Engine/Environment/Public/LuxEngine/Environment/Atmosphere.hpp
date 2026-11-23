#pragma once

#include <LuxEngine/Core/Math.hpp>
#include <vector>

namespace LuxEngine {
struct AtmosphereSettings {
    // Direction from the observer toward the sun. Y is up.
    Vec3 sunDirection{-.5, 1, -.5};
    // Linear RGB irradiance in the range [0, 10000] per channel.
    Vec3 solarIrradiance{22, 22, 22};
    // Linear RGB reflectance in the range [0, 1] per channel.
    Vec3 groundAlbedo{.12, .14, .12};
    // The spherical atmosphere extends 100 km above the ground.
    double observerAltitudeMeters = 2;
    // Multiplier for the default aerosol profile in the range [0, 10].
    double aerosolDensity = 1;
};

struct AtmospherePanorama {
    uint32 width;
    uint32 height;
    // Linear RGB, top to bottom starting at zenith. The sun's direct light belongs to the
    // scene directional light. The panorama contains its scattered light.
    std::vector<float> pixels;
};

// Bake an Earth-like spherical atmosphere. Throws std::invalid_argument for invalid settings.
LUX_API AtmospherePanorama BakeAtmospherePanorama(const AtmosphereSettings& settings = {});
} // namespace LuxEngine
