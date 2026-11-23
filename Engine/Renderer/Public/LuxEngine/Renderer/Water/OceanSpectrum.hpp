#pragma once

#include <LuxEngine/Core/Math.hpp>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace LuxEngine {

struct OceanSpectrumSettings {
    uint32 resolution = 64;
    double patchLength = 64;
    Vec2 windDirection{1, 0.35};
    double windSpeed = 12;
    double spectrumAmplitude = 0.001;
    double choppiness = 1;
    double foamThreshold = 0.85;
    double foamSoftness = 0.35;
    uint32 seed = 1;
};

struct OceanSample {
    Vec3 displacement;
    Vec3 normal{0, 1, 0};
    Vec3 velocity;
    double jacobian = 1;
    double foam = 0;
};

// Periodic spectral ocean reference. Initialization seeds the wind spectrum and
// allocates the transform workspace. Update uses a separable inverse FFT on the CPU.
class LUX_API OceanSpectrum {
public:
    OceanSpectrum();
    ~OceanSpectrum();
    OceanSpectrum(OceanSpectrum&&) noexcept;
    OceanSpectrum& operator=(OceanSpectrum&&) noexcept;
    OceanSpectrum(const OceanSpectrum&) = delete;
    OceanSpectrum& operator=(const OceanSpectrum&) = delete;

    bool Initialize(const OceanSpectrumSettings& settings = {});
    bool Update(double timeSeconds);
    // Row-major grid with X changing fastest. Each sample is displaced from its grid position.
    std::span<const OceanSample> Grid() const;
    // The parametric XZ coordinate repeats every patchLength world units.
    std::optional<OceanSample> Sample(Vec2 localXZ) const;
    // Samples at a displaced XZ coordinate for buoyancy. Overturning waves can have
    // multiple solutions, so the bounded solver returns the nearest estimate it finds.
    std::optional<OceanSample> SampleAtSurface(Vec2 localXZ) const;

private:
    friend class Mesh3D;
    bool BuildGpuSeed(OceanSpectrumSettings& settings, std::vector<float>& modes,
                      Vec3& displacementBound) const;
    struct State;
    std::unique_ptr<State> state_;
};

struct OceanBuoyancySettings {
    double radius = .25;
    double fluidDensity = 1000;
    double gravity = 9.81;
    double drag = 3;
};

// Returns a world impulse for a sphere from submerged volume and drag relative to the water.
// bodyHeight is the sphere center measured from the ocean's undeformed surface.
LUX_API Vec3 OceanBuoyancyImpulse(const OceanSample& water, double bodyHeight, Vec3 bodyVelocity,
                                  double bodyMass, double dt,
                                  const OceanBuoyancySettings& settings = {});

} // namespace LuxEngine
