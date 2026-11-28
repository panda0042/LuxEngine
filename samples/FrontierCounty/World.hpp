#pragma once

#include <LuxEngine/Physics/PhysicsEngine.hpp>
#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <array>
#include <compare>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Frontier {
using LuxEngine::Vec2;
using LuxEngine::Vec3;
using LuxEngine::Vec4;

inline constexpr int ChunkSize = 256;
inline constexpr int WorldHalf = 8192;
inline constexpr double LakeLevel = 12;
inline constexpr Vec2 LakeCenter{305, 390};
inline constexpr Vec2 LakeSize{232, 186};
inline constexpr Vec2 Spawn{14, 14};
inline constexpr Vec2 Depot{32, 15};
inline constexpr Vec2 Dock{250, 328};

struct Region {
    const char* name;
    Vec2 center;
};

inline constexpr std::array Regions{Region{"Ashford", {0, 0}},
                                    Region{"Pinewood", {-720, -243}},
                                    Region{"Blackwater Ridge", {-374, -840}},
                                    Region{"Silverlake", {239, 296}},
                                    Region{"Redfield", {602, -247}},
                                    Region{"Iron Creek", {645, 198}}};

struct ChunkKey {
    int x = 0, z = 0;
    auto operator<=>(const ChunkKey&) const = default;
};

struct Structure {
    std::string_view model;
    Vec2 position;
    Vec3 size;
    double yaw = 0;
};

std::span<const Structure> Structures();

struct Plant {
    Vec3 ground, dimensions;
    double yaw = 0;
};

inline constexpr int CoverTileSize = 12;
std::array<std::vector<Plant>, 2> GroundCover(ChunkKey tile, unsigned seed);

struct Obstacle {
    Vec3 center;
    Vec3 size;
};

struct MeshData {
    std::vector<LuxEngine::Vertex3D> vertices;
    std::vector<LuxEngine::uint32> indices;
    void Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec4 color);
    void Box(Vec3 center, Vec3 size, Vec4 color);
    LuxEngine::uint64 UploadBytes() const;
    void Upload(LuxEngine::GpuDevice& device, LuxEngine::Mesh3D& mesh) const;
};

Vec2 Horizontal(Vec3 p);
double Distance(Vec2 a, Vec2 b);
const Region& RegionAt(Vec2 point);
double RoadDistance(Vec2 point);
double Height(Vec2 point, unsigned seed);
bool InLake(Vec2 point);
Vec3 OnGround(Vec2 point, unsigned seed, double offset = 0);
ChunkKey ChunkAt(Vec2 point);
Vec2 ChunkOrigin(ChunkKey key);
std::shared_ptr<const LuxEngine::TriangleMeshCollider3DData> TerrainCollision(ChunkKey key,
                                                                              unsigned seed);

class World {
public:
    struct Stats {
        unsigned loaded = 0, pending = 0, trees = 0;
        unsigned built = 0, evicted = 0, uploadedMeshes = 0;
        double lastBuildMs = 0;
    };

    World();
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void Start(unsigned seed, unsigned vegetation, int radius, Vec2 initial,
               LuxEngine::GpuDevice& device);
    void Stream(Vec2 at, Vec2 velocity, LuxEngine::GpuDevice& device, bool allowUploads);
    void Draw(LuxEngine::Renderer3D::Context& renderer, const LuxEngine::Material3D& ground,
              const LuxEngine::Material3D& scenery, const LuxEngine::Material3D& foliage,
              const LuxEngine::Material3D& road, Vec3 eye, std::vector<Plant>& nearTrees) const;
    std::vector<Obstacle> NearbyObstacles(Vec2 at, double distance) const;
    bool HasChunk(ChunkKey key) const;
    Stats GetStats() const;
    void Stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace Frontier
