# 레이 조명과 볼류메트릭 포그

## 실시간 GPU 렌더링

Direct3D 12의 DXR 1.1 및 Vulkan 레이 쿼리를 지원하는 장치는 `GpuDevice::GetInfo().rayTracing`을 반환한다. `Render3DSettings::rayTracing`에서 방향광 그림자, 반사, 수면 굴절과 간접 확산광을 각각 활성화할 수 있다. `waterReflections`는 물 재질에만 하드웨어 반사를 적용한다. 광선은 카메라 상대 좌표의 The Forge BLAS와 TLAS를 순회한다. 메시 BLAS는 정점이나 인덱스가 갱신될 때까지 재사용하며, 씬을 그릴 때 현재 인스턴스의 TLAS를 만든다. 캡처에서 화면 밖 가림체와 반사체도 확인할 수 있다.

간접 확산광은 두 개의 코사인 가중 반구 광선을 샘플링하고 레이 결과와 기존 IBL을 혼합한다. 반사·굴절의 최대 바운스와 거리는 각각 `maxBounces`, `maxDistance`로 지정한다. 이차 히트는 삼각형 법선, 정점 색의 평균, 텍스처 대표색과 재질 속성으로 셰이딩한다. 수면 굴절의 첫 히트가 불투명 깊이와 일치하면 화면에 렌더링된 색을 재사용한다. 컴퓨트로 생성한 FFT 정점은 가속 구조의 교차 대상에서 제외되며, FFT 수면에서도 다른 메시를 대상으로 반사와 굴절을 계산할 수 있다. 지원하지 않는 장치에서는 기존 래스터·환경 반사·화면 공간 수면 합성을 사용한다.

`Render3DSettings::fog`의 `density`가 양수이면 컴퓨트 셰이더가 프러스텀 셀을 따라 투과율과 단일 산란을 누적한다. 최종 HDR 이미지와 불투명 또는 수면 깊이로 포그를 합성한다. `rayTracedShadows`를 켜면 광원이 셀까지 도달하는지를 하드웨어 광선으로 검사한다. 이 옵션은 RT 재질 효과와 독립적이며 레이 쿼리를 사용할 수 없으면 씬 섀도우 맵을 사용한다.

```cpp
LuxEngine::Render3DSettings settings;
settings.rayTracing.shadows = true;
settings.rayTracing.reflections = true;
settings.rayTracing.refractions = true;
settings.rayTracing.indirectDiffuse = true;
settings.fog.density = .04f;
settings.fog.rayTracedShadows = true;
// Pass settings to Renderer3D::BeginScene or Scene::SetSettings.
```

샘플에서 효과를 켜고 끄며 비교할 수 있다. [SampleRayTracing](../samples/RayTracing/README.md)

## ReSTIR, Radiance Cascades와 가상 섀도우 맵

- `rayTracing.restirDirect`는 점광원 후보의 가중 reservoir를 생성한다. 깊이·법선·인스턴스가 맞는 주변 픽셀과 후보를 재표본화하고, 선택한 점광원으로 가시성 광선을 쏜다. 방향광은 기존 별도 경로로 계산한다.
- `rayTracing.restirIndirect`는 한 번 반사된 확산광 후보를 주변 픽셀과 공유하고 선택한 경로를 확인한다. 두 ReSTIR 모드는 해상도가 제한된 현재 프레임의 화면 공간 reservoir를 사용한다. 시간 누적은 아직 제공하지 않는다.
- `radianceCascades`는 세 단계의 3D 프로브가 서로 다른 거리 구간의 광선을 추적한다. 먼 구간부터 가시성을 고려해 병합하고, 첫 번째 단계의 방사휘도를 구면조화 계수로 저장해 불투명 재질에서 보간한다. `spacing`과 `strength`로 프로브 간격과 기여도를 정한다. 같은 픽셀에 유효한 ReSTIR GI가 있으면 그 값을 사용한다.
- `virtualShadows`는 방향광 섀도우 맵을 4×4 가상 페이지로 나눈다. 최대 9페이지는 3×3 아틀라스, 10~12페이지는 4×3 아틀라스에 배치한다. 그보다 넓게 덮는 패스는 일반 섀도우 맵에 한 번 렌더링해 같은 해상도를 유지한다. 페이지는 패스마다 다시 배치하며 PCF 탭마다 가상 좌표를 변환한다.

레이 트레이싱 포그, ReSTIR과 Radiance Cascades는 `GpuDevice::GetInfo().rayTracingCompute`가 참인 Vulkan 장치에서 실행한다. 미지원 장치에서는 기존 직접광, 환경광과 섀도우 맵을 사용한다.

## CPU 삼각형 BVH

TriangleRayScene은 월드 좌표의 삼각형과 재질을 복사해 BVH를 만든다. 렌더용 메시로부터 가장 가까운 교차점, 유한한 광선 구간의 가림 여부와 선형 방사휘도를 계산할 수 있다. 이 API는 CPU에서 실행되며 광원·반사 프로브 제작이나 광선 교차의 기준 결과를 계산할 때 사용할 수 있다.

```cpp
#include <LuxEngine/Renderer/Lighting/TriangleRayScene.hpp>
#include <array>
#include <stdexcept>

using namespace LuxEngine;
const std::array<RayTriangle, 1> floor{{{{-4, 0, -4}, {0, 0, 4}, {4, 0, -4}, 0}}};
TriangleRayScene rays;
if (!rays.Build(floor))
    throw std::runtime_error("Invalid ray geometry");
RayLighting lighting;
lighting.directionalIrradiance = {2, 1.8, 1.5};
lighting.environmentRadiance = {.10, .12, .16};
const Vec3 radiance = rays.TraceRadiance({{0, 2, 0}, {0, -1, 0}}, lighting);
```

재질은 선형 기본색, 발광, 금속성, 투과율과 굴절률을 받는다. 직접광은 방향광 하나를 대상으로 가림 광선을 검사한다. 반사는 이상적인 거울 방향으로, 굴절은 공기와 재질의 경계에서 Snell 법칙으로 계산한다. 경계마다 전반사 여부를 검사하며 최대 여덟 번의 바운스를 허용한다. 투과성 재질을 통과하는 그림자와 중첩 매질, 거칠기에 따른 반사 확산은 현재 범위 밖이다.

같은 스냅샷의 읽기 전용 질의는 여러 스레드에서 실행할 수 있다. 메시가 움직이면 호스트에서 새 월드 좌표로 Build를 다시 호출한다. Build는 동시 질의를 마친 뒤 호출한다.

Engine_triangle_ray_scene 검사는 10¹² 월드 좌표의 BVH 교차, 화면 밖 삼각형의 그림자, 두 번 반사된 발광, 비스듬한 입사의 굴절을 검증한다.
