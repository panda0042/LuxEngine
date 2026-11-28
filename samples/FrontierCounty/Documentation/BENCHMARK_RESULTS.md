# Measured results

## Windows / Direct3D 12

Windows Release build, GeForce RTX 5070 (driver 621.24), The Forge / Direct3D 12. All runs used 1600 × 900, High, seed 4805, TAA enabled and VSync off. Each build ran three times per scenario with 180 warmup frames and 240 measured frames. The tuned build completes scene startup before warmup. The table reports the median of the three runs for each metric.

| Scenario | Average FPS, baseline / tuned | 1% low FPS, baseline / tuned | CPU submit-to-present, baseline / tuned | Stutters, baseline / tuned |
| --- | ---: | ---: | ---: | ---: |
| Town | 61.8 / 149.4 | 17.2 / 65.7 | 16.09 / 6.64 ms | 20 / 0 |
| Water | 134.9 / 192.9 | 57.0 / 57.8 | 7.32 / 4.91 ms | 0 / 0 |
| Drive | 384.2 / 399.5 | 31.7 / 30.7 | 2.52 / 2.45 ms | 1 / 1 |

In town, the median GPU time was 0.63 ms before tuning and 0.65 ms after tuning. CPU profiling attributed most of the town frame time to repeated FBX pose evaluation. The tuned build reuses sampled poses at the existing animation rate. Water and drive each had one tuned run with a present interval above 300 ms, largely outside the CPU submit-to-present segment. Tuned drive results varied between 238.5 and 406.4 FPS.

Separate tuned runs measured 1,800 frames per scenario at the same settings. Town averaged 149.4 FPS with zero stutters and a peak sampled working set of 275.9 MiB. Drive averaged 409.0 FPS with four stutters, a peak sampled working set of 178.5 MiB, and 15 chunks evicted during the run.

### Drive frame-time diagnosis

The Forge telemetry was exercised at the same Windows settings in three 1,800-frame drives and one 6,000-frame drive. The 11,400 measured frames contained one stutter above 33.33 ms. The timings below show the worst frame and a recurring peak near the start of the route. Resource upload time overlaps the two streaming columns, and upload wait is part of resource upload time. GPU time is matched by timestamp frame ID.

| Sample | Present interval | Terrain streaming | Ground-cover streaming | Resource upload | Upload wait | GPU time |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Run 1, frame 926 | 41.01 ms | 16.91 ms | 21.39 ms | 21.58 ms | 13.84 ms | 0.90 ms |
| Run 3, frame 48 | 25.51 ms | 0.01 ms | 23.89 ms | 9.47 ms | 6.43 ms | 0.73 ms |

Frame 926 loaded terrain and built nearby foliage on the render thread. Frame 48 spent almost the entire interval building and uploading ground-cover meshes, with 23.61 MiB of upload payload. The 6,000-frame run also contained an isolated 27.53 ms frame with zero resource uploads and less than 0.02 ms in the two streaming stages. Its cause remains unclassified.

### Terrain and ground-cover upload scheduling

The Windows A/B run used the same High settings, seed, route, 180 warmup frames and 1,800 measured frames per trial. Each build ran three times, with the execution order reversed for the second pair. The table reports the median of each run metric, except stutters, which are summed across the three runs.

| Metric | Original scheduling | Staged uploads |
| --- | ---: | ---: |
| Average FPS | 451.5 | 490.3 |
| 1% low FPS | 45.0 | 144.0 |
| Maximum frame interval | 32.41 ms | 11.70 ms |
| Peak terrain streaming time | 17.52 ms | 3.26 ms |
| Peak ground-cover streaming time | 25.33 ms | 10.08 ms |
| Peak upload payload per frame | 23.61 MiB | 8.80 MiB |
| Stutters across 5,400 frames | 1 | 0 |
| Peak tracked GPU payload | 371.9 MiB | 391.3 MiB |
| Frames before benchmark warmup | 12 | 174 |

The staged build recorded zero frames with both a terrain mesh upload and a ground-cover mesh upload. It waits for nearby cover to settle before benchmark warmup. The retained cover ring increases the tracked GPU payload during the route. The same geometry and vegetation density are used when tiles have finished loading.

An additional 6,000-frame pair used the same settings. The maximum frame interval fell from 29.40 ms to 14.19 ms, average FPS rose from 437.3 to 455.1, and 1% low FPS rose from 45.3 to 115.2. Both runs recorded zero stutters at the benchmark threshold.

### Interactive vehicle interpolation

An instrumented interactive drive applied the same straight-line throttle input on the same Windows route. Each run recorded 720 rendered frames after the truck reached motion. A held interval is a pair of consecutive rendered frames whose position differs by less than one micrometer.

| Settings | Held truck intervals, before / after | Held camera target intervals, before / after |
| --- | ---: | ---: |
| High, default presentation settings | 479 / 0 | 479 / 0 |
| High, VSync and TAA disabled | 644 / 0 | 644 / 0 |

The physical truck coordinates matched exactly at every shared tick in both pairs. The rendered truck and camera target moved by the same displacement during each straight-line trace. Additional tests verified sharp steering, reverse steering, the camera's 180-degree angle wrap, first-person mode, pause and resume, and resetting vehicle position. The throughput benchmark continues to advance one full simulation step per rendered frame.

### Chase camera through turns and reverse

The repeatable driving sequence exercised rapid steering reversals, braking and reverse steering, with three camera samples for each 60 Hz physics tick. The before and after runs simulated the same inputs. A separate reverse-steering fixture moved a narrow wall to 35 positions around the camera path and checked the actual line from the vehicle to the camera.

| Metric | Before | After |
| --- | ---: | ---: |
| Peak change in view yaw rate between samples | 38.94°/s | 6.99°/s |
| Horizontal chase distance during open reverse | 7.09–7.43 m | 7.55 m |
| Samples with the camera past the wall | 144 | 0 |

The vehicle chase camera uses a common filtered aim for its position and target. It follows the interpolated truck position and extends the camera arm gradually after an obstruction clears. The camera pose changes leave the vehicle simulation and steering forces untouched.

## Linux / Vulkan

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

These measurements were collected using an earlier build of Frontier County.
