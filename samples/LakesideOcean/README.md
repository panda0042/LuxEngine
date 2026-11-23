# Lakeside Sunrise sample

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

The lake samples a water-only reflection environment from `Render3DSettings::waterReflection`. Its 2,048 × 1,024 RGBE panorama was reduced from the official 24,576 × 12,288 OpenEXR by averaging 12 × 12 blocks in linear light. The fourfold reflection intensity compensates for the encoding scale and preserves the source highlight range. The source is credited and its digest is recorded in the asset manifest. [Lakeside Sunrise by Greg Zaal and Jarod Guest](https://polyhaven.com/a/lakeside_sunrise)

The sky and diffuse lighting use a separate panorama baked with `BakeAtmospherePanorama`. The observer is 1.53 m above the surface and the sun is 5.5° above the horizon. The panorama adds a solar disk with a radius of 0.27°. A matching warm directional light and forward-scattering height fog keep shore lighting consistent with the water reflection.
