# Ray tracing and fog sample

The scene includes a mirror, a shallow pool with submerged objects, tall occluders and height fog. The sample uses `Renderer3D::Context`, `Render3DSettings` and `GpuDevice` from the public engine SDK.

```sh
cmake --build out --target SampleRayTracing
out/bin/SampleRayTracing
```

- **R** toggles ray-traced shadows, reflections and water refraction.
- **G** toggles ray-traced indirect diffuse lighting.
- **D** toggles ReSTIR direct lighting for point lights.
- **I** toggles ReSTIR indirect lighting.
- **C** toggles Radiance Cascades.
- **V** toggles virtual directional shadow pages with dense fallback when needed.
- **F** toggles froxel fog.
- **T** toggles ray-traced sun visibility in the fog.
- **Escape** closes the sample.

Hardware ray queries are enabled on the Vulkan backend when the adapter supports them. The renderer uses raster lighting and shadow-map fog on other adapters. The sample reports the selected adapter and ray-query capability at startup.
