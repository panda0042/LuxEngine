#ifndef LUX_RAY_WAVELET_SRT_H
#define LUX_RAY_WAVELET_SRT_H
#include "LuxStructs.h"
BEGIN_SRT(LuxRayWaveletSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxSceneData), gScene)
DECL_BUFFER(Persistent, Buffer(float4), gFeatures)
DECL_BUFFER(Persistent, Buffer(float4), gSignal)
DECL_RWBUFFER(Persistent, RWBuffer(float4), gFiltered)
END_SRT_SET(Persistent)
END_SRT(LuxRayWaveletSrt)
#endif
