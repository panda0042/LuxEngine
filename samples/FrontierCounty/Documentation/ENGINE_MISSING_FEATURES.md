# Renderer and sample limits

| Area | Demonstrated | Remaining technical work |
| --- | --- | --- |
| Terrain | Seeded 16,384 m square, continuous surface on 256 m tiles, streamed geometry, draped asphalt, surface-clipped lane markings and two blended PBR layers | Additional layers, erosion data, terrain LOD and clipmaps, virtual textures and asynchronous resource streaming. |
| Vegetation | 3D spruce meshes, 3D grass and sedge batches, and distant spruce cards with weighted wind bending | GPU instancing and culling, smoother LOD transitions and player interaction. |
| Lighting and image | PBR, IBL, dynamic sky and clouds, froxel fog, soft shadows, GTAO, glossy reflections, TAA and HDR camera effects | Cloud shadows, dynamic environment probes, per-skinned-vertex and water velocities, temporal upscaling and more scene assets. |
| Water | Gerstner waves, Fresnel and HDRI reflection, scene-color refraction and depth absorption | Shoreline foam detail, underwater presentation, wake/flow data and richer ripples. Engine GPU FFT remains demonstrated by Lakeside Ocean. |
| Assets and bodies | Three skinned humanoids, nine textured static models and a Jolt pickup | GPU skinning at crowd scale, retargeting, character shadow/mesh LODs, physics heightfield and real vehicle constraints. |
| Profiling | Fixed-step camera and measurement modes, replayable seed, CSV/JSON telemetry | Larger coverage across device classes, true residency on backends without memory budget support, and 4K/long-run measurements. |

Presentation can still be improved through richer source material and engine work. Current examples are a renderer sample with controls for traversal and inspection.
