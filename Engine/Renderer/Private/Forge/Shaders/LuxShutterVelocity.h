#include "LuxTemporalParams.h"
#include "LuxTemporal.srt.h"

STRUCT(ShutterInput) {
    DATA(float4, position, SV_Position);
    DATA(float2, uv, TEXCOORD0);
};

float ShutterDepth(int2 pixel) {
    const float z = LoadTex2D(gCurrentDepth, NO_SAMPLER, pixel, 0).r;
    return z < 1.0f ? -gTemporal.up.w / max(-gTemporal.right.w - z, 1e-7f) : 1e12f;
}

float2 ShutterVelocity(float2 uvMotion) {
    const float2 velocity =
        (uvMotion - gTemporal.jitterDelta.xy) * gTemporal.viewport.xy * gTemporal.history.z;
    return velocity * min(1.0f, 24.0f / max(length(velocity), 0.001f));
}

float2 CameraShutterVelocity(int2 pixel, float depth) {
    const float2 topUv = (float2(pixel) + 0.5f) / gTemporal.viewport.xy;
    const float2 ndc = (topUv - 0.5f) * float2(2.0f, -2.0f) - gTemporal.viewport.zw;
    const float3 ray =
        gTemporal.forward.xyz + ndc.x * gTemporal.right.xyz + ndc.y * gTemporal.up.xyz;
    // Sky samples rotate with the camera but are invariant under translation.
    const float4 previousInput = depth < 1e11f ? float4(ray * depth, 1.0f) : float4(ray, 0.0f);
    const float4 last = mul(gTemporal.previousViewProjection, previousInput);
    if (last.w <= 0.0f)
        return float2(0, 0);
    return ShutterVelocity(last.xy / last.w * float2(0.5f, -0.5f) + 0.5f - topUv);
}
