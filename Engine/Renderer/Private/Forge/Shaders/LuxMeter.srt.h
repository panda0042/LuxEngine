#ifndef LUX_METER_SRT_H
#define LUX_METER_SRT_H
BEGIN_SRT(LuxMeterSrt)
BEGIN_SRT_SET(Persistent)
DECL_TEXTURE(Persistent, Tex2D(float4), gMeterSource)
DECL_RWBUFFER(Persistent, RWBuffer(uint2), gMeterBins)
END_SRT_SET(Persistent)
END_SRT(LuxMeterSrt)
#endif
