#include <LuxEngine/Renderer/Lighting/TriangleRayScene.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace LuxEngine {
namespace {
constexpr uint32 LeafCapacity = 4;
constexpr size_t MaxTriangles = 1u << 20;
constexpr double MaxCoordinate = 1e15;
constexpr double MaxRadiance = 65504;

bool Finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool CoordinatesValid(Vec3 v) {
    return Finite(v) && std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}) <= MaxCoordinate;
}

bool InRange(double value, double low, double high) {
    return value >= low && value <= high;
}

bool ColorValid(Vec3 v, double maximum) {
    return InRange(v.x, 0, maximum) && InRange(v.y, 0, maximum) && InRange(v.z, 0, maximum);
}

double Axis(Vec3 v, int axis) {
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

Vec3 Min(Vec3 a, Vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vec3 Max(Vec3 a, Vec3 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

Vec3 Multiply(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

std::optional<Vec3> Unit(Vec3 v) {
    if (!Finite(v))
        return std::nullopt;
    const double scale = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    if (scale == 0)
        return std::nullopt;
    v = {v.x / scale, v.y / scale, v.z / scale};
    return v * (1 / std::sqrt(v.Dot(v)));
}

struct Bounds {
    Vec3 minimum{MaxCoordinate, MaxCoordinate, MaxCoordinate};
    Vec3 maximum{-MaxCoordinate, -MaxCoordinate, -MaxCoordinate};

    void Expand(const Bounds& other) {
        minimum = Min(minimum, other.minimum);
        maximum = Max(maximum, other.maximum);
    }

    std::optional<double> Intersect(const TriangleRay& ray, double far) const {
        double near = ray.minimumDistance;
        for (int axis = 0; axis < 3; ++axis) {
            const double start = Axis(ray.origin, axis);
            const double dir = Axis(ray.direction, axis);
            const double low = Axis(minimum, axis), high = Axis(maximum, axis);
            if (dir == 0) {
                if (start < low || start > high)
                    return std::nullopt;
                continue;
            }
            double a = (low - start) / dir, b = (high - start) / dir;
            if (a > b)
                std::swap(a, b);
            near = std::max(near, a);
            far = std::min(far, b);
            if (near > far)
                return std::nullopt;
        }
        return near;
    }
};

struct Primitive {
    Bounds bounds;
    Vec3 center;
    Vec3 normal;
    uint32 index;
};

struct Node {
    Bounds bounds;
    uint32 first; // Primitive offset in a leaf, first child otherwise.
    uint32 count;
    uint32 right = 0;
};

struct RayProjection {
    int x, y, z;
    double sx, sy, sz;

    explicit RayProjection(Vec3 direction) {
        z = 0;
        if (std::abs(direction.y) > std::abs(direction.x))
            z = 1;
        if (std::abs(direction.z) > std::abs(Axis(direction, z)))
            z = 2;
        x = (z + 1) % 3;
        y = (z + 2) % 3;
        if (Axis(direction, z) < 0)
            std::swap(x, y);
        sz = 1 / Axis(direction, z);
        sx = Axis(direction, x) * sz;
        sy = Axis(direction, y) * sz;
    }

    Vec3 Project(Vec3 p) const {
        const double depth = Axis(p, z);
        return {Axis(p, x) - depth * sx, Axis(p, y) - depth * sy, depth * sz};
    }
};

std::optional<TriangleRay> NormalizeRay(const TriangleRay& ray) {
    if (!CoordinatesValid(ray.origin) || !std::isfinite(ray.minimumDistance) ||
        ray.minimumDistance < 0 || !(ray.maximumDistance >= ray.minimumDistance))
        return std::nullopt;
    const auto direction = Unit(ray.direction);
    if (!direction)
        return std::nullopt;
    return TriangleRay{ray.origin, *direction, ray.minimumDistance, ray.maximumDistance};
}

std::optional<TriangleRayHit> IntersectTriangle(const TriangleRay& ray,
                                                const RayProjection& projection,
                                                const RayTriangle& triangle,
                                                const Primitive& primitive, double far) {
    // Measure the three edge functions in a coordinate system where the ray is the Z axis.
    const Vec3 a = projection.Project(triangle.a - ray.origin);
    const Vec3 b = projection.Project(triangle.b - ray.origin);
    const Vec3 c = projection.Project(triangle.c - ray.origin);
    // Keep product/subtraction ordering symmetric for a shared edge read in reverse.
    const double u = b.x * c.y - b.y * c.x;
    const double v = c.x * a.y - c.y * a.x;
    const double w = a.x * b.y - a.y * b.x;
    if (std::min({u, v, w}) < 0 && std::max({u, v, w}) > 0)
        return std::nullopt;
    const double determinant = u + v + w;
    if (determinant == 0)
        return std::nullopt;
    const double t = (u * a.z + v * b.z + w * c.z) / determinant;
    if (!std::isfinite(t) || t < ray.minimumDistance || t > far)
        return std::nullopt;
    const bool front = ray.direction.Dot(primitive.normal) < 0;
    return TriangleRayHit{t,
                          ray.origin + ray.direction * t,
                          front ? primitive.normal : primitive.normal * -1,
                          {u / determinant, v / determinant, w / determinant},
                          primitive.index,
                          triangle.material,
                          front};
}

TriangleRay SpawnRay(const TriangleRayHit& hit, Vec3 outgoing) {
    const double side = outgoing.Dot(hit.normal) >= 0 ? 1 : -1;
    const double magnitude =
        std::max({std::abs(hit.position.x), std::abs(hit.position.y), std::abs(hit.position.z)});
    const double bias = std::max(1e-6, magnitude * std::numeric_limits<double>::epsilon() * 4);
    return {hit.position + hit.normal * (side * bias), outgoing};
}
} // namespace

struct TriangleRayScene::Data {
    std::vector<RayTriangle> triangles;
    std::vector<RaySurface> materials;
    std::vector<Primitive> primitives;
    std::vector<Node> nodes;

    uint32 BuildNode(uint32 first, uint32 count) {
        Bounds bounds, centers;
        for (uint32 i = first; i < first + count; ++i) {
            bounds.Expand(primitives[i].bounds);
            centers.Expand({primitives[i].center, primitives[i].center});
        }
        const auto index = static_cast<uint32>(nodes.size());
        nodes.push_back({bounds, first, count});
        if (count <= LeafCapacity)
            return index;
        const Vec3 extent = centers.maximum - centers.minimum;
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0
                         : extent.y >= extent.z                       ? 1
                                                                      : 2;
        const uint32 leftCount = count / 2;
        auto begin = primitives.begin() + first;
        std::nth_element(begin, begin + leftCount, begin + count,
                         [axis](const Primitive& a, const Primitive& b) {
                             const double x = Axis(a.center, axis), y = Axis(b.center, axis);
                             return x == y ? a.index < b.index : x < y;
                         });
        const uint32 left = BuildNode(first, leftCount);
        const uint32 right = BuildNode(first + leftCount, count - leftCount);
        nodes[index] = {bounds, left, 0, right};
        return index;
    }

    std::optional<TriangleRayHit> Cast(const TriangleRay& ray, bool any) const {
        if (nodes.empty())
            return std::nullopt;
        const auto rootNear = nodes.front().bounds.Intersect(ray, ray.maximumDistance);
        if (!rootNear)
            return std::nullopt;
        const RayProjection projection(ray.direction);

        struct Candidate {
            uint32 node;
            double near;
        };

        // Median splits bound the traversal depth by log2(MaxTriangles).
        std::array<Candidate, 32> stack;
        stack[0] = {0, *rootNear};
        size_t size = 1;
        double far = ray.maximumDistance;
        std::optional<TriangleRayHit> closest;
        while (size) {
            const auto [index, near] = stack[--size];
            if (near > far)
                continue;
            const Node& node = nodes[index];
            if (node.count) {
                for (uint32 i = node.first; i < node.first + node.count; ++i) {
                    const Primitive& p = primitives[i];
                    auto hit = IntersectTriangle(ray, projection, triangles[p.index], p, far);
                    if (!hit)
                        continue;
                    if (any)
                        return hit;
                    if (closest && hit->distance == far && hit->triangle > closest->triangle)
                        continue;
                    far = hit->distance;
                    closest = hit;
                }
            } else {
                const auto left = nodes[node.first].bounds.Intersect(ray, far);
                const auto right = nodes[node.right].bounds.Intersect(ray, far);
                if (left && right && *right < *left) {
                    stack[size++] = {node.first, *left};
                    stack[size++] = {node.right, *right};
                } else {
                    if (right)
                        stack[size++] = {node.right, *right};
                    if (left)
                        stack[size++] = {node.first, *left};
                }
            }
        }
        return closest;
    }

    Vec3 Trace(const TriangleRay& ray, const RayLighting& lighting, Vec3 toLight,
               uint32 bounces) const {
        const auto hit = Cast(ray, false);
        if (!hit)
            return lighting.environmentRadiance;
        const RaySurface& surface = materials[hit->material];
        const Vec3 normal = hit->normal;
        const double cosine = std::clamp(-normal.Dot(ray.direction), 0.0, 1.0);
        const double ratio = (surface.indexOfRefraction - 1) / (surface.indexOfRefraction + 1);
        const double baseReflectance = ratio * ratio;
        const Vec3 base =
            Vec3{baseReflectance, baseReflectance, baseReflectance} * (1 - surface.metallic) +
            surface.albedo * surface.metallic;
        const Vec3 one{1, 1, 1};
        const double eta =
            hit->frontFace ? 1 / surface.indexOfRefraction : surface.indexOfRefraction;
        const double transmittedCosineSquared = 1 - eta * eta * (1 - cosine * cosine);
        const bool internalReflection = surface.transmission > 0 && transmittedCosineSquared <= 0;
        const Vec3 fresnel =
            internalReflection ? one : base + (one - base) * std::pow(1 - cosine, 5);

        Vec3 result = surface.emission;
        const double lambert = normal.Dot(toLight) / std::numbers::pi;
        const double diffuse = (1 - surface.metallic) * (1 - surface.transmission);
        const auto irradiance = lighting.directionalIrradiance;
        if (diffuse > 0 && lambert > 0 && irradiance.x + irradiance.y + irradiance.z > 0 &&
            !Cast(SpawnRay(*hit, toLight), true))
            result +=
                Multiply(Multiply(surface.albedo, one - fresnel), irradiance) * (lambert * diffuse);
        if (!bounces)
            return result;

        const Vec3 reflected = ray.direction + normal * (2 * cosine);
        result +=
            Multiply(fresnel, Trace(SpawnRay(*hit, reflected), lighting, toLight, bounces - 1));
        if (surface.transmission > 0 && surface.metallic < 1 && !internalReflection) {
            const Vec3 refracted =
                ray.direction * eta + normal * (eta * cosine - std::sqrt(transmittedCosineSquared));
            result += Multiply(Multiply(one - fresnel, surface.albedo),
                               Trace(SpawnRay(*hit, refracted), lighting, toLight, bounces - 1)) *
                      (surface.transmission * (1 - surface.metallic));
        }
        return result;
    }
};

TriangleRayScene::TriangleRayScene() = default;
TriangleRayScene::~TriangleRayScene() = default;
TriangleRayScene::TriangleRayScene(TriangleRayScene&&) noexcept = default;
TriangleRayScene& TriangleRayScene::operator=(TriangleRayScene&&) noexcept = default;

bool TriangleRayScene::Build(std::span<const RayTriangle> triangles,
                             std::span<const RaySurface> materials) {
    if (triangles.size() > MaxTriangles || materials.size() > MaxTriangles)
        return false;
    auto next = std::make_unique<Data>();
    if (materials.empty())
        next->materials.emplace_back();
    else
        next->materials.assign(materials.begin(), materials.end());
    for (const auto& m : next->materials)
        if (!ColorValid(m.albedo, 1) || !ColorValid(m.emission, MaxRadiance) ||
            !InRange(m.metallic, 0, 1) || !InRange(m.transmission, 0, 1) ||
            !InRange(m.indexOfRefraction, 1, 4))
            return false;
    next->triangles.assign(triangles.begin(), triangles.end());
    next->primitives.reserve(triangles.size());
    for (size_t i = 0; i < triangles.size(); ++i) {
        const auto& t = triangles[i];
        if (!CoordinatesValid(t.a) || !CoordinatesValid(t.b) || !CoordinatesValid(t.c) ||
            t.material >= next->materials.size())
            return false;
        if (const auto normal = Unit((t.b - t.a).Cross(t.c - t.a))) {
            Bounds bounds{Min(Min(t.a, t.b), t.c), Max(Max(t.a, t.b), t.c)};
            const Vec3 center = (bounds.minimum + bounds.maximum) * .5;
            next->primitives.push_back({bounds, center, *normal, static_cast<uint32>(i)});
        }
    }
    if (!next->primitives.empty())
        next->BuildNode(0, static_cast<uint32>(next->primitives.size()));
    data_ = std::move(next);
    return true;
}

std::optional<TriangleRayHit> TriangleRayScene::ClosestHit(const TriangleRay& ray) const {
    const auto normalized = NormalizeRay(ray);
    return data_ && normalized ? data_->Cast(*normalized, false) : std::nullopt;
}

bool TriangleRayScene::Occluded(const TriangleRay& ray) const {
    const auto normalized = NormalizeRay(ray);
    return data_ && normalized && data_->Cast(*normalized, true).has_value();
}

Vec3 TriangleRayScene::TraceRadiance(const TriangleRay& ray, const RayLighting& lighting) const {
    const auto normalized = NormalizeRay(ray);
    const auto toLight = Unit(lighting.directionToLight);
    if (!normalized || !toLight || !ColorValid(lighting.directionalIrradiance, MaxRadiance) ||
        !ColorValid(lighting.environmentRadiance, MaxRadiance))
        return {};
    return data_ ? data_->Trace(*normalized, lighting, *toLight, std::min(lighting.maxBounces, 8u))
                 : lighting.environmentRadiance;
}

} // namespace LuxEngine
