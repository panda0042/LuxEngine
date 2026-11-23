#ifndef LUX_FOG_RAY_COMPUTE_SRT_H
#define LUX_FOG_RAY_COMPUTE_SRT_H

#include "LuxFogParams.h"

BEGIN_SRT(LuxFogRayComputeSrt)
BEGIN_SRT_SET(Persistent)
DECL_RAYTRACING(Persistent, RaytracingAccelerationStructure, gRayScene)
DECL_CBUFFER(Persistent, CBUFFER(LuxFogData), gFog)
DECL_RWBUFFER(Persistent, RWBuffer(float4), gFogVolume)
END_SRT_SET(Persistent)
END_SRT(LuxFogRayComputeSrt)

#endif
