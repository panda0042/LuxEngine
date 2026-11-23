#pragma once

struct LuxForgeRadianceCascades {
    Shader* shader = nullptr;
    Pipeline* pipeline = nullptr;
    DescriptorSet* set = nullptr;
    Buffer* constants = nullptr;
    Buffer* radiance[2] = {};
    Buffer* irradiance = nullptr;
    bool ready = false;
};
