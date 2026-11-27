#pragma once

#include <stddef.h>
#include <stdint.h>

struct AccelerationStructure;
struct Buffer;
struct BufferDesc;
struct Cmd;
struct ForgeRayTracing;
struct LuxForgeSceneDraw;
struct Renderer;

struct ForgeRayDraw {
    const LuxForgeSceneDraw* draw;
    Buffer* vertices;
    Buffer* indices;
    const float* textureColor;
};

struct ForgeRayScene {
    AccelerationStructure* scene = nullptr;
    Buffer* instances = nullptr;
    Buffer* triangles = nullptr;
    uint32_t builds = 0;
};

ForgeRayTracing* CreateForgeRayTracing(Renderer* renderer);
void DestroyForgeRayTracing(ForgeRayTracing* ray);
void CollectForgeRayTracing(ForgeRayTracing* ray, uint64_t completedFrame);
void MarkForgeRayGeometry(const ForgeRayTracing* ray, BufferDesc& desc);
void UpdateForgeRayBuffer(ForgeRayTracing* ray, Buffer* buffer, int type, uint32_t totalSize,
                          const void* data, uint32_t size, uint32_t offset, uint64_t frame);
void ForgetForgeRayBuffer(ForgeRayTracing* ray, Buffer* buffer, uint64_t frame);
ForgeRayScene BuildForgeRayScene(ForgeRayTracing* ray, Cmd* cmd, uint64_t frame,
                                 const ForgeRayDraw* draws, size_t count);
