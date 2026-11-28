#ifndef LUX_FOG_COMPUTE_SRT_H
#define LUX_FOG_COMPUTE_SRT_H

#include "LuxFogParams.h"

BEGIN_SRT(LuxFogComputeSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxFogData), gFog)
DECL_TEXTURE(Persistent, Tex2D(float), gFogShadow)
DECL_RWBUFFER(Persistent, RWBuffer(float4), gFogVolume)
DECL_BUFFER(Persistent, Buffer(float4), gFogHistory)
DECL_TEXTURE(Persistent, Tex2D(float4), gFogCloud)
END_SRT_SET(Persistent)
END_SRT(LuxFogComputeSrt)

#endif
