# 삼각형 BVH와 소프트웨어 광선 추적

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

같은 스냅샷의 읽기 전용 질의는 여러 스레드에서 실행할 수 있다. 메시가 움직이면 호스트에서 새 월드 좌표로 Build를 다시 호출한다. Build는 동시 질의를 마친 뒤 호출한다. 이 코드는 The Forge GPU 가속 구조나 실시간 조명 패스에 아직 연결되지 않았다.

Engine_triangle_ray_scene 검사는 10¹² 월드 좌표의 BVH 교차, 화면 밖 삼각형의 그림자, 두 번 반사된 발광, 비스듬한 입사의 굴절을 검증한다.
