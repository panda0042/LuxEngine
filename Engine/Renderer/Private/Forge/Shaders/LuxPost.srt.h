#ifndef LUX_POST_SRT_H
#define LUX_POST_SRT_H
BEGIN_SRT(LuxPostSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxPostData), gPost)
DECL_TEXTURE(Persistent, Tex2D(float4), gPostSource)
DECL_TEXTURE(Persistent, Tex2D(float), gPostDepth)
DECL_TEXTURE(Persistent, Tex2D(float4), gBloomNear)
DECL_TEXTURE(Persistent, Tex2D(float4), gBloomMid)
DECL_TEXTURE(Persistent, Tex2D(float4), gBloomFar)
DECL_TEXTURE(Persistent, Tex2D(float4), gAdaptation)
DECL_TEXTURE(Persistent, Tex2D(float4), gGradingLut)
DECL_TEXTURE(Persistent, Tex2D(float4), gGradingLutB)
DECL_BUFFER(Persistent, Buffer(uint2), gHistogram)
END_SRT_SET(Persistent)
END_SRT(LuxPostSrt)
#endif
