// Included by LuxForge.cpp after DrawSceneMeshes.
#include "Shaders/LuxVirtualShadowLayout.h"

struct VirtualShadowPlan {
    uint32_t atlasWidth = 0;
    uint32_t atlasHeight = 0;
    uint32_t virtualResolution = 0;
    std::array<uint32_t, 4> packed = {};
    uint32_t count = 0;
    uint32_t requestedPages = 0;
};

uint32_t FogShadowPageMask(const LuxForgeScene& scene) {
    if (!(scene.fogColorDensity[3] > 0.f && scene.fogOptions[0] > 0.f))
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

VirtualShadowPlan PlanVirtualShadow(const LuxForgeScene& scene,
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
    uint32_t next = 0;
    for (uint32_t page = 0; page < Grid * Grid; ++page) {
        if ((mask & (1u << page)) == 0)
            continue;
        const uint32_t shift = (page & 7u) * 4u;
        uint32_t& entries = plan.packed[2 + page / 8];
        entries = (entries & ~(15u << shift)) | (next++ << shift);
    }
    return plan;
}

void DrawVirtualShadowPages(LuxForgeDevice* device, Cmd* cmd, const std::vector<SceneMesh>& visible,
                            uint32_t firstDraw, uint32_t entry, const VirtualShadowPlan& plan) {
    const auto masked = std::any_of(visible.begin(), visible.end(), [](const SceneMesh& mesh) {
        return mesh.draw->alphaMode == 1 && mesh.draw->shadowPageMask && !WaterDraw(*mesh.draw);
    });
    auto* shadowPipeline = masked ? device->pipelineShadow : device->pipelineOpaqueShadow;
    if (!plan.virtualResolution) {
        const float resolution = static_cast<float>(plan.atlasWidth);
        cmdSetViewport(cmd, 0.f, 0.f, resolution, resolution, 0.f, 1.f);
        cmdSetScissor(cmd, 0, 0, plan.atlasWidth, plan.atlasHeight);
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
        if (physical == LUX_VIRTUAL_SHADOW_INVALID_PAGE)
            continue;
        const uint32_t atlasX = physical % atlasGrid * tileSize;
        const uint32_t atlasY = physical / atlasGrid * tileSize;
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
