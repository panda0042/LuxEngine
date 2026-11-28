#pragma once

struct LuxForgeFog {
    Shader* computeShader = nullptr;
    Shader* compositeShader = nullptr;
    Pipeline* computePipeline = nullptr;
    Shader* hdrShader = nullptr;
    Pipeline* hdrPipeline = nullptr;
    Pipeline* compositePipeline = nullptr;
    DescriptorSet* computeSet = nullptr;
    DescriptorSet* compositeSet = nullptr;
    Buffer* constants = nullptr;
    Buffer* volume = nullptr;
    Buffer* history = nullptr;
    uint64_t lastFrame = 0, lastView = 0;
    double origin[3] = {};
    float projection[16] = {}, forward[3] = {}, maxDistance = 0;
    bool ready = false;
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
    Shader* rayShader = nullptr;
    Pipeline* rayPipeline = nullptr;
    DescriptorSet* raySet = nullptr;
    bool rayAttempted = false;
#endif
};
