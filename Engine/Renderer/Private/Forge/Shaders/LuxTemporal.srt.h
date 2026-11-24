#ifndef LUX_TEMPORAL_SRT_H
#define LUX_TEMPORAL_SRT_H

BEGIN_SRT(LuxTemporalSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxTemporalData), gTemporal)
DECL_TEXTURE(Persistent, Tex2D(float4), gCurrentColor)
DECL_TEXTURE(Persistent, Tex2D(float), gCurrentDepth)
DECL_TEXTURE(Persistent, Tex2D(float4), gPreviousColor)
DECL_TEXTURE(Persistent, Tex2D(float), gPreviousDepth)
END_SRT_SET(Persistent)
END_SRT(LuxTemporalSrt)

#endif
