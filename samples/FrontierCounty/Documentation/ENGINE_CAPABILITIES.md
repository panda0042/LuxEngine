# LuxEngine capability audit

Baseline a3d2d81 on main. This inventory covers the public runtime subsystems, their entry points and the paths relevant to the Frontier County renderer sample. API presence and sample runtime verification are tracked separately.

| Subsystem | Evidence in LuxEngine | Capability and limit |
| --- | --- | --- |
| Core and platform | `Engine/Core/Public`, `Engine/Platform/Public`, `docs/WorldCoordinates.md` | Double-precision world math, camera-relative GPU coordinates, SDL windows, framebuffer size, input action bindings, gamepad buttons and lifecycle events. |
| Application and runtime | `LuxEngine/Application.hpp`, `Engine/Runtime/Public`, `samples/ScenePlayer` | Fixed-step callbacks, frame/present lifecycle, scene player and standalone sample executables. |
| RHI and renderer | `Engine/RHI/Public`, `LuxEngine/Renderer/Renderer3D/Renderer3D.hpp`, `docs/TheForge.md` | The Forge uses Vulkan on Linux, D3D12 on Windows and Metal on macOS. Public PBR/HDR/ACES, directional and point lights, IBL, skybox, screen color/depth water refraction, atmosphere panorama baking, dynamic atmosphere, ray-marched volumetric clouds, froxel fog and HDR camera effects. |
| Shadows, reflections and GI | `LuxEngine/Renderer/Renderer3D/Render3DSettings.hpp`, `samples/RayTracing`, `samples/LakesideOcean` | Raster shadows, capability-dependent virtual shadows and hardware ray tracing, water reflection capture, public ReSTIR and radiance cascade settings. These paths require hardware and individual passes to be verified. |
| Water | `LuxEngine/Renderer/Water/WaterSurface.hpp`, `OceanSpectrum.hpp` | Public Gerstner surface and CPU sample, GPU FFT patch, spectral sampling and buoyancy impulse helper. |
| Assets | `Engine/Assets/Public`, `Engine/Renderer/Private/Renderer3D`, `tools/PrepareHdrEnvironment.py` | Images, models, HDR import, mipmaps, sampling and renderer texture caches. No public sparse texture residency or virtual texture streaming controller was found. |
| Scene and ECS | `LuxEngine/Scene/Scene.hpp`, `LuxEngine/Scene/Entity.hpp`, `Engine/ECS/Public` | Components bound to scene entities, scene update, serialization and asset resolution. No public world partition. |
| Physics | `LuxEngine/Physics/PhysicsWorld.hpp`, `LuxEngine/Scene/Scene.hpp`, `docs/JoltPhysics.md` | Jolt box bodies, sensors, collision events, CCD, raycast, grounded and impulse helpers. No public triangle terrain collider, vehicle constraints, shape cast or capsule character controller. |
| Animation | `Engine/Animation/Public`, `docs/AnimationWorkspace.md` | Clips, FBX skeletal poses, blend tree, layers, masks, root motion and events. Public full-body IK, ragdoll and retargeting paths were not found. The same-rig external FBX clip API is exercised by Frontier County. |
| Audio | `Engine/Audio/Public`, `Engine/Audio/Private` | Sound loading, playback, volume, pitch, positional source and listener facilities. No public gameplay reverb zone, obstruction or streaming mixer was found. |
| UI | `LuxEngine/Renderer/Renderer2D/Renderer2D.hpp`, `samples/Common`, `Editor/LuxUI` | Runtime quads and text, sample HUD helpers. LuxUI is the Qt editor framework. |
| Scripting | `Engine/Scripting/Public`, `Engine/Scripting/Managed`, `docs/CSharpScripting.md` | .NET 10 native host and managed game bindings with an independently versioned ABI. This sample uses native C++ to reach public APIs that lack managed bindings. |
| Threading and loading | `Engine/Core/Public`, `Engine/Physics/Private/PhysicsWorld.cpp`, `Engine/RHI` | Internal async HDR preparation and upload helpers. No general public task scheduler, and the current Jolt adapter selects a single-threaded job system. |
| Builds | `CMakeLists.txt`, `samples/CMakeLists.txt`, `cmake/` | CMake C++20 targets and CTest, engine dependency presets, The Forge shader tooling and runtime deployment. |

See [API usage](ENGINE_API_USAGE.md) for paths exercised by this sample and [missing features](ENGINE_MISSING_FEATURES.md) for remaining dependencies and game content.
