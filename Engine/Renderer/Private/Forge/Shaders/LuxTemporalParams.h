STRUCT(LuxTemporalData) {
    DATA(float4x4, previousViewProjection, None);
    DATA(float4, right, None); // w: perspective coefficient A
    DATA(float4, up, None);    // w: perspective coefficient B
    DATA(float4, forward, None);
    DATA(float4, viewport, None);    // width, height and current NDC jitter
    DATA(float4, history, None);     // TAA history, water, shutter fraction
    DATA(float4, jitterDelta, None); // previous minus current jitter in UV
};
