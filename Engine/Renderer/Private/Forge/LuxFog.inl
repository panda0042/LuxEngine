// Compiled inside LuxForge.cpp alongside the scene and device resource helpers.

constexpr uint32_t kFogUniformStride = 256;
constexpr ResourceState kFogVolumeSample = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr ResourceState kFogVolumeWrite = RESOURCE_STATE_UNORDERED_ACCESS;

struct FogConstants {
    float right[4];
    float up[4];
    float forward[4];
    float colorDensity[4];
    float options[4];
    float lightDirection[4];
    float lightRadiance[4];
    float ambientRadiance[4];
    float shadowMatrix[16];
    float shadowOptions[4];
    float composite[4];
    uint32_t virtualShadowPages[4];
};

static_assert(sizeof(FogConstants) == 240);

void ShutdownFog(LuxForgeDevice* device) {
    auto* fog = device->fog;
    if (!fog)
        return;
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
    if (fog->raySet)
        removeDescriptorSet(device->renderer, fog->raySet);
    if (fog->rayPipeline)
        removePipeline(device->renderer, fog->rayPipeline);
    if (fog->rayShader)
        removeShader(device->renderer, fog->rayShader);
#endif
    for (auto* set : {fog->computeSet, fog->compositeSet})
        if (set)
            removeDescriptorSet(device->renderer, set);
    for (auto* pipeline : {fog->computePipeline, fog->compositePipeline})
        if (pipeline)
            removePipeline(device->renderer, pipeline);
    for (auto* shader : {fog->computeShader, fog->compositeShader})
        if (shader)
            removeShader(device->renderer, shader);
    for (auto* buffer : {fog->constants, fog->volume})
        if (buffer)
            removeResource(buffer);
    delete fog;
    device->fog = nullptr;
}

bool InitFog(LuxForgeDevice* device) {
    if (device->fog)
        return device->fog->ready;
    auto& fog = *(device->fog = new LuxForgeFog);
    ShaderLoadDesc load = {};
    load.mComp.pFileName = "luxfog.comp";
    addShader(device->renderer, &load, &fog.computeShader);
    fog.compositeShader = LoadShader(device, "luxfullscreen.vert", "luxfogcomposite.frag");
    if (!fog.computeShader || !fog.compositeShader)
        return false;

    const uint32_t entries = device->framesInFlight * LuxForgeDevice::kMaxScenePasses;
    const DescriptorSetDesc computeSet = SRT_SET_DESC(LuxFogComputeSrt, Persistent, entries, 0);
    const DescriptorSetDesc compositeSet = SRT_SET_DESC(LuxFogCompositeSrt, Persistent, entries, 0);
    addDescriptorSet(device->renderer, &computeSet, &fog.computeSet);
    addDescriptorSet(device->renderer, &compositeSet, &fog.compositeSet);

    BufferLoadDesc constants = {};
    constants.ppBuffer = &fog.constants;
    constants.mDesc.mSize = entries * kFogUniformStride;
    constants.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    constants.mDesc.mDescriptors = UniformBufferDescriptor;
    constants.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    constants.mDesc.mStartState = RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    constants.mDesc.pName = "LuxForge froxel constants";
    addResource(&constants, nullptr);

    BufferLoadDesc volume = {};
    volume.ppBuffer = &fog.volume;
    constexpr uint32_t froxelCount = LUX_FOG_COLUMNS * LUX_FOG_ROWS * LUX_FOG_SLICES;
    volume.mDesc.mSize = froxelCount * sizeof(float[4]);
    volume.mDesc.mElementCount = froxelCount;
    volume.mDesc.mStructStride = sizeof(float[4]);
    volume.mDesc.mDescriptors =
        static_cast<DescriptorType>(DESCRIPTOR_TYPE_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER);
    volume.mDesc.mStartState = kFogVolumeSample;
    volume.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
    volume.mDesc.pName = "LuxForge integrated froxels";
    addResource(&volume, nullptr);
    if (!fog.computeSet || !fog.compositeSet || !fog.constants ||
        !fog.constants->pCpuMappedAddress || !fog.volume)
        return false;

    PipelineDesc compute = {};
    compute.mType = PIPELINE_TYPE_COMPUTE;
    PIPELINE_LAYOUT_DESC(compute, SRT_LAYOUT_DESC(LuxFogComputeSrt, Persistent), NULL, NULL, NULL);
    compute.mComputeDesc.pShaderProgram = fog.computeShader;
    addPipeline(device->renderer, &compute, &fog.computePipeline);

    PipelineDesc composite = {};
    composite.mType = PIPELINE_TYPE_GRAPHICS;
    PIPELINE_LAYOUT_DESC(composite, SRT_LAYOUT_DESC(LuxFogCompositeSrt, Persistent), NULL, NULL,
                         NULL);
    auto& graphics = composite.mGraphicsDesc;
    graphics.pShaderProgram = fog.compositeShader;
    graphics.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    graphics.mRenderTargetCount = 1;
    auto swapFormat = static_cast<TinyImageFormat>(device->swapChain->ppRenderTargets[0]->mFormat);
    graphics.pColorFormats = &swapFormat;
    graphics.mDepthStencilFormat = kDepthFormat;
    graphics.mSampleCount = SAMPLE_COUNT_1;
    RasterizerStateDesc raster = {};
    raster.mCullMode = CULL_MODE_NONE;
    graphics.pRasterizerState = &raster;
    DepthStateDesc depth = {};
    graphics.pDepthState = &depth;
    BlendStateDesc blend = {};
    blend.mSrcFactors[0] = BC_SRC_ALPHA;
    blend.mDstFactors[0] = BC_ONE_MINUS_SRC_ALPHA;
    blend.mBlendModes[0] = BM_ADD;
    blend.mSrcAlphaFactors[0] = BC_ONE;
    blend.mDstAlphaFactors[0] = BC_ONE_MINUS_SRC_ALPHA;
    blend.mBlendAlphaModes[0] = BM_ADD;
    blend.mColorWriteMasks[0] = COLOR_MASK_ALL;
    blend.mRenderTargetMask = BLEND_STATE_TARGET_0;
    graphics.pBlendState = &blend;
    addPipeline(device->renderer, &composite, &fog.compositePipeline);
    fog.ready = fog.computePipeline && fog.compositePipeline;
    return fog.ready;
}

#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
bool InitRayFog(LuxForgeDevice* device, LuxForgeFog& fog) {
    if (fog.rayAttempted)
        return fog.rayPipeline && fog.raySet;
    fog.rayAttempted = true;
    ShaderLoadDesc load = {};
    load.mComp.pFileName = "luxfogray.comp";
    addShader(device->renderer, &load, &fog.rayShader);
    if (!fog.rayShader)
        return false;
    const auto entries = device->framesInFlight * LuxForgeDevice::kMaxScenePasses;
    const DescriptorSetDesc set = SRT_SET_DESC(LuxFogRayComputeSrt, Persistent, entries, 0);
    addDescriptorSet(device->renderer, &set, &fog.raySet);
    PipelineDesc pipeline = {};
    pipeline.mType = PIPELINE_TYPE_COMPUTE;
    PIPELINE_LAYOUT_DESC(pipeline, SRT_LAYOUT_DESC(LuxFogRayComputeSrt, Persistent), NULL, NULL,
                         NULL);
    pipeline.mComputeDesc.pShaderProgram = fog.rayShader;
    addPipeline(device->renderer, &pipeline, &fog.rayPipeline);
    return fog.rayPipeline && fog.raySet;
}
#endif

void FillFogConstants(LuxForgeDevice* device, const LuxForgeScene& scene, bool shadows,
                      FogConstants& fog) {
    std::memcpy(fog.right, scene.right, sizeof(fog.right));
    std::memcpy(fog.up, scene.up, sizeof(fog.up));
    std::memcpy(fog.forward, scene.forward, sizeof(fog.forward));
    std::memcpy(fog.colorDensity, scene.fogColorDensity, sizeof(fog.colorDensity));
    std::memcpy(fog.options, scene.fogOptions, sizeof(fog.options));
    std::memcpy(fog.shadowMatrix, scene.shadowMatrix, sizeof(fog.shadowMatrix));
    if (shadows)
        std::memcpy(fog.shadowOptions, scene.shadowOptions, sizeof(fog.shadowOptions));
    fog.composite[0] = 1.f / device->width;
    fog.composite[1] = 1.f / device->height;
    fog.composite[2] = scene.camera[3];
    fog.composite[3] = float(uint32_t(scene.options[2]) & 1u);

    const auto* lights = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(device->lightBuffers[device->frameSlot]->pCpuMappedAddress) +
        static_cast<size_t>(device->currentLight) * LuxForgeDevice::kLightStride);
    for (uint32_t axis = 0; axis < 3; ++axis) {
        fog.lightDirection[axis] = -lights[axis];
        fog.lightRadiance[axis] = std::max(lights[axis + 4], 0.f) * std::max(lights[7], 0.f);
        fog.ambientRadiance[axis] = scene.environment[axis] * scene.environment[3];
    }
}

bool RenderFog(LuxForgeDevice* device, const LuxForgeScene& scene, uint32_t pass, bool water,
               bool shadows, [[maybe_unused]] AccelerationStructure* rayScene = nullptr,
               const uint32_t* virtualShadowPages = nullptr, Texture* resolved = nullptr) {
    if (!(scene.fogColorDensity[3] > 0) || !(scene.fogOptions[0] > 0) || !InitFog(device))
        return false;
    auto& fog = *device->fog;
    const uint32_t entry = device->frameSlot * LuxForgeDevice::kMaxScenePasses + pass;
    const uint32_t offset = entry * kFogUniformStride;
    FogConstants constants = {};
    FillFogConstants(device, scene, shadows, constants);
    if (virtualShadowPages)
        std::memcpy(constants.virtualShadowPages, virtualShadowPages,
                    sizeof(constants.virtualShadowPages));
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
    const bool ray = rayScene && InitRayFog(device, fog);
    if (ray)
        constants.shadowOptions[3] = scene.rayOptions[1];
#endif
    std::memcpy(static_cast<uint8_t*>(fog.constants->pCpuMappedAddress) + offset, &constants,
                sizeof(constants));

    auto& color = water ? device->waterHdr : device->hdr;
    auto& depth = water ? device->waterDepth : device->sceneDepth;
    DescriptorDataRange range = {offset, sizeof(FogConstants), 0};
    DescriptorData compute[3] = {};
    compute[0].ppBuffers = &fog.constants;
    compute[0].pRanges = &range;
    compute[2].ppBuffers = &fog.volume;
    auto* computeSet = fog.computeSet;
    auto* computePipeline = fog.computePipeline;
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
    if (ray) {
        computeSet = fog.raySet;
        computePipeline = fog.rayPipeline;
        compute[0].mIndex = SRT_RES_IDX(LuxFogRayComputeSrt, Persistent, gFog);
        compute[1].mIndex = SRT_RES_IDX(LuxFogRayComputeSrt, Persistent, gRayScene);
        compute[1].ppAccelerationStructures = &rayScene;
        compute[2].mIndex = SRT_RES_IDX(LuxFogRayComputeSrt, Persistent, gFogVolume);
        updateDescriptorSet(device->renderer, entry, computeSet, 3, compute);
    } else
#endif
    {
        Texture* shadow = shadows ? device->shadow.target->pTexture
                                  : device->textures[device->whiteTexture].texture;
        compute[0].mIndex = SRT_RES_IDX(LuxFogComputeSrt, Persistent, gFog);
        compute[1].mIndex = SRT_RES_IDX(LuxFogComputeSrt, Persistent, gFogShadow);
        compute[1].ppTextures = &shadow;
        compute[2].mIndex = SRT_RES_IDX(LuxFogComputeSrt, Persistent, gFogVolume);
        updateDescriptorSet(device->renderer, entry, computeSet, 3, compute);
    }

    DescriptorData composite[4] = {};
    composite[0].mIndex = SRT_RES_IDX(LuxFogCompositeSrt, Persistent, gFog);
    composite[0].ppBuffers = &fog.constants;
    composite[0].pRanges = &range;
    composite[1].mIndex = SRT_RES_IDX(LuxFogCompositeSrt, Persistent, gFogHdr);
    Texture* source = resolved ? resolved : color.target->pTexture;
    composite[1].ppTextures = &source;
    composite[2].mIndex = SRT_RES_IDX(LuxFogCompositeSrt, Persistent, gFogDepth);
    composite[2].ppTextures = &depth.target->pTexture;
    composite[3].mIndex = SRT_RES_IDX(LuxFogCompositeSrt, Persistent, gFogVolume);
    composite[3].ppBuffers = &fog.volume;
    updateDescriptorSet(device->renderer, entry, fog.compositeSet, 4, composite);

    auto* cmd = device->element.pCmds[0];
    // Reuse one grid for this viewport. Both the compute writes and the tone pass
    // reads are serialized on the same graphics queue across all frames.
    BufferBarrier write = {fog.volume, kFogVolumeSample, kFogVolumeWrite};
    BufferBarrier sample = {fog.volume, kFogVolumeWrite, kFogVolumeSample};
    cmdResourceBarrier(cmd, 1, &write, 0, nullptr, 0, nullptr);
    cmdBindPipeline(cmd, computePipeline);
    BindDescriptors(device, cmd, entry, computeSet);
    cmdDispatch(cmd, (LUX_FOG_COLUMNS + 7) / 8, (LUX_FOG_ROWS + 7) / 8, 1);
    cmdResourceBarrier(cmd, 1, &sample, 0, nullptr, 0, nullptr);
    Transition(cmd, depth, RESOURCE_STATE_SHADER_RESOURCE);
    ++device->frameStats.pipelineBinds;
    ++device->frameStats.computeDispatches;

    BindPass(device, false, nullptr);
    BindPipeline(device, cmd, fog.compositePipeline);
    BindDescriptors(device, cmd, entry, fog.compositeSet);
    cmdDraw(cmd, 3, 0);
    device->frameStats.uniformBinds += 2;
    ++device->frameStats.drawCalls;
    ++device->frameStats.volumetricFogPasses;
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
    if (ray)
        ++device->frameStats.rayVolumetricFogPasses;
#endif
    return true;
}
