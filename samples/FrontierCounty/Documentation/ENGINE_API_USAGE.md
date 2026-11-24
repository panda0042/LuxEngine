# Public APIs exercised

| Area | Entry points | Sample use |
| --- | --- | --- |
| Window and input | Application, ApplicationSpecification::resizable, Window::GetFramebufferSize, Input | Walk and drive controls, fixed-step updates, relative X/Y mouse input and non-resizable benchmark window and frame presentation. |
| World and RHI | Vec2/Vec3, Mat4, Mesh3D::Upload, Renderer3D::Context::DrawMesh | Double-coordinate chunks, heightfield, triangle-clipped asphalt ribbons, textured FBX models and near 3D spruce, grass and sedge. The worker generates CPU drafts and the render thread uploads them. |
| Lighting and water | Render3DSettings, WaterSurface::Configure/SetTime/SyncGpu/MakeMaterial | PBR/IBL, ACES tone mapping, shadow maps, froxel fog, Gerstner waves, refraction, absorption and environment reflection. G enables optional hardware RT effects if supported. |
| Terrain blending | Material3D::terrainLayer, Vertex3D::color.a | Smooth blend between gravel and Leafy Grass albedo, normal and roughness using slope, dampness, altitude and shoreline distance. |
| Antialiasing | Render3DSettings::temporalAntialiasing and screenSpaceAntialiasing | Jitter and depth-tested reprojection for the primary HDR view. H switches TAA in Frontier County. Existing scenes keep their default behavior. |
| Motion | ModelAsset::LoadFbx, LoadFbxAnimation, Evaluate, EvaluateAnimation, DrawMeshAsset | Three rigged people with idle and walk. One uses an external motion FBX from the same rig. Smoke compares deformed skin vertices at different times. |
| Physics | Scene::CreateEntity, OnUpdate, SetVelocity3D, AddImpulse3D, GetVelocity3D, Raycast3D | Jolt pickup, camera raycasts and static building blockers. The sample supplies terrain-derived spring forces and surface-dependent traction. |
| Diagnostics | GpuDevice::GetFrameStats/GetMemoryStats/GetInfo, SetVSync | Frame timing, draw/dispatch counts, memory and CPU waits. Optional values depend on capabilities. |

The sample calls engine public headers and shared sample text. It uses SDL for locating the deployed asset directory. The renderer and physical integration stay in LuxEngine.
