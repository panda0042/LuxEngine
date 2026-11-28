# Renderer and sample limits

| Area | Demonstrated | Remaining technical work |
| --- | --- | --- |
| Terrain | Seeded 16,384 m square, continuous surface on 256 m tiles, streamed geometry, draped asphalt, surface-clipped lane markings and an eight-layer triplanar PBR palette, watershed erosion and a stitched coarse horizon | Tessellation, displacement silhouettes, clipmaps and virtual texture residency. |
| Vegetation | 3D spruce meshes, 3D grass and sedge batches, and distant spruce cards with weighted wind bending | GPU instancing and culling, further LOD transitions, higher-density vegetation and physics-aware interaction. |
| Lighting and image | Stabilized four-split sun shadows, cached virtual pages, contact shadows, cloud-shadow transmittance, bent-normal GTAO, hierarchical SSR, dynamic sky lighting, local and height fog, TAA and HDR camera effects. Supported Vulkan devices can run ReSTIR DI/GI, radiance cascades and an opaque GI/reflection denoiser. | Local reflection-probe capture, screen-independent reflections on raster-only hardware, temporal upscaling and independent water reflection histories. The virtual far cascade has 16 addressable pages and switches to dense coverage above 12 resident pages. |
| Water | Gerstner waves, Fresnel and HDRI reflection, scene-color refraction and depth absorption | Dedicated water reflection denoising, continuous hull wakes and more detailed underwater composition. Engine GPU FFT remains demonstrated by Lakeside Ocean. |
| Assets and bodies | Three skinned humanoids, optional compute skinning, scanned environmental assets and a Jolt pickup | GPU skinning at crowd scale, retargeting, character shadow/mesh LODs, physics heightfield and real vehicle constraints. |
| Profiling | Fixed-step camera and measurement modes, replayable seed, CSV/JSON telemetry | Larger coverage across device classes, true residency on backends without memory budget support, and 4K/long-run measurements. |

Presentation can still be improved through richer source material and engine work. Current examples are a renderer sample with controls for traversal and inspection.
