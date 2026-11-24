// Included from LuxForge.cpp with the scene target and descriptor helpers in scope.
bool InitTemporal(LuxForgeDevice* device) {
    auto& temporal = device->temporal;
    if (temporal.ready)
        return true;
    if (temporal.attempted)
        return false;
    temporal.attempted = true;
    temporal.shader = LoadShader(device, "luxfullscreen.vert", "luxtemporal.frag");
    if (!temporal.shader)
        return false;
    const DescriptorSetDesc layout =
        SRT_SET_DESC(LuxTemporalSrt, Persistent, device->framesInFlight, 0);
    addDescriptorSet(device->renderer, &layout, &temporal.set);

    BufferLoadDesc constants = {};
    constants.ppBuffer = &temporal.constants;
    constants.mDesc.mSize = device->framesInFlight * LuxForgeDevice::kSceneStride;
    constants.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    constants.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    constants.mDesc.mDescriptors = UniformBufferDescriptor;
    constants.mDesc.pName = "LuxForge temporal constants";
    addResource(&constants, nullptr);
    waitForAllResourceLoads();
    if (!temporal.set || !temporal.constants || !temporal.constants->pCpuMappedAddress)
        return false;

    PipelineDesc desc = {};
    desc.mType = PIPELINE_TYPE_GRAPHICS;
    PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(LuxTemporalSrt, Persistent), NULL, NULL, NULL);
    auto& graphics = desc.mGraphicsDesc;
    RasterizerStateDesc raster = {};
    raster.mCullMode = CULL_MODE_NONE;
    DepthStateDesc noDepth = {};
    TinyImageFormat formats[] = {kHdrFormat, TinyImageFormat_R32_SFLOAT};
    graphics.pShaderProgram = temporal.shader;
    graphics.pRasterizerState = &raster;
    graphics.pDepthState = &noDepth;
    graphics.pColorFormats = formats;
    graphics.mRenderTargetCount = 2;
    graphics.mSampleCount = SAMPLE_COUNT_1;
    graphics.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    addPipeline(device->renderer, &desc, &temporal.pipeline);
    temporal.ready = temporal.pipeline != nullptr;
    return temporal.ready;
}

bool PrepareTemporal(LuxForgeDevice* device) {
    auto& temporal = device->temporal;
    if (!InitTemporal(device))
        return false;
    if (temporal.color[0].target)
        return true;
    for (unsigned i = 0; i < 2; ++i) {
        if (!AddSceneTarget(device, temporal.color[i], device->width, device->height, kHdrFormat, 8,
                            "LuxForge temporal color") ||
            !AddSceneTarget(device, temporal.depth[i], device->width, device->height,
                            TinyImageFormat_R32_SFLOAT, 4, "LuxForge temporal depth")) {
            RetireTemporal(device);
            return false;
        }
    }
    return true;
}

float RadicalInverse(unsigned index, unsigned base) {
    float value = 0, place = 1;
    while (index) {
        place /= float(base);
        value += float(index % base) * place;
        index /= base;
    }
    return value;
}

Texture* ResolveTemporal(LuxForgeDevice* device, Cmd* cmd, const LuxForgeScene& scene,
                         LuxForgeDevice::SceneTarget& color, LuxForgeDevice::SceneTarget& depth,
                         const std::array<float, 2>& jitter, uint64_t frame) {
    auto& temporal = device->temporal;
    const auto old = temporal.index;
    const unsigned output = old ^ 1u;
    std::array<double, 3> displacement{};
    double distanceSquared = 0, alignment = 0;
    for (unsigned axis = 0; axis < 3; ++axis) {
        displacement[axis] = scene.temporalOrigin[axis] - temporal.origin[axis];
        distanceSquared += displacement[axis] * displacement[axis];
        alignment += scene.forward[axis] * temporal.forward[axis];
    }
    const bool history = temporal.frame != 0 && temporal.frame + 1 == frame &&
                         temporal.view == scene.temporalView &&
                         temporal.environment == scene.environmentMap &&
                         distanceSquared < 24.0 * 24.0 && alignment > 0.75;
    std::array<float, 36> values{};
    if (history) {
        std::copy(temporal.matrix.begin(), temporal.matrix.end(), values.begin());
        for (unsigned row = 0; row < 4; ++row) {
            double translation = values[12 + row];
            for (unsigned axis = 0; axis < 3; ++axis)
                translation += double(values[4 * axis + row]) * displacement[axis];
            values[12 + row] = float(translation);
        }
    }
    std::copy_n(scene.right, 4, values.begin() + 16);
    std::copy_n(scene.up, 4, values.begin() + 20);
    std::copy_n(scene.forward, 4, values.begin() + 24);
    values[28] = float(device->width);
    values[29] = float(device->height);
    values[30] = jitter[0];
    values[31] = jitter[1];
    values[32] = history ? 1.f : 0.f;

    const uint32_t offset = device->frameSlot * LuxForgeDevice::kSceneStride;
    std::memcpy(static_cast<uint8_t*>(temporal.constants->pCpuMappedAddress) + offset,
                values.data(), sizeof(values));
    DescriptorDataRange range{offset, LuxForgeDevice::kSceneStride, 0};
    DescriptorData params[5] = {};
    params[0].mIndex = SRT_RES_IDX(LuxTemporalSrt, Persistent, gTemporal);
    params[0].pRanges = &range;
    params[0].ppBuffers = &temporal.constants;
    Texture* textures[] = {color.target->pTexture, depth.target->pTexture,
                           (history ? temporal.color[old] : color).target->pTexture,
                           (history ? temporal.depth[old] : depth).target->pTexture};
    const uint32_t slots[] = {SRT_RES_IDX(LuxTemporalSrt, Persistent, gCurrentColor),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gCurrentDepth),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gPreviousColor),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gPreviousDepth)};
    for (unsigned i = 0; i < 4; ++i) {
        params[i + 1].mIndex = slots[i];
        params[i + 1].ppTextures = &textures[i];
    }
    updateDescriptorSet(device->renderer, device->frameSlot, temporal.set, 5, params);

    Transition(cmd, depth, RESOURCE_STATE_SHADER_RESOURCE);
    Transition(cmd, temporal.color[output], RESOURCE_STATE_RENDER_TARGET);
    Transition(cmd, temporal.depth[output], RESOURCE_STATE_RENDER_TARGET);
    BindRenderTargetsDesc targets = {};
    targets.mRenderTargetCount = 2;
    targets.mRenderTargets[0].pRenderTarget = temporal.color[output].target;
    targets.mRenderTargets[1].pRenderTarget = temporal.depth[output].target;
    for (unsigned i = 0; i < 2; ++i) {
        targets.mRenderTargets[i].mLoadAction = LOAD_ACTION_CLEAR;
        targets.mRenderTargets[i].mStoreAction = STORE_ACTION_STORE;
    }
    cmdBindRenderTargets(cmd, &targets);
    cmdSetViewport(cmd, 0, 0, float(device->width), float(device->height), 0, 1);
    cmdSetScissor(cmd, 0, 0, device->width, device->height);
    BindPipeline(device, cmd, temporal.pipeline);
    BindDescriptors(device, cmd, device->frameSlot, temporal.set);
    cmdDraw(cmd, 3, 0);
    cmdBindRenderTargets(cmd, nullptr);
    Transition(cmd, temporal.color[output], RESOURCE_STATE_SHADER_RESOURCE);
    Transition(cmd, temporal.depth[output], RESOURCE_STATE_SHADER_RESOURCE);

    temporal.color[output].lastUsedFrame = temporal.depth[output].lastUsedFrame = frame;
    if (history)
        temporal.color[old].lastUsedFrame = temporal.depth[old].lastUsedFrame = frame;
    temporal.index = output;
    temporal.frame = frame;
    temporal.view = scene.temporalView;
    temporal.environment = scene.environmentMap;
    std::copy_n(scene.temporalOrigin, 3, temporal.origin.begin());
    std::copy_n(scene.forward, 3, temporal.forward.begin());
    std::copy_n(scene.viewProjection, 16, temporal.matrix.begin());
    ++device->frameStats.drawCalls;
    ++device->frameStats.temporalResolvePasses;
    if (history)
        ++device->frameStats.temporalHistoryPasses;
    return temporal.color[output].target->pTexture;
}
