// A graphics-queue capture is consumed by both forward and ray-query shading.
// All descriptors for recorded commands are immutable until the frame slot is recycled.
void ShutdownSkyCache(LuxForgeDevice* device) {
    auto& sky = device->skyCache;
    for (auto& stage : sky.stages) {
        RetireTarget(device, stage.output);
        if (stage.pipeline)
            removePipeline(device->renderer, stage.pipeline);
        if (stage.shader)
            removeShader(device->renderer, stage.shader);
    }
    if (sky.set)
        removeDescriptorSet(device->renderer, sky.set);
    sky = {};
}

bool PrepareSkyCache(LuxForgeDevice* device) {
    auto& sky = device->skyCache;
    if (sky.set)
        return true;
    const uint32_t entries = 8 * device->framesInFlight * LuxForgeDevice::kMaxScenePasses;
    const DescriptorSetDesc setDesc = SRT_SET_DESC(LuxSkyCacheSrt, Persistent, entries, 0);
    addDescriptorSet(device->renderer, &setDesc, &sky.set);
    if (!sky.set)
        return false;
    TinyImageFormat format = kHdrFormat;
    PipelineDesc desc{};
    desc.mType = PIPELINE_TYPE_GRAPHICS;
    PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(LuxSkyCacheSrt, Persistent), NULL, NULL, NULL);
    auto& graphics = desc.mGraphicsDesc;
    RasterizerStateDesc raster{};
    raster.mCullMode = CULL_MODE_NONE;
    DepthStateDesc depth{};
    graphics.pRasterizerState = &raster;
    graphics.pDepthState = &depth;
    graphics.pColorFormats = &format;
    graphics.mRenderTargetCount = 1;
    graphics.mSampleCount = SAMPLE_COUNT_1;
    graphics.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    const char* names[] = {"luxskycapture.frag",  "luxskysh.frag",         "luxskygloss.frag",
                           "luxcloudshadow.frag", "luxtransmittance.frag", "luxmultiscatter.frag",
                           "luxskyview.frag",     "luxaerial.frag"};
    const uint32_t widths[] = {204, 9, 204, 256, 128, 256, 514, 768},
                   heights[] = {34, 1, 170, 128, 64, 64, 256, 16};
    for (unsigned i = 0; i < 8; ++i) {
        auto& stage = sky.stages[i];
        stage.shader = LoadShader(device, "luxfullscreen.vert", names[i]);
        if (stage.shader) {
            graphics.pShaderProgram = stage.shader;
            addPipeline(device->renderer, &desc, &stage.pipeline);
        }
        if (!stage.pipeline ||
            !AddSceneTarget(device, stage.output, widths[i], heights[i], format, 8, names[i])) {
            ShutdownSkyCache(device);
            return false;
        }
    }
    return true;
}

void UpdateSkyCache(LuxForgeDevice* device, const LuxForgeScene& scene, uint32_t pass,
                    Texture* fallback) {
    auto& sky = device->skyCache;
    const auto slot = device->frameSlot;
    DescriptorDataRange sceneRange{pass * LuxForgeDevice::kSceneStride,
                                   LuxForgeDevice::kSceneStride, 0};
    DescriptorDataRange lightRange{device->currentLight * LuxForgeDevice::kLightStride,
                                   LuxForgeDevice::kLightStride, 0};
    const auto* lights = reinterpret_cast<const float*>(
        static_cast<const uint8_t*>(device->lightBuffers[slot]->pCpuMappedAddress) +
        lightRange.mOffset);
    const std::array key = {std::round(lights[0] * 128.f),
                            std::round(lights[1] * 128.f),
                            std::round(lights[2] * 128.f),
                            std::round(lights[4] * lights[7] * 64.f),
                            std::round(lights[5] * lights[7] * 64.f),
                            std::round(lights[6] * lights[7] * 64.f),
                            std::round(scene.atmosphere[1] * 32.f),
                            std::round(scene.atmosphere[2] * 128.f),
                            std::round(scene.atmosphere[3] / 16.f),
                            std::round(scene.weatherOrigin[2] / 16.f),
                            std::round(scene.weatherOrigin[3] / 16.f),
                            scene.skyLighting[0],
                            scene.skyLighting[1] > 0.f ? 1.f : 0.f,
                            scene.atmosphere[0],
                            scene.solarSource[0],
                            scene.solarSource[1],
                            scene.solarSource[2]};
    // Publish diffuse and gloss together. Small weather changes amortize cube capture
    // over six submissions. A view switch, sun cut or large weather change refreshes
    // the entire capture before drawing the first surface using the new lighting.
    const bool hardChange =
        !sky.valid || sky.lastPass != pass || sky.key[11] != key[11] || sky.key[12] != key[12] ||
        sky.key[13] != key[13] ||
        std::abs(sky.key[0] - key[0]) + std::abs(sky.key[1] - key[1]) +
                std::abs(sky.key[2] - key[2]) >
            9.f ||
        std::abs(sky.key[3] - key[3]) > 5.f || std::abs(sky.key[6] - key[6]) > 3.f ||
        std::abs(sky.key[7] - key[7]) > 12.f || std::abs(sky.key[8] - key[8]) > 8.f ||
        std::abs(sky.key[9] - key[9]) + std::abs(sky.key[10] - key[10]) > 32.f ||
        sky.key[14] != key[14];
    if (!hardChange && sky.nextFace == 6 && sky.key == key)
        return;
    const bool captureWeather = hardChange || sky.nextFace == 6;
    if (captureWeather) {
        sky.nextFace = !hardChange && scene.skyLighting[0] > 0.f ? 0 : 6;
        sky.key = key;
        sky.valid = true;
        sky.lastPass = pass;
    }
    const bool partial = sky.nextFace < 6;
    Cmd* cmd = device->element.pCmds[0];
    const float aerosol = std::round(scene.atmosphere[1] * 128.f);
    const bool opticsDirty = sky.opticalAerosol != aerosol;
    if (scene.atmosphere[0] > 0.f)
        sky.opticalAerosol = aerosol;
    constexpr uint32_t order[] = {4, 5, 6, 7, 0, 1, 2, 3};
    for (const auto i : order) {
        if ((i == 3 && scene.skyLighting[1] <= 0.f) || (i < 3 && scene.skyLighting[0] == 0.f) ||
            (i >= 4 && scene.atmosphere[0] == 0.f) || ((i == 4 || i == 5) && !opticsDirty))
            continue;
        if (!captureWeather && i >= 3)
            continue;
        if ((i == 1 || i == 2) && partial && sky.nextFace != 5)
            continue;
        auto& stage = sky.stages[i];
        Texture* source = fallback;
        if (i == 5 || i == 6)
            source = sky.stages[i - 1].output.target->pTexture;
        if (i == 7)
            source = sky.stages[5].output.target->pTexture;
        if (i == 0)
            source = sky.stages[6].output.target->pTexture;
        if (i == 1 || i == 2)
            source = sky.stages[0].output.target->pTexture;
        DescriptorData params[3]{};
        params[0].mIndex = SRT_RES_IDX(LuxSkyCacheSrt, Persistent, gWeather);
        params[0].ppBuffers = &device->sceneUniforms[slot];
        params[0].pRanges = &sceneRange;
        params[1].mIndex = SRT_RES_IDX(LuxSkyCacheSrt, Persistent, gSolar);
        params[1].ppBuffers = &device->lightBuffers[slot];
        params[1].pRanges = &lightRange;
        params[2].mIndex = SRT_RES_IDX(LuxSkyCacheSrt, Persistent, gSkySource);
        params[2].ppTextures = &source;
        const uint32_t entry = (slot * LuxForgeDevice::kMaxScenePasses + pass) * 8 + i;
        updateDescriptorSet(device->renderer, entry, sky.set, 3, params);
        if (i == 0 && partial) {
            Transition(cmd, stage.output, RESOURCE_STATE_RENDER_TARGET);
            BindRenderTargetsDesc bind{};
            bind.mRenderTargetCount = 1;
            bind.mRenderTargets[0].pRenderTarget = stage.output.target;
            bind.mRenderTargets[0].mLoadAction = LOAD_ACTION_LOAD;
            bind.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
            cmdBindRenderTargets(cmd, &bind);
            cmdSetViewport(cmd, 0, 0, 204, 34, 0, 1);
            cmdSetScissor(cmd, 34 * sky.nextFace, 0, 34, 34);
        } else {
            BindPostTarget(device, stage.output);
        }
        BindPipeline(device, cmd, stage.pipeline);
        BindDescriptors(device, cmd, entry, sky.set);
        cmdDraw(cmd, 3, 0);
        ++device->frameStats.drawCalls;
        FinishPostTarget(device, stage.output);
    }
    if (partial)
        ++sky.nextFace;
}
