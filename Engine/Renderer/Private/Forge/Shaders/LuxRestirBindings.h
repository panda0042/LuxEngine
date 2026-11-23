BEGIN_SRT_SET(Persistent)
DECL_CBUFFER(Persistent, CBUFFER(LuxSceneData), gScene)
DECL_RAYTRACING(Persistent, RaytracingAccelerationStructure, gRayScene)
DECL_BUFFER(Persistent, Buffer(float4), gRayInstances)
DECL_BUFFER(Persistent, Buffer(float4), gRayTriangles)
#ifdef LUX_RESTIR_SEEDING
DECL_TEXTURE(Persistent, TexCube(float4), gRestirEnvironment)
DECL_RWBUFFER(Persistent, RWBuffer(float4), gRestirCandidates)
#else
DECL_BUFFER(Persistent, Buffer(float4), gRestirCandidates)
DECL_RWBUFFER(Persistent, RWBuffer(float4), gRestirResult)
#endif
END_SRT_SET(Persistent)
BEGIN_SRT_SET(PerFrame)
DECL_CBUFFER(PerFrame, CBUFFER(LuxLights), gLights)
END_SRT_SET(PerFrame)
