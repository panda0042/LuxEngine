struct ScenePass {
    LuxForgeDevice* device;
    Cmd* command;
    const SceneDrawList& draws;
    uint32_t firstDraw;
    uint64_t frame;
};

void SetSceneViewport(const LuxForgeDevice* device, Cmd* cmd) {
    cmdSetViewport(cmd, 0.0f, 0.0f, static_cast<float>(device->width),
                   static_cast<float>(device->height), 0.0f, 1.0f);
    cmdSetScissor(cmd, 0, 0, device->width, device->height);
}

void DrawShadowPass(const ScenePass& pass, uint32_t entry, const VirtualShadowPlan& shadowPlan) {
    auto* device = pass.device;
    auto* cmd = pass.command;
    QueryDesc shadowQuery{};
    if (device->timingQueries) {
        shadowQuery.mIndex = device->frameSlot * LuxForgeDevice::kTimingQueriesPerSlot +
                             ++device->shadowQueries[device->frameSlot];
        cmdBeginQuery(cmd, device->timingQueries, &shadowQuery);
    }
    Transition(cmd, device->shadow, RESOURCE_STATE_DEPTH_WRITE);
    BindRenderTargetsDesc bind = {};
    bind.mDepthStencil.pDepthStencil = device->shadow.target;
    bind.mDepthStencil.mLoadAction = LOAD_ACTION_CLEAR;
    bind.mDepthStencil.mStoreAction = STORE_ACTION_STORE;
    bind.mDepthStencil.mLoadActionStencil = LOAD_ACTION_DONTCARE;
    bind.mDepthStencil.mStoreActionStencil = STORE_ACTION_DONTCARE;
    cmdBindRenderTargets(cmd, &bind);
    DrawVirtualShadowPages(device, cmd, pass.draws.meshes, pass.firstDraw, entry, shadowPlan);
    device->frameStats.virtualShadowPages += shadowPlan.count;
    device->frameStats.requestedShadowPages += shadowPlan.requestedPages;
    cmdBindRenderTargets(cmd, nullptr);
    Transition(cmd, device->shadow, RESOURCE_STATE_SHADER_RESOURCE);
    if (device->timingQueries)
        cmdEndQuery(cmd, device->timingQueries, &shadowQuery);
    device->shadow.lastUsedFrame = pass.frame;
}

void DrawHdrPass(const ScenePass& pass, uint32_t entry, bool showSkybox, bool water,
                 bool rayTracing, LuxForgeDevice::SceneTarget& color,
                 LuxForgeDevice::SceneTarget& depth) {
    auto* device = pass.device;
    auto* cmd = pass.command;
    // HDR pass. Alpha 0 marks pixels no 3D geometry touched; the tone pass skips them.
    Transition(cmd, color, RESOURCE_STATE_RENDER_TARGET);
    Transition(cmd, depth, RESOURCE_STATE_DEPTH_WRITE);
    {
        BindRenderTargetsDesc bind = {};
        bind.mRenderTargetCount = 1;
        bind.mRenderTargets[0].pRenderTarget = color.target;
        bind.mRenderTargets[0].mLoadAction = LOAD_ACTION_CLEAR;
        bind.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
        bind.mRenderTargets[0].mClearValue = {{0.0f, 0.0f, 0.0f, 0.0f}};
        bind.mRenderTargets[0].mOverrideClearValue = 1;
        bind.mDepthStencil.pDepthStencil = depth.target;
        bind.mDepthStencil.mLoadAction = LOAD_ACTION_CLEAR;
        bind.mDepthStencil.mStoreAction = STORE_ACTION_STORE;
        bind.mDepthStencil.mLoadActionStencil = LOAD_ACTION_DONTCARE;
        bind.mDepthStencil.mStoreActionStencil = STORE_ACTION_DONTCARE;
        cmdBindRenderTargets(cmd, &bind);
    }
    SetSceneViewport(device, cmd);
    if (showSkybox) {
        BindPipeline(device, cmd, device->pipelineSky);
        BindDescriptors(device, cmd,
                        device->frameSlot * LuxForgeDevice::kMaxLightBlocks + device->currentLight,
                        device->lightSet);
        BindDescriptors(device, cmd, entry, device->sceneSet);
        cmdDraw(cmd, 3, 0);
        ++device->frameStats.drawCalls;
    }
    const size_t end = water ? pass.draws.opaqueEnd : pass.draws.colorEnd;
    DrawColorMeshes(device, cmd, pass.draws.meshes, pass.firstDraw, 0, end, entry, rayTracing);
    cmdBindRenderTargets(cmd, nullptr);
    Transition(cmd, color, RESOURCE_STATE_SHADER_RESOURCE);
    color.lastUsedFrame = depth.lastUsedFrame = pass.frame;
}

void DrawWaterPass(const ScenePass& pass, uint32_t entry, bool waterRayTracing, bool rayTracing,
                   bool compositeBlend) {
    auto* device = pass.device;
    auto* cmd = pass.command;
    Transition(cmd, device->sceneDepth, RESOURCE_STATE_SHADER_RESOURCE);
    Transition(cmd, device->waterHdr, RESOURCE_STATE_RENDER_TARGET);
    Transition(cmd, device->waterDepth, RESOURCE_STATE_DEPTH_WRITE);
    // D3D12 requires initialization before drawing to a non-zeroed target.
    const LoadActionType loadAction =
        device->waterHdr.lastUsedFrame == 0 ? LOAD_ACTION_CLEAR : LOAD_ACTION_DONTCARE;
    BindRenderTargetsDesc bind = {};
    bind.mRenderTargetCount = 1;
    bind.mRenderTargets[0].pRenderTarget = device->waterHdr.target;
    bind.mRenderTargets[0].mLoadAction = loadAction;
    bind.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
    bind.mDepthStencil.pDepthStencil = device->waterDepth.target;
    bind.mDepthStencil.mLoadAction = loadAction;
    bind.mDepthStencil.mStoreAction = STORE_ACTION_STORE;
    bind.mDepthStencil.mLoadActionStencil = LOAD_ACTION_DONTCARE;
    bind.mDepthStencil.mStoreActionStencil = STORE_ACTION_DONTCARE;
    cmdBindRenderTargets(cmd, &bind);
    SetSceneViewport(device, cmd);
    BindPipeline(device, cmd, device->pipelineOpaqueCopy);
    BindDescriptors(device, cmd, entry, device->sceneSet);
    cmdDraw(cmd, 3, 0);
    ++device->frameStats.drawCalls;
    // Both the opaque snapshot and the composite are stable until subsequent frames on
    // this queue. Water writes the composite's depth before standard transparent geometry.
    DrawColorMeshes(device, cmd, pass.draws.meshes, pass.firstDraw, pass.draws.opaqueEnd,
                    pass.draws.waterEnd, entry, waterRayTracing);
    if (!compositeBlend)
        DrawColorMeshes(device, cmd, pass.draws.meshes, pass.firstDraw, pass.draws.waterEnd,
                        pass.draws.colorEnd, entry, rayTracing);
    cmdBindRenderTargets(cmd, nullptr);
    Transition(cmd, device->waterHdr, RESOURCE_STATE_SHADER_RESOURCE);
    device->waterHdr.lastUsedFrame = device->waterDepth.lastUsedFrame = pass.frame;
}

void DrawResolvedOverlays(const ScenePass& pass, uint32_t entry,
                          LuxForgeDevice::SceneTarget& currentDepth,
                          LuxForgeDevice::SceneTarget& composite, bool rayTracing) {
    auto* device = pass.device;
    auto* cmd = pass.command;
    Transition(cmd, currentDepth, RESOURCE_STATE_DEPTH_WRITE);
    BindRenderTargetsDesc bind = {};
    bind.mRenderTargetCount = 1;
    bind.mRenderTargets[0].pRenderTarget = composite.target;
    bind.mRenderTargets[0].mLoadAction = LOAD_ACTION_LOAD;
    bind.mRenderTargets[0].mStoreAction = STORE_ACTION_STORE;
    bind.mDepthStencil.pDepthStencil = currentDepth.target;
    bind.mDepthStencil.mLoadAction = LOAD_ACTION_LOAD;
    bind.mDepthStencil.mStoreAction = STORE_ACTION_STORE;
    bind.mDepthStencil.mLoadActionStencil = LOAD_ACTION_DONTCARE;
    bind.mDepthStencil.mStoreActionStencil = STORE_ACTION_DONTCARE;
    cmdBindRenderTargets(cmd, &bind);
    SetSceneViewport(device, cmd);
    DrawColorMeshes(device, cmd, pass.draws.meshes, pass.firstDraw, pass.draws.waterEnd,
                    pass.draws.colorEnd, entry, rayTracing);
    cmdBindRenderTargets(cmd, nullptr);
    Transition(cmd, composite, RESOURCE_STATE_SHADER_RESOURCE);
}

void DrawTonePass(const ScenePass& pass, uint32_t entry) {
    auto* device = pass.device;
    auto* cmd = pass.command;
    BindPass(device, false, nullptr);
    BindPipeline(device, cmd, device->pipelineTone);
    BindDescriptors(device, cmd, entry, device->sceneSet);
    cmdDraw(cmd, 3, 0);
    ++device->frameStats.drawCalls;
    device->boundPipeline = device->pipelineTone;
    device->boundTexture = 0;
}
