# Jolt 물리

엔진의 물리 백엔드를 Jolt Physics 5.6.0으로 통일했다. Box2D 의존성·씬 어댑터와 자체 적분/바닥 충돌 구현을 제거했다. 공개 `PhysicsWorld`는 프레임워크 앱에서도 사용할 수 있고, 씬은 같은 API를 사용하는 별도의 2D·3D 월드를 소유한다.

## 설치와 구성

프로젝트 CMake는 다운로드하지 않는다. 공식 Jolt의 `Build/`를 별도 빌드·설치하고 `Jolt_DIR` 또는 `CMAKE_PREFIX_PATH`, `LUX_JOLT_ROOT`로 설치 위치를 지정한다. 사용자 로컬 `~/.local`도 탐색한다. 준비한 기준은 공식 리비전 `e77f175595e64cb44218cc9d9d56fc365ad0e36a`이다.

```sh
cmake -S JoltPhysics/Build -B jolt-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_DEBUG_POSTFIX=d \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DTARGET_UNIT_TESTS=OFF -DTARGET_HELLO_WORLD=OFF -DTARGET_SAMPLES=OFF \
  -DTARGET_VIEWER=OFF -DTARGET_PERFORMANCE_TEST=OFF \
  -DDOUBLE_PRECISION=ON \
  -DJPH_USE_DX12=OFF -DJPH_USE_VK=OFF -DJPH_USE_MTL=OFF -DJPH_USE_CPU_COMPUTE=OFF \
  -DINTERPROCEDURAL_OPTIMIZATION=OFF -DOVERRIDE_CXX_FLAGS=OFF \
  -DCPP_RTTI_ENABLED=ON \
  -DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=OFF -DPROFILER_IN_DEBUG_AND_RELEASE=OFF
cmake --build jolt-debug --target install --parallel
```

Release도 별도 빌드 디렉터리에서 같은 prefix에 설치한다. Debug/Release 아카이브를 섞지 않는다. Windows는 `USE_STATIC_MSVC_RUNTIME_LIBRARY=OFF`로 엔진과 같은 동적 CRT를 사용한다. `tools/InstallDeps.ps1`과 `tools/ci/SetupUnixDependencies.sh`에 준비 절차를 반영했다. Jolt GPU 컴퓨트는 사용하지 않아 DXC가 필요하지 않다.

Unix CI는 Jolt의 `CPP_RTTI_ENABLED=ON`으로 빌드한다. UBSan의 가상 함수 검사는 Jolt 클래스의 C++ 타입 정보를 참조하므로, RTTI가 없는 기본 Jolt 아카이브와 결합하면 링크에 실패한다.

엔진의 Debug는 Jolt Debug, Release는 Jolt Release를 사용하며 두 구성을 서로 대체하지 않는다. RelWithDebInfo·MinSizeRel은 Release 계열로 매핑한다. 이 제한은 `Jolt::Jolt`에만 적용하고 Qt 도구·SDL·X11 등의 일반 가져온 타깃은 설치된 구성과 구성 없는 위치를 사용할 수 있다. Jolt 구성 매핑에 Debug와 Release를 동시에 넣으면 가져온 타깃의 조건부 `NDEBUG` 정의가 잘못 적용될 수 있어, Debug 엔진과 Jolt의 검증 코드·내부 구조가 달라진다. 필요한 구성의 라이브러리를 먼저 설치해야 한다.

## 씬과 프레임워크

`<LuxEngine/Physics/PhysicsWorld.hpp>`는 Jolt 타입을 노출하지 않는다. 바디 설명을 동기화한 뒤 고정 시간으로 `Step`하고 `GetState`로 결과를 읽는다. `Contacts`는 현재 접촉, `BeganContacts`는 접촉 시작 이벤트를 제공한다. 메시·렌더러·Qt에 의존하지 않는다.

- 3D 박스 바디 사이의 충돌과 고정·동적·키네마틱·트리거를 Jolt가 처리한다. `BoxCollider3DComponent`만 있는 엔티티는 정적 충돌체다.
- 2D는 Jolt의 XY 평면 자유도 제약을 사용한다. 2D 렌더 순서용 Z는 씬에 보존하며 물리적 깊이로 사용하지 않는다. Box2D와 수치적으로 같은 결과를 보장하지 않는다.
- 씬은 월드 좌표에서 시뮬레이션하고 계층의 부모부터 로컬 Transform으로 환산한다. 크기·충돌체·질량·트리거 변경과 엔티티 삭제는 네이티브 바디에 반영한다.
- 바닥은 사용자가 배치한 정적 충돌체다. Y=0에서 물체를 멈추는 숨겨진 바닥은 없다.
- `Scene::GetCollisionEvents2D/3D`로 접촉 시작 이벤트를 읽는다. 3D 이벤트에는 월드 접점과 법선이 포함된다.

`PhysicsEngine::Step2D/Step3D` 단일 바디 호환 함수는 제거했다. 기존 프레임워크 앱은 `Scene::OnUpdate` 또는 `PhysicsWorld`로 옮긴다. 기존 Rigidbody/BoxCollider 씬 필드는 유지한다. 월드 좌표는 `double`이며 Jolt의 double-precision 위치와 연결한다.

박스의 월드 축 크기는 `0.000001~10000`을 지원한다. 작은 크기는 변환 Scale로 줄여도 동일하게 시뮬레이션한다. 0 크기·비유한 값 등으로 바디를 제외하면 엔티티 ID와 지원 범위를 로그에 한 번 알리고, 같은 오류를 매 프레임 반복하지 않는다. `PhysicsWorld::GetLastError()`로 독립 월드의 동기화 오류를 읽을 수 있다.

## 게임 코드에서의 사용

`Scene::GetVelocity3D`, `SetVelocity3D`, `AddImpulse3D`, `IsGrounded3D`, `Raycast3D`는 엔진 타입만 노출한다. 속도·중심 충격량·레이 좌표는 월드 좌표이며, C++ Scene API에서 사용한다. 이 함수들의 C# 바인딩은 아직 제공하지 않는다.

속도 설정과 중심 충격량은 Rigidbody3D 값에 반영되고 다음 고정 스텝에서 Jolt에 전달된다. 충격량의 속도 변화는 `impulse / mass`다. 정적 바디의 이동과 운동학 바디의 동적 충격량은 거절한다. 잘못된 값·삭제된 엔티티는 실패를 반환하고, 속도 조회 실패는 출력값을 비운다. 접지는 마지막으로 동기화한 물리 결과를 조회한다.

레이캐스트는 가장 가까운 박스 충돌을 반환하고 방향을 정규화한다. 최대거리는 양수이며 100000 이하의 월드 거리다. 기본적으로 트리거를 제외하고, 무시할 엔티티와 트리거 포함 여부를 지정할 수 있다. 형상 내부에서 시작하면 거리 0·시작점·반대 방향 법선을 반환한다. 미검출이나 잘못된 입력은 false와 비어 있는 결과를 반환한다. 질의는 마지막 동기화 상태를 사용하며, 엔티티 삭제는 즉시 질의 대상에서 제거한다.

```cpp
LuxEngine::RaycastHit3D hit;
if (scene.Raycast3D(origin, forward, 5.0f, hit, playerId)) {
    const auto target = hit.entityId;
}
if (scene.IsGrounded3D(playerId))
    scene.AddImpulse3D(playerId, {0, jumpImpulse, 0});
```

## 동기화와 진단

`PhysicsWorld::GetLastError()`는 잘못된 바디의 엔티티 ID, 중복 ID, 월드 한도·할당 실패와 잘못된 시간·중력을 진단한다. 월드 한도는 16384개이며, 전체 바디 목록을 교체할 때 사라진 바디를 먼저 해제해 용량을 확보한다. 중복 ID는 해당 바디 전체를 거절하고 나머지 정상 바디는 처리한다. 정적 바디는 속도 0으로 처리한다. 정적·이동 바디의 broadphase 계층을 분리하며 키네마틱 바디도 정적 트리거와 접촉할 수 있다.

씬 API의 속도 입력은 축별 절댓값을 100000으로 제한한다. Jolt의 합성 속도 한도는 200000이며 입력으로 허용된 대각선 속도를 수용한다. 속도 조회는 물리 계산 결과를 Jolt의 한도까지 반환한다.

Scene은 `GetLastPhysicsError2D/3D()`로 최근 물리 스텝의 진단을 제공한다. 잘못된 바디를 조용히 무시하는 대신 엔티티를 식별할 수 있으며, 에디터 실행은 물리 진단이 발생하면 로그와 함께 일시정지한다. 값을 고친 뒤 정상 스텝을 진행하면 진단을 해제하고 바디를 다시 생성할 수 있다.

비균일 스케일의 부모 아래 회전한 자식 등 3D 월드 변환에 전단이 생긴 박스는 근사 충돌체를 만들지 않고 진단한다. 퇴화한 축도 같은 방식으로 거절한다. 지원되는 TRS 변환으로 고치면 다음 스텝에서 충돌체와 질의를 복원한다.

충돌체의 크기 자체는 양수여야 한다. 음수 크기를 절댓값으로 숨기지 않고 2D·3D 모두 진단하며, 미러링을 위한 Transform의 음수 스케일은 계속 지원한다.

접촉 시작은 네이티브 바디 핸들 대신 엔티티 쌍과 트리거 종류로 구분한다. 겹쳐 있는 상태에서 질량·크기 변경으로 Jolt 바디를 다시 생성해도 트리거가 중복 시작되지 않는다. 실제로 벗어났다가 다시 들어오면 새 시작 이벤트가 발생한다. 동적 비트리거 바디는 기존 LinearCast CCD를 유지한다.

`Engine_physics_integration`은 질의·필터·실패 출력, 속도·충격량·접지, 2D/3D CCD와 키네마틱 트리거, 런타임 질량·형상 변경, 중복 ID·오류 복구, 최대 용량 교체·초과 처리를 검사한다.

현재 충돌체 어댑터는 박스를 지원한다. 구·캡슐·메시 충돌체·캐릭터 컨트롤러·조인트, 전체 충돌 레이어·마스크와 휴면 최적화는 별도 확장 범위다. Jolt가 제공하는 모든 기능을 에디터에서 이미 노출했다는 뜻은 아니다.

## 64비트 위치 설정

Jolt는 `-DDOUBLE_PRECISION=ON`으로 빌드해야 한다. 새 설정은 `tools/ci/SetupUnixDependencies.sh`와 `tools/InstallDeps.ps1`에 포함했다. 기존 single-precision 설치와 ABI가 다르므로 재빌드하고 `Jolt_DIR` 또는 `LUX_JOLT_ROOT`를 지정한다. CMake는 라이브러리를 다운로드하지 않는다. 자세한 좌표·전송 경계는 [WorldCoordinates.md](WorldCoordinates.md)를 참조한다.
