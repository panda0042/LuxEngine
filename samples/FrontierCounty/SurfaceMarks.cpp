#include "SurfaceMarks.hpp"
#include <LuxEngine/Assets/AssetManager.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <stdexcept>

namespace Frontier {
using namespace LuxEngine;

namespace {
constexpr double TileSize = 14;

struct RockPlacement {
    Vec2 point;
    unsigned scan;
    double scale, yaw;
};

constexpr std::array Rocks{
    RockPlacement{{232.7, 329.4}, 1, .48, 1.3},  RockPlacement{{235.2, 331.9}, 2, .27, 2.8},
    RockPlacement{{238.9, 333.5}, 0, .76, .36},  RockPlacement{{264.2, 326.1}, 2, .82, 1.02},
    RockPlacement{{278.2, 307.1}, 0, 1.1, 2.45}, RockPlacement{{211.7, 282.8}, 1, .78, 4.7},
    RockPlacement{{237.5, 321.3}, 2, .32, 3.5}};
// Ground anchor and horizontal footprint in source meters from the scanned mesh bounds.
constexpr std::array<Vec3, 3> RockBounds{Vec3{2.238, -.6566, 3.364}, Vec3{2.109, -.4109, 2.080},
                                         Vec3{2.127, -.661, 1.964}};

ChunkKey Tile(Vec2 p, double size) {
    return {int(std::floor(p.x / size)), int(std::floor(p.y / size))};
}

uint32 Hash(ChunkKey cell, unsigned seed) {
    uint32 x = uint32(cell.x) * 0x85ebca77u ^ uint32(cell.z) * 0xc2b2ae3du ^ seed;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    return x ^ (x >> 16u);
}

Vec3 SurfaceNormal(Vec2 point, unsigned seed) {
    return Vec3{Height(point - Vec2{.4, 0}, seed) - Height(point + Vec2{.4, 0}, seed), .8,
                Height(point - Vec2{0, .4}, seed) - Height(point + Vec2{0, .4}, seed)}
        .Normalized();
}

std::string Path(const std::filesystem::path& p) {
    const auto bytes = p.generic_u8string();
    return {bytes.begin(), bytes.end()};
}
} // namespace

void SurfaceMarks::Initialize(const std::filesystem::path& assets) {
    const auto rockDir = assets / "rainy-creek";
    const std::array names{"rock_a.obj", "rock_b.obj", "rock_c.obj"};
    for (size_t i = 0; i < names.size(); ++i) {
        rockMeshes_[i] = Path(rockDir / "Models" / names[i]);
        bool textured = false;
        if (!AssetManager::ValidateMeshFile(rockMeshes_[i], &textured) || !textured)
            throw std::runtime_error("Could not load shoreline scan: " + rockMeshes_[i]);
    }
    rockMaterial_.roughness = .88f;
    rockMaterial_.baseColorTexture = Path(rockDir / "Textures" / "rock.png");
    rockMaterial_.textureSampling = {true, TextureAddressMode::Clamp, true, false};
    rockMaterial_.wettingResponse = .82f;
    const auto dir = assets / "FrontierMarks";
    material_.alphaMode = MaterialAlphaMode::Blend;
    material_.baseColorTexture = Path(dir / "albedo.png");
    material_.normalTexture = Path(dir / "normal.png");
    material_.roughnessTexture = Path(dir / "roughness.png");
    material_.occlusionTexture = Path(dir / "occlusion.png");
    material_.roughness = 1;
    material_.metallic = 0;
    material_.textureSampling = {true, TextureAddressMode::Clamp, true, false};
    material_.wettingResponse = .28f;
}

void SurfaceMarks::Update(GpuDevice& gpu, Vec3 eye, Vec3 actor, bool driving, double wetness,
                          unsigned seed) {
    const auto position = Horizontal(actor);
    const auto viewTile = Tile(Horizontal(eye), TileSize);
    const int wetBand = int(wetness * 4);
    bool changed = !viewTile_ || viewTile != *viewTile_ || wetBand != lastWetness_;
    if (!previous_ || driving_ != driving || Distance(position, *previous_) > 10)
        previous_ = position;
    driving_ = driving;
    const double travel = Distance(position, *previous_);
    const double stride = driving ? 1.05 : .68;
    if (travel >= stride) {
        const Vec2 direction = (position - *previous_) * (1. / travel);
        const Vec2 across{-direction.y, direction.x};
        const bool onRoad = RoadDistance(position) < 3.9;
        const auto height = Height(position, seed);
        const bool dryGround = !InLake(position) || height > LakeLevel - .015;
        if (dryGround && actor.y < height + (driving ? 1.8 : .45)) {
            if (driving) {
                const float strength = float(onRoad ? .17 + .23 * wetness : .56 + .20 * wetness);
                for (double side : {-1., 1.})
                    trail_.push_back({position + across * (side * .75),
                                      direction,
                                      {1.15, .28},
                                      0,
                                      strength,
                                      onRoad});
            } else if (!onRoad || wetness > .15) {
                rightFoot_ = !rightFoot_;
                trail_.push_back({position + across * (rightFoot_ ? .10 : -.10),
                                  direction,
                                  {.31, .13},
                                  rightFoot_ ? 2u : 1u,
                                  float(onRoad ? .23 : .58),
                                  onRoad});
            }
            changed = true;
        }
        previous_ = position;
        while (trail_.size() > 96)
            trail_.pop_front();
    }
    if (changed) {
        Rebuild(gpu, Horizontal(eye), wetness, seed);
        viewTile_ = viewTile;
        lastWetness_ = wetBand;
    }
}

void SurfaceMarks::Rebuild(GpuDevice& gpu, Vec2 eye, double wetness, unsigned seed) {
    std::map<ChunkKey, std::vector<DecalProjector>> cells;
    const auto stamp = [&](const Mark& mark, int layer) {
        if (Distance(eye, mark.center) > 48)
            return;
        DecalProjector p;
        p.center = OnGround(mark.center, seed);
        p.normal = SurfaceNormal(mark.center, seed);
        p.tangent = {mark.direction.x, 0, mark.direction.y};
        p.halfSize = {mark.size.x * .5, mark.size.y * .5, .42};
        p.normalOffset = (mark.onRoad ? .019 : .007) + .001 * layer;
        p.tint = {1, 1, 1, mark.opacity};
        p.uvOrigin = {double(mark.tile % 4) / 4., double(mark.tile / 4) / 2.};
        p.uvSize = {.25, .5};
        p.layer = layer;
        const auto reach = std::hypot(p.halfSize.x, p.halfSize.y);
        const auto lo = Tile(mark.center - Vec2{reach, reach}, TerrainStep);
        const auto hi = Tile(mark.center + Vec2{reach, reach}, TerrainStep);
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x)
                cells[{x, z}].push_back(p);
    };
    const auto near = Tile(eye, TileSize);
    for (int z = near.z - 3; z <= near.z + 3; ++z)
        for (int x = near.x - 3; x <= near.x + 3; ++x) {
            const auto h = Hash({x, z}, seed);
            const Vec2 p{(x + .13 + .74 * double(h & 255) / 255.) * TileSize,
                         (z + .13 + .74 * double((h >> 8) & 255) / 255.) * TileSize};
            const auto height = Height(p, seed);
            const double angle = 2 * std::numbers::pi * double(h >> 16) / 65535.;
            const Vec2 direction{std::cos(angle), std::sin(angle)};
            const double road = RoadDistance(p);
            if (road < 3.8)
                stamp({p, direction, {3.8, 1.6}, 3, .67f, true}, 1);
            else if (height > LakeLevel - .04 && height < LakeLevel + 1.5 &&
                     Distance(p, LakeCenter) < 160) {
                stamp({p, direction, {4.5, 1.6}, 4, .68f, false}, 0);
                if ((h & 3) == 0)
                    stamp({p + direction * .7, direction, {2.5, 1.2}, 5, .55f, false}, 1);
            } else if (wetness > .36 && road < 12)
                stamp({p, direction, {2.7, 1.2}, 5, float(wetness * .67), false}, 1);
            else if (height > LakeLevel && SurfaceNormal(p, seed).y < .96 && (h & 1) == 0)
                stamp({p, direction, {3.6, 1.4}, h & 2 ? 6u : 7u, .54f, false}, 0);
        }
    visibleRocks_.clear();
    for (const auto& rock : Rocks) {
        if (Distance(eye, rock.point) > 120)
            continue;
        const auto bounds = RockBounds[rock.scan];
        const Vec2 tangent{std::cos(rock.yaw), -std::sin(rock.yaw)};
        const double s = rock.scale;
        const auto transform = Mat4::Translate(OnGround(rock.point, seed, (-bounds.y - .20) * s)) *
                               Mat4::RotateY(rock.yaw) * Mat4::Scale({s, s, s});
        visibleRocks_.emplace_back(rock.scan, transform);
        stamp({rock.point, tangent, {bounds.x * s * 1.5, bounds.z * s * 1.5}, 6, .76f, false}, 1);
    }
    for (const auto& mark : trail_)
        stamp(mark, 2);
    MeshData surface;
    constexpr std::array<uint32, 6> triangles{0, 2, 1, 1, 2, 3};
    for (const auto& [cell, marks] : cells) {
        std::array<Vertex3D, 4> receiver;
        for (unsigned i = 0; i < 4; ++i) {
            const Vec2 point{(cell.x + int(i & 1)) * TerrainStep,
                             (cell.z + int(i >> 1)) * TerrainStep};
            receiver[i].position = OnGround(point, seed);
            receiver[i].normal = SurfaceNormal(point, seed);
        }
        if (!AppendProjectedDecals(marks, receiver, triangles, surface.vertices, surface.indices))
            throw std::runtime_error("Invalid surface-mark projection");
    }
    if (surface.indices.empty())
        mesh_.Release();
    else
        surface.Upload(gpu, mesh_);
}

void SurfaceMarks::Draw(Renderer3D::Context& renderer) const {
    for (const auto& [scan, transform] : visibleRocks_)
        renderer.DrawMeshAsset(rockMeshes_[scan], transform, rockMaterial_);
    if (mesh_.IsValid())
        renderer.DrawMesh(mesh_, Mat4::Identity(), material_);
}
} // namespace Frontier
