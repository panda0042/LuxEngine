// Included from LuxForge.cpp with the scene target and descriptor helpers in scope.
Pipeline* CreateTemporalPipeline(LuxForgeDevice* device, Shader* shader, uint32_t targetCount) {
    PipelineDesc desc = {};
    desc.mType = PIPELINE_TYPE_GRAPHICS;
    PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(LuxTemporalSrt, Persistent), NULL, NULL, NULL);
    auto& graphics = desc.mGraphicsDesc;
    RasterizerStateDesc raster = {};
    raster.mCullMode = CULL_MODE_NONE;
    DepthStateDesc noDepth = {};
    TinyImageFormat formats[] = {kHdrFormat, TinyImageFormat_R32G32_SFLOAT, kHdrFormat};
    graphics.pShaderProgram = shader;
    graphics.pRasterizerState = &raster;
    graphics.pDepthState = &noDepth;
    graphics.pColorFormats = formats;
    graphics.mRenderTargetCount = targetCount;
    graphics.mSampleCount = SAMPLE_COUNT_1;
    graphics.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    Pipeline* pipeline = nullptr;
    addPipeline(device->renderer, &desc, &pipeline);
    return pipeline;
}

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
        SRT_SET_DESC(LuxTemporalSrt, Persistent, device->framesInFlight * 4, 0);
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

    temporal.motionShader = LoadShader(device, "luxmotion.vert", "luxmotion.frag");
    if (!temporal.motionShader)
        return false;
    PipelineDesc motion = {};
    motion.mType = PIPELINE_TYPE_GRAPHICS;
    PIPELINE_LAYOUT_DESC(
        motion, SRT_LAYOUT_DESC(LuxSceneSrt, Persistent), SRT_LAYOUT_DESC(LuxSceneSrt, PerFrame),
        SRT_LAYOUT_DESC(LuxSceneSrt, PerBatch), SRT_LAYOUT_DESC(LuxSceneSrt, PerDraw));
    VertexLayout meshLayout;
    MeshVertexLayout(meshLayout);
    meshLayout.mBindingCount = 2;
    meshLayout.mBindings[1].mStride = LuxForgeDevice::kVertexStride;
    auto& previousPosition = meshLayout.mAttribs[meshLayout.mAttribCount++];
    previousPosition = meshLayout.mAttribs[0];
    previousPosition.mBinding = 1;
    previousPosition.mLocation = 5;
    previousPosition.mSemantic = SEMANTIC_TEXCOORD2;
    DepthStateDesc equal = {};
    equal.mDepthTest = true;
    equal.mDepthFunc = CMP_EQUAL;
    RasterizerStateDesc raster = {};
    raster.mCullMode = CULL_MODE_NONE;
    auto& graphics = motion.mGraphicsDesc;
    graphics.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    graphics.mSampleCount = SAMPLE_COUNT_1;
    graphics.mDepthStencilFormat = kDepthFormat;
    graphics.mRenderTargetCount = 1;
    TinyImageFormat format = kHdrFormat;
    graphics.pColorFormats = &format;
    graphics.pShaderProgram = temporal.motionShader;
    graphics.pVertexLayout = &meshLayout;
    graphics.pRasterizerState = &raster;
    graphics.pDepthState = &equal;
    addPipeline(device->renderer, &motion, &temporal.motionPipeline);
    if (!temporal.motionPipeline)
        return false;
    temporal.pipeline = CreateTemporalPipeline(device, temporal.shader, 2);
    temporal.ready = temporal.pipeline != nullptr;
    return temporal.ready;
}

bool PrepareTemporal(LuxForgeDevice* device, bool taa, bool composite, bool blur,
                     bool separateComposite) {
    auto& temporal = device->temporal;
    if (!InitTemporal(device))
        return false;
    if (taa && composite && !blur && !temporal.compositePipeline) {
        if (temporal.compositeAttempted)
            return false;
        temporal.compositeAttempted = true;
        temporal.compositeShader =
            LoadShader(device, "luxfullscreen.vert", "luxtemporal_composite.frag");
        if (!temporal.compositeShader)
            return false;
        temporal.compositePipeline = CreateTemporalPipeline(device, temporal.compositeShader, 3);
        if (!temporal.compositePipeline)
            return false;
    }
    if (taa && composite && !blur && separateComposite && !temporal.overlay.target &&
        !AddSceneTarget(device, temporal.overlay, device->width, device->height, kHdrFormat, 8,
                        "LuxForge overlay composition"))
        return false;
    if (!temporal.motion.target &&
        !AddSceneTarget(device, temporal.motion, device->width, device->height, kHdrFormat, 8,
                        "LuxForge per-object motion vectors"))
        return false;
    if (blur && !temporal.blurAttempted) {
        temporal.blurAttempted = true;
        temporal.blurShader = LoadShader(device, "luxfullscreen.vert", "luxmotionblur.frag");
        if (temporal.blurShader)
            temporal.blurPipeline = CreateTemporalPipeline(device, temporal.blurShader, 1);
    }
    if (blur && !temporal.tileShader) {
        temporal.tileShader = LoadShader(device, "luxfullscreen.vert", "luxshuttertile.frag");
        if (temporal.tileShader)
            temporal.tilePipeline = CreateTemporalPipeline(device, temporal.tileShader, 1);
    }
    if (blur && !temporal.neighborShader) {
        temporal.neighborShader =
            LoadShader(device, "luxfullscreen.vert", "luxshutterneighbor.frag");
        if (temporal.neighborShader)
            temporal.neighborPipeline = CreateTemporalPipeline(device, temporal.neighborShader, 1);
    }
    if (blur && (!temporal.tilePipeline || !temporal.neighborPipeline))
        return false;
    if (blur && !temporal.tiles.target &&
        !AddSceneTarget(device, temporal.tiles, (device->width + 15) / 16,
                        (device->height + 15) / 16, kHdrFormat, 8,
                        "LuxForge maximum shutter velocity per tile"))
        return false;
    if (blur && !temporal.neighbors.target &&
        !AddSceneTarget(device, temporal.neighbors, (device->width + 15) / 16,
                        (device->height + 15) / 16, kHdrFormat, 8,
                        "LuxForge maximum neighboring velocity"))
        return false;
    if (blur && !temporal.blurred.target) {
        if (!temporal.blurPipeline ||
            !AddSceneTarget(device, temporal.blurred, device->width, device->height, kHdrFormat, 8,
                            "LuxForge shutter-integrated color"))
            return false;
    }
    if (!taa || temporal.color[0].target)
        return true;
    for (unsigned i = 0; i < 2; ++i) {
        if (!AddSceneTarget(device, temporal.color[i], device->width, device->height, kHdrFormat, 8,
                            "LuxForge temporal color") ||
            !AddSceneTarget(device, temporal.depth[i], device->width, device->height,
                            TinyImageFormat_R32G32_SFLOAT, 8,
                            "LuxForge temporal depth and surface ID")) {
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

// Screen-space velocity is previous UV minus current UV. Reusing the final opaque/water
// depth with an equality test keeps motion off occluded surfaces and alpha-cutout holes.
void DrawObjectMotion(LuxForgeDevice* device, Cmd* cmd, const LuxForgeScene& scene,
                      LuxForgeDevice::SceneTarget& depth, const std::vector<SceneMesh>& visible,
                      uint32_t firstDraw, uint32_t entry, uint64_t frameNumber) {
    auto& temporal = device->temporal;
    Transition(cmd, temporal.motion, RESOURCE_STATE_RENDER_TARGET);
    Transition(cmd, depth, RESOURCE_STATE_DEPTH_WRITE);
    BindRenderTargetsDesc bind = {};
    bind.mRenderTargetCount = 1;
    bind.mRenderTargets[0].pRenderTarget = temporal.motion.target;
    bind.mRenderTargets[0].mLoadAction = LOAD_ACTION_CLEAR;
    bind.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
    bind.mRenderTargets[0].mOverrideClearValue = 1;
    bind.mDepthStencil.pDepthStencil = depth.target;
    bind.mDepthStencil.mLoadAction = LOAD_ACTION_LOAD;
    bind.mDepthStencil.mStoreAction = STORE_ACTION_STORE;
    bind.mDepthStencil.mLoadActionStencil = LOAD_ACTION_DONTCARE;
    bind.mDepthStencil.mStoreActionStencil = STORE_ACTION_DONTCARE;
    cmdBindRenderTargets(cmd, &bind);
    cmdSetViewport(cmd, 0, 0, float(device->width), float(device->height), 0, 1);
    cmdSetScissor(cmd, 0, 0, device->width, device->height);
    const uint32_t strides[] = {LuxForgeDevice::kVertexStride, LuxForgeDevice::kVertexStride};
    const uint64_t offsets[] = {0, 0};
    auto& frame = device->frames[device->frameSlot];
    bool bound = false;
    const bool continuous =
        temporal.view == scene.temporalView && temporal.frame + 1 == frameNumber;
    decltype(temporal.previousObjects) next;
    // Retain IDs for surviving keys. New keys reject history on their first frame and may
    // safely reuse departed IDs. Every visible draw fits in the compact exact-half ID space.
    std::bitset<LuxForgeDevice::kMaxDraws> assigned;
    if (continuous)
        for (const auto& mesh : visible) {
            auto prior =
                temporal.previousObjects.find({mesh.draw->motionInstance, mesh.draw->indexBuffer});
            if (prior != temporal.previousObjects.end())
                assigned.set(prior->second.tag);
        }
    uint32_t freeTag = 0;
    for (size_t i = 0; i < visible.size(); ++i) {
        const auto& mesh = visible[i];
        const auto& draw = *mesh.draw;
        const bool windy = draw.foliage[0] > 0.f &&
                           (std::abs(scene.wind[0]) + std::abs(scene.wind[1]) > 0.001f ||
                            std::abs(temporal.wind[0]) + std::abs(temporal.wind[1]) > 0.001f);
        const auto& slot = device->buffers[draw.vertexBuffer];
        const bool water = mesh.layer == SceneLayer::Water;
        if ((!draw.motionInstance && !windy && !water) ||
            (mesh.layer != SceneLayer::Opaque && !water))
            continue;
        if (!bound) {
            bound = true;
            BindPipeline(device, cmd, temporal.motionPipeline);
            BindDescriptors(device, cmd, entry, device->sceneSet);
        }
        const auto key = std::make_pair(draw.motionInstance, draw.indexBuffer);
        const auto previous = temporal.previousObjects.find(key);
        const uint32_t index = firstDraw + static_cast<uint32_t>(i);
        auto* block = static_cast<uint8_t*>(frame.uniforms->pCpuMappedAddress) +
                      size_t(index) * LuxForgeDevice::kDrawStride;
        const auto multiply = [](const float* vp, const float* model) {
            std::array<float, 16> product{};
            for (unsigned col = 0; col < 4; ++col)
                for (unsigned row = 0; row < 4; ++row)
                    for (unsigned k = 0; k < 4; ++k)
                        product[4 * col + row] += vp[4 * k + row] * model[4 * col + k];
            return product;
        };
        uint32_t tag = 0;
        const bool known = continuous && previous != temporal.previousObjects.end();
        if (known) {
            tag = previous->second.tag;
        } else if (const auto it = next.find(key); it != next.end()) {
            tag = it->second.tag;
        } else {
            while (freeTag < assigned.size() && assigned.test(freeTag))
                ++freeTag;
            if (freeTag >= assigned.size())
                continue;
            tag = freeTag++;
            assigned.set(tag);
        }
        // Use binary16 exponent as well as mantissa to carry 16384 exact positive codes.
        const float surfaceTag = std::ldexp(1.f + float(tag % 1024u) / 1024.f, int(tag / 1024u));
        std::memcpy(block + offsetof(LuxDraw, previousWind) + sizeof(float), &surfaceTag,
                    sizeof(float));
        const bool previousStream = draw.previousVertexBuffer &&
                                    draw.previousVertexBuffer < device->buffers.size() &&
                                    device->buffers[draw.previousVertexBuffer].buffer &&
                                    device->buffers[draw.previousVertexBuffer].size == slot.size;
        const float oceanHistory = ((slot.ocean && slot.ocean->lastDispatchFrame == frameNumber) ||
                                    (slot.skin && slot.skin->frame == frameNumber))
                                       ? 1.f
                                       : 0.f;
        std::memcpy(block + offsetof(LuxDraw, previousWind) + 2 * sizeof(float), &oceanHistory,
                    sizeof(float));
        if (known && ((previous->second.vertices == draw.vertexBuffer &&
                       (previous->second.revision == draw.geometryRevision || slot.skin)) ||
                      (previousStream && previous->second.vertices == draw.previousVertexBuffer))) {
            const auto& object = previous->second;
            const float deformation[] = {temporal.wind[0], temporal.wind[1], object.amplitude,
                                         object.breakerHeight};
            const float coastal[] = {float(temporal.origin[0]) - temporal.weatherOrigin[0],
                                     float(temporal.origin[2]) - temporal.weatherOrigin[1],
                                     temporal.breakerTime, 0};
            std::memcpy(block + offsetof(LuxDraw, previousMvp), object.mvp.data(),
                        sizeof object.mvp);
            std::memcpy(block + offsetof(LuxDraw, previousPhaseX), object.phaseX.data(),
                        sizeof object.phaseX);
            std::memcpy(block + offsetof(LuxDraw, previousPhaseZ), object.phaseZ.data(),
                        sizeof object.phaseZ);
            std::memcpy(block + offsetof(LuxDraw, previousDeformation), deformation,
                        sizeof deformation);
            std::memcpy(block + offsetof(LuxDraw, previousCoast), coastal, sizeof coastal);
            std::memcpy(block + offsetof(LuxDraw, previousWindProjection), temporal.matrix.data(),
                        16 * sizeof(float));
            std::memcpy(block + offsetof(LuxDraw, previousWind), &temporal.wind[2], sizeof(float));
        }
        auto& object = next[key];
        object.mvp = multiply(scene.viewProjection, draw.model);
        for (unsigned c = 0; c < 4; ++c) {
            object.phaseX[c] = draw.model[c * 4];
            object.phaseZ[c] = draw.model[c * 4 + 2];
        }
        object.phaseX[3] += scene.weatherOrigin[0];
        object.phaseZ[3] += scene.weatherOrigin[1];
        object.amplitude = draw.foliage[0];
        object.breakerHeight =
            slot.ocean && water && scene.waterReflection[3] >= 1.f ? scene.forward[3] : 0.f;
        object.tag = tag;
        object.vertices = draw.vertexBuffer;
        object.revision = draw.geometryRevision;
        BindDescriptors(device, cmd, mesh.material % LuxForgeDevice::kMaterialBankSize,
                        device->materialSets[device->frameSlot]
                                            [mesh.material / LuxForgeDevice::kMaterialBankSize]);
        BindDescriptors(device, cmd, index, frame.drawSet);
        Buffer* last =
            previousStream ? device->buffers[draw.previousVertexBuffer].buffer : slot.buffer;
        Buffer* streams[] = {slot.buffer, last};
        if (previousStream) {
            device->buffers[draw.previousVertexBuffer].lastUsedFrame = frameNumber;
            device->frameStats.deformedMotionDraws += last != slot.buffer;
        }
        device->frameStats.deformedMotionDraws += slot.skin && slot.skin->frame == frameNumber;
        device->frameStats.waterMotionDraws += water;
        cmdBindVertexBuffer(cmd, 2, streams, strides, offsets);
        cmdBindIndexBuffer(cmd, device->buffers[draw.indexBuffer].buffer, INDEX_TYPE_UINT32, 0);
        cmdDrawIndexed(cmd, draw.indexCount, 0, 0);
        ++device->frameStats.objectMotionDraws;
        ++device->frameStats.drawCalls;
    }
    cmdBindRenderTargets(cmd, nullptr);
    temporal.previousObjects.swap(next);
    std::copy_n(scene.wind, 3, temporal.wind.begin());
    std::copy_n(scene.weatherOrigin, 2, temporal.weatherOrigin.begin());
    temporal.breakerTime = scene.waterReflection[3] - 1.f;
    std::memcpy(temporal.foliageContacts, scene.foliageContacts, sizeof scene.foliageContacts);
    temporal.motion.lastUsedFrame = frameNumber;
    Transition(cmd, temporal.motion, RESOURCE_STATE_SHADER_RESOURCE);
}

bool WriteTemporalConstants(LuxForgeDevice* device, const LuxForgeScene& scene,
                            const std::array<float, 2>& jitter, uint64_t frame, bool water) {
    auto& temporal = device->temporal;
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
    std::array<float, 40> values{};
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
    const bool taaHistory = history && temporal.taaFrame + 1 == frame;
    values[32] = taaHistory ? 1.f : 0.f;
    values[33] = water ? 1.f : 0.f;
    const auto now = Clock::now();
    const float frameSeconds = std::chrono::duration<float>(now - temporal.shutterTime).count();
    temporal.shutterTime = now;
    const float shutter = scene.shutterSeconds > 0 && frameSeconds > 0
                              ? std::min(1.f, scene.shutterSeconds / frameSeconds)
                              : scene.motionBlurShutter;
    values[34] = history ? shutter : 0.f;
    values[35] = scene.temporalSharpness;
    values[36] = (temporal.jitter[0] - jitter[0]) * .5f;
    values[37] = (temporal.jitter[1] - jitter[1]) * -.5f;
    values[38] = float(scene.temporalDebugView);

    const uint32_t offset = device->frameSlot * LuxForgeDevice::kSceneStride;
    std::memcpy(static_cast<uint8_t*>(temporal.constants->pCpuMappedAddress) + offset,
                values.data(), sizeof(values));
    return taaHistory;
}

Texture* ResolveTemporal(LuxForgeDevice* device, Cmd* cmd, LuxForgeDevice::SceneTarget& color,
                         LuxForgeDevice::SceneTarget& depth,
                         LuxForgeDevice::SceneTarget* opaqueDepth,
                         LuxForgeDevice::SceneTarget* composite, bool history, uint64_t frame) {
    auto& temporal = device->temporal;
    const auto old = temporal.index;
    const unsigned output = old ^ 1u;
    const uint32_t offset = device->frameSlot * LuxForgeDevice::kSceneStride;
    DescriptorDataRange range{offset, LuxForgeDevice::kSceneStride, 0};
    DescriptorData params[7] = {};
    params[0].mIndex = SRT_RES_IDX(LuxTemporalSrt, Persistent, gTemporal);
    params[0].pRanges = &range;
    params[0].ppBuffers = &temporal.constants;
    Texture* textures[] = {color.target->pTexture,
                           depth.target->pTexture,
                           (history ? temporal.color[old] : color).target->pTexture,
                           (history ? temporal.depth[old] : depth).target->pTexture,
                           (opaqueDepth ? opaqueDepth : &depth)->target->pTexture,
                           temporal.motion.target->pTexture};
    const uint32_t slots[] = {SRT_RES_IDX(LuxTemporalSrt, Persistent, gCurrentColor),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gCurrentDepth),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gPreviousColor),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gPreviousDepth),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gTemporalOpaqueDepth),
                              SRT_RES_IDX(LuxTemporalSrt, Persistent, gMotionVectors)};
    for (unsigned i = 0; i < std::size(textures); ++i) {
        params[i + 1].mIndex = slots[i];
        params[i + 1].ppTextures = &textures[i];
    }
    updateDescriptorSet(device->renderer, device->frameSlot, temporal.set, std::size(params),
                        params);

    Transition(cmd, depth, RESOURCE_STATE_SHADER_RESOURCE);
    Transition(cmd, temporal.color[output], RESOURCE_STATE_RENDER_TARGET);
    Transition(cmd, temporal.depth[output], RESOURCE_STATE_RENDER_TARGET);
    BindRenderTargetsDesc targets = {};
    targets.mRenderTargetCount = 2;
    targets.mRenderTargets[0].pRenderTarget = temporal.color[output].target;
    targets.mRenderTargets[1].pRenderTarget = temporal.depth[output].target;
    if (composite) {
        Transition(cmd, *composite, RESOURCE_STATE_RENDER_TARGET);
        targets.mRenderTargetCount = 3;
        targets.mRenderTargets[2].pRenderTarget = composite->target;
    }
    for (unsigned i = 0; i < targets.mRenderTargetCount; ++i) {
        targets.mRenderTargets[i].mLoadAction = LOAD_ACTION_CLEAR;
        targets.mRenderTargets[i].mStoreAction = STORE_ACTION_STORE;
    }
    if (composite)
        targets.mRenderTargets[2].mLoadAction = LOAD_ACTION_DONTCARE;
    cmdBindRenderTargets(cmd, &targets);
    cmdSetViewport(cmd, 0, 0, float(device->width), float(device->height), 0, 1);
    cmdSetScissor(cmd, 0, 0, device->width, device->height);
    BindPipeline(device, cmd, composite ? temporal.compositePipeline : temporal.pipeline);
    BindDescriptors(device, cmd, device->frameSlot, temporal.set);
    cmdDraw(cmd, 3, 0);
    cmdBindRenderTargets(cmd, nullptr);
    Transition(cmd, temporal.color[output], RESOURCE_STATE_SHADER_RESOURCE);
    Transition(cmd, temporal.depth[output], RESOURCE_STATE_SHADER_RESOURCE);

    temporal.color[output].lastUsedFrame = temporal.depth[output].lastUsedFrame = frame;
    if (history)
        temporal.color[old].lastUsedFrame = temporal.depth[old].lastUsedFrame = frame;
    temporal.index = output;
    temporal.taaFrame = frame;
    ++device->frameStats.drawCalls;
    ++device->frameStats.temporalResolvePasses;
    if (history)
        ++device->frameStats.temporalHistoryPasses;
    return temporal.color[output].target->pTexture;
}

void SaveTemporalView(LuxForgeDevice* device, const LuxForgeScene& scene,
                      const std::array<float, 2>& jitter, uint64_t frame) {
    auto& temporal = device->temporal;
    temporal.jitter = jitter;
    temporal.frame = frame;
    temporal.view = scene.temporalView;
    temporal.environment = scene.environmentMap;
    std::copy_n(scene.temporalOrigin, 3, temporal.origin.begin());
    std::copy_n(scene.forward, 3, temporal.forward.begin());
    std::copy_n(scene.viewProjection, 16, temporal.matrix.begin());
}

Texture* BlurMotion(LuxForgeDevice* device, Cmd* cmd, Texture* source,
                    LuxForgeDevice::SceneTarget& depth, uint64_t frame) {
    auto& temporal = device->temporal;
    const uint32_t offset = device->frameSlot * LuxForgeDevice::kSceneStride;
    DescriptorDataRange range{offset, LuxForgeDevice::kSceneStride, 0};
    DescriptorData params[5]{};
    params[0].mIndex = SRT_RES_IDX(LuxTemporalSrt, Persistent, gTemporal);
    params[0].ppBuffers = &temporal.constants;
    params[0].pRanges = &range;
    params[1].mIndex = SRT_RES_IDX(LuxTemporalSrt, Persistent, gCurrentColor);
    params[1].ppTextures = &source;
    params[2].mIndex = SRT_RES_IDX(LuxTemporalSrt, Persistent, gCurrentDepth);
    params[2].ppTextures = &depth.target->pTexture;
    params[3].mIndex = SRT_RES_IDX(LuxTemporalSrt, Persistent, gMotionVectors);
    params[3].ppTextures = &temporal.motion.target->pTexture;
    params[4].mIndex = SRT_RES_IDX(LuxTemporalSrt, Persistent, gShutterTiles);
    Transition(cmd, depth, RESOURCE_STATE_SHADER_RESOURCE);
    const auto pass = [&](LuxForgeDevice::SceneTarget& target, Pipeline* pipeline, uint32_t slot,
                          Texture* tileInput) {
        const uint32_t entry = slot * device->framesInFlight + device->frameSlot;
        params[4].ppTextures = &tileInput;
        updateDescriptorSet(device->renderer, entry, temporal.set, std::size(params), params);
        Transition(cmd, target, RESOURCE_STATE_RENDER_TARGET);
        BindRenderTargetsDesc targets{};
        targets.mRenderTargetCount = 1;
        targets.mRenderTargets[0].pRenderTarget = target.target;
        targets.mRenderTargets[0].mLoadAction = LOAD_ACTION_DONTCARE;
        targets.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
        cmdBindRenderTargets(cmd, &targets);
        cmdSetViewport(cmd, 0, 0, float(target.target->mWidth), float(target.target->mHeight), 0,
                       1);
        cmdSetScissor(cmd, 0, 0, target.target->mWidth, target.target->mHeight);
        BindPipeline(device, cmd, pipeline);
        BindDescriptors(device, cmd, entry, temporal.set);
        cmdDraw(cmd, 3, 0);
        cmdBindRenderTargets(cmd, nullptr);
        Transition(cmd, target, RESOURCE_STATE_SHADER_RESOURCE);
        target.lastUsedFrame = frame;
        ++device->frameStats.drawCalls;
    };
    pass(temporal.tiles, temporal.tilePipeline, 1, temporal.motion.target->pTexture);
    pass(temporal.neighbors, temporal.neighborPipeline, 2, temporal.tiles.target->pTexture);
    pass(temporal.blurred, temporal.blurPipeline, 3, temporal.neighbors.target->pTexture);
    return temporal.blurred.target->pTexture;
}
