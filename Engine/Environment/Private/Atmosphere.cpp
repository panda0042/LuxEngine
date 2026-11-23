#include <LuxEngine/Environment/Atmosphere.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace LuxEngine {
namespace {
constexpr double GroundRadius = 6'360'000;
constexpr double AtmosphereHeight = 100'000;
constexpr double TopRadius = GroundRadius + AtmosphereHeight;
constexpr double Pi = std::numbers::pi;
constexpr uint32 AltitudeSamples = 64;
constexpr uint32 DirectionSamples = 64;
constexpr uint32 TransmittanceSteps = 40;
constexpr uint32 ViewSteps = 32;
constexpr uint32 Width = 512;
constexpr uint32 Height = Width / 2;

// Scattering and extinction in linear RGB per metre. An exponentially stratified molecular and
// aerosol atmosphere is combined with a triangular ozone altitude profile.
constexpr Vec3 Rayleigh{5.802e-6, 13.558e-6, 33.1e-6};
constexpr Vec3 Ozone{.650e-6, 1.881e-6, .085e-6};
constexpr double MieScattering = 3.996e-6;
constexpr double MieExtinction = 4.440e-6;
constexpr double AerosolAnisotropy = .8;

double Radius(Vec3 position) {
    return std::sqrt(position.Dot(position));
}

double HorizonCosine(double radius) {
    return -std::sqrt(std::max(0., 1 - (GroundRadius / radius) * (GroundRadius / radius)));
}

double DistanceToTop(double radius, double mu) {
    const double b = radius * mu;
    return -b + std::sqrt(std::max(0., b * b + (TopRadius - radius) * (TopRadius + radius)));
}

double DistanceToGround(double radius, double mu) {
    const double b = radius * mu;
    const double c = (radius - GroundRadius) * (radius + GroundRadius);
    // The alternative small-root formula avoids subtracting two large nearly equal numbers.
    return c / (-b + std::sqrt(std::max(0., b * b - c)));
}

Vec3 Density(double height) {
    height = std::max(0., height);
    return {std::exp(-height / 8000), std::exp(-height / 1200),
            std::max(0., 1 - std::abs(height - 25'000) / 15'000)};
}

Vec3 Product(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

Vec3 Transmission(Vec3 depth, double aerosolDensity) {
    const auto opticalDepth = Rayleigh * depth.x + Ozone * depth.z;
    const auto hazeDepth = MieExtinction * aerosolDensity * depth.y;
    return {std::exp(-opticalDepth.x - hazeDepth), std::exp(-opticalDepth.y - hazeDepth),
            std::exp(-opticalDepth.z - hazeDepth)};
}

std::array<Vec3, AltitudeSamples * DirectionSamples> BuildSolarDepth() {
    std::array<Vec3, AltitudeSamples * DirectionSamples> table{};
    for (uint32 y = 0; y < AltitudeSamples; ++y) {
        const double f = double(y) / (AltitudeSamples - 1);
        const double radius = GroundRadius + AtmosphereHeight * f * f;
        const double horizon = HorizonCosine(radius);
        for (uint32 x = 0; x < DirectionSamples; ++x) {
            const double g = double(x) / (DirectionSamples - 1);
            const double mu = horizon + (1 - horizon) * g * g;
            const double distance = DistanceToTop(radius, mu);
            Vec3 depth;
            double previous = 0;
            for (uint32 i = 1; i <= TransmittanceSteps; ++i) {
                const double u = double(i) / TransmittanceSteps;
                const double next = distance * u * u;
                const double t = (previous + next) * .5;
                const double sampleRadius = std::sqrt(radius * radius + t * (t + 2 * radius * mu));
                depth += Density(sampleRadius - GroundRadius) * (next - previous);
                previous = next;
            }
            table[y * DirectionSamples + x] = depth;
        }
    }
    return table;
}

Vec3 SolarDepth(double radius, double mu, double horizon) {
    // Height and angle are concentrated near the denser lower atmosphere and the horizon.
    static const auto table = BuildSolarDepth();
    const double u =
        std::sqrt(std::clamp((mu - horizon) / (1 - horizon), 0., 1.)) * (DirectionSamples - 1);
    const double v = std::sqrt(std::clamp((radius - GroundRadius) / AtmosphereHeight, 0., 1.)) *
                     (AltitudeSamples - 1);
    const auto x = static_cast<uint32>(u), y = static_cast<uint32>(v);
    const auto xx = std::min(x + 1, DirectionSamples - 1);
    const auto yy = std::min(y + 1, AltitudeSamples - 1);
    const auto at = [](uint32 tx, uint32 ty) { return table[ty * DirectionSamples + tx]; };
    const double fx = u - x, fy = v - y;
    const auto top = at(x, y) * (1 - fx) + at(xx, y) * fx;
    const auto bottom = at(x, yy) * (1 - fx) + at(xx, yy) * fx;
    return top * (1 - fy) + bottom * fy;
}

Vec3 SunTransmission(Vec3 point, double radius, Vec3 sun, double aerosolDensity) {
    const double mu = point.Dot(sun) / radius;
    const double horizon = HorizonCosine(radius);
    if (mu < horizon)
        return {};
    return Transmission(SolarDepth(radius, mu, horizon), aerosolDensity);
}

Vec3 SkyRay(Vec3 observer, Vec3 direction, Vec3 sun, const AtmosphereSettings& settings) {
    const bool hitsGround = direction.y < HorizonCosine(observer.y);
    const double distance = hitsGround ? DistanceToGround(observer.y, direction.y)
                                       : DistanceToTop(observer.y, direction.y);
    const double angle = std::clamp(sun.Dot(direction), -1., 1.);
    const double rayleighPhase = 3 / (16 * Pi) * (1 + angle * angle);
    const double g = AerosolAnisotropy;
    const double miePhase = (1 - g * g) / (4 * Pi * std::pow(1 + g * g - 2 * g * angle, 1.5));
    const Vec3 rayleigh = Rayleigh * rayleighPhase;
    const double mie = MieScattering * settings.aerosolDensity * miePhase;
    Vec3 radiance, viewDepth;
    double previous = 0;
    for (uint32 i = 1; i <= ViewSteps; ++i) {
        const double u = double(i) / ViewSteps;
        const double next = distance * u * u;
        const double step = next - previous;
        const auto point = observer + direction * ((previous + next) * .5);
        const double radius = Radius(point);
        const auto density = Density(radius - GroundRadius);
        const auto viewTransmission =
            Transmission(viewDepth + density * (step * .5), settings.aerosolDensity);
        const auto solar = SunTransmission(point, radius, sun, settings.aerosolDensity);
        const double aerosol = mie * density.y;
        const Vec3 scatter = rayleigh * density.x + Vec3{aerosol, aerosol, aerosol};
        radiance += Product(Product(viewTransmission, solar), scatter) * step;
        viewDepth += density * step;
        previous = next;
    }
    if (hitsGround) {
        const auto ground = observer + direction * distance;
        const double facing = std::max(0., ground.Dot(sun) / GroundRadius);
        const auto solar = SunTransmission(ground, GroundRadius, sun, settings.aerosolDensity);
        radiance += Product(Product(Transmission(viewDepth, settings.aerosolDensity), solar),
                            settings.groundAlbedo) *
                    (facing / Pi);
    }
    return Product(radiance, settings.solarIrradiance);
}
} // namespace

AtmospherePanorama BakeAtmospherePanorama(const AtmosphereSettings& settings) {
    const auto inRange = [](double value, double low, double high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    const auto color = [&](Vec3 value, double maximum) {
        return inRange(value.x, 0, maximum) && inRange(value.y, 0, maximum) &&
               inRange(value.z, 0, maximum);
    };
    const double sunLengthSquared = settings.sunDirection.Dot(settings.sunDirection);
    if (!inRange(sunLengthSquared, 1e-12, 1e12) ||
        !inRange(settings.observerAltitudeMeters, 0, AtmosphereHeight) ||
        !inRange(settings.aerosolDensity, 0, 10) || !color(settings.groundAlbedo, 1) ||
        !color(settings.solarIrradiance, 10000))
        throw std::invalid_argument("Invalid atmosphere properties");
    const auto sun = settings.sunDirection * (1 / std::sqrt(sunLengthSquared));
    const Vec3 observer{0, GroundRadius + settings.observerAltitudeMeters, 0};
    AtmospherePanorama panorama{Width, Height, {}};
    panorama.pixels.reserve(size_t(Width) * Height * 3);
    std::array<Vec2, Width> horizontal;
    for (uint32 x = 0; x < Width; ++x) {
        const double phi = ((x + .5) / Width - .5) * 2 * Pi;
        horizontal[x] = {std::cos(phi), std::sin(phi)};
    }
    for (uint32 y = 0; y < Height; ++y) {
        const double theta = (y + .5) / Height * Pi;
        const double sinTheta = std::sin(theta), cosTheta = std::cos(theta);
        for (const auto [x, z] : horizontal) {
            const auto radiance =
                SkyRay(observer, {x * sinTheta, cosTheta, z * sinTheta}, sun, settings);
            panorama.pixels.push_back(static_cast<float>(std::min(radiance.x, 65504.)));
            panorama.pixels.push_back(static_cast<float>(std::min(radiance.y, 65504.)));
            panorama.pixels.push_back(static_cast<float>(std::min(radiance.z, 65504.)));
        }
    }
    return panorama;
}
} // namespace LuxEngine
