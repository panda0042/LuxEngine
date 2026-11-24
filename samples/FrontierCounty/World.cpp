#include "World.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
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
constexpr unsigned Cells = 32;

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
double Relief(Vec2 p, unsigned seed) {
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
    const double bed = LakeLevel + (shore - .82) * (shore < .82 ? 10. : 23.);
    return Blend(bed, y, Curve(.96, 1.45, shore));
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
        const Vec3 normal{-side.z, .27, side.x};
        const Vec3 edge = side * (height * .28125);
        const Vec3 up{0, height, 0};
        const auto a = uint32(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(),
                             {{base - edge, (normal - side * .47).Normalized(), {0, 1}, color},
                              {base - edge + up, (normal - side * .38).Normalized(), {0, 0}, color},
                              {base + edge + up, (normal + side * .38).Normalized(), {1, 0}, color},
                              {base + edge, (normal + side * .47).Normalized(), {1, 1}, color}});
        mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + 2, a, a + 2, a + 3});
    }
}

double Cross2(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}

void AddRoadSurface(MeshData& mesh, const std::array<Vec2, 4>& ribbon, Vec2 origin, unsigned seed,
                    double bias, Vec4 color) {
    constexpr double step = double(ChunkSize) / Cells;
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

Draft Generate(ChunkKey key, unsigned seed, unsigned vegetation) {
    const auto start = std::chrono::steady_clock::now();
    Draft result;
    result.key = key;
    const Vec2 origin{double(key.x * ChunkSize), double(key.z * ChunkSize)};
    const unsigned stride = Cells + 1;
    auto& ground = result.terrain;
    ground.vertices.reserve(stride * stride);
    ground.indices.reserve(Cells * Cells * 6);
    for (unsigned z = 0; z <= Cells; ++z) {
        for (unsigned x = 0; x <= Cells; ++x) {
            const Vec2 p =
                origin + Vec2{double(x * ChunkSize / Cells), double(z * ChunkSize / Cells)};
            const double y = Height(p, seed);
            const auto normal =
                Vec3{AnalyticHeight(p - Vec2{.6, 0}, seed) - AnalyticHeight(p + Vec2{.6, 0}, seed),
                     1.2,
                     AnalyticHeight(p - Vec2{0, .6}, seed) - AnalyticHeight(p + Vec2{0, .6}, seed)}
                    .Normalized();
            const double moisture = .62 * Noise(p, 61, seed + 31) + .38 * Noise(p, 16, seed + 55);
            const double slope = 1 - normal.y;
            const double bank = Curve(.90, 1.16, LakeRadius(p));
            const double vegetationWeight = bank * (1 - Curve(.08, .31, slope)) *
                                            Curve(.20, .60, moisture) * (1 - Curve(70, 105, y)) *
                                            Curve(5, 13, RoadDistance(p));
            const float tone = float(.83 + .19 * moisture);
            ground.vertices.push_back({{p.x - origin.x, y, p.y - origin.y},
                                       normal,
                                       {p.x * .5, p.y * .5},
                                       {tone, tone, tone, float(vegetationWeight)}});
            if (x < Cells && z < Cells) {
                const auto a = z * stride + x, b = a + stride;
                ground.indices.insert(ground.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
            }
        }
    }
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
            patch(-4.05, 4.05, .012, {.88f, .88f, .87f, 1}, result.road);
            if (i % 2 == 0)
                patch(-.065, .065, .018, {.49f, .33f, .11f, 1}, result.scenery);
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
        const double moisture = Noise(point, 145, seed + 81);
        const double height = Height(point, seed);
        if (a > (.24 + .72 * moisture) * (1 - Curve(67, 105, height)))
            continue;
        if (std::ranges::any_of(shoreTrees,
                                [&](Vec3 tree) { return Distance(Horizontal(tree), point) < 4; }))
            continue;
        addTree(point, 5.2 + 11.8 * a, Unit(Hash(base, 3, seed)));
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
    constexpr double step = double(ChunkSize) / Cells;
    const Vec2 base{step * std::floor(point.x / step), step * std::floor(point.y / step)};
    const double u = (point.x - base.x) / step, v = (point.y - base.y) / step;
    const double a = AnalyticHeight(base, seed);
    if (u == 0 && v == 0)
        return a;
    const double right = AnalyticHeight(base + Vec2{step, 0}, seed);
    const double bottom = AnalyticHeight(base + Vec2{0, step}, seed);
    // Match the diagonal used by Generate so wheels, feet and terrain agree.
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

void MeshData::Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec4 color) {
    const auto normal = (b - a).Cross(c - a).Normalized();
    const auto first = uint32(vertices.size());
    vertices.insert(vertices.end(), {{a, normal, {0, 0}, color},
                                     {b, normal, {1, 0}, color},
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

struct World::State {
    unsigned seed = 0, vegetation = 0;
    int radius = 0;
    Stats stats;
    std::map<ChunkKey, Loaded> loaded;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<ChunkKey> requests;
    std::deque<Draft> ready;
    std::optional<ChunkKey> active;
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

    void Commit(Draft&& draft, GpuDevice& device) {
        auto [it, inserted] = loaded.try_emplace(draft.key);
        if (!inserted)
            return;
        auto& chunk = it->second;
        draft.terrain.Upload(device, chunk.ground);
        draft.road.Upload(device, chunk.road);
        draft.scenery.Upload(device, chunk.scenery);
        for (size_t i = 0; i < draft.foliage.size(); ++i) {
            auto& src = draft.foliage[i];
            if (src.plants.empty())
                continue;
            auto& dst = chunk.foliage[i];
            dst.cards = std::make_unique<Mesh3D>();
            src.cards.Upload(device, *dst.cards);
            dst.plants = std::move(src.plants);
        }
        chunk.obstacles = std::move(draft.obstacles);
        chunk.trees = draft.trees;
        ++stats.built;
        stats.lastBuildMs = draft.buildMs;
    }
};

std::array<std::vector<Plant>, 2> GroundCover(ChunkKey tile, unsigned seed) {
    std::array<std::vector<Plant>, 2> instances;
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
            const size_t species = shore < 1.07 && random(3) > .30 ? 1 : 0;
            const double size = (species ? .39 : .29) + .42 * random(4);
            instances[species].push_back({{p.x, h - .035, p.y},
                                          {size * 1.35, size, size * 1.28},
                                          random(5) * 2 * std::numbers::pi});
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
    s.Commit(Generate(KeyFor(initial), seed, vegetation), device);
    s.worker = std::thread([&s] { s.Work(); });
    Stream(initial, {}, device);
}

void World::Stream(Vec2 at, Vec2 velocity, GpuDevice& device) {
    auto& s = *state_;
    const ChunkKey center = KeyFor(at);
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
    std::deque<Draft> upload;
    {
        const std::lock_guard lock(s.mutex);
        if (s.workerError)
            std::rethrow_exception(s.workerError);
        std::erase_if(s.ready, [&](const Draft& draft) { return !contains(draft.key); });
        while (upload.size() < 2 && !s.ready.empty()) {
            upload.push_back(std::move(s.ready.front()));
            s.ready.pop_front();
        }
        s.requests.clear();
        for (const auto key : desired) {
            const auto has = [key](const Draft& draft) { return draft.key == key; };
            if (!s.loaded.contains(key) && s.active != key && std::ranges::none_of(s.ready, has) &&
                std::ranges::none_of(upload, has))
                s.requests.push_back(key);
        }
        s.stats.pending =
            unsigned(s.requests.size() + s.ready.size() + size_t(s.active.has_value()));
    }
    s.wake.notify_one();
    for (auto& draft : upload)
        s.Commit(std::move(draft), device);
    s.stats.loaded = unsigned(s.loaded.size());
    s.stats.trees = 0;
    for (const auto& [key, chunk] : s.loaded)
        s.stats.trees += chunk.trees;
}

void World::Draw(Renderer3D::Context& renderer, const Material3D& ground, const Material3D& scenery,
                 const Material3D& foliage, const Material3D& road, Vec3 eye,
                 std::vector<Plant>& nearTrees) const {
    for (const auto& [key, chunk] : state_->loaded) {
        const auto model =
            Mat4::Translate({double(key.x * ChunkSize), 0, double(key.z * ChunkSize)});
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
            if (dx * dx + dz * dz < 26. * 26.)
                nearTrees.insert(nearTrees.end(), cell.plants.begin(), cell.plants.end());
            else
                renderer.DrawMesh(*cell.cards, model, foliage);
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
