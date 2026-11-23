# Spectral ocean sample

The sample draws a periodic wind-driven ocean with Jacobian crest foam and four Jolt bodies that float on the waves. Escape closes the window.

```sh
cmake --build out --target SampleOceanSpectrum
out/bin/SampleOceanSpectrum
```

Initialize an `OceanSpectrum` with a power-of-two resolution from 8 to 256 and upload it with `Mesh3D::UploadOcean`. During an open frame, call `Mesh3D::UpdateOcean` with the simulation time before draws that use this mesh. Use `WaterSurface::MakeMaterial` for scene-color refraction, environment reflections and foam. The mesh spans one patch centered on its origin and can be instanced at patch-length intervals.

The Forge updates the seeded wave phases and runs the two axes of an unnormalized inverse FFT in compute shaders. A third dispatch writes positions, normals and compression-based foam directly to the vertex buffer. The static index buffer and GPU vertex output are shared by every visible tile.

For Jolt, update the CPU spectrum with the same time passed to the mesh. Sample displacement and velocity with `SampleAtSurface`, adding half the patch length to each horizontal coordinate relative to the mesh origin. Pass the sample to `OceanBuoyancyImpulse` and apply the returned impulse through `Scene::AddImpulse3D` before stepping physics. The CPU reference uses double-precision math and requires no GPU readback.

```sh
cmake --build out --target LuxOceanSpectrumTests LuxForgeBackendTests SampleOceanSpectrum
ctest --test-dir out --output-on-failure -R '^(Engine_ocean_spectrum|Forge_ocean_fft|SampleOceanSpectrum_smoke)$'
```

The CPU test checks the spectrum and buoyancy math. The Forge test compares two animated GPU frames against CPU meshes that use the same water material, and the sample smoke test exercises the Jolt bodies and GPU dispatches together.
