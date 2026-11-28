// HDR bloom reduction, luminance adaptation and camera/display composite on the graphics queue.
bool InitPost(LuxForgeDevice* device) {
    using State = LuxForgeDevice::PostState;
    auto& post = device->post;
    if (post.attempted)
        return post.ready;
    post.attempted = true;
    const uint32_t entries =
        device->framesInFlight * LuxForgeDevice::kMaxScenePasses * State::kStages;
    const DescriptorSetDesc layout = SRT_SET_DESC(LuxPostSrt, Persistent, entries, 0);
    addDescriptorSet(device->renderer, &layout, &post.set);
    BufferLoadDesc constants{};
    constants.ppBuffer = &post.constants;
    constants.mDesc.mSize = entries * LuxForgeDevice::kSceneStride;
    constants.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    constants.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    constants.mDesc.mDescriptors = UniformBufferDescriptor;
    constants.mDesc.mStartState = RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    constants.mDesc.pName = "LuxForge camera postprocessing constants";
    addResource(&constants, nullptr);
    waitForAllResourceLoads();
    if (!post.set || !post.constants || !post.constants->pCpuMappedAddress)
        return false;

    PipelineDesc desc{};
    desc.mType = PIPELINE_TYPE_GRAPHICS;
    PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(LuxPostSrt, Persistent), NULL, NULL, NULL);
    auto& graphics = desc.mGraphicsDesc;
    RasterizerStateDesc raster{};
    raster.mCullMode = CULL_MODE_NONE;
    DepthStateDesc depth{};
    graphics.pRasterizerState = &raster;
    graphics.pDepthState = &depth;
    graphics.mSampleCount = SAMPLE_COUNT_1;
    graphics.mRenderTargetCount = 1;
    graphics.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    BlendStateDesc blend{};
    blend.mSrcFactors[0] = BC_SRC_ALPHA;
    blend.mDstFactors[0] = BC_ONE_MINUS_SRC_ALPHA;
    blend.mBlendModes[0] = BM_ADD;
    blend.mSrcAlphaFactors[0] = BC_ONE;
    blend.mDstAlphaFactors[0] = BC_ONE_MINUS_SRC_ALPHA;
    blend.mBlendAlphaModes[0] = BM_ADD;
    blend.mColorWriteMasks[0] = COLOR_MASK_ALL;
    blend.mRenderTargetMask = BLEND_STATE_TARGET_0;
    const char* names[] = {"luxpostreduce.frag", "luxpostexposure.frag", "luxpostfinal.frag"};
    auto format = kHdrFormat;
    graphics.pColorFormats = &format;
    for (uint32_t i = 0; i < 3; ++i) {
        post.shaders[i] = LoadShader(device, "luxfullscreen.vert", names[i]);
        if (!post.shaders[i])
            return false;
        if (i == 2) {
            graphics.pBlendState = &blend;
            graphics.mDepthStencilFormat = kDepthFormat;
            format = static_cast<TinyImageFormat>(device->swapChain->ppRenderTargets[0]->mFormat);
        }
        graphics.pShaderProgram = post.shaders[i];
        addPipeline(device->renderer, &desc, &post.pipelines[i]);
        if (!post.pipelines[i])
            return false;
    }
    post.ready = true;
    return true;
}

bool PreparePost(LuxForgeDevice* device, const LuxForgeScene& scene, uint32_t pass,
                 uint32_t& levels) {
    if (!InitPost(device))
        return false;
    auto& post = device->post;
    // Touch the cache even on partial allocation, so the idle sweep will reclaim it.
    post.frame = device->frameNumber + 1;
    const auto ensure = [&](LuxForgeDevice::SceneTarget& target, uint32_t w, uint32_t h,
                            const char* name) {
        return target.target || AddSceneTarget(device, target, w, h, kHdrFormat, 8, name);
    };
    if (scene.fogColorDensity[3] > 0 &&
        !ensure(post.fogColor, device->width, device->height, "LuxForge fogged HDR"))
        return false;
    if (scene.postEffects[0] > 0 || scene.autoExposure > 0 || scene.lensFlareStrength > 0) {
        uint32_t w = device->width, h = device->height;
        do {
            w = std::max(1u, (w + 1) / 2);
            h = std::max(1u, (h + 1) / 2);
            if (!ensure(post.pyramid[levels], w, h,
                        "LuxForge HDR highlight and luminance reduction"))
                return false;
            ++levels;
        } while ((w > 1 || h > 1) && levels < LuxForgeDevice::PostState::kLevels &&
                 (scene.autoExposure > 0 || levels < 6));
    }
    if (scene.autoExposure > 0) {
        for (auto& target : post.exposure[pass].values)
            if (!ensure(target, 1, 1, "LuxForge adapted exposure"))
                return false;
    }
    return true;
}

void BindPostTarget(LuxForgeDevice* device, LuxForgeDevice::SceneTarget& output) {
    Cmd* cmd = device->element.pCmds[0];
    Transition(cmd, output, RESOURCE_STATE_RENDER_TARGET);
    BindRenderTargetsDesc targets{};
    targets.mRenderTargetCount = 1;
    targets.mRenderTargets[0].pRenderTarget = output.target;
    targets.mRenderTargets[0].mLoadAction = LOAD_ACTION_CLEAR;
    targets.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
    cmdBindRenderTargets(cmd, &targets);
    cmdSetViewport(cmd, 0, 0, float(output.target->mWidth), float(output.target->mHeight), 0, 1);
    cmdSetScissor(cmd, 0, 0, output.target->mWidth, output.target->mHeight);
}

void FinishPostTarget(LuxForgeDevice* device, LuxForgeDevice::SceneTarget& output) {
    Cmd* cmd = device->element.pCmds[0];
    cmdBindRenderTargets(cmd, nullptr);
    Transition(cmd, output, RESOURCE_STATE_SHADER_RESOURCE);
    output.lastUsedFrame = device->frameNumber + 1;
}

struct PostConstants {
    float dimensions[4]{};
    float lens[4]{};
    float camera[4]{};
    float grading[4]{};
    float cameraEffects[4]{};
    float optics[4]{};
    float flare[4]{};
    float adaptation[4]{};
};

void DrawPost(LuxForgeDevice* device, uint32_t pass, uint32_t stage, uint32_t pipeline,
              const PostConstants& constants, const std::array<Texture*, 7>& textures,
              LuxForgeDevice::SceneTarget* output) {
    using State = LuxForgeDevice::PostState;
    auto& post = device->post;
    const uint32_t entry =
        (device->frameSlot * LuxForgeDevice::kMaxScenePasses + pass) * State::kStages + stage;
    const uint32_t offset = entry * LuxForgeDevice::kSceneStride;
    std::memcpy(static_cast<uint8_t*>(post.constants->pCpuMappedAddress) + offset, &constants,
                sizeof(constants));
    DescriptorDataRange range{offset, sizeof(constants), 0};
    DescriptorData data[8]{};
    const uint32_t slots[] = {SRT_RES_IDX(LuxPostSrt, Persistent, gPostSource),
                              SRT_RES_IDX(LuxPostSrt, Persistent, gPostDepth),
                              SRT_RES_IDX(LuxPostSrt, Persistent, gBloomNear),
                              SRT_RES_IDX(LuxPostSrt, Persistent, gBloomMid),
                              SRT_RES_IDX(LuxPostSrt, Persistent, gBloomFar),
                              SRT_RES_IDX(LuxPostSrt, Persistent, gAdaptation),
                              SRT_RES_IDX(LuxPostSrt, Persistent, gGradingLut)};
    data[0].mIndex = SRT_RES_IDX(LuxPostSrt, Persistent, gPost);
    data[0].ppBuffers = &post.constants;
    data[0].pRanges = &range;
    // The Forge descriptor API accepts mutable arrays even for sampled-only bindings.
    auto bound = textures;
    for (uint32_t i = 0; i < bound.size(); ++i) {
        data[i + 1].mIndex = slots[i];
        data[i + 1].ppTextures = &bound[i];
    }
    updateDescriptorSet(device->renderer, entry, post.set, 8, data);
    if (output)
        BindPostTarget(device, *output);
    else
        BindPass(device, false, nullptr);
    Cmd* cmd = device->element.pCmds[0];
    BindPipeline(device, cmd, post.pipelines[pipeline]);
    BindDescriptors(device, cmd, entry, post.set);
    cmdDraw(cmd, 3, 0);
    if (output)
        FinishPostTarget(device, *output);
    ++device->frameStats.drawCalls;
}

void RenderPost(LuxForgeDevice* device, const LuxForgeScene& scene, uint32_t pass, uint32_t levels,
                Texture* source, LuxForgeDevice::SceneTarget& depth) {
    auto& post = device->post;
    PostConstants constants;
    std::copy_n(scene.postEffects, 4, constants.lens);
    std::copy_n(scene.optics, 4, constants.optics);
    constants.flare[0] = scene.lensFlareStrength;
    constants.flare[1] = float(device->frameNumber % 4096);
    constants.cameraEffects[0] = scene.physicallyBasedBloom != 0;
    constants.cameraEffects[1] = scene.localExposure;
    constants.cameraEffects[2] = float(scene.apertureBlades);
    constants.lens[2] *= float(device->height) / 1080.f;
    if (scene.physicalCocFraction > 0) {
        constants.cameraEffects[3] = scene.physicalCocFraction * device->height;
        constants.lens[2] = std::min(32.f, device->height * (32.f / 1080.f));
    }
    constants.adaptation[2] =
        scene.physicalExposureGain > 0 ? std::log2(scene.physicalExposureGain) : 0;
    constants.camera[0] = scene.right[3];
    constants.camera[1] = scene.up[3];
    constants.camera[2] = scene.camera[3];
    constants.camera[3] = scene.options[2];
    std::array<Texture*, 7> inputs;
    inputs.fill(device->textures[device->whiteTexture].texture);
    inputs[0] = source;
    for (uint32_t i = 0; i < levels; ++i) {
        auto& level = post.pyramid[i];
        constants.dimensions[0] = 1.f / level.target->mWidth;
        constants.dimensions[1] = 1.f / level.target->mHeight;
        constants.grading[3] = i == 0;
        DrawPost(device, pass, i, 0, constants, inputs, &level);
        inputs[0] = level.target->pTexture;
    }
    using State = LuxForgeDevice::PostState;
    if (scene.autoExposure > 0) {
        auto& history = post.exposure[pass];
        const auto now = std::chrono::steady_clock::now();
        const uint64_t frame = device->frameNumber + 1;
        const bool valid =
            history.frame != 0 && history.frame + 1 == frame && history.view == scene.postView;
        constants.adaptation[1] = valid;
        if (valid) {
            inputs[5] = history.values[history.index].target->pTexture;
            history.values[history.index].lastUsedFrame = frame;
            constants.adaptation[0] =
                std::clamp(std::chrono::duration<float>(now - history.time).count(), 0.f, 1.f);
        }
        history.index ^= 1u;
        auto& next = history.values[history.index];
        DrawPost(device, pass, State::kLevels, 1, constants, inputs, &next);
        inputs[5] = next.target->pTexture;
        history.time = now;
        history.view = scene.postView;
        history.frame = frame;
    }
    inputs[0] = source;
    if (scene.postEffects[2] > 0 || scene.physicalCocFraction > 0 || scene.localExposure > 0) {
        Transition(device->element.pCmds[0], depth, RESOURCE_STATE_SHADER_RESOURCE);
        inputs[1] = depth.target->pTexture;
    }
    if (scene.postEffects[0] > 0 || scene.lensFlareStrength > 0) {
        constexpr uint32_t widths[] = {1, 3, 5};
        for (uint32_t i = 0; i < 3; ++i)
            inputs[2 + i] = post.pyramid[std::min(widths[i], levels - 1)].target->pTexture;
    }
    if (HasTexture(device, scene.gradingTexture)) {
        inputs[6] = device->textures[scene.gradingTexture].texture;
        constants.grading[0] = float(inputs[6]->mHeight);
        constants.grading[1] = scene.gradingStrength;
    }
    constants.dimensions[0] = 1.f / device->width;
    constants.dimensions[1] = 1.f / device->height;
    constants.grading[2] = scene.autoExposure;
    DrawPost(device, pass, State::kLevels + 1, 2, constants, inputs, nullptr);
    device->boundPipeline = post.pipelines[2];
}
