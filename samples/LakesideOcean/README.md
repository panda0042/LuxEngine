# Lakeside at Noon sample

This scene uses a low, moving camera over a GPU FFT lake, with a sloping shore, foreground rocks, conifers, and Jolt buoys. A normal map adds capillary ripples over the spectral waves. The water pass uses Fresnel reflectance, depth-based absorption, refraction, and shore and crest foam.

```sh
cmake --build out --target SampleLakesideOcean
out/bin/SampleLakesideOcean
```

- **Space** pauses the waves and camera motion.
- **F** toggles height fog.
- **R** toggles hardware reflection and refraction for nearby geometry, along with hardware shadows.
- **C** toggles Radiance Cascades when hardware ray queries are available.
- **Escape** closes the sample.

Use a fixed simulation tick and camera to compare rendering changes. The sample waits for the sky and water reflection to load, saves a PPM image, and reports the number of virtual shadow pages and ray-traced passes.

```sh
out/bin/SampleLakesideOcean --capture lake.ppm --tick 120 --view default
out/bin/SampleLakesideOcean --capture shore.ppm --tick 120 --view shore
out/bin/SampleLakesideOcean --capture waterline.ppm --tick 120 --view waterline
```

The default capture tick is 120, which is two seconds of simulation. The sample accepts ticks from 0 through 600.

Run `SampleLakesideOcean --benchmark` on a backend with GPU timestamp support to compare shadow changes. After the environment loads, it warms up for 60 frames and reports GPU frame and shadow-pass p50/p95 over 240 completed frames. The benchmark advances the lake by one simulation tick per rendered frame with VSync disabled. The last frame reports resident and requested virtual pages. The default scene requests all 16 pages with fog enabled and uses the full-resolution shadow map.

The lake samples a 256-pixel-per-face water-only reflection cube from `Render3DSettings::waterReflection`. Its 2,048 × 1,024 Radiance panorama was area-filtered in linear light from the official 4K EXR with `tools/PrepareHdrEnvironment.py`. The fourfold reflection intensity compensates for the −2 EV encoding adjustment. The asset manifest records the source and digest. [Radkow Lake by Grzegorz Wronkowski](https://polyhaven.com/a/radkow_lake)

The shoreline uses a 4K albedo map and 2K tangent-space normal and linear roughness maps from Poly Haven. UVs repeat the material at its documented 1.8 m scale. [Rocky Gravel by Dimitrios Savva and Dario Barresi](https://polyhaven.com/a/rocky_gravel)

The sky and diffuse lighting use a separate panorama baked with `BakeAtmospherePanorama`. Its sun is 38.2° above the horizon and aligned with the reflected photograph. The nearly neutral directional light and light blue height haze model a clear noon rather than sunrise colors.
