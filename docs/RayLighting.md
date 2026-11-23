# 레이 조명과 볼류메트릭 포그

## 실시간 GPU 렌더링

Vulkan에서 레이 쿼리와 가속 구조를 지원하는 장치는 `GpuDevice::GetInfo().rayTracing`을 반환한다. `Render3DSettings::rayTracing`에서 방향광 그림자, 반사, 수면 굴절과 간접 확산광을 각각 활성화할 수 있다. 광선은 카메라 상대 좌표의 The Forge BLAS와 TLAS를 순회한다. 메시 BLAS는 정점이나 인덱스가 갱신될 때까지 재사용하며, 씬을 그릴 때 현재 인스턴스의 TLAS를 만든다. 캡처에서 화면 밖 가림체와 반사체도 확인할 수 있다.

간접 확산광은 두 개의 코사인 가중 반구 광선을 샘플링하고 레이 결과와 기존 IBL을 혼합한다. 반사·굴절의 최대 바운스와 거리는 각각 `maxBounces`, `maxDistance`로 지정한다. 이차 히트는 삼각형 법선과 재질의 기본색·금속성·거칠기를 사용한다. 텍스처와 알파 컷아웃은 기존 래스터 패스에서 처리한다. 컴퓨트로 생성한 FFT 정점은 가속 구조의 교차 대상에서 제외되며, FFT 수면에서도 다른 메시를 대상으로 반사와 굴절을 계산할 수 있다. 지원하지 않는 장치에서는 기존 래스터·환경 반사·화면 공간 수면 합성을 사용한다.

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
