#pragma once

#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace LuxEngine {

// Direction is in the local XZ plane. Amplitude and wavelength are in world units,
// speed is in world units per second, and phase is in radians. Steepness is the
// horizontal displacement as a fraction of the vertical amplitude (0 to 1).
struct WaterWave {
    Vec2 direction{1, 0};
    double amplitude = .025;
    double wavelength = 2;
    double speed = 1;
    double phase = 0;
    double steepness = .5;
};

struct WaterSurfaceDesc {
    Vec2 size{10, 10};
    uint32 columns = 32;
    uint32 rows = 32;
    double level = 0;
};

struct WaterSample {
    double height = 0;
    Vec3 normal{0, 1, 0};
    Vec3 velocity;
    double jacobian = 1;
    double foam = 0;
};

// A finite XZ water grid deformed by Gerstner waves with a stable index buffer.
class LUX_API WaterSurface {
public:
    static constexpr uint32 MaxSegments = 256;
    static constexpr size_t MaxWaves = 4;

    // Provide an optional X offset per Z row to follow a winding channel.
    bool Configure(const WaterSurfaceDesc& desc, std::span<const WaterWave> waves,
                   std::span<const double> rowCenters = {});
    bool SetTime(double seconds);
    // Finds the displaced local XZ point inside the water grid. Values come from the
    // analytic waves, so the triangle approximation can differ between vertices.
    std::optional<WaterSample> Sample(double x, double z) const;
    std::span<const Vertex3D> GetVertices() const;
    std::span<const uint32> GetIndices() const;

    // Synchronizes changed vertices before this surface is submitted for drawing.
    bool SyncGpu(GpuDevice& device);
    void ReleaseGpu();
    const Mesh3D& GetMesh() const;

    static Material3D MakeMaterial();

private:
    struct Wave {
        Vec2 waveVector;
        Vec2 horizontalAmplitude;
        double amplitude = 0;
        double angularVelocity = 0, period = 0, phase = 0;
    };

    struct GridPoint {
        Vec2 planar;
        std::array<Vec2, MaxWaves> phases{};
    };

    struct Evaluation {
        Vec2 displacement;
        WaterSample sample;
    };

    Evaluation Evaluate(const std::array<Vec2, MaxWaves>& spatial) const;
    std::array<Wave, MaxWaves> waves_{};
    std::array<Vec2, MaxWaves> temporal_{};
    size_t waveCount_ = 0;
    Vec2 size_;
    std::vector<double> rowCenters_;
    double level_ = 0;
    std::optional<double> time_;
    std::vector<GridPoint> grid_;
    std::vector<Vertex3D> vertices_;
    std::vector<uint32> indices_;
    Mesh3D mesh_;
    GpuDevice* meshDevice_ = nullptr;
    bool gpuDirty_ = true;
};

} // namespace LuxEngine
