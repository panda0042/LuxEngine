# Lakeside at Noon sample

This scene uses a 128 × 128 GPU FFT lake with a sloping shore, foreground rocks, conifers, and Jolt buoys. A normal map adds capillary ripples over the spectral waves. The water pass uses dielectric Fresnel reflectance, depth-based absorption, shore and crest foam, and hardware reflection and refraction on DXR 1.1 and Vulkan ray-query devices.

```sh
cmake --build out --target SampleLakesideOcean
out/bin/SampleLakesideOcean
```

- **Space** pauses the waves and camera motion.
- **F** toggles height fog.
- **R** toggles hardware reflection and refraction for nearby water.
- **C** toggles Radiance Cascades when hardware ray queries are available.
- **Escape** closes the sample.

Use a fixed simulation tick and camera to compare rendering changes. The sample waits for the sky and water reflection to load, saves a PPM image, and reports the number of virtual shadow pages and ray-traced passes.

```sh
out/bin/SampleLakesideOcean --capture lake.ppm --tick 120 --view default
out/bin/SampleLakesideOcean --capture shore.ppm --tick 120 --view shore
out/bin/SampleLakesideOcean --capture waterline.ppm --tick 120 --view waterline
out/bin/SampleLakesideOcean --capture raster.ppm --tick 120 --rt off
```

The default capture tick is 120, which is two seconds of simulation. The sample accepts ticks from 0 through 600.

Run `SampleLakesideOcean --benchmark --rt on` and `--benchmark --rt off` on a backend with GPU timestamp support. The sample warms up for 120 frames and reports observed frame rate, GPU time, and frame submission intervals over 240 completed frames. With a swimmer asset, `--benchmark-paced` uses a native VSync interval on supported backends and display-aware software pacing elsewhere.

## 해랑이 수영

`--swimmer PATH.fbx`로 모델을 지정하거나 실행 파일 옆 `assets/HaerangSwim/swimmer.fbx`에 모델과 텍스처를 두면 지형이 없는 열린 바다와 캐릭터 카메라가 활성화됩니다. 카메라 주변에 FFT 수면 패치를 배치하며, 해랑이는 물 위에서 평영하거나 제자리에서 물을 밟습니다. 마우스 오른쪽 버튼을 누른 채 드래그하면 카메라가 회전합니다. 휠로 확대·축소하고 **Home**으로 카메라를 초기화합니다. **WASD**는 카메라 방향을 기준으로 이동합니다.

`--sequence DIRECTORY`는 60 Hz 시뮬레이션으로 연속 PPM을 캡처합니다. 기본 180장을 `--frames COUNT`로 바꾸고, `--tick`으로 첫 프레임의 시점을 정합니다. 해랑이 시점의 캡처에는 `--pitch RADIANS`로 위에서 내려다보는 각도를 지정할 수 있습니다.

**V**는 넓은 파도 보기와 해랑이 따라가기 시점을 전환합니다. 바다 프리셋은 256×256 FFT 수면에 세 방향의 너울과 높이 계수 3.60의 쇄파를 더합니다. 마루에서 튄 물보라는 중력에 따라 떨어지고, 닿은 곳에 잔거품을 남겨 수면을 따라 퍼집니다. 파봉과 낙수가 포말의 범위를 정하고, 2048×2048 수면 맵의 알파가 얇은 기포막과 물이 비치는 틈을 표현합니다.

OpenCV와 NumPy를 사용하는 Python으로 저장소 루트에서 구름 환경맵을 다시 생성할 수 있습니다.

```sh
python samples/LakesideOcean/PrepareCoastalSky.py samples/Assets/LakesideOcean/lakeside_physical_noon.hdr samples/Assets/LakesideOcean/coastal_clouds.hdr
```

Blender로 전달받은 해병대 모델을 열고 자동 실행을 끈 상태에서 아래 스크립트를 실행하면 별도 수정본과 엔진용 FBX 및 텍스처를 생성합니다.

```powershell
blender --disable-autoexec -b "원본.blend" --python-exit-code 1 --python samples/LakesideOcean/PrepareSwimmer.py -- "수정본.blend" "assets/HaerangSwim/swimmer.fbx"
```

The lake samples a 256-pixel-per-face water-only reflection cube from `Render3DSettings::waterReflection`. Its 2,048 × 1,024 Radiance panorama was area-filtered in linear light from the official 4K EXR with `tools/PrepareHdrEnvironment.py`. The fourfold reflection intensity compensates for the −2 EV encoding adjustment. The asset manifest records the source and digest. [Radkow Lake by Grzegorz Wronkowski](https://polyhaven.com/a/radkow_lake)

The shoreline uses a 4K albedo map and 2K tangent-space normal and linear roughness maps from Poly Haven. UVs repeat the material at its documented 1.8 m scale. [Rocky Gravel by Dimitrios Savva and Dario Barresi](https://polyhaven.com/a/rocky_gravel)

The sky and diffuse lighting use a separate panorama baked with `BakeAtmospherePanorama`. Its sun is 38.2° above the horizon and aligned with the reflected photograph. The nearly neutral directional light and light blue height haze model a clear noon rather than sunrise colors.
