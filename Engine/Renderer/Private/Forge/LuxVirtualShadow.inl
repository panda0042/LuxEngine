// Included by LuxForge.cpp after DrawSceneMeshes.
#include "Shaders/LuxVirtualShadowLayout.h"

struct VirtualShadowPlan {
    bool cascaded = false;
    bool preserve = false;
    uint32_t dirty = 0xffffu;
    uint32_t reused = 0;
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
    uint32_t virtualResolution = 0;
    std::array<uint32_t, 4> packed = {};
    uint32_t count = 0;
    uint32_t requestedPages = 0;
};

uint32_t FogShadowPageMask(const LuxForgeScene& scene) {
    if (!((scene.fogColorDensity[3] > 0.f || scene.fogMedia[3] > 0.f) && scene.fogOptions[0] > 0.f))
        return 0;

    // The view frustum up to the fog distance encloses every froxel represented
    // by the volume. Project its tip and four far corners conservatively.
    const auto& shadow = scene.shadowMatrix;
    if (shadow[3] != 0.f || shadow[7] != 0.f || shadow[11] != 0.f || !(shadow[15] > 0.f) ||
        !std::isfinite(shadow[15]))
        return LUX_VIRTUAL_SHADOW_ALL_PAGES;
    const auto project = [&](int screenX, int screenY, double distance, int row) {
        double coordinate = shadow[12 + row];
        for (int axis = 0; axis < 3; ++axis) {
            const double endpoint =
                scene.camera[axis] +
                (scene.forward[axis] + screenX * scene.right[axis] + screenY * scene.up[axis]) *
                    distance;
            coordinate += shadow[axis * 4 + row] * endpoint;
        }
        return coordinate / shadow[15];
    };
    double minX = .5 + .5 * project(0, 0, 0, 0);
    double maxX = minX;
    double minY = .5 - .5 * project(0, 0, 0, 1);
    double maxY = minY;
    for (int y : {-1, 1})
        for (int x : {-1, 1}) {
            const double u = .5 + .5 * project(x, y, scene.fogOptions[0], 0);
            const double v = .5 - .5 * project(x, y, scene.fogOptions[0], 1);
            if (!std::isfinite(u) || !std::isfinite(v))
                return LUX_VIRTUAL_SHADOW_ALL_PAGES;
            minX = std::min(minX, u);
            maxX = std::max(maxX, u);
            minY = std::min(minY, v);
            maxY = std::max(maxY, v);
        }
    if (!std::isfinite(minX + maxX + minY + maxY))
        return LUX_VIRTUAL_SHADOW_ALL_PAGES;

    const double guard = 2. / scene.shadowResolution;
    uint32_t mask = 0;
    constexpr int Grid = LUX_VIRTUAL_SHADOW_PAGE_GRID;
    for (int y = 0; y < Grid; ++y)
        for (int x = 0; x < Grid; ++x) {
            if (minX - guard <= (x + 1.) / Grid && maxX + guard >= double(x) / Grid &&
                minY - guard <= (y + 1.) / Grid && maxY + guard >= double(y) / Grid)
                mask |= 1u << (y * Grid + x);
        }
    return mask;
}

VirtualShadowPlan PlanVirtualShadow(const LuxForgeDevice* device, const LuxForgeScene& scene,
                                    const std::vector<SceneMesh>& visible, size_t colorEnd) {
    VirtualShadowPlan plan;
    plan.atlasWidth = plan.atlasHeight = scene.shadowResolution;
    constexpr uint32_t Grid = LUX_VIRTUAL_SHADOW_PAGE_GRID;
    constexpr uint32_t Atlas = LUX_VIRTUAL_SHADOW_ATLAS_GRID;
    // The translated shadow viewport must fit the coordinate limits on the target device.
    if (!scene.virtualShadows || scene.shadowResolution < 16 || scene.shadowResolution > 4096 ||
        scene.shadowResolution % Grid)
        return plan;

    uint32_t mask = FogShadowPageMask(scene);
    for (size_t i = 0; i < colorEnd; ++i)
        mask |= visible[i].draw->shadowPageMask;
    mask &= LUX_VIRTUAL_SHADOW_ALL_PAGES;
    uint32_t count = 0;
    for (uint32_t bits = mask; bits != 0; bits &= bits - 1)
        ++count;
    plan.requestedPages = count;
    if (count > Grid * Atlas)
        return plan;

    plan.count = count;
    plan.virtualResolution = scene.shadowResolution;
    const uint32_t tileSize = scene.shadowResolution / Grid;
    const uint32_t columns = count > Atlas * Atlas ? Grid : Atlas;
    plan.atlasWidth = tileSize * columns;
    plan.atlasHeight = tileSize * Atlas;
    plan.packed = {tileSize, columns, UINT32_MAX, UINT32_MAX};
    uint32_t occupied = 0;
    auto assign = [&](uint32_t page, uint32_t physical) {
        const uint32_t shift = (page & 7u) * 4u;
        uint32_t& entries = plan.packed[2 + page / 8];
        entries = (entries & ~(15u << shift)) | (physical << shift);
        occupied |= 1u << physical;
    };
    if (device->shadowPageCacheValid && plan.packed[0] == device->lastShadowTable[0] &&
        plan.packed[1] == device->lastShadowTable[1]) {
        for (uint32_t page = 0; page < Grid * Grid; ++page) {
            const auto slot = (device->lastShadowTable[2 + page / 8] >> (4 * (page & 7u))) & 15u;
            if ((mask & (1u << page)) && slot != LUX_VIRTUAL_SHADOW_INVALID_PAGE)
                assign(page, slot);
        }
    }
    for (uint32_t page = 0; page < Grid * Grid; ++page) {
        if (!(mask & (1u << page)) ||
            ((plan.packed[2 + page / 8] >> (4 * (page & 7u))) & 15u) != 15u)
            continue;
        uint32_t next = 0;
        while (occupied & (1u << next))
            ++next;
        assign(page, next);
    }
    return plan;
}

// Immutable textures have generation numbers; a geometry upload invalidates only pages
// touched by that caster. Hash the raster transform and alpha/wind inputs, not lighting.
void TrackVirtualShadowPages(LuxForgeDevice* device, const LuxForgeScene& scene,
                             const std::vector<SceneMesh>& draws, VirtualShadowPlan& plan,
                             bool temporal) {
    if (!plan.virtualResolution) {
        device->shadowPageCacheValid = false;
        return;
    }
    const auto mix = [](uint64_t& hash, uint64_t value) {
        hash = (hash ^ value) * UINT64_C(1099511628211);
    };
    const auto bits = [](float value) {
        uint32_t result;
        std::memcpy(&result, &value, sizeof result);
        return result;
    };
    std::array<uint64_t, 16> keys;
    keys.fill(UINT64_C(14695981039346656037));
    for (const auto& mesh : draws) {
        const auto& draw = *mesh.draw;
        if (!draw.shadowPageMask || draw.alphaMode == 2 || WaterDraw(draw))
            continue;
        uint64_t key = UINT64_C(14695981039346656037);
        mix(key, device->buffers[draw.vertexBuffer].revision);
        mix(key, device->buffers[draw.indexBuffer].revision);
        mix(key, draw.indexCount);
        for (float coefficient : draw.model)
            mix(key, bits(coefficient));
        mix(key, bits(draw.lodTransition));
        if (draw.alphaMode == 1) {
            mix(key, device->textures[draw.texture ? draw.texture : device->whiteTexture].revision);
            mix(key, bits(draw.alphaCutoff));
            mix(key, bits(draw.albedo[3]));
            mix(key, draw.sampling);
            mix(key, temporal);
            mix(key, draw.motionInstance);
        }
        if (draw.foliage[0] > 0.f) {
            mix(key, bits(draw.foliage[0]));
            for (const auto& region : scene.foliageContacts)
                for (float value : region)
                    mix(key, bits(value));
            for (float value : scene.wind)
                mix(key, bits(value));
        }
        for (unsigned page = 0; page < 16; ++page)
            if (draw.shadowPageMask & (1u << page))
                mix(keys[page], key);
    }
    plan.preserve = device->shadowPageCacheValid && plan.packed[0] == device->lastShadowTable[0] &&
                    plan.packed[1] == device->lastShadowTable[1];
    plan.dirty = 0;
    for (uint32_t page = 0; page < 16; ++page) {
        const uint32_t shift = (page & 7u) * 4u;
        const auto slot = (plan.packed[2 + page / 8] >> shift) & 15u;
        if (slot == LUX_VIRTUAL_SHADOW_INVALID_PAGE)
            continue;
        for (float value : scene.shadowMatrix)
            mix(keys[page], bits(value));
        mix(keys[page], plan.cascaded);
        if (!plan.preserve || slot != ((device->lastShadowTable[2 + page / 8] >> shift) & 15u) ||
            device->shadowPageKeys[page] != keys[page])
            plan.dirty |= 1u << page;
        else
            ++plan.reused;
    }
    device->shadowPageKeys = keys;
    device->lastShadowTable = plan.packed;
    device->shadowPageCacheValid = true;
}

void ClearShadowTile(LuxForgeDevice* device, Cmd* cmd, uint32_t x, uint32_t y, uint32_t size) {
    cmdSetViewport(cmd, float(x), float(y), float(size), float(size), 1.f, 1.f);
    cmdSetScissor(cmd, x, y, size, size);
    BindPipeline(device, cmd, device->shadowClearPipeline);
    cmdDraw(cmd, 3, 0);
    ++device->frameStats.drawCalls;
}

void DrawVirtualShadowPages(LuxForgeDevice* device, Cmd* cmd, const std::vector<SceneMesh>& visible,
                            uint32_t firstDraw, uint32_t entry, const VirtualShadowPlan& plan) {
    const auto masked = std::any_of(visible.begin(), visible.end(), [](const SceneMesh& mesh) {
        return (mesh.draw->alphaMode == 1 || mesh.draw->lodTransition != 0) &&
               mesh.draw->shadowPageMask && !WaterDraw(*mesh.draw);
    });
    auto* shadowPipeline = masked ? device->pipelineShadow : device->pipelineOpaqueShadow;
    const uint32_t outerOrigin = plan.cascaded ? plan.atlasWidth / 2 : 0;
    if (plan.cascaded) {
        const float size = static_cast<float>(outerOrigin);
        for (uint32_t cascade = 0; cascade < 3; ++cascade) {
            const uint32_t x = (cascade & 1u) * outerOrigin, y = (cascade >> 1u) * outerOrigin;
            if (plan.preserve)
                ClearShadowTile(device, cmd, x, y, outerOrigin);
            cmdSetViewport(cmd, float(x), float(y), size, size, 0.f, 1.f);
            cmdSetScissor(cmd, x, y, outerOrigin, outerOrigin);
            DrawSceneMeshes(device, cmd, visible, firstDraw, 0, visible.size(), nullptr,
                            entry + 4 + cascade, shadowPipeline);
        }
    }
    if (!plan.virtualResolution) {
        const float resolution = static_cast<float>(plan.atlasWidth - outerOrigin);
        cmdSetViewport(cmd, float(outerOrigin), float(outerOrigin), resolution, resolution, 0.f,
                       1.f);
        cmdSetScissor(cmd, outerOrigin, outerOrigin, plan.atlasWidth - outerOrigin,
                      plan.atlasHeight - outerOrigin);
        DrawSceneMeshes(device, cmd, visible, firstDraw, 0, visible.size(), nullptr, entry,
                        shadowPipeline);
        return;
    }
    constexpr uint32_t Grid = LUX_VIRTUAL_SHADOW_PAGE_GRID;
    const auto tileSize = plan.packed[0];
    const auto atlasGrid = plan.packed[1];
    const float resolution = static_cast<float>(plan.virtualResolution);
    Pipeline* pipeline = nullptr;
    for (uint32_t page = 0; page < Grid * Grid; ++page) {
        const uint32_t physical = (plan.packed[2 + page / 8] >> ((page & 7u) * 4u)) & 15u;
        if (physical == LUX_VIRTUAL_SHADOW_INVALID_PAGE || !(plan.dirty & (1u << page)))
            continue;
        const uint32_t atlasX = outerOrigin + physical % atlasGrid * tileSize;
        const uint32_t atlasY = outerOrigin + physical / atlasGrid * tileSize;
        if (plan.preserve) {
            ClearShadowTile(device, cmd, atlasX, atlasY, tileSize);
            pipeline = nullptr;
        }
        const float originX =
            static_cast<float>(atlasX) - static_cast<float>(page % Grid * tileSize);
        const float originY =
            static_cast<float>(atlasY) - static_cast<float>(page / Grid * tileSize);
        // Keep the original clip planes and texel density. Translate only rasterization and
        // constrain writes to the physical page.
        cmdSetViewport(cmd, originX, originY, resolution, resolution, 0.f, 1.f);
        cmdSetScissor(cmd, atlasX, atlasY, tileSize, tileSize);
        pipeline = DrawSceneMeshes(device, cmd, visible, firstDraw, 0, visible.size(), pipeline,
                                   entry, shadowPipeline, false, 1u << page);
    }
}
