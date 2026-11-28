#include "World.hpp"
#include <LuxEngine/Environment/TerrainHeightfield.hpp>
#include <LuxEngine/Renderer/Renderer3D/Decal.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Frontier {
namespace {
using namespace LuxEngine;

constexpr std::array Roads{
    std::array<Vec2, 2>{{{-720, -243}, {-518, -170}}},
    std::array<Vec2, 2>{{{-518, -170}, {-222, -48}}},
    std::array<Vec2, 2>{{{-222, -48}, {-22, 0}}},
    std::array<Vec2, 2>{{{-80, 0}, {98, 0}}},
    std::array<Vec2, 2>{{{98, 0}, {242, -5}}},
    std::array<Vec2, 2>{{{242, -5}, {420, -78}}},
    std::array<Vec2, 2>{{{420, -78}, {602, -247}}},
    std::array<Vec2, 2>{{{0, -64}, {0, 116}}},
    std::array<Vec2, 2>{{{0, 116}, {112, 204}}},
    std::array<Vec2, 2>{{{112, 204}, {163, 278}}},
    std::array<Vec2, 2>{{{163, 278}, {228, 295}}},
    std::array<Vec2, 2>{{{242, -5}, {438, 101}}},
    std::array<Vec2, 2>{{{438, 101}, {645, 198}}},
    std::array<Vec2, 2>{{{-720, -243}, {-710, -412}}},
    std::array<Vec2, 2>{{{-710, -412}, {-550, -541}}},
    std::array<Vec2, 2>{{{-550, -541}, {-447, -667}}},
    std::array<Vec2, 2>{{{-447, -667}, {-374, -840}}},
};
constexpr unsigned Cells = TerrainCells;

double Blend(double from, double to, double fraction) {
    return from + (to - from) * fraction;
}

double Curve(double low, double high, double value) {
    const double t = std::clamp((value - low) / (high - low), 0., 1.);
    return t * t * (3 - 2 * t);
}

unsigned Hash(unsigned a, unsigned b, unsigned seed) {
    unsigned n = a * 0x9e3779b9u ^ b * 0x85ebca6bu ^ seed;
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    return n ^ (n >> 16);
}

double Unit(unsigned value) {
    return double(value >> 8) / 16777216.;
}

double Noise(Vec2 p, double length, unsigned seed) {
    const double x = p.x / length, z = p.y / length;
    const int i = int(std::floor(x)), j = int(std::floor(z));
    const double fx = Curve(0, 1, x - i), fz = Curve(0, 1, z - j);
    const auto at = [seed](int a, int b) { return Unit(Hash(unsigned(a), unsigned(b), seed)); };
    return Blend(Blend(at(i, j), at(i + 1, j), fx), Blend(at(i, j + 1), at(i + 1, j + 1), fx), fz);
}

double LakeRadius(Vec2 p) {
    const double x = 2 * (p.x - LakeCenter.x) / LakeSize.x;
    const double z = 2 * (p.y - LakeCenter.y) / LakeSize.y;
    return std::hypot(x, z);
}

// A broad drainage valley joins the county's inhabited areas. Narrower gullies
// cut across the uplift along the northwestern ridge.
double UnweatheredRelief(Vec2 p, unsigned seed) {
    const double local = Distance(p, Regions[0].center);
    const double west = Curve(122, 710, -p.x);
    const double north = Curve(100, 780, -p.y);
    const double far = Curve(900, 3400, local);
    const double broad = Noise(p, 467, seed);
    const double uplift = west * 33 + north * 38 + far * 168;
    const double ridge = 1 - std::abs(2 * Noise({p.x + p.y * .43, p.y}, 146, seed + 19) - 1);
    const double gully = std::pow(1 - std::abs(2 * Noise(p, 91, seed + 41) - 1), 9);
    const double localRoughness = Curve(52, 230, local);
    double y = 17 + uplift * (.28 + .83 * broad + .45 * ridge * ridge - .17 * gully) +
               localRoughness * (Noise(p, 32, seed + 17) - .51) * 4.5;
    const double shore = LakeRadius(p);
    const double eastBank = Curve(300, 470, p.x);
    const double southBank = Curve(390, 615, p.y);
    const double shorelineRidges = Curve(1.04, 2.5, shore) *
                                   (1 - Curve(285, 705, Distance(p, LakeCenter))) *
                                   (21 * eastBank + 18 * southBank);
    y += shorelineRidges * (.58 + 1.17 * ridge);
    // Folded strata and branching ridges above the opposite shore. Keep the lake basin,
    // the floodplain and the settled valley at their surveyed elevations.
    const Vec2 warped{p.x + 73 * (Noise(p, 330, seed + 92) - .5),
                      p.y + 59 * (Noise(p, 290, seed + 47) - .5)};
    const double foothills = Curve(1.20, 3.65, shore) * Curve(350, 625, p.x + p.y * .35) *
                             (1. - Curve(670, 1450, Distance(p, LakeCenter)));
    double ridges = 0, amplitude = .54, wavelength = 380, previous = 1;
    for (unsigned octave = 0; octave < 4; ++octave) {
        const double r =
            1. - std::abs(2. * Noise(warped, wavelength, seed + 127 + octave * 7) - 1.);
        ridges += amplitude * r * r * previous;
        previous = std::min(1., 1.65 * r);
        wavelength *= .49;
        amplitude *= .51;
    }
    y += foothills * (47. + 127. * ridges);
    const double bed = LakeLevel + (shore - .82) * (shore < .82 ? 10. : 23.);
    return Blend(bed, y, Curve(.96, 1.45, shore));
}

// The coarse watershed spans the populated basin. Its shared erosion correction is
// blended into the high-frequency survey, including the collision and road surfaces.
struct Watershed {
    static constexpr int Count = 145;
    static constexpr double Spacing = 20;
    static constexpr Vec2 Origin{-1440, -1440};
    TerrainHeightfield survey, bed;

    static TerrainHeightfield Survey(unsigned seed) {
        std::vector<float> h(Count * Count);
        for (int z = 0; z < Count; ++z)
            for (int x = 0; x < Count; ++x)
                h[z * Count + x] =
                    float(UnweatheredRelief(Origin + Vec2{x * Spacing, z * Spacing}, seed));
        return {Count, Count, Spacing, Origin, h};
    }

    explicit Watershed(unsigned seed) : survey(Survey(seed)), bed(survey) {
        bed.Erode({38, .12, .16, .42, .64});
    }
};

const Watershed& Basin(unsigned seed) {
    // Main, terrain streaming and foliage preparation can sample different worlds concurrently.
    static std::mutex mutex;
    static std::map<unsigned, std::weak_ptr<const Watershed>> basins;
    thread_local unsigned localSeed{};
    thread_local std::shared_ptr<const Watershed> local;
    if (!local || localSeed != seed) {
        std::lock_guard lock(mutex);
        std::erase_if(basins, [](const auto& entry) { return entry.second.expired(); });
        auto& stored = basins[seed];
        local = stored.lock();
        if (!local) {
            local = std::make_shared<Watershed>(seed);
            stored = local;
        }
        localSeed = seed;
    }
    return *local;
}

double Relief(Vec2 p, unsigned seed) {
    const double original = UnweatheredRelief(p, seed);
    const double influence = 1. - Curve(1320., 1440., std::max(std::abs(p.x), std::abs(p.y)));
    if (influence == 0.)
        return original;
    const auto& basin = Basin(seed);
    return original + influence * (basin.bed.Height(p) - basin.survey.Height(p));
}

struct RoadPoint {
    double distance;
    Vec2 point;
};

RoadPoint NearestRoad(Vec2 p) {
    RoadPoint result{1e8, {}};
    for (auto [a, b] : Roads) {
        const Vec2 direction = b - a;
        const Vec2 delta = p - a;
        const double t = std::clamp((delta.x * direction.x + delta.y * direction.y) /
                                        (direction.x * direction.x + direction.y * direction.y),
                                    0., 1.);
        const Vec2 closest = a + direction * t;
        const double distance = Distance(closest, p);
        if (distance < result.distance)
            result = {distance, closest};
    }
    return result;
}

double AnalyticHeight(Vec2 point, unsigned seed) {
    const auto road = NearestRoad(point);
    const double offRoad = Curve(5, 23, road.distance);
    const double raw = Relief(point, seed);
    if (offRoad == 1)
        return raw;
    return Blend(Relief(road.point, seed), raw, offRoad);
}

constexpr std::array Sites{
    Structure{"corner-shop-block", {-27, -20}, {26, 11, 15}, 0},
    Structure{"workshop-building", {-57, -19}, {24, 7, 17}, 0},
    Structure{"village-shop", {23, -20}, {18, 6, 12}, 0},
    Structure{"roadside-diner", {58, -21}, {21, 5.6, 11}, 0},
    Structure{"workshop-building", {-27, 22}, {26, 7, 18}, std::numbers::pi},
    Structure{"village-house", {-53, 23}, {13, 9, 15}, std::numbers::pi},
    Structure{"abandoned-fuel-station", {34, 34}, {19, 6, 14}, std::numbers::pi},
    Structure{"village-shop", {69, 29}, {16, 5, 13}, std::numbers::pi},
    Structure{"village-house", {-27, 61}, {12, 9, 10}, .17},
    Structure{"village-house", {-49, 58}, {13, 10, 12}, -.31},
    Structure{"village-shop", {24, 64}, {18, 6, 11}, -.17},
    Structure{"village-house", {-733, -227}, {12, 8, 10}, .12},
    Structure{"hay-barn", {-699, -214}, {23, 9, 17}, .15},
    Structure{"village-house", {-745, -267}, {11, 9, 10}, .31},
    Structure{"village-house", {-391, -826}, {15, 8, 10}, .54},
    Structure{"workshop-building", {-357, -854}, {20, 8, 15}, -.31},
    Structure{"boathouse", {215, 281}, {9, 6, 15}, -.42},
    Structure{"village-house", {189, 264}, {13, 9, 13}, -.24},
    Structure{"hay-barn", {621, -252}, {30, 12, 18}, 0},
    Structure{"village-house", {582, -268}, {16, 9, 15}, .22},
    Structure{"hay-barn", {619, -224}, {12, 9, 13}, std::numbers::pi},
    Structure{"workshop-building", {617, 213}, {30, 11, 23}, -.21},
    Structure{"hay-barn", {668, 200}, {34, 13, 22}, -.24},
    Structure{"workshop-building", {667, 164}, {30, 9, 20}, -.28},
};
constexpr Vec4 Timber{.31f, .21f, .12f, 1};

ChunkKey KeyFor(Vec2 p) {
    return {int(std::floor(p.x / ChunkSize)), int(std::floor(p.y / ChunkSize))};
}

constexpr unsigned FoliageCells = 4;
constexpr int FoliageCellSize = ChunkSize / FoliageCells;

struct FoliageDraft {
    MeshData cards;
    std::vector<Plant> plants;
};

struct Draft {
    ChunkKey key;
    MeshData terrain, road, scenery;
    std::array<FoliageDraft, FoliageCells * FoliageCells> foliage;
    std::vector<Obstacle> obstacles;
    unsigned trees = 0;
    double buildMs = 0;
};

void Tree(MeshData& mesh, Vec3 base, double height, double variation) {
    const float tint = float(.77 + .23 * variation);
    const Vec4 color{tint, tint, tint, 1};
    constexpr double Tau = 2 * std::numbers::pi;
    for (int plane = 0; plane < 3; ++plane) {
        const double angle = (variation + plane / 3.) * Tau;
        const Vec3 side{std::cos(angle), 0, std::sin(angle)};
        const Vec3 normal{-side.z, 0., side.x};
        const Vec3 edge = side * (height * .30);
        const Vec3 up{0, height, 0};
        const auto a = uint32(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),
                             {{base - edge, normal, {double(plane) / 3., 1}, color},
                              {base - edge + up, normal, {double(plane) / 3., 0}, color, 1},
                              {base + edge + up, normal, {double(plane + 1) / 3., 0}, color, 1},
                              {base + edge, normal, {double(plane + 1) / 3., 1}, color}});
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + 2, a, a + 2, a + 3});
    }
}

double Cross2(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}

void AddRoadSurface(MeshData& mesh, const std::array<Vec2, 4>& ribbon, Vec2 origin, unsigned seed,
                    double bias, Vec4 color) {
    constexpr double step = TerrainStep;
    const auto [minX, maxX] = std::ranges::minmax(ribbon, {}, &Vec2::x);
    const auto [minZ, maxZ] = std::ranges::minmax(ribbon, {}, &Vec2::y);
    for (double z = std::floor(minZ.y / step) * step; z <= maxZ.y; z += step) {
        for (double x = std::floor(minX.x / step) * step; x <= maxX.x; x += step) {
            const Vec2 a{x, z}, b{x, z + step}, c{x + step, z}, d{x + step, z + step};
            const std::array triangles{std::array{a, b, c}, std::array{c, b, d}};
            for (const auto& tri : triangles) {
                std::vector<Vec2> poly(ribbon.begin(), ribbon.end());
                for (size_t edge = 0; edge < 3 && !poly.empty(); ++edge) {
                    const auto from = tri[edge], direction = tri[(edge + 1) % 3] - from;
                    std::vector<Vec2> clipped;
                    auto prev = poly.back();
                    double prevSide = Cross2(direction, prev - from);
                    for (const auto p : poly) {
                        const double side = Cross2(direction, p - from);
                        if ((prevSide < 0) != (side < 0))
                            clipped.push_back(prev + (p - prev) * (prevSide / (prevSide - side)));
                        if (side <= 0)
                            clipped.push_back(p);
                        prev = p;
                        prevSide = side;
                    }
                    poly = std::move(clipped);
                }
                if (poly.size() < 3)
                    continue;
                const Vec3 pa = OnGround(tri[0], seed), pb = OnGround(tri[1], seed),
                           pc = OnGround(tri[2], seed);
                const auto normal = (pb - pa).Cross(pc - pa).Normalized();
                const auto first = uint32(mesh.vertices.size());
                for (const auto p : poly) {
                    const auto world = OnGround(p, seed, bias);
                    mesh.vertices.push_back(
                        {world - Vec3{origin.x, 0, origin.y}, normal, {p.x * .5, p.y * .5}, color});
                }
                for (uint32 i = 1; i + 1 < uint32(poly.size()); ++i)
                    mesh.indices.insert(mesh.indices.end(), {first, first + i, first + i + 1});
            }
        }
    }
}

template <typename Emit> void ForTerrainVertex(Vec2 origin, unsigned seed, Emit emit) {
    for (unsigned z = 0; z <= Cells; ++z)
        for (unsigned x = 0; x <= Cells; ++x) {
            const Vec2 local{x * TerrainStep, z * TerrainStep};
            const Vec2 point = origin + local;
            emit(point, Vec3{local.x, Height(point, seed), local.y});
        }
}

const std::vector<uint32>& TerrainIndices() {
    static const auto indices = [] {
        std::vector<uint32> values;
        values.reserve(Cells * Cells * 6);
        constexpr unsigned stride = Cells + 1;
        for (unsigned z = 0; z < Cells; ++z)
            for (unsigned x = 0; x < Cells; ++x) {
                const auto a = z * stride + x, b = a + stride;
                values.insert(values.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        return values;
    }();
    return indices;
}

Vertex3D TerrainVertex(Vec2 p, Vec3 position, unsigned seed, double normalStep) {
    const double y = position.y;
    const auto normal = Vec3{AnalyticHeight(p - Vec2{normalStep, 0}, seed) -
                                 AnalyticHeight(p + Vec2{normalStep, 0}, seed),
                             2. * normalStep,
                             AnalyticHeight(p - Vec2{0, normalStep}, seed) -
                                 AnalyticHeight(p + Vec2{0, normalStep}, seed)}
                            .Normalized();
    const double shore = LakeRadius(p);
    const double drainage = Basin(seed).bed.Drainage(p);
    const double moisture =
        std::clamp(.67 * Noise(p, 115, seed + 31) + .22 * Noise(p, 32, seed + 55) +
                       .15 * Curve(2, 10, drainage) + .28 * (1 - Curve(.97, 1.27, shore)),
                   0., 1.);
    const double rock = Curve(.075, .38, 1. - normal.y) + .3 * Curve(65, 145, y);
    const double beach = 1. - Curve(.90, 1.22, shore);
    const double meadow = (1. - beach) * (1. - Curve(.07, .25, 1. - normal.y)) *
                          (.6 + .38 * Noise(p, 90, seed + 16)) * Curve(5, 14, RoadDistance(p));
    const double cliff = std::clamp(rock, 0., 1.);
    return {position,
            normal,
            {p.x * .32, p.y * .32},
            {float(cliff), float((1. - cliff) * meadow), float((1. - cliff) * beach),
             float(Curve(.21, .62, moisture))}};
}

MeshData DistantTerrain(ChunkKey center, unsigned seed, int radius) {
    MeshData result;
    // Each coarse chunk shares all edge vertices with its neighbours. Sample its four edges
    // at the fine mesh's 8 m interval, then triangulate the 32 m interior to those points.
    constexpr int steps = 8;
    const Vec2 origin = ChunkOrigin(center);
    for (int dz = -9; dz <= 9; ++dz)
        for (int dx = -9; dx <= 9; ++dx) {
            if (std::max(std::abs(dx), std::abs(dz)) <= radius)
                continue;
            const ChunkKey key{center.x + dx, center.z + dz};
            const Vec2 chunk = ChunkOrigin(key);
            if (chunk.x < -WorldHalf || chunk.y < -WorldHalf || chunk.x >= WorldHalf ||
                chunk.y >= WorldHalf)
                continue;
            const auto point = [&](double x, double z) {
                const Vec2 p = chunk + Vec2{x, z};
                const auto id = uint32(result.vertices.size());
                result.vertices.push_back(TerrainVertex(
                    p, {p.x - origin.x, AnalyticHeight(p, seed), p.y - origin.y}, seed, 10));
                return id;
            };
            std::array<uint32, (steps + 1) * (steps + 1)> grid{};
            constexpr double stride = double(ChunkSize) / steps;
            for (int z = 0; z <= steps; ++z)
                for (int x = 0; x <= steps; ++x)
                    grid[z * (steps + 1) + x] = point(x * stride, z * stride);
            for (int z = 0; z < steps; ++z)
                for (int x = 0; x < steps; ++x) {
                    const int index = z * (steps + 1) + x;
                    const std::array corners = {grid[index], grid[index + steps + 1],
                                                grid[index + steps + 2], grid[index + 1]};
                    const bool edge = std::max(std::abs(dx), std::abs(dz)) == radius + 1 &&
                                      (x == 0 || z == 0 || x == steps - 1 || z == steps - 1);
                    if (!edge) {
                        result.indices.insert(result.indices.end(),
                                              {corners[0], corners[1], corners[3], corners[3],
                                               corners[1], corners[2]});
                        continue;
                    }
                    const auto middle = point((x + .5) * stride, (z + .5) * stride);
                    const std::array<Vec2, 4> coords = {
                        Vec2{double(x), double(z)}, Vec2{double(x), z + 1.}, Vec2{x + 1., z + 1.},
                        Vec2{x + 1., double(z)}};
                    const std::array boundary = {
                        x == 0 && dx == radius + 1, z == steps - 1 && dz == -radius - 1,
                        x == steps - 1 && dx == -radius - 1, z == 0 && dz == radius + 1};
                    for (unsigned e = 0; e < 4; ++e) {
                        const unsigned n = (e + 1) % 4;
                        auto previous = corners[e];
                        if (boundary[e])
                            for (int k = 1; k < 4; ++k) {
                                const auto pt =
                                    (coords[e] + (coords[n] - coords[e]) * (double(k) / 4)) *
                                    stride;
                                const auto next = point(pt.x, pt.y);
                                result.indices.insert(result.indices.end(),
                                                      {middle, previous, next});
                                previous = next;
                            }
                        result.indices.insert(result.indices.end(), {middle, previous, corners[n]});
                    }
                }
        }
    return result;
}

Draft Generate(ChunkKey key, unsigned seed, unsigned vegetation) {
    const auto start = std::chrono::steady_clock::now();
    Draft result;
    result.key = key;
    const Vec2 origin = ChunkOrigin(key);
    auto& ground = result.terrain;
    ground.vertices.reserve((Cells + 1) * (Cells + 1));
    ground.indices = TerrainIndices();
    ForTerrainVertex(origin, seed, [&](Vec2 p, Vec3 position) {
        ground.vertices.push_back(TerrainVertex(p, position, seed, 4));
    });
    for (const auto& [a, b] : Roads) {
        const Vec2 ray = b - a;
        const double length = Distance(a, b);
        const Vec2 across{-ray.y / length, ray.x / length};
        const int pieces = int(std::ceil(length / 5));
        for (int i = 0; i < pieces; ++i) {
            const Vec2 p = a + ray * (double(i) / pieces);
            const Vec2 q = a + ray * (double(i + 1) / pieces);
            if (KeyFor((p + q) * .5) != key)
                continue;
            const auto patch = [&](double left, double right, double bias, Vec4 color,
                                   MeshData& mesh) {
                AddRoadSurface(
                    mesh,
                    {p + across * left, p + across * right, q + across * right, q + across * left},
                    origin, seed, bias, color);
            };
            const auto roadStart = result.road.indices.size();
            patch(-4.05, 4.05, .012, {.88f, .88f, .87f, 1}, result.road);
            if (i % 2 == 0) {
                DecalProjector stripe;
                const auto midpoint = (p + q) * .5;
                stripe.center = OnGround(midpoint, seed) - Vec3{origin.x, 0, origin.y};
                stripe.tangent = {ray.x / length, 0, ray.y / length};
                stripe.halfSize = {Distance(p, q) * .5, .065, 6};
                stripe.normalOffset = .006;
                stripe.tint = {.49f, .33f, .11f, 1};
                AppendProjectedDecal(stripe, result.road.vertices,
                                     std::span(result.road.indices).subspan(roadStart),
                                     result.scenery.vertices, result.scenery.indices);
            }
        }
    }
    for (const auto& site : Sites) {
        if (KeyFor(site.position) == key)
            result.obstacles.push_back(
                {OnGround(site.position, seed, site.size.y * .5), site.size});
    }
    if (KeyFor(Dock) == key) {
        const Vec4 wood{.42f, .31f, .19f, 1};
        for (int i = 0; i < 23; ++i) {
            const Vec3 deck{Dock.x + i * .49 - origin.x, LakeLevel + .29, Dock.y - origin.y};
            result.scenery.Box(deck, {.46, .16, 3.2}, wood);
            if (i % 5 == 0)
                for (double side : {-1., 1.})
                    result.scenery.Box(deck + Vec3{0, -.65, side * 1.45}, {.22, 1.65, .22}, Timber);
        }
    }
    const auto addTree = [&](Vec2 point, double size, double variation) {
        const auto local = point - origin;
        const auto cellX = unsigned(local.x / FoliageCellSize);
        const auto cellZ = unsigned(local.y / FoliageCellSize);
        auto& cell = result.foliage[cellZ * FoliageCells + cellX];
        const double height = Height(point, seed);
        Tree(cell.cards, {local.x, height - .2, local.y}, size, variation);
        cell.plants.push_back({{point.x, height - .13, point.y},
                               {size * .49, size, size * .45},
                               variation * 2 * std::numbers::pi});
        ++result.trees;
    };
    // A few older trees anchor the walk from the road down to Silverlake.
    constexpr std::array shoreTrees{Vec3{235, 10.8, 310}, Vec3{255, 13.4, 313}, Vec3{229, 8.2, 335},
                                    Vec3{220, 9.7, 339}};
    for (size_t i = 0; i < shoreTrees.size(); ++i) {
        const auto tree = shoreTrees[i];
        if (KeyFor(Horizontal(tree)) == key)
            addTree(Horizontal(tree), tree.y, .21 + double(i) * .11);
    }
    for (unsigned i = 0; i < vegetation; ++i) {
        const unsigned base = Hash(unsigned(key.x), unsigned(key.z), seed + i * 17);
        const double a = Unit(base);
        const Vec2 point = origin + Vec2{Unit(Hash(base, 0, seed)) * ChunkSize,
                                         Unit(Hash(base, 1, seed)) * ChunkSize};
        const auto& region = RegionAt(point);
        const double settlementExclusion = &region == &Regions[0] ? 70 : 39;
        if (RoadDistance(point) < 8 || InLake(point) ||
            Distance(point, region.center) < settlementExclusion)
            continue;
        const double canopy =
            .65 * Noise(point, 105, seed + 81) + .35 * Noise(point, 38, seed + 63);
        const double moisture = Curve(.24, .72, canopy);
        const double height = Height(point, seed);
        if (a > (.08 + .92 * moisture) * (1 - Curve(100, 167, height)))
            continue;
        if (std::ranges::any_of(shoreTrees,
                                [&](Vec3 tree) { return Distance(Horizontal(tree), point) < 4; }))
            continue;
        const double separation = 2.7 + .8 * moisture;
        const auto local = point - origin;
        bool crowded = false;
        for (int z = std::max(0, int((local.y - separation) / FoliageCellSize));
             z <= std::min(int(FoliageCells) - 1, int((local.y + separation) / FoliageCellSize));
             ++z)
            for (int x = std::max(0, int((local.x - separation) / FoliageCellSize));
                 x <=
                 std::min(int(FoliageCells) - 1, int((local.x + separation) / FoliageCellSize));
                 ++x)
                crowded |= std::ranges::any_of(
                    result.foliage[z * FoliageCells + x].plants, [&](const Plant& plant) {
                        return Distance(point, Horizontal(plant.ground)) < separation;
                    });
        if (!crowded)
            addTree(point, 4.1 + 14.8 * Unit(Hash(base, 4, seed)), Unit(Hash(base, 3, seed)));
    }
    result.buildMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
}

struct Loaded {
    Mesh3D ground, road, scenery;

    struct Foliage {
        std::unique_ptr<Mesh3D> cards;
        std::vector<Plant> plants;
    };

    std::array<Foliage, FoliageCells * FoliageCells> foliage;
    std::vector<Obstacle> obstacles;
    unsigned trees = 0;
};
} // namespace

Vec2 Horizontal(Vec3 p) {
    return {p.x, p.z};
}

std::span<const Structure> Structures() {
    return Sites;
}

double Distance(Vec2 a, Vec2 b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

const Region& RegionAt(Vec2 point) {
    return *std::ranges::min_element(
        Regions, {}, [point](const Region& region) { return Distance(point, region.center); });
}

double RoadDistance(Vec2 point) {
    return NearestRoad(point).distance;
}

double Height(Vec2 point, unsigned seed) {
    constexpr double step = TerrainStep;
    const Vec2 base{step * std::floor(point.x / step), step * std::floor(point.y / step)};
    const double u = (point.x - base.x) / step, v = (point.y - base.y) / step;
    const double a = AnalyticHeight(base, seed);
    if (u == 0 && v == 0)
        return a;
    const double right = AnalyticHeight(base + Vec2{step, 0}, seed);
    const double bottom = AnalyticHeight(base + Vec2{0, step}, seed);
    // Match the diagonal used by TerrainIndices so wheels, feet and terrain agree.
    if (u + v <= 1)
        return a * (1 - u - v) + right * u + bottom * v;
    const double corner = AnalyticHeight(base + Vec2{step, step}, seed);
    return corner * (u + v - 1) + right * (1 - v) + bottom * (1 - u);
}

bool InLake(Vec2 point) {
    return LakeRadius(point) < .82;
}

Vec3 OnGround(Vec2 point, unsigned seed, double offset) {
    return {point.x, Height(point, seed) + offset, point.y};
}

ChunkKey ChunkAt(Vec2 point) {
    return KeyFor(point);
}

Vec2 ChunkOrigin(ChunkKey key) {
    return {double(key.x * ChunkSize), double(key.z * ChunkSize)};
}

std::shared_ptr<const LuxEngine::TriangleMeshCollider3DData> TerrainCollision(ChunkKey key,
                                                                              unsigned seed) {
    auto mesh = std::make_shared<LuxEngine::TriangleMeshCollider3DData>();
    mesh->vertices.reserve((Cells + 1) * (Cells + 1));
    mesh->indices = TerrainIndices();
    ForTerrainVertex(ChunkOrigin(key), seed,
                     [&](Vec2, Vec3 position) { mesh->vertices.push_back(position); });
    return mesh;
}

void MeshData::Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec4 color) {
    const auto normal = (b - a).Cross(c - a).Normalized();
    const auto first = uint32(vertices.size());
    vertices.insert(vertices.end(), {{a, normal, {0, 0}, color, 1},
                                     {b, normal, {1, 0}, color, 1},
                                     {c, normal, {1, 1}, color},
                                     {d, normal, {0, 1}, color}});
    indices.insert(indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
}

void MeshData::Box(Vec3 center, Vec3 size, Vec4 color) {
    const Vec3 low = center - size * .5, high = center + size * .5;
    const Vec3 a{low.x, low.y, low.z}, b{high.x, low.y, low.z}, c{high.x, low.y, high.z},
        d{low.x, low.y, high.z}, e{low.x, high.y, low.z}, f{high.x, high.y, low.z},
        g{high.x, high.y, high.z}, h{low.x, high.y, high.z};
    Quad(a, b, c, d, color);
    Quad(e, h, g, f, color);
    Quad(a, e, f, b, color);
    Quad(b, f, g, c, color);
    Quad(c, g, h, d, color);
    Quad(d, h, e, a, color);
}

void MeshData::Upload(GpuDevice& device, Mesh3D& mesh) const {
    if (!indices.empty() && !mesh.Upload(&device, vertices, indices))
        throw std::runtime_error("Frontier County mesh upload failed: " + device.GetLastError());
}

uint64 MeshData::UploadBytes() const {
    return uint64(vertices.size()) * sizeof(RenderVertex) + uint64(indices.size()) * sizeof(uint32);
}

struct World::State {
    struct UploadingChunk {
        Draft draft;
        Loaded chunk;
        size_t nextMesh = 0;
    };

    unsigned seed = 0, vegetation = 0;
    int radius = 0;
    Stats stats;
    std::map<ChunkKey, Loaded> loaded;
    Mesh3D horizon;
    std::optional<ChunkKey> horizonAt;
    ChunkKey preparingHorizon{};
    std::future<MeshData> horizonTask;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<ChunkKey> requests;
    std::deque<Draft> ready;
    std::optional<ChunkKey> active;
    std::optional<UploadingChunk> uploading;
    std::exception_ptr workerError;
    bool stopping = false;
    std::thread worker;

    ~State() {
        if (!worker.joinable())
            return;
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }

    void Work() {
        try {
            std::unique_lock lock(mutex);
            for (;;) {
                wake.wait(lock,
                          [&] { return stopping || (!requests.empty() && ready.size() < 4); });
                if (stopping)
                    return;
                const auto key = requests.front();
                requests.pop_front();
                active = key;
                lock.unlock();
                auto draft = Generate(key, seed, vegetation);
                lock.lock();
                ready.push_back(std::move(draft));
                active.reset();
            }
        } catch (...) {
            const std::lock_guard lock(mutex);
            workerError = std::current_exception();
        }
    }

    unsigned AdvanceUpload(GpuDevice& device) {
        constexpr unsigned MeshBudget = 3;
        constexpr uint64 ByteBudget = 1024ull * 1024;
        auto& [draft, chunk, nextMesh] = *uploading;
        const std::array geometry{&draft.terrain, &draft.road, &draft.scenery};
        const std::array targets{&chunk.ground, &chunk.road, &chunk.scenery};
        unsigned meshes = 0;
        uint64 bytes = 0;
        for (; nextMesh < geometry.size() + draft.foliage.size(); ++nextMesh) {
            const bool baseMesh = nextMesh < geometry.size();
            const size_t cell = baseMesh ? 0 : nextMesh - geometry.size();
            auto& source = baseMesh ? *geometry[nextMesh] : draft.foliage[cell].cards;
            if (source.indices.empty())
                continue;
            const uint64 size = source.UploadBytes();
            if (meshes && (meshes >= MeshBudget || bytes + size > ByteBudget))
                return meshes;
            if (baseMesh) {
                source.Upload(device, *targets[nextMesh]);
            } else {
                auto& dst = chunk.foliage[cell];
                dst.cards = std::make_unique<Mesh3D>();
                source.Upload(device, *dst.cards);
                dst.plants = std::move(draft.foliage[cell].plants);
            }
            ++meshes;
            bytes += size;
            source = {};
        }
        chunk.obstacles = std::move(draft.obstacles);
        chunk.trees = draft.trees;
        loaded.try_emplace(draft.key, std::move(chunk));
        ++stats.built;
        stats.lastBuildMs = draft.buildMs;
        uploading.reset();
        return meshes;
    }
};

GroundPlants GroundCover(ChunkKey tile, unsigned seed) {
    GroundPlants instances;
    constexpr int Steps = 9;
    constexpr double spacing = double(CoverTileSize) / Steps;
    for (int j = 0; j < Steps; ++j) {
        for (int i = 0; i < Steps; ++i) {
            const auto hash =
                Hash(unsigned(tile.x * Steps + i), unsigned(tile.z * Steps + j), seed);
            const auto random = [&](unsigned k) { return Unit(Hash(hash, k, seed + 137)); };
            const Vec2 p{tile.x * CoverTileSize + (i + .08 + .84 * random(0)) * spacing,
                         tile.z * CoverTileSize + (j + .08 + .84 * random(1)) * spacing};
            if (std::abs(p.x) >= WorldHalf || std::abs(p.y) >= WorldHalf || RoadDistance(p) < 5.7)
                continue;
            const double h = Height(p, seed);
            const double shore = LakeRadius(p);
            if (h < LakeLevel + .07 || h > 94)
                continue;
            if (std::ranges::any_of(Sites, [&](const Structure& site) {
                    return std::abs(p.x - site.position.x) < site.size.x * .55 &&
                           std::abs(p.y - site.position.y) < site.size.z * .55;
                }))
                continue;
            const double density = .18 + .69 * Noise(p, 9, seed + 179);
            if (random(2) > density)
                continue;
            const bool sedge = shore < 1.07 && random(3) > .30;
            GroundSpecies species = sedge ? GroundSpecies::Sedge : GroundSpecies::Grass;
            const double size = (sedge ? .39 : .29) + .42 * random(4);
            Vec3 dimensions{size * 1.35, size, size * 1.28};
            const double moisture = Noise(p, 38, seed + 137) + (shore < 1.20 ? .24 : 0);
            const double variety = random(6), scale = .75 + .50 * random(7);
            if (moisture > .55 && variety < .005) {
                species = GroundSpecies::Deadwood;
                dimensions = Vec3{3.05, .30, .28} * scale;
            } else if (moisture > .48 && variety < .017) {
                species = GroundSpecies::Sapling;
                dimensions = Vec3{.54, 1.04, .73} * scale;
            } else if (moisture > .54 && variety < .14) {
                species = GroundSpecies::Fern;
                dimensions = Vec3{.60, .32, .64} * scale;
            } else if (!sedge && variety > .86 && Noise(p, 6, seed + 269) > .48) {
                species = GroundSpecies::Dandelion;
                dimensions = Vec3{.23, .17, .26} * scale;
            }
            double ground = h - (species == GroundSpecies::Dandelion ? .008 : .035);
            const double yaw = random(5) * 2 * std::numbers::pi;
            double tilt = 0;
            if (species == GroundSpecies::Deadwood) {
                Vec2 half{std::cos(yaw) * dimensions.x * .45, -std::sin(yaw) * dimensions.x * .45};
                const double a = Height(p - half, seed), b = Height(p + half, seed);
                tilt = std::atan2(b - a, dimensions.x * .90);
                ground = std::min(ground, (a + b) * .5 - .035);
            }
            instances[size_t(species)].push_back({{p.x, ground, p.y}, dimensions, yaw, 0, tilt});
        }
    }
    return instances;
}

World::World() = default;
World::~World() = default;

void World::Start(unsigned seed, unsigned vegetation, int radius, Vec2 initial, GpuDevice& device) {
    state_ = std::make_unique<State>();
    auto& s = *state_;
    s.seed = seed;
    s.vegetation = vegetation;
    s.radius = radius;
    s.uploading.emplace(State::UploadingChunk{Generate(KeyFor(initial), seed, vegetation), {}});
    while (s.uploading)
        s.AdvanceUpload(device);
    s.worker = std::thread([&s] { s.Work(); });
    Stream(initial, {}, device, false);
}

void World::Stream(Vec2 at, Vec2 velocity, GpuDevice& device, bool allowUploads) {
    auto& s = *state_;
    const ChunkKey center = KeyFor(at);
    if (s.horizonTask.valid() && allowUploads &&
        s.horizonTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto mesh = s.horizonTask.get();
        if (s.preparingHorizon == center) {
            mesh.Upload(device, s.horizon);
            s.horizonAt = center;
            allowUploads = false;
        }
    }
    if (s.horizonAt != center && !s.horizonTask.valid()) {
        s.preparingHorizon = center;
        s.horizonTask = std::async(std::launch::async, DistantTerrain, center, s.seed, s.radius);
    }
    const Vec2 predicted = at + velocity * 2.5;
    const auto predictedChunk = KeyFor(predicted);
    std::vector<ChunkKey> desired;
    for (int z = center.z - s.radius; z <= center.z + s.radius; ++z)
        for (int x = center.x - s.radius; x <= center.x + s.radius; ++x)
            if (x * ChunkSize >= -WorldHalf && x * ChunkSize < WorldHalf &&
                z * ChunkSize >= -WorldHalf && z * ChunkSize < WorldHalf)
                desired.push_back({x, z});
    const auto priority = [&](ChunkKey key) {
        return 2 * (std::abs(key.x - center.x) + std::abs(key.z - center.z)) +
               std::abs(key.x - predictedChunk.x) + std::abs(key.z - predictedChunk.z);
    };
    std::ranges::stable_sort(desired, {}, priority);
    auto contains = [&](ChunkKey key) { return std::ranges::find(desired, key) != desired.end(); };
    s.stats.evicted +=
        unsigned(std::erase_if(s.loaded, [&](const auto& item) { return !contains(item.first); }));
    if (s.uploading && !contains(s.uploading->draft.key))
        s.uploading.reset();
    {
        const std::lock_guard lock(s.mutex);
        if (s.workerError)
            std::rethrow_exception(s.workerError);
        std::erase_if(s.ready, [&](const Draft& draft) { return !contains(draft.key); });
        if (allowUploads && !s.uploading && !s.ready.empty()) {
            const auto next = std::ranges::min_element(
                s.ready, {}, [&](const Draft& draft) { return priority(draft.key); });
            s.uploading.emplace(State::UploadingChunk{std::move(*next), {}});
            s.ready.erase(next);
        }
        s.requests.clear();
        for (const auto key : desired) {
            const auto has = [key](const Draft& draft) { return draft.key == key; };
            if (!s.loaded.contains(key) && s.active != key &&
                (!s.uploading || s.uploading->draft.key != key) &&
                std::ranges::none_of(s.ready, has))
                s.requests.push_back(key);
        }
    }
    s.wake.notify_one();
    s.stats.uploadedMeshes = allowUploads && s.uploading ? s.AdvanceUpload(device) : 0;
    s.stats.loaded = unsigned(s.loaded.size());
    s.stats.pending = unsigned(desired.size()) - s.stats.loaded + unsigned(s.horizonAt != center);
    s.stats.trees = 0;
    for (const auto& [key, chunk] : s.loaded)
        s.stats.trees += chunk.trees;
}

void World::Draw(Renderer3D::Context& renderer, const Material3D& ground, const Material3D& scenery,
                 const Material3D& foliage, const Material3D& road, Vec3 eye,
                 std::vector<Plant>& nearTrees) const {
    if (state_->horizonAt && state_->horizon.IsValid()) {
        const auto origin = ChunkOrigin(*state_->horizonAt);
        renderer.DrawMesh(state_->horizon, Mat4::Translate({origin.x, 0, origin.y}), ground);
    }
    for (const auto& [key, chunk] : state_->loaded) {
        const auto origin = ChunkOrigin(key);
        const auto model = Mat4::Translate({origin.x, 0, origin.y});
        renderer.DrawMesh(chunk.ground, model, ground);
        if (chunk.road.IsValid())
            renderer.DrawMesh(chunk.road, model, road);
        if (chunk.scenery.IsValid())
            renderer.DrawMesh(chunk.scenery, model, scenery);
        for (unsigned i = 0; i < chunk.foliage.size(); ++i) {
            const auto& cell = chunk.foliage[i];
            if (!cell.cards)
                continue;
            const double x = key.x * ChunkSize + (i % FoliageCells + .5) * FoliageCellSize;
            const double z = key.z * ChunkSize + (i / FoliageCells + .5) * FoliageCellSize;
            const double dx = std::max(0., std::abs(eye.x - x) - FoliageCellSize * .5);
            const double dz = std::max(0., std::abs(eye.z - z) - FoliageCellSize * .5);
            const float t = float(std::clamp((std::hypot(dx, dz) - 24.) / 22., 0., 1.));
            const float fade = t * t * (3.f - 2.f * t);
            if (fade < 1.f) {
                for (auto tree : cell.plants) {
                    tree.lodTransition = fade;
                    nearTrees.push_back(tree);
                }
            }
            if (fade > 0.f) {
                auto distant = foliage;
                distant.lodTransition = fade < 1.f ? -fade : 0.f;
                renderer.DrawMesh(*cell.cards, model, distant);
            }
        }
    }
}

std::vector<Obstacle> World::NearbyObstacles(Vec2 at, double distance) const {
    std::vector<Obstacle> obstacles;
    for (const auto& [key, chunk] : state_->loaded)
        for (const auto& solid : chunk.obstacles)
            if (Distance(at, Horizontal(solid.center)) < distance)
                obstacles.push_back(solid);
    return obstacles;
}

bool World::HasChunk(ChunkKey key) const {
    return state_->loaded.contains(key);
}

World::Stats World::GetStats() const {
    return state_->stats;
}

void World::Stop() {
    state_.reset();
}
} // namespace Frontier
