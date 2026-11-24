# Frontier County

A LuxEngine outdoor rendering sample. Silverlake, the forest, a textured roadside streetscape, and a driveable pickup provide several views of the same streamed 16,384 × 16,384 m terrain.

## Run

    cmake -S . -B out
    cmake --build out --target SampleFrontierCounty -j 6
    ./out/bin/SampleFrontierCounty --scenario water

| Key | Action |
| --- | --- |
| WASD, mouse | Walk or drive, look |
| Shift, Space, C | Jog or brake, jump, crouch |
| E, V | Enter or leave pickup, change camera |
| T, G, H | Cycle weather, toggle hardware ray tracing if supported, toggle TAA |
| F1, F3, P, Esc | Controls, diagnostics, pause, exit |

The default sky starts at noon. Twenty-four in-world hours take 3,600 real seconds. Use --day-seconds 180 for a shorter cycle. Quality presets low, medium, high, ultra, cinematic and extreme control chunk radius, vegetation population, shadow-map resolution, fog distance and virtual shadows. Use --rt for hardware ray effects. High and above enable TAA. Use --no-taa to disable temporal filtering. Use --no-edge-aa to disable the spatial filter on the standard tone path. P pauses and releases the cursor.

## Capture and benchmark

    ./out/bin/SampleFrontierCounty --scenario forest --capture /tmp/frontier-forest.ppm
    ./out/bin/SampleFrontierCounty --benchmark --scenario water --resolution 1920x1080 \
      --warmup 30 --frames 60 --output /tmp/frontier-water

Use town, drive, forest, water or weather as the scenario. Capture and benchmarking use fixed camera and simulation conditions. The benchmark writes actual framebuffer size and timing in JSON and individual frame measurements in CSV.

[Engine capability audit](Documentation/ENGINE_CAPABILITIES.md) · [API use](Documentation/ENGINE_API_USAGE.md) · [Renderer limits](Documentation/ENGINE_MISSING_FEATURES.md) · [Engine findings](Documentation/ENGINE_BUG_REPORT.md) · [Performance](Documentation/PERFORMANCE_REPORT.md) · [Measured results](Documentation/BENCHMARK_RESULTS.md) · [Asset manifest](ASSET_MANIFEST.json) · [Third-party notices](THIRD_PARTY_NOTICES.md)
