#include "LuxRayTracing.hpp"

#if defined(__linux__) || defined(_WIN32)
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include "LuxForgeApi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

// clang-format off
#include "Graphics/GraphicsConfig.h"
#include "Graphics/Interfaces/IGraphics.h"
#include "Graphics/Interfaces/IRay.h"
#include "Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "Resources/ResourceLoader/ThirdParty/OpenSource/tinyimageformat/tinyimageformat_base.h"
// clang-format on

namespace {
using Float3 = std::array<float, 3>;
using Float4 = std::array<float, 4>;
using InstanceData = std::array<Float4, 5>;
constexpr uint32_t MaxTriangles = 2'000'000;
constexpr uint32_t MaxInstances = 16'384;
constexpr uint32_t VertexStride = sizeof(LuxForgeVertex);
constexpr uint32_t PositionBytes = sizeof(Float3);
constexpr uint32_t ColorOffset = offsetof(LuxForgeVertex, color);
static_assert(offsetof(LuxForgeVertex, position) == 0 && PositionBytes == 12);

struct MeshKey {
    Buffer* vertices;
    Buffer* indices;
    uint32_t indexCount;

    bool operator==(const MeshKey& other) const {
        return vertices == other.vertices && indices == other.indices &&
               indexCount == other.indexCount;
    }
};

struct MeshKeyHash {
    size_t operator()(const MeshKey& key) const {
        const auto vertices = std::hash<Buffer*>{}(key.vertices);
        const auto indices = std::hash<Buffer*>{}(key.indices);
        return vertices ^ (indices >> 4) ^ key.indexCount;
    }
};

struct SourceVertex {
    Float3 position;
    Float4 color{1, 1, 1, 0};
};

struct SourceBuffer {
    std::vector<SourceVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshKey> meshes;
};

struct RayMesh {
    AccelerationStructure* acceleration = nullptr;
    uint32_t firstTriangle = 0;
    uint32_t count = 0;
    uint64_t buildFrame = 0;
    float minimumAlpha = 0;
};

struct FrameScene {
    AccelerationStructure* acceleration;
    Buffer* instances;
    uint64_t frame;
};

struct RetiredAcceleration {
    AccelerationStructure* acceleration;
    uint64_t frame;
};

struct RetiredBuffer {
    Buffer* buffer;
    uint64_t frame;
};

bool PassesMask(float alpha, const LuxForgeSceneDraw& draw) {
    // A white base-color map cannot reject interpolation of indexed vertex alpha
    // when the smallest vertex passes the same mask cutoff as the raster shader.
    return draw.alphaMode != 1 || draw.water[3] > 1 ||
           (!draw.texture && std::isfinite(alpha) && draw.albedo[3] >= 0 &&
            alpha * draw.albedo[3] >= draw.alphaCutoff);
}

Float3 Position(const LuxForgeSceneDraw& draw, int column) {
    return {draw.model[column * 4], draw.model[column * 4 + 1], draw.model[column * 4 + 2]};
}

Float3 Cross(const Float3& a, const Float3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

Float3 Subtract(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float Dot(const Float3& a, const Float3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool NormalMatrix(const LuxForgeSceneDraw& draw, InstanceData& instance) {
    if (draw.model[3] != 0 || draw.model[7] != 0 || draw.model[11] != 0 || draw.model[15] != 1)
        return false;
    const Float3 x = Position(draw, 0), y = Position(draw, 1), z = Position(draw, 2);
    const Float3 row0 = Cross(y, z), row1 = Cross(z, x), row2 = Cross(x, y);
    const float determinant = Dot(x, row0);
    if (determinant == 0 || !std::isfinite(determinant))
        return false;
    const float inverse = 1.f / determinant;
    for (int row = 0; row < 3; ++row) {
        instance[2 + row] = {row0[row] * inverse, row1[row] * inverse, row2[row] * inverse, 0};
        if (!std::isfinite(instance[2 + row][0]) || !std::isfinite(instance[2 + row][1]) ||
            !std::isfinite(instance[2 + row][2]))
            return false;
    }
    return true;
}

Float4 FaceNormal(const Float3& a, const Float3& b, const Float3& c) {
    const Float3 n = Cross(Subtract(b, a), Subtract(c, a));
    const float length = std::hypot(n[0], n[1], n[2]);
    if (!std::isfinite(length) || length <= 1e-20f)
        return {0, 1, 0, 0};
    return {n[0] / length, n[1] / length, n[2] / length, 0};
}

Buffer* CreateMetadata(uint32_t count, const char* name) {
    BufferLoadDesc load = {};
    auto& desc = load.mDesc;
    desc.mSize = static_cast<uint64_t>(count) * sizeof(Float4);
    desc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    desc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
    desc.mElementCount = count;
    desc.mStructStride = sizeof(Float4);
    desc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
#if defined(VULKAN)
    desc.mFlags |= BUFFER_CREATION_FLAG_HOST_COHERENT;
#endif
    desc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
    desc.pName = name;
    Buffer* buffer = nullptr;
    load.ppBuffer = &buffer;
    addResource(&load, nullptr);
    if (buffer && !buffer->pCpuMappedAddress) {
        removeResource(buffer);
        return nullptr;
    }
    return buffer;
}

#if defined(VULKAN)
void SynchronizeBuild(Cmd* cmd, VkPipelineStageFlags source, VkAccessFlags writes,
                      VkPipelineStageFlags destination, VkAccessFlags reads) {
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = writes;
    barrier.dstAccessMask = reads;
    vkCmdPipelineBarrier(cmd->mVk.pCmdBuf, source, destination, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}
#endif
} // namespace

struct ForgeRayTracing {
    Renderer* renderer = nullptr;
    Raytracing* backend = nullptr;
    std::unordered_map<Buffer*, SourceBuffer> sources;
    std::unordered_map<MeshKey, RayMesh, MeshKeyHash> meshes;
    std::vector<RayMesh*> scratch;
    std::vector<FrameScene> scenes;
    std::vector<RetiredAcceleration> retiredAcceleration;
    std::vector<RetiredBuffer> retiredBuffers;
    Buffer* triangles = nullptr;
    uint32_t triangleCount = 0;
    uint32_t triangleCapacity = 0;

    bool ReserveTriangles(uint32_t count, uint64_t frame) {
        if (triangleCount + count <= triangleCapacity)
            return true;

        // Snapshots let already recorded and submitted passes retain their normal offsets.
        uint32_t used = 0;
        for (const auto& item : meshes)
            used += item.second.count;
        if (count > MaxTriangles - used)
            return false;

        uint32_t capacity = std::max(triangleCapacity, 256u);
        while (capacity < used + count)
            capacity = std::min(capacity * 2, MaxTriangles);
        auto* replacement = CreateMetadata(capacity, "LuxForge ray triangle normals");
        if (!replacement)
            return false;
        auto* destination = static_cast<Float4*>(replacement->pCpuMappedAddress);
        uint32_t next = 0;
        if (triangles) {
            const auto* source = static_cast<const Float4*>(triangles->pCpuMappedAddress);
            for (auto& item : meshes) {
                auto& mesh = item.second;
                std::memcpy(destination + next, source + mesh.firstTriangle,
                            mesh.count * sizeof(Float4));
                mesh.firstTriangle = next;
                next += mesh.count;
            }
            retiredBuffers.push_back({triangles, frame});
        }
        triangles = replacement;
        triangleCapacity = capacity;
        triangleCount = next;
        return true;
    }

    RayMesh* FindOrCreate(const MeshKey& key, const LuxForgeSceneDraw& draw, uint64_t frame) {
        const auto cached = meshes.find(key);
        if (cached != meshes.end())
            return PassesMask(cached->second.minimumAlpha, draw) ? &cached->second : nullptr;
        const auto positions = sources.find(key.vertices), indices = sources.find(key.indices);
        if (key.vertices == key.indices || positions == sources.end() || indices == sources.end() ||
            key.indexCount > indices->second.indices.size() || key.indexCount < 3 ||
            key.indexCount % 3)
            return nullptr;
        const auto& vertices = positions->second.vertices;
        const auto& i = indices->second.indices;
        const uint32_t count = key.indexCount / 3;
        if (vertices.empty() || count > MaxTriangles)
            return nullptr;
        float minimumAlpha = std::numeric_limits<float>::infinity();
        for (uint32_t n = 0; n < key.indexCount; ++n) {
            if (i[n] >= vertices.size())
                return nullptr;
            const auto& vertex = vertices[i[n]];
            const auto& p = vertex.position;
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                return nullptr;
            const float alpha = vertex.color[3];
            minimumAlpha = std::isfinite(alpha) ? std::min(minimumAlpha, alpha)
                                                : -std::numeric_limits<float>::infinity();
        }
        if (!PassesMask(minimumAlpha, draw) || !ReserveTriangles(count, frame))
            return nullptr;

        AccelerationStructureGeometryDesc geometry = {};
        geometry.pVertexBuffer = key.vertices;
        geometry.pIndexBuffer = key.indices;
        geometry.mVertexCount = static_cast<uint32_t>(vertices.size());
        geometry.mVertexStride = VertexStride;
        geometry.mVertexFormat = TinyImageFormat_R32G32B32_SFLOAT;
        geometry.mIndexCount = key.indexCount;
        geometry.mIndexType = INDEX_TYPE_UINT32;
        geometry.mFlags = ACCELERATION_STRUCTURE_GEOMETRY_FLAG_OPAQUE;
        AccelerationStructureDesc desc = {};
        desc.mType = ACCELERATION_STRUCTURE_TYPE_BOTTOM;
        desc.mFlags = ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        desc.mBottom = {1, &geometry};
        AccelerationStructure* blas = nullptr;
        addAccelerationStructure(backend, &desc, &blas);
        if (!blas)
            return nullptr;

        auto* normals = static_cast<Float4*>(triangles->pCpuMappedAddress) + triangleCount;
        for (uint32_t face = 0; face < count; ++face) {
            const auto& a = vertices[i[3 * face]];
            const auto& b = vertices[i[3 * face + 1]];
            const auto& c = vertices[i[3 * face + 2]];
            normals[face] = FaceNormal(a.position, b.position, c.position);
            uint32_t packed = 0;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const float mean = (a.color[channel] + b.color[channel] + c.color[channel]) / 3;
                const float value = std::isfinite(mean) ? std::clamp(mean, 0.f, 1.f) : 1.f;
                packed |= static_cast<uint32_t>(std::lround(value * 1023)) << (10 * channel);
            }
            std::memcpy(&normals[face][3], &packed, sizeof(packed));
        }
        RayMesh& mesh =
            meshes.emplace(key, RayMesh{blas, triangleCount, count, 0, minimumAlpha}).first->second;
        positions->second.meshes.push_back(key);
        indices->second.meshes.push_back(key);
        triangleCount += count;
        return &mesh;
    }

    void Invalidate(SourceBuffer& source, Buffer* buffer, uint64_t frame) {
        for (const auto& key : source.meshes) {
            const auto mesh = meshes.find(key);
            if (mesh == meshes.end())
                continue;
            auto* other = key.vertices == buffer ? key.indices : key.vertices;
            auto& references = sources.at(other).meshes;
            references.erase(std::remove(references.begin(), references.end(), key),
                             references.end());
            scratch.erase(std::remove(scratch.begin(), scratch.end(), &mesh->second),
                          scratch.end());
            if (mesh->second.buildFrame)
                retiredAcceleration.push_back({mesh->second.acceleration, frame});
            else
                removeAccelerationStructure(backend, mesh->second.acceleration);
            meshes.erase(mesh);
        }
        source.meshes.clear();
    }
};

ForgeRayTracing* CreateForgeRayTracing(Renderer* renderer) {
    if (!renderer || !renderer->pGpu->mRayQuerySupported)
        return nullptr;
#if defined(VULKAN)
    if (!renderer->pGpu->mBufferDeviceAddressSupported || !renderer->pGpu->mSpirv14Extension)
        return nullptr;
    VkFormatProperties format = {};
    vkGetPhysicalDeviceFormatProperties(renderer->pGpu->mVk.pGpu, VK_FORMAT_R32G32B32_SFLOAT,
                                        &format);
    if (!(format.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR))
        return nullptr;
#elif defined(DIRECT3D12)
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_5};
    if (FAILED(renderer->mDx.pDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel,
                                                          sizeof(shaderModel))) ||
        shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_5)
        return nullptr;
#endif
    Raytracing* backend = nullptr;
    if (!initRaytracing(renderer, &backend))
        return nullptr;
    auto* ray = new ForgeRayTracing;
    ray->renderer = renderer;
    ray->backend = backend;
    return ray;
}

void CollectForgeRayTracing(ForgeRayTracing* ray, uint64_t completedFrame) {
    if (!ray)
        return;
    const auto collect = [completedFrame](auto& list, const auto& release) {
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const auto& item) { return release(item, completedFrame); }),
                   list.end());
    };
    // A TLAS references the original BLAS allocation for its entire lifetime.
    collect(ray->scenes, [ray](const FrameScene& scene, uint64_t complete) {
        if (scene.frame > complete)
            return false;
        removeAccelerationStructure(ray->backend, scene.acceleration);
        removeResource(scene.instances);
        return true;
    });
    collect(ray->retiredAcceleration,
            [ray](const RetiredAcceleration& acceleration, uint64_t complete) {
                if (acceleration.frame > complete)
                    return false;
                removeAccelerationStructure(ray->backend, acceleration.acceleration);
                return true;
            });
    collect(ray->retiredBuffers, [](const RetiredBuffer& entry, uint64_t complete) {
        if (entry.frame > complete)
            return false;
        removeResource(entry.buffer);
        return true;
    });
    collect(ray->scratch, [ray](RayMesh* mesh, uint64_t complete) {
        if (mesh->buildFrame > complete)
            return false;
        removeAccelerationStructureScratch(ray->backend, mesh->acceleration);
        return true;
    });
}

void DestroyForgeRayTracing(ForgeRayTracing* ray) {
    if (!ray)
        return;
    CollectForgeRayTracing(ray, UINT64_MAX);
    for (const auto& item : ray->meshes)
        removeAccelerationStructure(ray->backend, item.second.acceleration);
    if (ray->triangles)
        removeResource(ray->triangles);
    exitRaytracing(ray->renderer, ray->backend);
    delete ray;
}

void MarkForgeRayGeometry(const ForgeRayTracing* ray, BufferDesc& desc) {
#if defined(VULKAN)
    if (ray && (desc.mDescriptors & (DESCRIPTOR_TYPE_VERTEX_BUFFER | DESCRIPTOR_TYPE_INDEX_BUFFER)))
        desc.mFlags |= BUFFER_CREATION_FLAG_ACCELERATION_STRUCTURE_BUILD_INPUT |
                       BUFFER_CREATION_FLAG_SHADER_DEVICE_ADDRESS;
#elif defined(DIRECT3D12)
    if (ray)
        desc.mStartState |= RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
#endif
}

void UpdateForgeRayBuffer(ForgeRayTracing* ray, Buffer* buffer, int type, uint32_t totalSize,
                          const void* data, uint32_t size, uint32_t offset, uint64_t frame) {
    if (!ray || !buffer || (type != LUX_FORGE_BUFFER_VERTEX && type != LUX_FORGE_BUFFER_INDEX))
        return;
    auto& source = ray->sources[buffer];
    ray->Invalidate(source, buffer, frame);
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (type == LUX_FORGE_BUFFER_INDEX) {
        source.indices.resize(totalSize / sizeof(uint32_t));
        const uint32_t limit = static_cast<uint32_t>(source.indices.size() * sizeof(uint32_t));
        if (offset < limit)
            std::memcpy(reinterpret_cast<unsigned char*>(source.indices.data()) + offset, bytes,
                        std::min(size, limit - offset));
        return;
    }

    source.vertices.resize(totalSize / VertexStride);
    // Retain positions and colors for acceleration structures and ray-hit shading.
    const uint32_t end = offset + size;
    const auto copy = [&](void* destination, uint32_t position, uint32_t length) {
        const uint32_t start = std::max(offset, position);
        const uint32_t finish = std::min(end, position + length);
        if (start < finish)
            std::memcpy(static_cast<unsigned char*>(destination) + start - position,
                        bytes + start - offset, finish - start);
    };
    for (uint32_t index = offset / VertexStride;
         index < source.vertices.size() && index * VertexStride < end; ++index) {
        const uint32_t position = index * VertexStride;
        auto& vertex = source.vertices[index];
        copy(vertex.position.data(), position, PositionBytes);
        copy(vertex.color.data(), position + ColorOffset, sizeof(vertex.color));
    }
}

void ForgetForgeRayBuffer(ForgeRayTracing* ray, Buffer* buffer, uint64_t frame) {
    if (!ray || !buffer)
        return;
    const auto source = ray->sources.find(buffer);
    if (source == ray->sources.end())
        return;
    ray->Invalidate(source->second, buffer, frame);
    ray->sources.erase(source);
}

ForgeRayScene BuildForgeRayScene(ForgeRayTracing* ray, Cmd* cmd, uint64_t frame,
                                 const ForgeRayDraw* draws, size_t count) {
    if (!ray || !count)
        return {};
    // Resolve every mesh first. Growing/compacting the atlas can relocate an earlier mesh.
    std::vector<RayMesh*> meshes;
    std::vector<AccelerationStructureInstanceDesc> instances;
    std::vector<InstanceData> materials;
    const auto capacity = std::min<size_t>(count, MaxInstances);
    meshes.reserve(capacity);
    instances.reserve(capacity);
    materials.reserve(capacity);
    for (size_t i = 0; i < count && instances.size() < capacity; ++i) {
        const auto& input = draws[i];
        const auto& draw = *input.draw;
        const bool water = draw.water[3] > 1;
        if (!water && (draw.alphaMode > 1 || (draw.alphaMode == 1 && draw.texture)))
            continue;
        InstanceData material{};
        if (!NormalMatrix(draw, material))
            continue;
        RayMesh* mesh =
            ray->FindOrCreate({input.vertices, input.indices, draw.indexCount}, draw, frame);
        if (!mesh)
            continue;

        AccelerationStructureInstanceDesc instance = {};
        instance.pBottomAS = mesh->acceleration;
        instance.mInstanceID = static_cast<uint32_t>(instances.size());
        instance.mInstanceMask = water ? 2 : 1;
        instance.mFlags = ACCELERATION_STRUCTURE_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column)
                instance.mTransform[row * 4 + column] = draw.model[column * 4 + row];
        instances.push_back(instance);
        const auto* tint = input.textureColor;
        material[0] = {draw.albedo[0] * (tint ? tint[0] : 1.f),
                       draw.albedo[1] * (tint ? tint[1] : 1.f),
                       draw.albedo[2] * (tint ? tint[2] : 1.f),
                       std::isfinite(draw.metallic) ? std::clamp(draw.metallic, 0.f, 1.f) : 0};
        material[1] = {std::isfinite(draw.roughness) ? std::clamp(draw.roughness, .045f, 1.f) : .5f,
                       water ? draw.water[3] : 1.f, 0, water ? 1.f : 0.f};
        materials.push_back(material);
        meshes.push_back(mesh);
    }
    if (instances.empty())
        return {};

    auto* instanceBuffer =
        CreateMetadata(static_cast<uint32_t>(instances.size()) * 5, "LuxForge ray instances");
    if (!instanceBuffer)
        return {};
    for (size_t i = 0; i < meshes.size(); ++i) {
        const uint32_t offset = meshes[i]->firstTriangle;
        std::memcpy(&materials[i][1][2], &offset, sizeof(offset));
    }
    std::memcpy(instanceBuffer->pCpuMappedAddress, materials.data(),
                materials.size() * sizeof(InstanceData));

    AccelerationStructureDesc desc = {};
    desc.mType = ACCELERATION_STRUCTURE_TYPE_TOP;
    desc.mFlags = ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    desc.mTop = {static_cast<uint32_t>(instances.size()), instances.data()};
    AccelerationStructure* tlas = nullptr;
    addAccelerationStructure(ray->backend, &desc, &tlas);
    if (!tlas) {
        removeResource(instanceBuffer);
        return {};
    }

    uint32_t builds = 0;
#if defined(VULKAN)
    constexpr auto buildStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    constexpr bool issueBarrier = false;
#else
    constexpr bool issueBarrier = true;
#endif
    for (RayMesh* mesh : meshes) {
        if (mesh->buildFrame)
            continue;
#if defined(VULKAN)
        if (!builds)
            SynchronizeBuild(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                             buildStage, VK_ACCESS_SHADER_READ_BIT);
#endif
        RaytracingBuildASDesc build = {mesh->acceleration, issueBarrier};
        cmdBuildAccelerationStructure(cmd, ray->backend, &build);
        mesh->buildFrame = frame;
        ray->scratch.push_back(mesh);
        ++builds;
    }
#if defined(VULKAN)
    if (builds)
        SynchronizeBuild(cmd, buildStage, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                         buildStage, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
#endif

    RaytracingBuildASDesc build = {tlas, issueBarrier};
    cmdBuildAccelerationStructure(cmd, ray->backend, &build);
#if defined(VULKAN)
    SynchronizeBuild(cmd, buildStage, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
#endif
    ray->scenes.push_back({tlas, instanceBuffer, frame});
    return {tlas, instanceBuffer, ray->triangles, builds + 1};
}

#else
ForgeRayTracing* CreateForgeRayTracing(Renderer*) {
    return nullptr;
}

void DestroyForgeRayTracing(ForgeRayTracing*) {}

void CollectForgeRayTracing(ForgeRayTracing*, uint64_t) {}

void MarkForgeRayGeometry(const ForgeRayTracing*, BufferDesc&) {}

void UpdateForgeRayBuffer(ForgeRayTracing*, Buffer*, int, uint32_t, const void*, uint32_t, uint32_t,
                          uint64_t) {}

void ForgetForgeRayBuffer(ForgeRayTracing*, Buffer*, uint64_t) {}

ForgeRayScene BuildForgeRayScene(ForgeRayTracing*, Cmd*, uint64_t, const ForgeRayDraw*, size_t) {
    return {};
}
#endif
