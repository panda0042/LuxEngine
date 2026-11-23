#ifndef LUX_FOG_COMPOSITE_SRT_H
#define LUX_FOG_COMPOSITE_SRT_H

#include "LuxFogParams.h"

BEGIN_SRT(LuxFogCompositeSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxFogData), gFog)
DECL_TEXTURE(Persistent, Tex2D(float4), gFogHdr)
DECL_TEXTURE(Persistent, Tex2D(float), gFogDepth)
DECL_BUFFER(Persistent, Buffer(float4), gFogVolume)
END_SRT_SET(Persistent)
END_SRT(LuxFogCompositeSrt)

#endif
