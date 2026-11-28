#ifndef LUX_RADIANCE_CASCADES_SRT_H
#define LUX_RADIANCE_CASCADES_SRT_H

#include "LuxRadianceCascadesParams.h"

BEGIN_SRT(LuxRadianceCascadesSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxRadianceCascadesData), gCascade)
#if defined(VULKAN)
DECL_RAYTRACING(Persistent, RaytracingAccelerationStructure, gRayScene)
DECL_BUFFER(Persistent, Buffer(float4), gRayInstances)
DECL_BUFFER(Persistent, Buffer(float4), gRayTriangles)
DECL_TEXTURE(Persistent, TexCube(float4), gCascadeSky)
DECL_TEXTURE(Persistent, Tex2D(float4), gRaySkyGloss)
DECL_TEXTURE(Persistent, Tex2D(float4), gRaySkySH)
#endif
DECL_BUFFER(Persistent, Buffer(float4), gCascadePrevious)
DECL_RWBUFFER(Persistent, RWBuffer(float4), gCascadeOutput)
END_SRT_SET(Persistent)
BEGIN_SRT_SET(PerFrame)
DECL_CBUFFER(PerFrame, CBUFFER(LuxLights), gLights)
END_SRT_SET(PerFrame)
END_SRT(LuxRadianceCascadesSrt)

#endif
