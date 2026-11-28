#ifndef LUX_SKIN_SRT_H
#define LUX_SKIN_SRT_H
BEGIN_SRT(LuxSkinSrt)
BEGIN_SRT_SET(Persistent)
DECL_BUFFER(Persistent, Buffer(float4), gSkinSeed)
DECL_BUFFER(Persistent, Buffer(float4), gSkinPalette)
DECL_RWBUFFER(Persistent, RWByteBuffer, gSkinOutput)
END_SRT_SET(Persistent)
END_SRT(LuxSkinSrt)
#endif
