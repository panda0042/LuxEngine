#pragma once

struct LuxForgeFog {
    Shader* computeShader = nullptr;
    Shader* compositeShader = nullptr;
    Pipeline* computePipeline = nullptr;
    Pipeline* compositePipeline = nullptr;
    DescriptorSet* computeSet = nullptr;
    DescriptorSet* compositeSet = nullptr;
    Buffer* constants = nullptr;
    Buffer* volume = nullptr;
    bool ready = false;
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
    Shader* rayShader = nullptr;
    Pipeline* rayPipeline = nullptr;
    DescriptorSet* raySet = nullptr;
    bool rayAttempted = false;
#endif
};
