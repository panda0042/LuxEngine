# Performance measurement

Automated runs use a fixed seed, noon start, fixed timestep, and scenario start and camera. The benchmark disables VSync, uses a non-resizable window and discards warmup frames. A framebuffer dimension change during measurement aborts the run so results never combine multiple resolutions. CSV rows record the interval between frame presentations, CPU submit-to-present time, game plus physics time, backend waits, draw and dispatch counts, and chunk queue counts. F3 shows a subset in play.

JSON average FPS is 1,000 divided by mean present interval. The 1% and 0.1% lows use the mean of the slowest ceiling of the corresponding sample fraction. A stutter is a frame interval of at least 33.33 ms and at least twice the run mean. Short runs offer limited tail resolution. GPU time uses fresh backend timing IDs when the backend advertises stamps.

Linux RSS is read from the active process. Driver-reported device-local usage and budget require backend memory-budget support. Tracked GPU bytes are engine buffer and texture payload, which can include pending releases. Unavailable values appear as JSON null and empty CSV fields. Tree counts cover loaded chunk meshes before visibility culling. Physics counts include static body descriptors.

The 16,384 m world extent is independent of the active window. High requests 5 × 5 chunks. Extreme requests 7 × 7 chunks. The --vegetation option increases vegetation candidates subject to moisture, elevation, settlement and water filters. Draft availability can differ between runs because the worker is asynchronous. Settled scenarios should allow sufficient warmup.

See [actual results](BENCHMARK_RESULTS.md) for measured scope and environment.
