// Included by LuxForge.cpp. Runtime ray tracing is currently enabled on Vulkan.
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
constexpr uint32_t kRestirMaxAxis = 640;
constexpr uint32_t kRestirCandidateVectors = 9;
constexpr uint32_t kRestirOutputVectors = 5;
constexpr uint32_t kRestirHistoryVectors = 11;
constexpr uint32_t kRestirVectorSize = sizeof(float) * 4;
constexpr uint32_t kRestirEffects = 16u | 32u;

void ShutdownRestir(LuxForgeDevice* device) {
    if (!device->restir)
        return;
    auto& restir = *device->restir;
    for (uint32_t i = 0; i < 4; ++i) {
        if (restir.sets[i])
            removeDescriptorSet(device->renderer, restir.sets[i]);
        if (restir.pipelines[i])
            removePipeline(device->renderer, restir.pipelines[i]);
        if (restir.shaders[i])
            removeShader(device->renderer, restir.shaders[i]);
    }
    if (restir.candidates)
        removeResource(restir.candidates);
    if (restir.output)
        removeResource(restir.output);
    if (restir.temporalParams)
        removeResource(restir.temporalParams);
    for (auto* history : restir.history)
        if (history)
            removeResource(history);
    for (const auto& resources : restir.retired) {
        removeResource(resources.candidates);
        removeResource(resources.output);
        for (auto* history : resources.history)
            removeResource(history);
    }
    delete device->restir;
    device->restir = nullptr;
}

void CollectRestir(LuxForgeDevice* device) {
    if (!device->restir)
        return;
    auto& retired = device->restir->retired;
    retired.erase(std::remove_if(retired.begin(), retired.end(),
                                 [device](const auto& resources) {
                                     if (resources.frame > device->completedFrames)
                                         return false;
                                     removeResource(resources.candidates);
                                     removeResource(resources.output);
                                     for (auto* history : resources.history)
                                         removeResource(history);
                                     return true;
                                 }),
                  retired.end());
}

bool InitRestir(LuxForgeDevice* device, LuxForgeRestir& restir) {
    if (restir.initAttempted)
        return restir.pipelines[0] && restir.pipelines[1] && restir.sets[0] && restir.sets[1];
    restir.initAttempted = true;

    const uint32_t entries = device->framesInFlight * LuxForgeDevice::kMaxScenePasses;
    BufferLoadDesc constants = {};
    constants.ppBuffer = &restir.temporalParams;
    constants.mDesc.mSize = uint64_t(entries) * LuxForgeDevice::kSceneStride;
    constants.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    constants.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    constants.mDesc.mDescriptors = UniformBufferDescriptor;
    constants.mDesc.pName = "LuxForge temporal ray parameters";
    addResource(&constants, nullptr);
    waitForAllResourceLoads();
    if (!restir.temporalParams || !restir.temporalParams->pCpuMappedAddress)
        return false;
    const DescriptorSetDesc initSet = SRT_SET_DESC(LuxRestirInitSrt, Persistent, entries, 0);
    const DescriptorSetDesc resolveSet = SRT_SET_DESC(LuxRestirResolveSrt, Persistent, entries, 0);
    addDescriptorSet(device->renderer, &initSet, &restir.sets[0]);
    addDescriptorSet(device->renderer, &resolveSet, &restir.sets[1]);

    ShaderLoadDesc load = {};
    load.mComp.pFileName = "luxrestirinit.comp";
    addShader(device->renderer, &load, &restir.shaders[0]);
    load.mComp.pFileName = "luxrestirresolve.comp";
    addShader(device->renderer, &load, &restir.shaders[1]);
    if (!restir.shaders[0] || !restir.shaders[1])
        return false;

    PipelineDesc init = {};
    init.mType = PIPELINE_TYPE_COMPUTE;
    PIPELINE_LAYOUT_DESC(init, SRT_LAYOUT_DESC(LuxRestirInitSrt, Persistent),
                         SRT_LAYOUT_DESC(LuxRestirInitSrt, PerFrame), NULL, NULL);
    init.mComputeDesc.pShaderProgram = restir.shaders[0];
    addPipeline(device->renderer, &init, &restir.pipelines[0]);

    PipelineDesc resolve = {};
    resolve.mType = PIPELINE_TYPE_COMPUTE;
    PIPELINE_LAYOUT_DESC(resolve, SRT_LAYOUT_DESC(LuxRestirResolveSrt, Persistent),
                         SRT_LAYOUT_DESC(LuxRestirResolveSrt, PerFrame), NULL, NULL);
    resolve.mComputeDesc.pShaderProgram = restir.shaders[1];
    addPipeline(device->renderer, &resolve, &restir.pipelines[1]);
    return restir.pipelines[0] && restir.pipelines[1] && restir.sets[0] && restir.sets[1];
}

bool InitRayWavelet(LuxForgeDevice* device, LuxForgeRestir& restir) {
    if (restir.pipelines[3])
        return true;
    const uint32_t entries = device->framesInFlight * LuxForgeDevice::kMaxScenePasses;
    const DescriptorSetDesc set = SRT_SET_DESC(LuxRayWaveletSrt, Persistent, entries, 0);
    for (unsigned i = 2; i < 4; ++i) {
        if (!restir.sets[i])
            addDescriptorSet(device->renderer, &set, &restir.sets[i]);
        ShaderLoadDesc load = {};
        load.mComp.pFileName = i == 2 ? "luxraywavelet.comp" : "luxraywaveletwide.comp";
        if (!restir.shaders[i])
            addShader(device->renderer, &load, &restir.shaders[i]);
        if (!restir.sets[i] || !restir.shaders[i])
            return false;
        PipelineDesc pipeline = {};
        pipeline.mType = PIPELINE_TYPE_COMPUTE;
        PIPELINE_LAYOUT_DESC(pipeline, SRT_LAYOUT_DESC(LuxRayWaveletSrt, Persistent), NULL, NULL,
                             NULL);
        pipeline.mComputeDesc.pShaderProgram = restir.shaders[i];
        if (!restir.pipelines[i])
            addPipeline(device->renderer, &pipeline, &restir.pipelines[i]);
        if (!restir.pipelines[i])
            return false;
    }
    return true;
}

Buffer* CreateRestirBuffer(uint32_t elements, ResourceState sampleState, const char* name) {
    Buffer* buffer = nullptr;
    BufferLoadDesc load = {};
    load.ppBuffer = &buffer;
    load.mDesc.mSize = static_cast<uint64_t>(elements) * kRestirVectorSize;
    load.mDesc.mElementCount = elements;
    load.mDesc.mStructStride = kRestirVectorSize;
    load.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
    load.mDesc.mDescriptors =
        static_cast<DescriptorType>(DESCRIPTOR_TYPE_RW_BUFFER | DESCRIPTOR_TYPE_BUFFER);
    load.mDesc.mStartState = sampleState;
    load.mDesc.pName = name;
    addResource(&load, nullptr);
    return buffer;
}

bool ResizeRestir(LuxForgeDevice* device, LuxForgeRestir& restir, uint32_t cells,
                  bool fullHistory) {
    if (cells <= restir.capacity && restir.fullHistory == fullHistory)
        return true;
    Buffer* candidates = CreateRestirBuffer(cells * kRestirCandidateVectors,
                                            RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            "LuxForge ReSTIR candidate reservoirs");
    Buffer* output =
        CreateRestirBuffer(cells * kRestirOutputVectors, RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           "LuxForge ReSTIR resolved lighting");
    Buffer* history[2] = {};
    for (auto& buffer : history)
        buffer =
            CreateRestirBuffer(fullHistory ? cells * kRestirHistoryVectors : 1u,
                               RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "LuxForge ray history");
    if (!candidates || !output || !history[0] || !history[1]) {
        for (auto* buffer : history)
            if (buffer)
                removeResource(buffer);
        if (candidates)
            removeResource(candidates);
        if (output)
            removeResource(output);
        return false;
    }
    if (restir.candidates)
        restir.retired.push_back({restir.candidates,
                                  restir.output,
                                  {restir.history[0], restir.history[1]},
                                  device->frameNumber + 1});
    std::copy_n(history, 2, restir.history);
    restir.frame = 0;
    restir.candidates = candidates;
    restir.output = output;
    restir.capacity = cells;
    restir.fullHistory = fullHistory;
    return true;
}

Buffer* DispatchRestir(LuxForgeDevice* device, const LuxForgeScene& scene, uint32_t pass,
                       const ForgeRayScene& rays, Texture* specularEnvironment) {
    const uint32_t effects = static_cast<uint32_t>(scene.rayOptions[0]);
    const bool denoise = scene.rayDenoiser && (effects & (2u | 8u | 32u));
    if (!(effects & kRestirEffects) && !denoise)
        return nullptr;
    if (!rays.scene || !rays.instances || !rays.triangles || !specularEnvironment)
        return nullptr;
    if (!device->restir)
        device->restir = new LuxForgeRestir;
    auto& restir = *device->restir;
    const uint32_t spacing = std::max(
        2u, (std::max(device->width, device->height) + kRestirMaxAxis - 1) / kRestirMaxAxis);
    const uint32_t width = (device->width + spacing - 1) / spacing;
    const uint32_t height = (device->height + spacing - 1) / spacing;
    const bool temporalReuse = scene.temporalRestir && (effects & kRestirEffects);
    if (!InitRestir(device, restir) ||
        !ResizeRestir(device, restir, width * height, temporalReuse || denoise))
        return nullptr;

    const uint32_t entry = device->frameSlot * LuxForgeDevice::kMaxScenePasses + pass;
    DescriptorDataRange range = {pass * LuxForgeDevice::kSceneStride, LuxForgeDevice::kSceneStride,
                                 0};
    const uint64_t frame = device->frameNumber + 1;
    const uint32_t signature =
        effects | (denoise ? 512u : 0u) | (scene.temporalRestir ? 1024u : 0u);
    const uint32_t next = restir.index ^ 1u;
    std::array<float, 24> parameters{};
    double distanceSquared = 0, alignment = 0;
    for (unsigned i = 0; i < 3; ++i) {
        // Stored primary/secondary hit points use the camera origin of their frame.
        const double displacement = scene.temporalOrigin[i] - restir.origin[i];
        parameters[16 + i] = static_cast<float>(displacement);
        distanceSquared += displacement * displacement;
        alignment += scene.forward[i] * restir.forward[i];
    }
    if (denoise && !InitRayWavelet(device, restir))
        return nullptr;
    const auto* light = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(device->lightBuffers[device->frameSlot]->pCpuMappedAddress) +
        device->currentLight * LuxForgeDevice::kLightStride);
    std::array<float, 12> illumination{};
    for (unsigned i = 0; i < 3; ++i) {
        illumination[i] = light[i];
        illumination[3 + i] = std::log2(1.f + std::max(0.f, light[4 + i] * light[7]));
        illumination[6 + i] =
            std::log2(1.f + std::max(0.f, scene.environment[i] * scene.environment[3]));
    }
    illumination[9] = scene.skyLighting[0];
    illumination[10] = scene.atmosphere[2];
    illumination[11] = scene.rayOptions[2];
    bool relit = false;
    for (unsigned i = 0; i < illumination.size(); ++i)
        relit |= std::abs(illumination[i] - restir.illumination[i]) > .18f;
    restir.illumination = illumination;
    const bool history =
        !relit && restir.fullHistory && restir.frame && restir.frame + 1 == frame && pass == 0 &&
        restir.view == scene.temporalView && restir.environment == scene.environmentMap &&
        device->width == restir.width && device->height == restir.height &&
        restir.effects == signature && distanceSquared < 576 && alignment > .75;
    std::copy(restir.matrix.begin(), restir.matrix.end(), parameters.begin());
    parameters[19] = history ? 1.f : 0.f;
    parameters[20] = (scene.temporalRestir || denoise) ? float(frame % 65536) : 0.f;
    parameters[21] = temporalReuse ? 1.f : 0.f;
    parameters[22] = denoise ? 1.f : 0.f;
    DescriptorDataRange paramRange{entry * LuxForgeDevice::kSceneStride,
                                   LuxForgeDevice::kSceneStride, 0};
    std::memcpy(static_cast<uint8_t*>(restir.temporalParams->pCpuMappedAddress) +
                    paramRange.mOffset,
                parameters.data(), sizeof(parameters));
    auto* acceleration = rays.scene;
    auto* instanceData = rays.instances;
    auto* triangleData = rays.triangles;
    auto* fallback = device->textures[device->whiteTexture].texture;
    const bool dynamicSky = scene.skyLighting[0] > .5f;
    auto* gloss = dynamicSky ? device->skyCache.stages[2].output.target->pTexture : fallback;
    auto* harmonics = dynamicSky ? device->skyCache.stages[1].output.target->pTexture : fallback;
    DescriptorData init[9] = {};
    init[7].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRaySkyGloss);
    init[7].ppTextures = &gloss;
    init[8].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRaySkySH);
    init[8].ppTextures = &harmonics;
    init[0].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gScene);
    init[0].ppBuffers = &device->sceneUniforms[device->frameSlot];
    init[0].pRanges = &range;
    init[1].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRayScene);
    init[1].ppAccelerationStructures = &acceleration;
    init[2].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRayInstances);
    init[2].ppBuffers = &instanceData;
    init[3].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRayTriangles);
    init[3].ppBuffers = &triangleData;
    init[4].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRestirEnvironment);
    init[4].ppTextures = &specularEnvironment;
    init[5].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRestirCandidates);
    init[5].ppBuffers = &restir.candidates;
    init[6].mIndex = SRT_RES_IDX(LuxRestirInitSrt, Persistent, gRayHistoryParams);
    init[6].ppBuffers = &restir.temporalParams;
    init[6].pRanges = &paramRange;
    updateDescriptorSet(device->renderer, entry, restir.sets[0], std::size(init), init);

    DescriptorData resolve[9] = {};
    resolve[0].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gScene);
    resolve[0].ppBuffers = &device->sceneUniforms[device->frameSlot];
    resolve[0].pRanges = &range;
    resolve[1].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gRayScene);
    resolve[1].ppAccelerationStructures = &acceleration;
    resolve[2].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gRayInstances);
    resolve[2].ppBuffers = &instanceData;
    resolve[3].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gRayTriangles);
    resolve[3].ppBuffers = &triangleData;
    resolve[4].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gRestirCandidates);
    resolve[4].ppBuffers = &restir.candidates;
    resolve[5].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gRestirResult);
    resolve[5].ppBuffers = &restir.output;
    resolve[6].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gRayHistoryParams);
    resolve[6].ppBuffers = &restir.temporalParams;
    resolve[6].pRanges = &paramRange;
    resolve[7].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gPreviousReservoirs);
    resolve[7].ppBuffers = &restir.history[restir.index];
    resolve[8].mIndex = SRT_RES_IDX(LuxRestirResolveSrt, Persistent, gNextReservoirs);
    resolve[8].ppBuffers = &restir.history[next];
    updateDescriptorSet(device->renderer, entry, restir.sets[1], std::size(resolve), resolve);

    Cmd* cmd = device->element.pCmds[0];
    BufferBarrier write[3] = {
        {restir.history[next], RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
         RESOURCE_STATE_UNORDERED_ACCESS},
        {restir.candidates, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
         RESOURCE_STATE_UNORDERED_ACCESS},
        {restir.output, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}};
    cmdResourceBarrier(cmd, 3, write, 0, nullptr, 0, nullptr);
    const uint32_t lighting =
        device->frameSlot * LuxForgeDevice::kMaxLightBlocks + device->currentLight;
    BindPipeline(device, cmd, restir.pipelines[0]);
    BindDescriptors(device, cmd, entry, restir.sets[0]);
    BindDescriptors(device, cmd, lighting, device->lightSet);
    cmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    BufferBarrier readCandidates = {restir.candidates, RESOURCE_STATE_UNORDERED_ACCESS,
                                    RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    cmdResourceBarrier(cmd, 1, &readCandidates, 0, nullptr, 0, nullptr);
    BindPipeline(device, cmd, restir.pipelines[1]);
    BindDescriptors(device, cmd, entry, restir.sets[1]);
    BindDescriptors(device, cmd, lighting, device->lightSet);
    cmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    BufferBarrier readHistory = {restir.history[next], RESOURCE_STATE_UNORDERED_ACCESS,
                                 RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    cmdResourceBarrier(cmd, 1, &readHistory, 0, nullptr, 0, nullptr);
    if (denoise) {
        // The candidates are consumed by resampling. Reuse their allocation as the
        // first wavelet pass' scratch image, preserving independent diffuse/specular histories.
        BufferBarrier toScratch{restir.candidates, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                RESOURCE_STATE_UNORDERED_ACCESS};
        cmdResourceBarrier(cmd, 1, &toScratch, 0, nullptr, 0, nullptr);
        for (unsigned step = 0; step < 2; ++step) {
            Buffer* source = step ? restir.candidates : restir.history[next];
            Buffer* target = step ? restir.output : restir.candidates;
            DescriptorData filter[4] = {};
            filter[0].mIndex = SRT_RES_IDX(LuxRayWaveletSrt, Persistent, gScene);
            filter[0].ppBuffers = &device->sceneUniforms[device->frameSlot];
            filter[0].pRanges = &range;
            filter[1].mIndex = SRT_RES_IDX(LuxRayWaveletSrt, Persistent, gFeatures);
            filter[1].ppBuffers = &restir.history[next];
            filter[2].mIndex = SRT_RES_IDX(LuxRayWaveletSrt, Persistent, gSignal);
            filter[2].ppBuffers = &source;
            filter[3].mIndex = SRT_RES_IDX(LuxRayWaveletSrt, Persistent, gFiltered);
            filter[3].ppBuffers = &target;
            updateDescriptorSet(device->renderer, entry, restir.sets[2 + step], 4, filter);
            BindPipeline(device, cmd, restir.pipelines[2 + step]);
            BindDescriptors(device, cmd, entry, restir.sets[2 + step]);
            cmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
            if (step == 0) {
                BufferBarrier nextStep[] = {readCandidates,
                                            {restir.output, RESOURCE_STATE_UNORDERED_ACCESS,
                                             RESOURCE_STATE_UNORDERED_ACCESS}};
                cmdResourceBarrier(cmd, 2, nextStep, 0, nullptr, 0, nullptr);
            }
        }
        device->frameStats.computeDispatches += 2;
    }
    BufferBarrier readOutput{restir.output, RESOURCE_STATE_UNORDERED_ACCESS,
                             RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    cmdResourceBarrier(cmd, 1, &readOutput, 0, nullptr, 0, nullptr);
    device->frameStats.computeDispatches += 2;
    device->frameStats.uniformBinds += 2; // The two persistent sets each bind the scene cbuffer.
    device->frameStats.restirPasses += (effects & kRestirEffects) != 0;
    device->frameStats.temporalRestirPasses +=
        history && scene.temporalRestir && (effects & kRestirEffects);
    device->frameStats.rayDenoiserPasses += denoise;
    restir.index = next;
    restir.frame = pass == 0 ? frame : 0;
    restir.view = scene.temporalView;
    restir.width = device->width;
    restir.height = device->height;
    restir.effects = signature;
    restir.environment = scene.environmentMap;
    std::copy_n(scene.viewProjection, 16, restir.matrix.begin());
    std::copy_n(scene.temporalOrigin, 3, restir.origin.begin());
    std::copy_n(scene.forward, 3, restir.forward.begin());
    return restir.output;
}
#else
void ShutdownRestir(LuxForgeDevice*) {}

void CollectRestir(LuxForgeDevice*) {}
#endif
