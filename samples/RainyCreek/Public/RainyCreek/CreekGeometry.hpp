#pragma once

#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <vector>

namespace RainyCreek {

struct CpuMesh {
    std::vector<LuxEngine::Vertex3D> vertices;
    std::vector<LuxEngine::uint32> indices;
};

float StreamCenter(float z);
float TerrainHeight(float x, float z);
float WaterHeight();

CpuMesh BuildTerrain();
LuxEngine::WaterSurface BuildWater();
CpuMesh BuildRain(unsigned phase, unsigned phaseCount = 24);
CpuMesh BuildLampPost();
CpuMesh BuildLampGlass();

} // namespace RainyCreek
