#pragma once

#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
struct LuxForgeRestir {
    Shader* shaders[2] = {};
    Pipeline* pipelines[2] = {};
    DescriptorSet* sets[2] = {};
    Buffer* candidates = nullptr;
    Buffer* output = nullptr;
    Buffer* history[2] = {};
    Buffer* temporalParams = nullptr;
    uint32_t index = 0, width = 0, height = 0, effects = 0, environment = 0;
    uint64_t frame = 0, view = 0;
    std::array<float, 16> matrix{};
    std::array<float, 3> forward{};
    std::array<double, 3> origin{};
    size_t capacity = 0;

    struct Retired {
        Buffer* candidates;
        Buffer* output;
        Buffer* history[2];
        uint64_t frame;
    };

    std::vector<Retired> retired;
    bool fullHistory = false, initAttempted = false;
};
#endif
