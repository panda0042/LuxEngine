#ifndef LUX_SKY_CACHE_SRT_H
#define LUX_SKY_CACHE_SRT_H
BEGIN_SRT(LuxSkyCacheSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxSceneData), gWeather)
DECL_CBUFFER(Persistent, CBUFFER(LuxLights), gSolar)
DECL_TEXTURE(Persistent, Tex2D(float4), gSkySource)
END_SRT_SET(Persistent)
END_SRT(LuxSkyCacheSrt)
#endif
