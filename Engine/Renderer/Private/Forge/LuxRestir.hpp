#pragma once

#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
struct LuxForgeRestir {
    Shader* shaders[2] = {};
    Pipeline* pipelines[2] = {};
    DescriptorSet* sets[2] = {};
    Buffer* candidates = nullptr;
    Buffer* output = nullptr;
    size_t capacity = 0;

    struct Retired {
        Buffer* candidates;
        Buffer* output;
        uint64_t frame;
    };

    std::vector<Retired> retired;
    bool initAttempted = false;
};
#endif
