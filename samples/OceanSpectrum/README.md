# Spectral ocean sample

The sample uses a periodic, wind-driven ocean with crest foam and four Jolt bodies that float on the waves. Escape closes the window. The sky comes from the renderer's procedural environment.

```sh
cmake --build out --target SampleOceanSpectrum
out/bin/SampleOceanSpectrum
```

`OceanSpectrum` runs a separable inverse FFT on the CPU to produce height, horizontal displacement and surface velocity for one repeating patch. The model takes its amplitude and direction from a seeded wind spectrum. The mesh shares one vertex and index buffer across the visible tiles. Foam follows compression of the horizontal displacement, measured by its Jacobian. The Forge water pass uses the resulting normals and crest foam with scene-color refraction and environment reflections.

Each buoy samples the displaced surface to compute lift from submerged volume and drag relative to the moving water. The sample applies each impulse to the Jolt body before stepping physics. It uses a spherical volume approximation around the rigid body's center and a box collider for contacts. The waves drive the bodies in one direction.

```sh
ctest --test-dir out -R '^Engine_ocean_spectrum$' --output-on-failure
out/bin/SampleOceanSpectrum --smoke-test
```

## On-device FFT implementation design

Use `OceanSpectrum` as the numerical reference. Allocate the spectrum and transform buffers per ocean. Upload the seeded coefficients, angular frequencies, signed wave directions, twiddles and bit-reversal indices at initialization. Advance phase on the device using elapsed time relative to a rebased epoch. The spectrum uses an unnormalized inverse sum with the frequency-domain sample area already included in each seed.

Evolve the coefficients and run the transform in the following order.

1. Pack height and X displacement, Z displacement and Y velocity, and X and Z velocity as three complex fields. Zero the lateral displacement multiplier on its own Nyquist axis. Keep the IFFT sign convention positive.
2. Run one compute group per row. Load the row in bit-reversed order, execute the butterfly stages in group-shared memory and write to the intermediate buffer. Run a second dispatch for columns using the same stages. A transposed intermediate layout keeps each dispatch's input reads contiguous.
3. Generate normals and Jacobian foam with wrapped central differences. Store the final displacement and normal/foam fields in GPU-readable buffers. Keep one output per frame slot until its graphics fence signals.
4. Render the periodic grid with vertex shader reads from those buffers. Use the same displaced position for all passes that draw the surface. Use conservative displacement bounds for CPU frustum culling. Insert Forge barriers for intermediate UAV dependencies and for the final compute-to-vertex-shader transition.

The render loop keeps the fields on the GPU through vertex shading. Buoyancy can evaluate the same coefficients in a lower-resolution CPU grid at fixed ticks, preserving frequencies shared with the GPU grid. An alternative is to compute the selected body probes on the GPU and consume a fence-complete result from an earlier frame. This provides the exact GPU field at the sampled tick and introduces a delay. Cache probe data and match it by tick and body identity.

For parity tests, use a fixed seed and epoch. Compare each field at the same resolution and tick, starting with an 8×8 direct inverse DFT on a known input. Cover still water, a single conjugate wave pair, Nyquist bins, periodic seams, vertical and lateral velocity and the Jacobian. Test small differences with floating-point tolerances because the CPU reference uses double precision. In GPU tests, read back only after the test fence signals. Run D3D12, Metal and Vulkan shader compilation, and validate barriers and frames-in-flight before measuring dispatch and graphics timing.
