// Included by LuxForge.cpp. Runtime ray tracing is currently enabled on Vulkan.
#if defined(VULKAN) && defined(ENABLE_RAYTRACING)
constexpr uint32_t kRestirMaxAxis = 640;
constexpr uint32_t kRestirCandidateVectors = 8;
constexpr uint32_t kRestirOutputVectors = 4;
constexpr uint32_t kRestirVectorSize = sizeof(float) * 4;
constexpr uint32_t kRestirEffects = 16u | 32u;

void ShutdownRestir(LuxForgeDevice* device) {
    if (!device->restir)
        return;
    auto& restir = *device->restir;
    for (uint32_t i = 0; i < 2; ++i) {
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
    for (const auto& resources : restir.retired) {
        removeResource(resources.candidates);
        removeResource(resources.output);
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
                                     return true;
                                 }),
                  retired.end());
}

bool InitRestir(LuxForgeDevice* device, LuxForgeRestir& restir) {
    if (restir.initAttempted)
        return restir.pipelines[0] && restir.pipelines[1] && restir.sets[0] && restir.sets[1];
    restir.initAttempted = true;

    const uint32_t entries = device->framesInFlight * LuxForgeDevice::kMaxScenePasses;
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

bool ResizeRestir(LuxForgeDevice* device, LuxForgeRestir& restir, uint32_t cells) {
    if (cells <= restir.capacity)
        return true;
    Buffer* candidates = CreateRestirBuffer(cells * kRestirCandidateVectors,
                                            RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                            "LuxForge ReSTIR candidate reservoirs");
    Buffer* output =
        CreateRestirBuffer(cells * kRestirOutputVectors, RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           "LuxForge ReSTIR resolved lighting");
    if (!candidates || !output) {
        if (candidates)
            removeResource(candidates);
        if (output)
            removeResource(output);
        return false;
    }
    if (restir.candidates)
        restir.retired.push_back({restir.candidates, restir.output, device->frameNumber + 1});
    restir.candidates = candidates;
    restir.output = output;
    restir.capacity = cells;
    return true;
}

Buffer* DispatchRestir(LuxForgeDevice* device, const LuxForgeScene& scene, uint32_t pass,
                       const ForgeRayScene& rays, Texture* specularEnvironment) {
    if ((static_cast<uint32_t>(scene.rayOptions[0]) & kRestirEffects) == 0)
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
    if (!InitRestir(device, restir) || !ResizeRestir(device, restir, width * height))
        return nullptr;

    const uint32_t entry = device->frameSlot * LuxForgeDevice::kMaxScenePasses + pass;
    DescriptorDataRange range = {pass * LuxForgeDevice::kSceneStride, LuxForgeDevice::kSceneStride,
                                 0};
    auto* acceleration = rays.scene;
    auto* instanceData = rays.instances;
    auto* triangleData = rays.triangles;
    DescriptorData init[6] = {};
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
    updateDescriptorSet(device->renderer, entry, restir.sets[0], 6, init);

    DescriptorData resolve[6] = {};
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
    updateDescriptorSet(device->renderer, entry, restir.sets[1], 6, resolve);

    Cmd* cmd = device->element.pCmds[0];
    BufferBarrier write[2] = {
        {restir.candidates, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
         RESOURCE_STATE_UNORDERED_ACCESS},
        {restir.output, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS}};
    cmdResourceBarrier(cmd, 2, write, 0, nullptr, 0, nullptr);
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

    BufferBarrier readOutput = {restir.output, RESOURCE_STATE_UNORDERED_ACCESS,
                                RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    cmdResourceBarrier(cmd, 1, &readOutput, 0, nullptr, 0, nullptr);
    device->frameStats.computeDispatches += 2;
    device->frameStats.uniformBinds += 2; // The two persistent sets each bind the scene cbuffer.
    ++device->frameStats.restirPasses;
    return restir.output;
}
#else
void ShutdownRestir(LuxForgeDevice*) {}

void CollectRestir(LuxForgeDevice*) {}
#endif
