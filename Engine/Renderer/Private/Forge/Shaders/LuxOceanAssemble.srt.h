#ifndef LUX_OCEAN_ASSEMBLE_SRT_H
#define LUX_OCEAN_ASSEMBLE_SRT_H

#include "LuxOceanParams.h"

BEGIN_SRT(LuxOceanAssembleSrt)
BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxOceanParams), gOcean)
DECL_BUFFER(Persistent, Buffer(float2), gInput)
DECL_RWBUFFER(Persistent, RWByteBuffer, gOutput)
END_SRT_SET(Persistent)
END_SRT(LuxOceanAssembleSrt)

#endif
