#ifndef LUX_VIRTUAL_SHADOW_H
#define LUX_VIRTUAL_SHADOW_H

#include "LuxVirtualShadowLayout.h"

// Map every PCF tap independently, so adjacent virtual pages need not share atlas edges.
int2 ShadowAtlasPixel(int2 virtualPixel, uint4 table) {
    if (table.x == 0u)
        return virtualPixel;
    const uint2 page = uint2(virtualPixel) / table.x;
    const uint index = page.y * LUX_VIRTUAL_SHADOW_PAGE_GRID + page.x;
    const uint packed = index < 8u ? table.z : table.w;
    const uint physical = (packed >> ((index & 7u) * 4u)) & 15u;
    if (physical == LUX_VIRTUAL_SHADOW_INVALID_PAGE)
        return int2(-1, -1);
    const uint2 atlas = uint2(physical % table.y, physical / table.y);
    return int2(atlas * table.x + uint2(virtualPixel) % table.x);
}

#endif
