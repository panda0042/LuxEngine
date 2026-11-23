# 워터 표면

`WaterSurface`는 XZ 평면의 격자에 Gerstner 파를 적용한다. 수평·수직 변위를 계산하고 접선의 외적에서 법선을 구한다. 렌더링과 부력 계산은 동일한 파형을 사용한다. 위치와 회전은 그릴 때 넘긴 모델 변환으로 설정하며, 수위 조회 좌표와 결과는 수면의 로컬 공간을 기준으로 한다.

```cpp
#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
#include <array>
#include <stdexcept>

LuxEngine::WaterSurface water;
const std::array<LuxEngine::WaterWave, 2> waves{{
    {{1, 1}, .035, 2, 1.2},
    {{1, -2}, .012, .8, .9},
}};
if (!water.Configure({{12, 20}, 40, 64, .2}, waves))
    throw std::runtime_error("Invalid water settings");

// 프레임을 시작한 뒤 3D 씬에 수면을 제출하기 전에 호출한다.
if (!water.SetTime(elapsedSeconds) || !water.SyncGpu(device))
    throw std::runtime_error("Could not update the water surface");

renderer.BeginScene(camera, renderSettings, exposure);
renderer.DrawMesh(water.GetMesh(), LuxEngine::Mat4::Identity(),
                  LuxEngine::WaterSurface::MakeMaterial());
renderer.EndScene();
```

각 파형의 방향, 진폭, 파장, 진행 속도, 위상, 가파른 정도를 설정한다. 각 격자 행의 중심 X 좌표를 넘기면 구부러진 수로도 만들 수 있다. 격자는 각 축 최대 256구간이며 파형은 최대 네 개다. `Sample(x, z)`는 변위가 적용된 위치를 역으로 풀어 현재 시각의 높이·법선·흐름 속도와 파봉 거품을 반환한다. 수면 범위 밖이면 빈 값을 반환한다. 물체가 잠긴 깊이와 수면 속도를 사용해 부력·감쇠를 계산할 수 있다. 허용 가능한 총 가파름을 제한해 파형이 접히거나 한 지점에 둘 이상의 수위가 생기는 것을 방지한다.

기본 재질은 금속성 0, 낮은 거칠기, 물의 굴절률에 가까운 1.333을 사용한다. 수평 변위의 야코비안 행렬식이 줄어드는 구간에서 파봉 거품을 만들고, 거품 강도를 정점 알파에 저장해 워터 패스로 전달한다. 정점 알파 1은 거품이 없는 표면을 뜻한다.

워터 패스는 불투명 장면의 HDR 색과 깊이를 읽고 화면 공간 굴절과 수심을 계산한다. 수심에 따른 채널별 흡수에는 Beer–Lambert 법칙을 적용하고 프레넬 반사로 HDRI와 굴절 색상을 합성한다. 얕은 곳에는 해안 거품을, 등진 조명에는 얕은 산란 효과를 추가한다. 화면 밖 물체의 굴절이나 기하 반사에는 별도 렌더링이 필요하다.

Forge는 물이 보이는 프레임에서 불투명 씬의 색과 깊이를 수면 합성의 입력으로 보존한다. 굴절된 화면 픽셀이 수면 앞의 물체를 가리키면 원래 픽셀을 사용한다. 프레넬 반사는 환경 큐브맵에서 샘플링하며, 깊이 차이로 구한 물 통과 거리에 Beer–Lambert 감쇠를 적용한다. 얕은 영역의 해안 거품과 파봉의 전방 산란 근사도 함께 계산한다. 수면 합성 후 일반 반투명 재질을 그린다.

공간 위상의 사인과 코사인은 격자 구성 시 계산한다. 시간 갱신은 각 파형의 시간 위상을 한 번 계산한 뒤 정점에서 재사용한다. 인덱스 버퍼는 파형이 움직여도 유지한다. 같은 시간이나 움직이지 않는 파형에서는 정점을 다시 올리지 않는다. 현재 정점 갱신은 Mesh3D의 GPU 업로드 경로를 사용하므로, 해상도를 높일 때는 CPU 정점 계산과 GPU 동기화 비용을 함께 측정한다.

Rainy Creek 샘플은 `SampleRainyCreek --preset day`로 시작할 수 있다. H로 물을 끄고 켜면서 HDRI 하늘과 물 반사를 비교할 수 있다. `RainyCreek_geometry`는 인덱스, 법선, 시간 주기, 메시와 수위 조회의 일치를 검사한다.

바다 예제는 [OceanSpectrum](../samples/OceanSpectrum/README.md)에서 실행할 수 있다. 현재 CPU 역 FFT로 계산한 주기 수면과 Jolt 부표를 그린다.
