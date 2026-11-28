// Immutable bind geometry, mutable bone palettes per in-flight frame and a graphics-queue output.
uint32_t CreateSkinBuffer(LuxForgeDevice* device, uint32_t verts, uint32_t bones, const float* data,
                          uint32_t entries) {
    if (!device || !data || !verts || verts > 1000000 || !bones || bones > 1024 ||
        entries < 3u * verts || entries > (1u << 24))
        return 0;
    for (uint32_t v = 0; v < verts; ++v) {
        const float* record = data + 12u * v;
        const float offset = record[3], count = record[7];
        if (!(offset >= 0 && offset <= entries && std::floor(offset) == offset && count >= 0 &&
              count <= 1024 && std::floor(count) == count) ||
            3u * verts + uint32_t(offset) + (uint32_t(count) + 1u) / 2u > entries)
            return 0;
        for (uint32_t i = 0; i < uint32_t(count); ++i) {
            const float* influence = data + 4u * (3u * verts + uint32_t(offset)) + 2u * i;
            if (!(influence[0] >= 0 && influence[0] < bones &&
                  std::floor(influence[0]) == influence[0] && influence[1] >= 0 &&
                  std::isfinite(influence[1])))
                return 0;
        }
    }
    if (!device->skinPipeline) {
        if (!device->skinShader) {
            ShaderLoadDesc load{};
            load.mComp.pFileName = "luxskin.comp";
            addShader(device->renderer, &load, &device->skinShader);
        }
        if (!device->skinShader)
            return 0;
        PipelineDesc p{};
        p.mType = PIPELINE_TYPE_COMPUTE;
        PIPELINE_LAYOUT_DESC(p, SRT_LAYOUT_DESC(LuxSkinSrt, Persistent), NULL, NULL, NULL);
        p.mComputeDesc.pShaderProgram = device->skinShader;
        addPipeline(device->renderer, &p, &device->skinPipeline);
        if (!device->skinPipeline)
            return 0;
    }
    const ResourceUpload upload(device);
    LuxForgeDevice::BufferSlot slot{};
    slot.skin = new LuxForgeDevice::SkinBuffers;
    auto& skin = *slot.skin;
    skin.verts = verts;
    skin.bones = bones;
    slot.type = LUX_FORGE_BUFFER_VERTEX;
    slot.size = verts * sizeof(LuxForgeVertex);
    BufferLoadDesc seed{};
    seed.ppBuffer = &skin.seed;
    seed.pData = data;
    seed.mDesc.mSize = uint64_t(entries) * 16;
    seed.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
    seed.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
    seed.mDesc.mStartState = kOceanRead;
    seed.mDesc.mElementCount = entries;
    seed.mDesc.mStructStride = 16;
    seed.mDesc.pName = "LuxForge skin bind geometry and normalized influences";
    SyncToken token{};
    addResource(&seed, &token);
    BufferLoadDesc output{};
    output.ppBuffer = &slot.buffer;
    output.mDesc.mSize = slot.size;
    output.mDesc.mDescriptors =
        static_cast<DescriptorType>(DESCRIPTOR_TYPE_VERTEX_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER_RAW);
    output.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
    output.mDesc.mStartState = kOceanVertex;
    output.mDesc.mElementCount = slot.size / 4;
    output.mDesc.mStructStride = 4;
    output.mDesc.pName = "LuxForge skinned current and previous positions";
    addResource(&output, nullptr);
    for (uint32_t i = 0; i < device->framesInFlight; ++i) {
        BufferLoadDesc palette{};
        palette.ppBuffer = &skin.palettes[i];
        palette.mDesc.mElementCount = 1u + 4u * bones;
        palette.mDesc.mStructStride = 16;
        palette.mDesc.mSize = 16u * palette.mDesc.mElementCount;
        palette.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
        palette.mDesc.mStartState = kOceanRead;
        palette.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        palette.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        palette.mDesc.pName = "LuxForge skin bone palette";
        addResource(&palette, nullptr);
    }
    upload.WaitFor(waitForAllResourceLoads);
    upload.Flush(device);
    const DescriptorSetDesc desc = SRT_SET_DESC(LuxSkinSrt, Persistent, device->framesInFlight, 0);
    if (skin.seed && slot.buffer)
        addDescriptorSet(device->renderer, &desc, &skin.set);
    if (!skin.set) {
        ReleaseBuffer(device, slot);
        return 0;
    }
    for (uint32_t i = 0; i < device->framesInFlight; ++i) {
        if (!skin.palettes[i] || !skin.palettes[i]->pCpuMappedAddress) {
            ReleaseBuffer(device, slot);
            return 0;
        }
        DescriptorData params[3]{};
        params[0].mIndex = SRT_RES_IDX(LuxSkinSrt, Persistent, gSkinSeed);
        params[0].ppBuffers = &skin.seed;
        params[1].mIndex = SRT_RES_IDX(LuxSkinSrt, Persistent, gSkinPalette);
        params[1].ppBuffers = &skin.palettes[i];
        params[2].mIndex = SRT_RES_IDX(LuxSkinSrt, Persistent, gSkinOutput);
        params[2].ppBuffers = &slot.buffer;
        updateDescriptorSet(device->renderer, i, skin.set, 3, params);
    }
    upload.AddBytes(seed.mDesc.mSize);
    return TrackBuffer(device, slot);
}

int UpdateSkinBuffer(LuxForgeDevice* device, uint32_t id, const float* palette, uint32_t bones) {
    if (!IsLiveBuffer(device, id) || !device->frameOpen || !palette)
        return 0;
    auto& slot = device->buffers[id];
    if (!slot.skin || slot.skin->bones != bones)
        return 0;
    auto& skin = *slot.skin;
    const uint64_t frame = device->frameNumber + 1;
    if (skin.frame == frame || slot.lastUsedFrame == frame)
        return 0;
    const uint32_t index = device->frameSlot;
    auto* dst = static_cast<float*>(skin.palettes[index]->pCpuMappedAddress);
    dst[0] = float(skin.verts);
    dst[1] = skin.frame ? 1.f : 0.f;
    dst[2] = dst[3] = 0;
    std::memcpy(dst + 4, palette, size_t(bones) * 16 * sizeof(float));
    EndPass(device);
    auto* cmd = device->element.pCmds[0];
    BufferBarrier before{slot.buffer, kOceanVertex, kOceanWrite},
        after{slot.buffer, kOceanWrite, kOceanVertex};
    cmdResourceBarrier(cmd, 1, &before, 0, nullptr, 0, nullptr);
    cmdBindPipeline(cmd, device->skinPipeline);
    cmdBindDescriptorSet(cmd, index, skin.set);
    cmdDispatch(cmd, (skin.verts + 63) / 64, 1, 1);
    cmdResourceBarrier(cmd, 1, &after, 0, nullptr, 0, nullptr);
    skin.frame = slot.lastUsedFrame = frame;
    slot.revision = ++device->resourceRevision;
    ++device->frameStats.pipelineBinds;
    ++device->frameStats.computeDispatches;
    ++device->frameStats.descriptorBinds;
    ++device->frameStats.skinningDispatches;
    return 1;
}
