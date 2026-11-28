#include <LuxEngine/Renderer/Renderer3D/Decal.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace LuxEngine {
namespace {
struct ClipVertex {
    Vec3 position, normal;
    std::array<double, 3> coordinate;
};

bool Finite(Vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// A triangle clipped by six planes has at most nine vertices.
using Polygon = std::array<ClipVertex, 10>;

size_t Clip(const Polygon& input, size_t count, Polygon& output, size_t axis, double sign,
            double halfSize) {
    size_t result = 0;
    auto previous = input[count - 1];
    double prev = halfSize - sign * previous.coordinate[axis];
    for (size_t i = 0; i < count; ++i) {
        const auto& current = input[i];
        const double distance = halfSize - sign * current.coordinate[axis];
        if ((distance < 0 && prev > 0) || (distance > 0 && prev < 0)) {
            const double t = prev / (prev - distance);
            auto& v = output[result++];
            v.position = previous.position + (current.position - previous.position) * t;
            v.normal = previous.normal + (current.normal - previous.normal) * t;
            for (size_t j = 0; j < 3; ++j)
                v.coordinate[j] = std::lerp(previous.coordinate[j], current.coordinate[j], t);
        }
        if (distance >= 0)
            output[result++] = current;
        previous = current;
        prev = distance;
    }
    return result;
}
} // namespace

bool AppendProjectedDecal(const DecalProjector& p, std::span<const Vertex3D> receiver,
                          std::span<const uint32> triangles, std::vector<Vertex3D>& vertices,
                          std::vector<uint32>& indices) {
    if (!Finite(p.center) || !Finite(p.normal) || !Finite(p.tangent) || !Finite(p.halfSize) ||
        std::min({p.halfSize.x, p.halfSize.y, p.halfSize.z}) <= 0 ||
        !std::isfinite(p.normalOffset) || p.normalOffset < 0 || !std::isfinite(p.minimumFacing) ||
        std::abs(p.minimumFacing) > 1 || !std::isfinite(p.tint.x) || !std::isfinite(p.tint.y) ||
        !std::isfinite(p.tint.z) || !std::isfinite(p.tint.w) || triangles.size() % 3 != 0)
        return false;
    const auto normal = p.normal.Normalized();
    const auto u = (p.tangent - normal * p.tangent.Dot(normal)).Normalized();
    if (normal.Dot(normal) < .99 || u.Dot(u) < .99)
        return false;
    const auto v = normal.Cross(u);
    for (auto index : triangles)
        if (index >= receiver.size() || !Finite(receiver[index].position) ||
            !Finite(receiver[index].normal))
            return false;
    const auto startVertices = vertices.size(), startIndices = indices.size();
    const std::array half{p.halfSize.x, p.halfSize.y, p.halfSize.z};
    for (size_t i = 0; i < triangles.size(); i += 3) {
        Polygon polygon, scratch;
        for (size_t j = 0; j < 3; ++j) {
            const auto& source = receiver[triangles[i + j]];
            const auto relative = source.position - p.center;
            polygon[j] = {source.position,
                          source.normal,
                          {relative.Dot(u), relative.Dot(v), relative.Dot(normal)}};
        }
        const auto face = (polygon[1].position - polygon[0].position)
                              .Cross(polygon[2].position - polygon[0].position)
                              .Normalized();
        if (face.Dot(face) < .99 || face.Dot(normal) < p.minimumFacing)
            continue;
        size_t count = 3;
        for (size_t axis = 0; axis < 3 && count >= 3; ++axis) {
            count = Clip(polygon, count, scratch, axis, 1, half[axis]);
            if (count >= 3)
                count = Clip(scratch, count, polygon, axis, -1, half[axis]);
        }
        if (count < 3)
            continue;
        if (vertices.size() > std::numeric_limits<uint32>::max() - count) {
            vertices.resize(startVertices);
            indices.resize(startIndices);
            return false;
        }
        const auto base = static_cast<uint32>(vertices.size());
        for (size_t j = 0; j < count; ++j) {
            const auto& point = polygon[j];
            auto n = point.normal.Normalized();
            if (n.Dot(n) < .99)
                n = face;
            vertices.push_back(
                {point.position + n * p.normalOffset,
                 n,
                 {.5 + .5 * point.coordinate[0] / half[0], .5 - .5 * point.coordinate[1] / half[1]},
                 p.tint});
        }
        for (uint32 j = 1; j + 1 < count; ++j)
            indices.insert(indices.end(), {base, base + j, base + j + 1});
    }
    return true;
}
} // namespace LuxEngine
