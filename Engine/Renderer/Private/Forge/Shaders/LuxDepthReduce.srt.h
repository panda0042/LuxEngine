#ifndef LUX_DEPTH_REDUCE_SRT_H
#define LUX_DEPTH_REDUCE_SRT_H
BEGIN_SRT(LuxDepthReduceSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxScreenLightingData), gReduce)
DECL_TEXTURE(Persistent, Tex2D(float4), gReduceSource)
END_SRT_SET(Persistent)
END_SRT(LuxDepthReduceSrt)
#endif
