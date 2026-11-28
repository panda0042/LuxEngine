// Surface and lighting views share their persisted IDs with PostDebugView.
#define LUX_DEBUG_BASE_COLOR 4u
#define LUX_DEBUG_WORLD_NORMAL 5u
#define LUX_DEBUG_GEOMETRIC_NORMAL 6u
#define LUX_DEBUG_ROUGHNESS 7u
#define LUX_DEBUG_METALLIC 8u
#define LUX_DEBUG_MATERIAL_AO 9u
#define LUX_DEBUG_GTAO 10u
#define LUX_DEBUG_BENT_NORMAL 11u
#define LUX_DEBUG_DIRECT_LIGHTING 12u
#define LUX_DEBUG_INDIRECT_LIGHTING 13u
#define LUX_DEBUG_IBL 14u
#define LUX_DEBUG_EMISSIVE 15u
#define LUX_DEBUG_SCENE_DEPTH 16u
#define LUX_DEBUG_LINEAR_DEPTH 17u
#define LUX_DEBUG_DEPTH_PYRAMID 18u
#define LUX_DEBUG_SHADOW_MAP 19u
#define LUX_DEBUG_SHADOW_CASCADE 20u
#define LUX_DEBUG_VIRTUAL_SHADOW_PAGE 21u
#define LUX_DEBUG_CONTACT_SHADOWS 22u
#define LUX_DEBUG_CLOUD_SHADOWS 23u
#define LUX_DEBUG_RESTIR_RESERVOIR 24u
#define LUX_DEBUG_RAY_NOISE 25u
#define LUX_DEBUG_DENOISED_LIGHTING 26u
#define LUX_DEBUG_SSR_CONFIDENCE 27u
#define LUX_DEBUG_VOLUMETRIC_FOG 28u
#define LUX_DEBUG_WATER_DEPTH 29u
#define LUX_DEBUG_CLOUD_DENSITY 30u
#define LUX_DEBUG_WATER_REFLECTION 31u
#define LUX_DEBUG_TERRAIN_WEIGHTS 32u
#define LUX_DEBUG_VEGETATION_LOD 33u

// View IDs follow the serialized PostDebugView enumeration.
float4 GeometryDiagnostic(uint debug, MeshOutput In, float3 shadowNormal, bool palette,
                          uint extraMaps) {
    if (debug == LUX_DEBUG_GEOMETRIC_NORMAL)
        return float4(shadowNormal * .5f + .5f, 1);
    if (debug == LUX_DEBUG_SCENE_DEPTH)
        return float4(float3(1, 1, 1) * In.position.z, 1);
    if (debug == LUX_DEBUG_LINEAR_DEPTH)
        return float4(
            float3(1, 1, 1) * saturate(log2(1.0f + ScreenEyeDepth(In.position.z)) / 12.0f), 1);
    if (debug == LUX_DEBUG_DEPTH_PYRAMID) {
        if (gScreenLighting.options.z <= 0.0f)
            return float4(0, 0, 0, 1);
        const int2 coord = int2(In.position.xy) / 16;
        const float2 bounds = LoadTex2D(gDepthSixteenth, NO_SAMPLER, coord, 0).rg;
        const float2 metric =
            log2(1.0f + float2(ScreenEyeDepth(bounds.x), ScreenEyeDepth(bounds.y))) / 12.0f;
        return float4(saturate(metric), 0, 1);
    }
    if (debug >= LUX_DEBUG_SHADOW_MAP && debug <= LUX_DEBUG_VIRTUAL_SHADOW_PAGE) {
        if (gScene.shadowOptions.x <= 0.0f)
            return float4(0, 0, 0, 1);
        const float distance = dot(In.world - gLights.camera.xyz, gScene.forward.xyz);
        const int split = gScene.shadowCascades[3].w <= 0.0f      ? -1
                          : distance < gScene.shadowCascades[0].w ? 0
                          : distance < gScene.shadowCascades[1].w ? 1
                          : distance < gScene.shadowCascades[2].w ? 2
                                                                  : 3;
        if (split >= 0 && distance >= gScene.shadowCascades[3].w)
            return float4(0, 0, 0, 1);
        if (debug == LUX_DEBUG_SHADOW_CASCADE) {
            const float3 tint = split == 0   ? float3(1, .18f, .08f)
                                : split == 1 ? float3(.1f, .8f, .14f)
                                : split == 2 ? float3(.1f, .22f, .9f)
                                             : float3(.83f, .68f, .1f);
            return float4(tint, 1);
        }
        const float4 clip = mul(gScene.shadowMatrix, float4(In.world, 1));
        const float4 crop = split < 0 ? float4(1, 0, 0, 0) : gScene.shadowCascades[max(split, 0)];
        const float2 uv = (clip.xy / clip.w * crop.x + crop.yz) * float2(.5f, -.5f) + .5f;
        if (uv.x < 0.0f || uv.x >= 1.0f || uv.y < 0.0f || uv.y >= 1.0f)
            return float4(0, 0, 0, 1);
        const int resolution = int(round(rcp(gScene.shadowOptions.y)));
        const int2 texel = int2(uv * float(resolution));
        if (debug == LUX_DEBUG_SHADOW_MAP)
            return float4(float3(1, 1, 1) * ShadowDepthAt(texel, resolution, split), 1);
        if ((split != -1 && split != 3) || gScene.virtualShadowPages.x == 0u)
            return float4(0, 0, 0, 1);
        const int2 key = ShadowAtlasPixel(texel, gScene.virtualShadowPages);
        if (key.x < 0)
            return float4(0, 0, 0, 1);
        const int2 page = key / int(gScene.virtualShadowPages.x);
        return float4(float2(page + 1) / (float(gScene.virtualShadowPages.y) + 1.0f), .25f, 1);
    }
    if (debug == LUX_DEBUG_CONTACT_SHADOWS)
        return float4(float3(1, 1, 1) * ScreenContactShadow(In.world, shadowNormal), 1);
    if (debug == LUX_DEBUG_CLOUD_SHADOWS)
        return float4(float3(1, 1, 1) * CloudVisibility(In.world), 1);
    if (debug == LUX_DEBUG_VOLUMETRIC_FOG || debug == LUX_DEBUG_CLOUD_DENSITY)
        return float4(0, 0, 0, 1);
    if (debug == LUX_DEBUG_TERRAIN_WEIGHTS)
        return float4(palette ? In.color.rgb : float3(0, 0, 0), 1);
    if (debug == LUX_DEBUG_VEGETATION_LOD) {
        const float blend = abs(gDraw.lod.x);
        const bool impostor = (extraMaps & 32u) != 0u;
        const bool plant = gDraw.foliage.x > 0.0f || impostor;
        return float4(plant ? (impostor ? float3(.9f, .22f, blend) : float3(blend, .8f, .2f))
                            : float3(0, 0, 0),
                      1);
    }
    return float4(0, 0, 0, -1);
}
