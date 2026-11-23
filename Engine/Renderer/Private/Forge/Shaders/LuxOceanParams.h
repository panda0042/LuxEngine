#ifndef LUX_OCEAN_PARAMS_H
#define LUX_OCEAN_PARAMS_H

STRUCT(LuxOceanParams) {
    DATA(uint4, shape, None);    // x: grid size, y: evolve immutable modes in the first pass
    DATA(float4, params, None);  // spacing, patch length, foam Jacobian threshold, foam softness
    DATA(float4, params2, None); // x: time in seconds, y: local water level
};

#endif
