# 64비트 월드 좌표

CPU의 `Vec2`, `Vec3`, `Mat4`, Transform, 카메라, 씬 계층, 모델 위치와 애니메이션 위치는 `double`을 사용한다. 씬 JSON·클립 에셋·편집기 속성·클립보드에서도 이 값을 float로 줄이지 않는다. 색상·텍스처·GPU 전송 형식의 자료형과 월드 좌표 자료형은 구분한다.

## 렌더링

월드 행렬과 뷰 행렬을 double로 계산한 뒤 카메라 위치를 원점으로 빼서 GPU에 전달한다. 메시 버퍼도 double 기준점을 따로 보관하고 정점의 상대 위치만 float로 전송한다. 조명과 그림자는 같은 좌표 기준을 사용한다. GPU의 float 지원 때문에 월드 Transform을 float로 되돌리지 않는다.

자유 Scene View의 기본 far clip은 최소 1,000,000이며 뷰 거리에 따라 확장한다. 기존 100,000 단위 탐색 거리 제한도 확장했다. 게임 카메라의 명시적인 near/far 값은 유지하며 새 카메라의 기본 far clip은 1,000,000이다. 화면보다 작은 먼 물체를 강제로 확대하는 기능은 아니다.

## 물리와 C#

Jolt는 반드시 `DOUBLE_PRECISION=ON`으로 빌드한다. `tools/ci/SetupUnixDependencies.sh`와 `tools/InstallDeps.ps1`에 반영했다. 프로젝트 CMake는 다운로드하지 않으며, 호환되지 않는 Jolt 설치를 발견하면 설정 단계에서 안내한다. 저장소 전용 설치는 `.deps/jolt64`에 둘 수 있다.

Jolt의 월드 위치와 접촉점은 double이다. Jolt 내부의 로컬 형상·회전·속도 계산과 GPU 정점은 각 라이브러리의 전송 형식을 사용한다. 물리·모델링·속성 편집의 월드 좌표 정책 상한은 10¹⁵ 단위다. double은 큰 범위에서도 무한한 소수 정밀도를 제공하지는 않는다.

C# `Transform.Position/Rotation/Scale`은 `Vector3d`다. 기존 `System.Numerics.Vector3`에서의 입력 변환은 제공하며, float 벡터로 내보낼 때는 명시적으로 변환한다. 네이티브 스크립트 ABI 버전은 4다. 이 변경은 C++ ABI를 바꾸므로 엔진 소비자와 C# 어셈블리를 함께 다시 빌드해야 한다.

## 직접 회귀 테스트

- `Engine_animation_system`: 10¹² 좌표에서 소수 이동, 계층·JSON 저장, Jolt 위치 정밀도.
- `Engine_world64_render`: 카메라와 물체를 함께 10¹² 이동해도 같은 화면 유지, 100,000 거리의 큰 물체 표시, 2D 표시, 블렌딩한 FBX 포즈 업로드.
- `Editor_animation_assets`: 기본 Scene View의 먼 지점 투영과 격리된 미리보기.
- `Engine_csharp`: 64비트 Transform 왕복과 Animator 제어·이벤트 전달.
