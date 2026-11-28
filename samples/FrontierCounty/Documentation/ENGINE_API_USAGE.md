# Public APIs exercised

| Area | Entry points | Sample use |
| --- | --- | --- |
| Window and input | Application, ApplicationSpecification::resizable, Window::GetFramebufferSize, Input | Walk and drive controls, fixed-step updates, relative X/Y mouse input and non-resizable benchmark window and frame presentation. |
| World and RHI | Vec2/Vec3, Mat4, Mesh3D::Upload, Renderer3D::Context::DrawMesh | Double-coordinate chunks, heightfield, triangle-clipped asphalt ribbons, textured FBX models and near 3D spruce, grass and sedge. The worker generates CPU drafts and the render thread uploads them. |
| Lighting and water | Render3DSettings, WaterSurface::Configure/SetTime/SyncGpu/MakeMaterial | PBR/IBL, ACES tone mapping, shadow maps, froxel fog, Gerstner waves, refraction, absorption and environment reflection. G enables optional hardware RT effects if supported. |
| Terrain blending | Material3D::terrainLayer, Vertex3D::color.a | Slope, dampness, altitude and shoreline distance paint the gravel–grass transition. Both measured displacement maps drive the height blend. High and above trace 3.5 cm of UV relief using Material3D::parallaxDepth. |
| Antialiasing | Render3DSettings::temporalAntialiasing and screenSpaceAntialiasing | Jitter and depth-tested reprojection for the primary HDR view. H switches TAA in Frontier County. Existing scenes keep their default behavior. |
| Atmosphere and vegetation | `dynamicAtmosphere`, `cloudCoverage`, `windVelocity`, `weatherTime`, `Vertex3D::windWeight` | The simulation drives the sun, moves the cloud layer and bends foliage. Masked grass, sedge and needles have a thin-sheet scattering material. |
| Screen lighting | `gtaoRadius`, `contactShadowDistance`, `shadowLightAngularRadius`, `hierarchicalReflections` | High enables ambient occlusion, contact and soft shadows. SSR supplies on-screen reflections in raster mode. |
| Camera effects | `autoExposure`, `bloomIntensity`, `colorGradingLut`, `focusDistance`, `depthOfFieldRadius` | High enables energy-conserving bloom, ISO 100 photographic exposure, eye adaptation and local exposure. Cinematic and Extreme add a thin-lens seven-blade bokeh and shutter-driven motion blur and lens effects. The sample includes its own noon grading LUT. |
| Surface weather | `surfaceWetness`, `Material3D::wettingResponse`, `WaterMaterial3D::rippleStrength` | Rain soaks the soil, asphalt and outdoor props. Sun and cloud cover govern drying. High and above add wind-driven multi-scale lake ripples. |
| Projected markings | `DecalProjector`, `AppendProjectedDecal` | Road paint uses triangles clipped to the surface of each asphalt segment. |
| Motion | ModelAsset::LoadFbx, LoadFbxAnimation, Evaluate, EvaluateAnimation, DrawMeshAsset | Three licensed rigged people use idle and walk clips, pose-based ground contact and skin/cloth tissue masks. The sample opts into compute skinning for compatible linear poses. Other poses and ray scenes use the CPU path. One actor uses an external motion FBX from the same rig. |
| Physics | Scene::CreateEntity, OnUpdate, SetVelocity3D, AddImpulse3D, GetVelocity3D, Raycast3D | Jolt pickup, camera raycasts and static building blockers. The sample supplies terrain-derived spring forces and surface-dependent traction. |
| Diagnostics | GpuDevice::GetFrameStats/GetMemoryStats/GetInfo, SetVSync | Frame timing, draw/dispatch counts, memory and CPU waits. Optional values depend on capabilities. |

The sample calls engine public headers and shared sample text. It uses SDL for locating the deployed asset directory. The renderer and physical integration stay in LuxEngine.
