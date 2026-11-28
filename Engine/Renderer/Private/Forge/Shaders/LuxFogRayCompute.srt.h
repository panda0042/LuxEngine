#ifndef LUX_FOG_RAY_COMPUTE_SRT_H
#define LUX_FOG_RAY_COMPUTE_SRT_H

#include "LuxFogParams.h"

BEGIN_SRT(LuxFogRayComputeSrt)
BEGIN_SRT_SET(Persistent)
DECL_RAYTRACING(Persistent, RaytracingAccelerationStructure, gRayScene)
DECL_CBUFFER(Persistent, CBUFFER(LuxFogData), gFog)
DECL_RWBUFFER(Persistent, RWBuffer(float4), gFogVolume)
DECL_BUFFER(Persistent, Buffer(float4), gFogHistory)
DECL_TEXTURE(Persistent, Tex2D(float4), gFogCloud)
END_SRT_SET(Persistent)
END_SRT(LuxFogRayComputeSrt)

#endif
