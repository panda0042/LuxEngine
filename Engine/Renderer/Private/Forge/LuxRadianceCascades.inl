// Included inside the Forge device implementation.

void ShutdownRadianceCascades(LuxForgeDevice* device) {
    auto* cascades = device->radianceCascades;
    if (!cascades)
        return;
    if (cascades->set)
        removeDescriptorSet(device->renderer, cascades->set);
    if (cascades->pipeline)
        removePipeline(device->renderer, cascades->pipeline);
    if (cascades->shader)
        removeShader(device->renderer, cascades->shader);
    for (auto* buffer :
         {cascades->constants, cascades->radiance[0], cascades->radiance[1], cascades->irradiance})
        if (buffer)
            removeResource(buffer);
    delete cascades;
    device->radianceCascades = nullptr;
}

#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
constexpr uint32_t kCascadeStages = 4; // far, middle, near, irradiance projection
constexpr uint32_t kCascadeUniformStride = 256;
constexpr ResourceState kCascadeSample = RESOURCE_STATE_SHADER_RESOURCE;
constexpr ResourceState kCascadeWrite = RESOURCE_STATE_UNORDERED_ACCESS;

struct CascadeConstants {
    uint32_t shape[4];
    float origin[4];
    float interval[4];
    float environment[4];
    float optics[4];
};

static_assert(sizeof(CascadeConstants) == 80);

constexpr uint32_t CascadeProbeCount(uint32_t width) {
    return width * width * width;
}

constexpr uint32_t CascadeRayCount(uint32_t width, uint32_t angles) {
    return CascadeProbeCount(width) * angles * angles;
}

bool InitRadianceCascades(LuxForgeDevice* device) {
    if (device->radianceCascades)
        return device->radianceCascades->ready;
    auto& cascades = *(device->radianceCascades = new LuxForgeRadianceCascades);
    ShaderLoadDesc shader = {};
    shader.mComp.pFileName = "luxradiancecascades.comp";
    addShader(device->renderer, &shader, &cascades.shader);
    if (!cascades.shader)
        return false;

    const auto entries = device->framesInFlight * LuxForgeDevice::kMaxScenePasses * kCascadeStages;
    const DescriptorSetDesc set = SRT_SET_DESC(LuxRadianceCascadesSrt, Persistent, entries, 0);
    addDescriptorSet(device->renderer, &set, &cascades.set);
    if (!cascades.set)
        return false;

    BufferLoadDesc constants = {};
    constants.ppBuffer = &cascades.constants;
    constants.mDesc.mSize = entries * kCascadeUniformStride;
    constants.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    constants.mDesc.mDescriptors = UniformBufferDescriptor;
    constants.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    constants.mDesc.mStartState = RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    constants.mDesc.pName = "LuxForge radiance cascades constants";
    addResource(&constants, nullptr);
    if (!cascades.constants || !cascades.constants->pCpuMappedAddress)
        return false;

    const auto makeBuffer = [](Buffer*& buffer, uint32_t count, const char* name) {
        BufferLoadDesc load = {};
        load.ppBuffer = &buffer;
        load.mDesc.mSize = count * sizeof(float[4]);
        load.mDesc.mElementCount = count;
        load.mDesc.mStructStride = sizeof(float[4]);
        load.mDesc.mDescriptors =
            static_cast<DescriptorType>(DESCRIPTOR_TYPE_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER);
        load.mDesc.mStartState = kCascadeSample;
        load.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
        load.mDesc.pName = name;
        addResource(&load, nullptr);
        return buffer != nullptr;
    };
    constexpr uint32_t coarse = CascadeRayCount(LUX_CASCADE_FAR_PROBES, LUX_CASCADE_FAR_ANGLES);
    constexpr uint32_t fine = CascadeRayCount(LUX_CASCADE_NEAR_PROBES, LUX_CASCADE_NEAR_ANGLES);
    constexpr uint32_t middle =
        CascadeRayCount(LUX_CASCADE_MIDDLE_PROBES, LUX_CASCADE_MIDDLE_ANGLES);
    constexpr uint32_t coefficients =
        LUX_CASCADE_SH_HEADER + LUX_CASCADE_SH_STRIDE * CascadeProbeCount(LUX_CASCADE_NEAR_PROBES);
    if (!makeBuffer(cascades.radiance[0], std::max(coarse, fine), "LuxForge coarse/fine rays") ||
        !makeBuffer(cascades.radiance[1], middle, "LuxForge middle rays") ||
        !makeBuffer(cascades.irradiance, coefficients, "LuxForge probe irradiance"))
        return false;

    PipelineDesc pipeline = {};
    pipeline.mType = PIPELINE_TYPE_COMPUTE;
    PIPELINE_LAYOUT_DESC(pipeline, SRT_LAYOUT_DESC(LuxRadianceCascadesSrt, Persistent),
                         SRT_LAYOUT_DESC(LuxRadianceCascadesSrt, PerFrame), NULL, NULL);
    pipeline.mComputeDesc.pShaderProgram = cascades.shader;
    addPipeline(device->renderer, &pipeline, &cascades.pipeline);
    cascades.ready = cascades.pipeline != nullptr;
    return cascades.ready;
}

struct CascadeStage {
    uint32_t probes;
    uint32_t angles;
    float spacing;
    float begin;
    float end;
};

Buffer* RenderRadianceCascades(LuxForgeDevice* device, const LuxForgeScene& scene, uint32_t pass,
                               const ForgeRayScene& rays) {
    if (!rays.scene || !(scene.cascadeOptions[0] > 0) || !(scene.cascadeOptions[1] > 0) ||
        !InitRadianceCascades(device))
        return nullptr;

    const auto& cascades = *device->radianceCascades;
    constexpr CascadeStage stages[] = {
        {LUX_CASCADE_FAR_PROBES, LUX_CASCADE_FAR_ANGLES, 4.f, 6.f, 14.f},
        {LUX_CASCADE_MIDDLE_PROBES, LUX_CASCADE_MIDDLE_ANGLES, 2.f, 2.f, 6.f},
        {LUX_CASCADE_NEAR_PROBES, LUX_CASCADE_NEAR_ANGLES, 1.f, 0.f, 2.f},
        {LUX_CASCADE_NEAR_PROBES, LUX_CASCADE_NEAR_ANGLES, 1.f, 0.f, 0.f}};
    const float spacing = scene.cascadeOptions[1];
    const uint32_t first =
        (device->frameSlot * LuxForgeDevice::kMaxScenePasses + pass) * kCascadeStages;
    auto* cmd = device->element.pCmds[0];
    BindPipeline(device, cmd, cascades.pipeline);
    BindDescriptors(device, cmd,
                    device->frameSlot * LuxForgeDevice::kMaxLightBlocks + device->currentLight,
                    device->lightSet);

    for (uint32_t index = 0; index < kCascadeStages; ++index) {
        const auto& stage = stages[index];
        const bool project = index == kCascadeStages - 1;
        CascadeConstants constants = {};
        constants.shape[0] = stage.probes;
        constants.shape[1] = stage.angles;
        if (index != 0 && !project) {
            constants.shape[2] = stages[index - 1].probes;
            constants.shape[3] = stages[index - 1].angles;
        }
        std::memcpy(constants.origin, scene.cascadeOrigin, sizeof(constants.origin));
        constants.origin[3] = spacing;
        constants.interval[0] = spacing * stage.spacing;
        constants.interval[1] = spacing * stage.begin;
        constants.interval[2] = spacing * stage.end;
        constants.interval[3] = project ? 1.f : 0.f;
        std::memcpy(constants.environment, scene.environment, sizeof(constants.environment));
        constants.optics[0] = scene.options[0];
        constants.optics[1] = scene.skyLighting[0] > .5f ? 2.f : scene.options[1];
        constants.optics[2] = scene.cascadeOptions[0];
        constants.optics[3] = std::max(14.f * spacing, scene.rayOptions[1]);

        const uint32_t entry = first + index;
        const uint32_t offset = entry * kCascadeUniformStride;
        std::memcpy(static_cast<uint8_t*>(cascades.constants->pCpuMappedAddress) + offset,
                    &constants, sizeof(constants));

        auto* uniform = cascades.constants;
        auto* tlas = rays.scene;
        auto* instances = rays.instances;
        auto* triangles = rays.triangles;
        auto* environment = device->environments[scene.environmentMap].maps[2];
        auto* input = cascades.radiance[(index + 1) % 2];
        auto* output = project ? cascades.irradiance : cascades.radiance[index % 2];
        DescriptorDataRange range = {offset, sizeof(CascadeConstants), 0};
        auto* fallback = device->textures[device->whiteTexture].texture;
        auto* gloss = constants.optics[1] > 1.5f
                          ? device->skyCache.stages[2].output.target->pTexture
                          : fallback;
        auto* harmonics = constants.optics[1] > 1.5f
                              ? device->skyCache.stages[1].output.target->pTexture
                              : fallback;
        DescriptorData params[9] = {};
        params[7].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gRaySkyGloss);
        params[7].ppTextures = &gloss;
        params[8].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gRaySkySH);
        params[8].ppTextures = &harmonics;
        params[0].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gCascade);
        params[0].ppBuffers = &uniform;
        params[0].pRanges = &range;
        params[1].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gRayScene);
        params[1].ppAccelerationStructures = &tlas;
        params[2].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gRayInstances);
        params[2].ppBuffers = &instances;
        params[3].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gRayTriangles);
        params[3].ppBuffers = &triangles;
        params[4].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gCascadeSky);
        params[4].ppTextures = &environment;
        params[5].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gCascadePrevious);
        params[5].ppBuffers = &input;
        params[6].mIndex = SRT_RES_IDX(LuxRadianceCascadesSrt, Persistent, gCascadeOutput);
        params[6].ppBuffers = &output;
        updateDescriptorSet(device->renderer, entry, cascades.set, 9, params);

        BufferBarrier write = {output, kCascadeSample, kCascadeWrite};
        cmdResourceBarrier(cmd, 1, &write, 0, nullptr, 0, nullptr);
        BindDescriptors(device, cmd, entry, cascades.set);
        const auto groups =
            project ? CascadeProbeCount(stage.probes) : CascadeRayCount(stage.probes, stage.angles);
        cmdDispatch(cmd, (groups + 63) / 64, 1, 1);
        BufferBarrier sample = {output, kCascadeWrite, kCascadeSample};
        cmdResourceBarrier(cmd, 1, &sample, 0, nullptr, 0, nullptr);
    }
    device->frameStats.uniformBinds += kCascadeStages;
    device->frameStats.computeDispatches += kCascadeStages;
    ++device->frameStats.radianceCascadePasses;
    return cascades.irradiance;
}
#endif
