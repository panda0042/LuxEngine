# Renderer and sample limits

| Area | Demonstrated | Remaining technical work |
| --- | --- | --- |
| Terrain | Seeded 16,384 m square, continuous surface on 256 m tiles, streamed geometry, draped asphalt and two blended PBR layers | Additional layers, erosion data, terrain LOD and clipmaps, decals, virtual textures and asynchronous resource streaming. |
| Vegetation | 3D spruce meshes, 3D grass and sedge batches, and distant spruce cards | GPU instancing and culling, smoother LOD transitions, and wind deformation. |
| Lighting and image | PBR, IBL, ACES, noon atmosphere panorama, dynamic direct light and froxel fog, optional hardware RT and camera/depth-reprojected TAA | Time-varying atmosphere, volumetric clouds, motion vectors for animated actors and water, temporal upscaling, contact lighting and more scene assets. |
| Water | Gerstner waves, Fresnel and HDRI reflection, scene-color refraction and depth absorption | Shoreline foam detail, underwater presentation, wake/flow data and richer ripples. Engine GPU FFT remains demonstrated by Lakeside Ocean. |
| Assets and bodies | Three skinned humanoids, nine textured static models and a Jolt pickup | GPU skinning at crowd scale, retargeting, character shadow/mesh LODs, physics heightfield and real vehicle constraints. |
| Profiling | Fixed-step camera and measurement modes, replayable seed, CSV/JSON telemetry | Larger coverage across device classes, true residency on backends without memory budget support, and 4K/long-run measurements. |

Presentation can still be improved through richer source material and engine work. Current examples are a renderer sample with controls for traversal and inspection.
