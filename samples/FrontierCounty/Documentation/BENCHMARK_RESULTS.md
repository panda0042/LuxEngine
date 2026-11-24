# Measured results

Linux/X11 and Vulkan, Fedora 43, Ryzen 5 2600X, GeForce RTX 2060, Debug build. The Silverlake camera ran at 1920 × 1080, High, seed 4805 and TAA enabled and VSync off. The recorder discarded 30 warmup frames and recorded 60 presented frames. The run used Vulkan synchronization validation.

| Metric | Value |
| --- | ---: |
| Average FPS | 31.64 |
| Mean present interval | 31.61 ms |
| Mean timed GPU work | 3.51 ms |
| Mean game and physics tick | 0.24 ms |
| Draws and compute dispatches per frame | 153 / 1 |
| Loaded and pending chunks | 25 / 0 |
| Trees in active chunks | 3,618 |
| Peak process RSS | 764 MiB |
| Peak tracked GPU payload | 356 MiB |

The backend did not expose device-local residency for this run. GPU durations come from backend timestamp queries and cover the timed GPU work. Present latency includes other work and pacing. The run is a short graphics-path measurement. Ultra/Extreme, 1440p/4K, hardware RT and long-duration stress targets remain unmeasured.

[Machine-readable summary](../Benchmarks/linux-rtx2060-1080p-water.json) · [Frame samples](../Benchmarks/linux-rtx2060-1080p-water.csv)

    ./out/bin/SampleFrontierCounty --benchmark --scenario water --preset high \
      --resolution 1920x1080 --warmup 30 --frames 60 --output /tmp/frontier-water
