#include <LuxEngine/Environment/Atmosphere.hpp>
#include <LuxEngine/Environment/EnvironmentMap.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace LuxEngine;

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

Vec3 PanoramaSample(const AtmospherePanorama& image, Vec3 direction) {
    const double pi = std::numbers::pi;
    const auto x =
        std::min(uint32((std::atan2(direction.z, direction.x) / (2 * pi) + .5) * image.width),
                 image.width - 1);
    const auto y = std::min(uint32(std::acos(direction.y) / pi * image.height), image.height - 1);
    const auto index = (size_t(y) * image.width + x) * 3;
    return {image.pixels[index], image.pixels[index + 1], image.pixels[index + 2]};
}

double Brightness(Vec3 color) {
    return color.Dot({.2126, .7152, .0722});
}
} // namespace

int main() {
    try {
        AtmosphereSettings settings;
        const auto sky = BakeAtmospherePanorama(settings);
        Check(sky.width == 2 * sky.height &&
                  sky.pixels.size() == size_t(sky.width) * sky.height * 3,
              "atmosphere outputs linear 2:1 RGB");
        Check(std::ranges::all_of(sky.pixels,
                                  [](float value) { return std::isfinite(value) && value >= 0; }),
              "atmosphere produces finite positive radiance");
        const auto zenith = PanoramaSample(sky, {0, 1, 0});
        Check(zenith.z > zenith.y && zenith.y > zenith.x && zenith.x > 0,
              "Rayleigh scattering separates visible wavelengths");
        const auto towardSun = settings.sunDirection.Normalized();
        const auto awayFromSun = Vec3{-towardSun.x, towardSun.y, -towardSun.z};
        Check(Brightness(PanoramaSample(sky, towardSun)) >
                  Brightness(PanoramaSample(sky, awayFromSun)) * 1.2,
              "forward aerosol scattering brightens the sunward sky");
        settings.sunDirection = {0, -1, 0};
        settings.observerAltitudeMeters = 0;
        const auto night = BakeAtmospherePanorama(settings);
        Check(Brightness(PanoramaSample(night, {0, 1, 0})) < 1e-5,
              "planet occludes direct solar radiance on its night side");
        settings.aerosolDensity = -1;
        try {
            BakeAtmospherePanorama(settings);
            throw std::runtime_error("invalid atmospheric properties accepted");
        } catch (const std::invalid_argument&) {
        }

        const auto environment = EnvironmentData::Prepare({});
        Check(environment && environment->error.empty(), "built-in sky prepares through IBL");
        const auto& cube = environment->sky;
        const auto plane = size_t(cube.size) * cube.size;
        const auto center = (2 * plane + (cube.size / 2) * cube.size + cube.size / 2) * 4;
        Check(std::abs(cube.pixels[center + 2] - zenith.z) < .02 &&
                  cube.pixels[center + 2] > cube.pixels[center] &&
                  environment->diffuse.layers == 6 && environment->specular.levels == 8,
              "built-in cubemap and environment lighting use the baked atmosphere");
        std::cout << "Atmosphere and built-in IBL passed. Zenith radiance " << zenith.x << ' '
                  << zenith.y << ' ' << zenith.z << ", IBL preparation "
                  << environment->preparationMs << " ms\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
