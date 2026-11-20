#include <LuxEngine/Assets/ModelAsset.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace LuxEngine {
namespace {
Mat4 Matrix(const ModelTransform& transform) {
    return Mat4::Translate(transform.position) * Mat4::RotateZ(transform.rotation.z) *
           Mat4::RotateY(transform.rotation.y) * Mat4::RotateX(transform.rotation.x) *
           Mat4::Scale(transform.scale);
}

Vec3 Normal(const Mat4& matrix, Vec3 normal) {
    Mat4 inverse;
    try {
        inverse = matrix.InverseAffine();
    } catch (const std::invalid_argument&) {
        const auto value = matrix.TransformDirection(normal).Normalized();
        return value.Dot(value) > 1e-16 ? value : normal;
    }
    const auto* m = inverse.elements;
    return Vec3{m[0] * normal.x + m[1] * normal.y + m[2] * normal.z,
                m[4] * normal.x + m[5] * normal.y + m[6] * normal.z,
                m[8] * normal.x + m[9] * normal.y + m[10] * normal.z}
        .Normalized();
}

Mat4 Skin(const ModelSubmesh& mesh, const ModelVertex& vertex, const std::vector<Mat4>& worlds) {
    if (vertex.weights.empty())
        return Mat4{};
    const auto inverse = worlds.at(mesh.node).InverseAffine();
    Mat4 result;
    std::fill(std::begin(result.elements), std::end(result.elements), 0);
    double sum = 0;
    for (const auto& weight : vertex.weights) {
        if (weight.weight <= 0)
            continue;
        const auto& bone = mesh.bones.at(weight.bone);
        const auto matrix = inverse * worlds.at(bone.node) * bone.inverseBind;
        for (size_t i = 0; i < 16; ++i)
            result.elements[i] += matrix.elements[i] * weight.weight;
        sum += weight.weight;
    }
    if (sum <= 0)
        return Mat4{};
    for (auto& value : result.elements)
        value /= sum;
    return result;
}

} // namespace

bool ModelAsset::NodeMatchesMask(size_t node, const std::vector<std::string>& mask) const {
    if (mask.empty())
        return true;
    std::string path = nodes[node].name;
    auto parent = nodes[node].parent;
    while (parent >= 0) {
        path = nodes[parent].name + "/" + path;
        parent = nodes[parent].parent;
    }
    return std::any_of(mask.begin(), mask.end(), [&](const auto& prefix) {
        return nodes[node].name == prefix || path == prefix || path.starts_with(prefix + "/") ||
               path.ends_with("/" + prefix) || path.find("/" + prefix + "/") != std::string::npos;
    });
}

bool ModelAsset::MixPoses(const std::vector<ModelPoseLayer>& layers, ModelPose& output,
                          std::string& error,
                          const std::vector<ModelPoseOverride>& overrides) const {
    try {
        ModelPose result;
        if (!Evaluate(0, -1, result, error))
            return false;
        std::map<size_t, std::vector<const ModelPoseLayer*>> groups;
        for (const auto& layer : layers) {
            if (!layer.pose || layer.pose->nodeLocalTransforms.size() != nodes.size() ||
                layer.pose->submeshes.size() != submeshes.size() || !std::isfinite(layer.weight) ||
                layer.weight < 0)
                throw std::runtime_error("호환되지 않는 애니메이션 포즈입니다.");
            if (layer.weight > 0)
                groups[layer.layer].push_back(&layer);
        }
        for (const auto& [id, group] : groups) {
            static_cast<void>(id);
            for (size_t n = 0; n < nodes.size(); ++n) {
                auto& destination = result.nodeLocalTransforms[n];
                ModelTransform value = destination;
                double total = 0;
                for (const auto* layer : group) {
                    if (!NodeMatchesMask(n, layer->mask))
                        continue;
                    const auto& source = layer->pose->nodeLocalTransforms[n];
                    const double t = layer->weight / (total + layer->weight);
                    if (total == 0)
                        value = source;
                    else {
                        value.position = value.position + (source.position - value.position) * t;
                        value.rotation = SlerpRotation(value.rotation, source.rotation, t);
                        value.scale = value.scale + (source.scale - value.scale) * t;
                    }
                    total += layer->weight;
                }
                const double weight = std::clamp(total, 0.0, 1.0);
                if (total == 0)
                    continue;
                if (group.front()->additive) {
                    const auto& rest = nodes[n].localTransform;
                    destination.position += (value.position - rest.position) * weight;
                    destination.rotation = AdditiveRotation(destination.rotation, rest.rotation,
                                                            value.rotation, weight);
                    destination.scale = destination.scale + (value.scale - rest.scale) * weight;
                } else {
                    destination.position += (value.position - destination.position) * weight;
                    destination.rotation =
                        SlerpRotation(destination.rotation, value.rotation, weight);
                    destination.scale += (value.scale - destination.scale) * weight;
                }
                destination.matrix = Matrix(destination);
            }
        }
        for (const auto& override : overrides) {
            auto& transform = result.nodeLocalTransforms.at(override.node);
            if (override.position)
                transform.position = *override.position;
            if (override.rotation)
                transform.rotation = *override.rotation;
            if (override.scale)
                transform.scale = *override.scale;
            transform.matrix = Matrix(transform);
        }
        // FBX node arrays need not be in parent-first order.
        std::vector<uint8> visited(nodes.size());
        auto world = [&](auto&& self, size_t node) -> void {
            if (visited[node] == 2)
                return;
            if (visited[node] == 1)
                throw std::runtime_error("애니메이션 본 계층에 순환이 있습니다.");
            visited[node] = 1;
            const int32 parent = nodes[node].parent;
            if (parent >= 0)
                self(self, static_cast<size_t>(parent));
            result.nodeWorldTransforms[node] =
                (parent < 0 ? Mat4{} : result.nodeWorldTransforms[parent]) *
                result.nodeLocalTransforms[node].matrix;
            visited[node] = 2;
        };
        for (size_t n = 0; n < nodes.size(); ++n)
            world(world, n);
        for (size_t meshIndex = 0; meshIndex < submeshes.size(); ++meshIndex) {
            const auto& mesh = submeshes[meshIndex];
            auto& vertices = result.submeshes[meshIndex];
            for (size_t vertex = 0; vertex < vertices.size(); ++vertex) {
                // Undo each source skin before mixing morph/rest coordinates, then apply the
                // blended skeleton once. Vertex-space crossfades would collapse rotating limbs.
                Vec3 rest = mesh.vertices[vertex].position, normal = mesh.vertices[vertex].normal;
                for (const auto& [id, group] : groups) {
                    static_cast<void>(id);
                    Vec3 position{}, direction{};
                    double total = 0;
                    for (const auto* layer : group) {
                        if (!NodeMatchesMask(mesh.node, layer->mask))
                            continue;
                        const auto& source = layer->pose->submeshes[meshIndex].at(vertex);
                        const auto skin = Skin(mesh, source, layer->pose->nodeWorldTransforms);
                        Vec3 input;
                        if (layer->pose->deformationPositions.size() == submeshes.size())
                            input = layer->pose->deformationPositions[meshIndex].at(vertex);
                        else
                            input = skin.InverseAffine().TransformPoint(source.position);
                        position += input * layer->weight;
                        const auto* m = skin.elements;
                        auto unskinned = Vec3{m[0] * source.normal.x + m[1] * source.normal.y +
                                                  m[2] * source.normal.z,
                                              m[4] * source.normal.x + m[5] * source.normal.y +
                                                  m[6] * source.normal.z,
                                              m[8] * source.normal.x + m[9] * source.normal.y +
                                                  m[10] * source.normal.z}
                                             .Normalized();
                        direction += unskinned * layer->weight;
                        total += layer->weight;
                    }
                    if (total > 0) {
                        const double weight = std::clamp(total, 0.0, 1.0);
                        rest += (position * (1 / total) - rest) * weight;
                        normal =
                            (normal + (direction * (1 / total) - normal) * weight).Normalized();
                    }
                }
                result.deformationPositions[meshIndex][vertex] = rest;
                const auto skin = Skin(mesh, mesh.vertices[vertex], result.nodeWorldTransforms);
                vertices[vertex].position = skin.TransformPoint(rest);
                vertices[vertex].normal = Normal(skin, normal);
            }
        }
        output = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}
} // namespace LuxEngine
