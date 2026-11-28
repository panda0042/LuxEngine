// Each frame slot owns immutable descriptors and constants for every recorded scene.
// GPU attachments are reused in queue order, including when two scenes share a frame.
using ScreenState = LuxForgeDevice::ScreenState;

enum class ScreenBlock : uint32_t { Surface = 1, Capture, Quarter, Sixteenth };
constexpr uint32_t kSceneUniformBanks =
    5; // existing scene block followed by the four screen blocks

uint32_t ScreenOffset(uint32_t pass, ScreenBlock block) {
    return (static_cast<uint32_t>(block) * LuxForgeDevice::kMaxScenePasses + pass) *
           LuxForgeDevice::kSceneStride;
}

void WriteScreenBlock(LuxForgeDevice* device, uint32_t pass, ScreenBlock block,
                      const std::array<float, 4>& values) {
    auto* data = static_cast<uint8_t*>(device->sceneUniforms[device->frameSlot]->pCpuMappedAddress);
    std::memcpy(data + ScreenOffset(pass, block), values.data(), sizeof values);
}

void RetireScreen(LuxForgeDevice* device) {
    auto& state = device->screen;
    RetireTarget(device, state.color);
    RetireTarget(device, state.depth);
    for (auto& level : state.levels)
        RetireTarget(device, level);
}

void ShutdownScreen(LuxForgeDevice* device) {
    auto& state = device->screen;
    RetireScreen(device);
    if (state.set)
        removeDescriptorSet(device->renderer, state.set);
    if (state.pipeline)
        removePipeline(device->renderer, state.pipeline);
    if (state.shader)
        removeShader(device->renderer, state.shader);
    state = {};
}

bool InitDepthReduction(LuxForgeDevice* device) {
    auto& state = device->screen;
    if (state.attempted)
        return state.pipeline && state.set;
    state.attempted = true;
    state.shader = LoadShader(device, "luxfullscreen.vert", "luxdepthreduce.frag");
    if (!state.shader)
        return false;
    const auto entries = 2 * device->framesInFlight * LuxForgeDevice::kMaxScenePasses;
    const DescriptorSetDesc layout = SRT_SET_DESC(LuxDepthReduceSrt, Persistent, entries, 0);
    addDescriptorSet(device->renderer, &layout, &state.set);
    if (!state.set)
        return false;
    PipelineDesc desc = {};
    desc.mType = PIPELINE_TYPE_GRAPHICS;
    PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(LuxDepthReduceSrt, Persistent), NULL, NULL, NULL);
    auto& graphics = desc.mGraphicsDesc;
    RasterizerStateDesc raster = {};
    raster.mCullMode = CULL_MODE_NONE;
    DepthStateDesc noDepth = {};
    TinyImageFormat format = TinyImageFormat_R32_SFLOAT;
    graphics.pShaderProgram = state.shader;
    graphics.pRasterizerState = &raster;
    graphics.pDepthState = &noDepth;
    graphics.pColorFormats = &format;
    graphics.mRenderTargetCount = 1;
    graphics.mSampleCount = SAMPLE_COUNT_1;
    graphics.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    addPipeline(device->renderer, &desc, &state.pipeline);
    return state.pipeline != nullptr;
}

bool PrepareScreen(LuxForgeDevice* device, bool hierarchy) {
    auto& state = device->screen;
    if (state.color.target && (state.color.target->mWidth != device->width ||
                               state.color.target->mHeight != device->height))
        RetireScreen(device);
    if (!state.color.target &&
        (!AddSceneTarget(device, state.color, device->width, device->height, kHdrFormat, 8,
                         "LuxForge opaque radiance snapshot") ||
         !AddSceneTarget(device, state.depth, device->width, device->height, kDepthFormat, 4,
                         "LuxForge opaque depth snapshot"))) {
        RetireScreen(device);
        return false;
    }
    if (!hierarchy || state.levels[1].target)
        return true;
    if (!InitDepthReduction(device))
        return false;
    uint32_t w = device->width, h = device->height;
    for (auto& level : state.levels) {
        w = (w + 3) / 4;
        h = (h + 3) / 4;
        if (!AddSceneTarget(device, level, w, h, TinyImageFormat_R32_SFLOAT, 4,
                            "LuxForge hierarchical minimum depth")) {
            RetireScreen(device);
            return false;
        }
    }
    return true;
}

float ShadowPenumbraScale(const LuxForgeScene& scene) {
    if (!scene.shadowResolution || !(scene.shadowLightAngularRadius > 0.f))
        return 0.f;
    const auto* m = scene.shadowMatrix;
    const float uvPerMetre = 0.5f * std::hypot(m[0], m[4], m[8]);
    const float depthPerMetre = std::hypot(m[2], m[6], m[10]);
    return depthPerMetre > 1e-8f ? std::tan(std::min(scene.shadowLightAngularRadius, 0.25f)) *
                                       scene.shadowResolution * uvPerMetre / depthPerMetre
                                 : 0.f;
}

void SetScreenInputs(LuxForgeDevice* device, uint32_t entry, DescriptorSet* set, uint32_t pass,
                     bool snapshot, bool hierarchy, Texture* fallback, ScreenBlock block) {
    auto& state = device->screen;
    Texture* textures[] = {snapshot ? state.depth.target->pTexture : fallback,
                           snapshot ? state.color.target->pTexture : fallback,
                           hierarchy ? state.levels[0].target->pTexture : fallback,
                           hierarchy ? state.levels[1].target->pTexture : fallback};
    const uint32_t slots[] = {SRT_RES_IDX(LuxSceneSrt, Persistent, gScreenDepth),
                              SRT_RES_IDX(LuxSceneSrt, Persistent, gScreenColor),
                              SRT_RES_IDX(LuxSceneSrt, Persistent, gDepthQuarter),
                              SRT_RES_IDX(LuxSceneSrt, Persistent, gDepthSixteenth)};
    DescriptorData params[5] = {};
    DescriptorDataRange range{ScreenOffset(pass, block), LuxForgeDevice::kSceneStride, 0};
    params[0].mIndex = SRT_RES_IDX(LuxSceneSrt, Persistent, gScreenLighting);
    params[0].pRanges = &range;
    params[0].ppBuffers = &device->sceneUniforms[device->frameSlot];
    for (uint32_t i = 0; i < 4; ++i) {
        params[i + 1].mIndex = slots[i];
        params[i + 1].ppTextures = &textures[i];
    }
    updateDescriptorSet(device->renderer, entry, set, 5, params);
}

void BuildDepthHierarchy(LuxForgeDevice* device, Cmd* cmd, uint32_t pass, uint64_t frame) {
    auto& state = device->screen;
    auto* source = state.depth.target;
    for (uint32_t i = 0; i < 2; ++i) {
        auto& output = state.levels[i];
        const auto block = i ? ScreenBlock::Sixteenth : ScreenBlock::Quarter;
        WriteScreenBlock(device, pass, block,
                         {float(source->mWidth), float(source->mHeight), 0, 0});
        DescriptorDataRange range{ScreenOffset(pass, block), LuxForgeDevice::kSceneStride, 0};
        DescriptorData params[2] = {};
        params[0].mIndex = SRT_RES_IDX(LuxDepthReduceSrt, Persistent, gReduce);
        params[0].pRanges = &range;
        params[0].ppBuffers = &device->sceneUniforms[device->frameSlot];
        params[1].mIndex = SRT_RES_IDX(LuxDepthReduceSrt, Persistent, gReduceSource);
        params[1].ppTextures = &source->pTexture;
        const uint32_t entry = (device->frameSlot * LuxForgeDevice::kMaxScenePasses + pass) * 2 + i;
        updateDescriptorSet(device->renderer, entry, state.set, 2, params);
        Transition(cmd, output, RESOURCE_STATE_RENDER_TARGET);
        BindRenderTargetsDesc bind = {};
        bind.mRenderTargetCount = 1;
        bind.mRenderTargets[0].pRenderTarget = output.target;
        bind.mRenderTargets[0].mLoadAction = LOAD_ACTION_CLEAR;
        bind.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
        cmdBindRenderTargets(cmd, &bind);
        cmdSetViewport(cmd, 0, 0, float(output.target->mWidth), float(output.target->mHeight), 0,
                       1);
        cmdSetScissor(cmd, 0, 0, output.target->mWidth, output.target->mHeight);
        BindPipeline(device, cmd, state.pipeline);
        BindDescriptors(device, cmd, entry, state.set);
        cmdDraw(cmd, 3, 0);
        ++device->frameStats.drawCalls;
        cmdBindRenderTargets(cmd, nullptr);
        Transition(cmd, output, RESOURCE_STATE_SHADER_RESOURCE);
        output.lastUsedFrame = frame;
        source = output.target;
    }
}
