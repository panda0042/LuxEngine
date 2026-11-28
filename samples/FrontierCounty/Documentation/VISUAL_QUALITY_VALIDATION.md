# Frontier County visual validation

Baseline is PR #90 at `513e7f17a8014ebb8a2f65737cddd4644588e287`. The comparison uses the same fixed-shot definitions on both builds, seed 4805, the High preset, and 24 simulation frames after scene readiness. Candidate verification uses an RTX 2060 on Linux with The Forge's Vulkan synchronization validation enabled.

## Renderer changes

| Area | Implementation and validation |
| --- | --- |
| Temporal rendering | TAA reconciles jitter and previous camera transforms. It rejects mismatched depth and surface identifiers, and reconstructs accepted history using Catmull–Rom filtering. Skinned positions, vegetation wind, Gerstner and FFT displacement contribute motion. The shutter pass estimates local velocity across adjacent tiles. |
| Vegetation | Alpha-coverage mips and material-space sampling apply to the associated depth, shadow, motion and color passes. Spruce LODs use complementary stochastic fades. Far representations contain color, normal and depth baked from the same licensed spruce used up close. Foliage uses a thin-surface lighting model. |
| Shadow and indirect light | Stable directional splits blend at the transition. The far virtual shadow cache tracks caster and light changes, and contact shadow rays reject the receiving plane. GTAO estimates bent directions and hierarchical SSR uses min/max depth. ReSTIR and radiance cascades use the active sky on supported devices. The opaque GPU denoiser filters separate diffuse and specular signals. |
| Atmosphere and fog | Transmittance, multiple scattering, sky-view and aerial perspective LUTs feed sky lighting. A cube-face atlas provides incrementally updated SH diffuse and roughness-prefiltered glossy environment lighting. Moving cloud density drives near and far sun-transmittance maps. Height and local fog have temporal reprojection and directional and point-light scattering. |
| Terrain and content | Watershed erosion supplies drainage to the terrain and shares the generated heights with road and physics sampling. The eight-layer triplanar PBR palette includes height blending, stochastic tiling and near-field parallax. A coarse horizon mesh meets the boundary of the streamed 8 m terrain grid at matching vertices. The sample uses actual scanned terrain materials, rocks, grass, saplings, deadwood, ferns and dandelions. |
| Water and materials | Water combines sky and scene reflections, absorption, depth-aware refraction, local wave packets and rain ripples. Shore foam depends on intersection depth and wave phase. Rain affects surface film, puddle normals and roughness. Principled materials provide optional coat, sheen, thickness, anisotropy and detail maps. |
| Camera and diagnostics | Photometric exposure, histogram adaptation, autofocus, progressive HDR bloom, grading and authorable post-process volumes run through public rendering settings. The editor and sample expose scene and temporal diagnostic views. |

The forward renderer records sky caches, shadow work, opaque depth and HDR snapshots, lighting, sky, water and transparencies, then resolves temporal and camera effects. Interfaces are in `Render3DSettings.hpp`, `Renderer3D.hpp`, `TerrainHeightfield.hpp`, `Decal.hpp` and `WaterSurface.hpp`. `SceneJson.cpp` stores authored rendering settings and optional material fields. The optional fields preserve previous scene defaults. See [asset manifest](../ASSET_MANIFEST.json) and [third-party notices](../THIRD_PARTY_NOTICES.md) for source resolutions, paths, licenses and hashes.

## Inspect the sample

Launch Frontier County from your IDE or application launcher. The in-game Rendering Debug panel opens with the sample and can be reopened with Tab. The view selector exposes framebuffer diagnostics. Toggle effects individually and use the three toolbar buttons to compare combinations. The historical capture results above were collected before the CLI was removed.

## Measured checks

- The Vulkan test's 18% diffuse reference yielded 5,872 cd/m² against an expected 5,819 cd/m², a 0.9% difference.
- The two-bone GPU skinning fixture matched the CPU result in every output color channel. The CPU deformation fixture changed 1,121 moving-region pixels and zero stationary-region pixels after applying the shutter pass. The FFT motion fixture changed 7,080 pixels relative to a paused surface with identical final geometry.
- In the environment fixture, a vertical test surface illuminated only by dynamic sky lighting produced a blue channel value of 93. The value fell to zero when the sun was disabled.
- The shadow projection fixture gave approximate texels of 2.54, 5.09, 9.68 and 27.99 cm for the four High splits at the noon viewpoint.

## Remaining coverage

The noon capture still shows dark foreground needles, a patterned lake surface and a visible water/ground boundary. Local reflection-probe capture and blending, a separate water reflection history, temporal cloud reprojection, and virtual texture residency are open. The existing water diagnostics exercise the composite and do not measure an independent reflection denoiser. The terrain uses seeded watershed erosion and a stitched coarse mesh, with hardware terrain tessellation still open. Direct3D 12 and Metal require native platform execution to verify GPU behavior.
