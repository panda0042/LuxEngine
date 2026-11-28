#pragma once
#include <LuxEngine/Core/Math.hpp>
#include <span>
#include <vector>

namespace LuxEngine {
struct TerrainErosionSettings {
    unsigned iterations = 24;
    double rainfall = .04;    // metres of water per iteration
    double evaporation = .2;  // fraction of water removed after transport
    double erosionRate = .22; // fraction of sediment capacity exchanged with the bed
    double talusSlope = .78;  // tangent of the stable slope
};

// A uniformly sampled Y-up heightfield with a closed (no outflow) boundary.
// Heights, horizontal sample spacing and positions are all in world metres.
class LUX_API TerrainHeightfield {
public:
    TerrainHeightfield(uint32 columns, uint32 rows, double spacing, Vec2 origin,
                       std::span<const float> elevations);
    double Height(Vec2 position) const;
    double Drainage(Vec2 position) const;
    Vec3 Normal(Vec2 position) const;
    void Erode(const TerrainErosionSettings& settings = {});

private:
    double Sample(std::span<const float> values, Vec2 position) const;
    uint32 columns_, rows_;
    double spacing_;
    Vec2 origin_;
    std::vector<float> heights_, drainage_;
};
} // namespace LuxEngine
