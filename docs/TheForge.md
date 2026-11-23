# The Forge 렌더러

엔진의 렌더링 경로를 The Forge로 통합했다. 이전 Native Vulkan 구현과 실행 시 백엔드 선택 분기는 제거했다. 엔진은 공개 RHI를 유지하고, 플랫폼별 그래픽 API와 The Forge의 전역 상태는 별도 `LuxForge` 공유 라이브러리 안에 두었다.

| 플랫폼 | 그래픽 API | 런타임 파일 |
| --- | --- | --- |
| Linux | Vulkan | `libLuxForge.so` |
| Windows / MSVC | Direct3D 12 | `LuxForge.dll` |
| macOS | Metal | `libLuxForge.dylib` |

세 플랫폼의 소스·셰이더·링크·배포 경로를 구성했다. Linux에서는 X11과 Wayland 각각에서 Forge 전용 GPU 회귀 6개를 통과했다. 이 변경에서 Windows·macOS의 현지 빌드 및 GPU 실행은 검증하지 못했다.

## 의존성 준비

CMake는 의존성을 다운로드하지 않는다. SDL3 3.2 이상, Jolt Physics 5.6.0, stb_image, nlohmann_json 3.11 이상, Python 3, .NET 10 SDK를 시스템이나 별도 개발 도구 경로에 준비한다. .NET 호스팅과 게임 스크립트 구성은 [CSharpScripting.md](CSharpScripting.md)에 정리했다.

The Forge 기준 리비전은 `cd5046893faba2dc7869243873bf01f02a6f0df9`이다. 기본 소스 위치는 Git에서 제외한 `Engine/ThirdParty/TheForge`이고, 다른 설치를 사용하려면 `LUX_FORGE_ROOT`를 지정한다. 소스 준비는 사용자가 빌드 전에 수행한다.

```sh
git clone --filter=blob:none --sparse https://github.com/ConfettiFX/The-Forge.git Engine/ThirdParty/TheForge
git -C Engine/ThirdParty/TheForge checkout --detach cd5046893faba2dc7869243873bf01f02a6f0df9
git -C Engine/ThirdParty/TheForge sparse-checkout set Common_3
```

CMake는 외부 체크아웃의 리비전을 변경하지 않는다. 기준 리비전은 [TheForge.cmake](../cmake/TheForge.cmake)에 기록했다.

Linux에는 Vulkan 드라이버·개발 헤더, `glslangValidator`, X11·Wayland 개발 패키지가 필요하다. Windows는 MSVC와 The Forge 체크아웃의 Direct3D 12 관련 라이브러리·DXC를 사용한다. macOS는 Xcode 도구의 Metal 컴파일러와 시스템 프레임워크를 사용한다.

## 빌드

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DLUX_BUILD_EDITOR=ON
cmake --build build --target LuxEditor LuxForgeBackendTests
ctest --test-dir build -L forge --output-on-failure
```

`LUX_ENABLE_FORGE`나 `LUX_RENDER_BACKEND`를 선택할 필요가 없다. 렌더러 파일이 없거나 엔진과 ABI가 맞지 않으면 초기화 오류를 반환한다. ABI 버전은 현재 `16`이며 엔진과 렌더러를 함께 다시 빌드해야 한다.

## 프레임과 자원 수명

각 창은 별도 스왑체인·명령 링·프레임 자원을 사용한다. The Forge의 렌더러·리소스 로더·정적 샘플러 루트는 프로세스 안에서 공유하며 마지막 장치가 종료될 때 해제한다. 화면 획득 세마포어는 프레임 슬롯과, 제출 완료를 기다리는 프레젠테이션 세마포어는 스왑체인 이미지와 수명을 맞췄다.

Vulkan에서도 `INIT_RS_DESC`와 `initRootSignature`를 호출해 기본 정적 샘플러 디스크립터를 생성한다. 이 초기화가 없으면 첫 파이프라인 바인딩에서 assertion이 발생했다.

기준 리비전의 리소스 로더는 업로드용 전송 큐에서 셰이더 접근 배리어를 실행한다. Vulkan 검증 계층과 호환되도록 Linux 빌드에서만 생성한 리소스 로더 사본의 업로드 큐를 그래픽 큐로 바꿨다. 외부 소스 체크아웃은 수정하지 않는다. 이 호환 처리는 CMake에 주석과 적용 범위를 기록했다.

## 3D 장면 합성

에디터와 독립 Player, 생성 게임은 모두 동일한 PBR 씬 패스를 사용한다. 방향광 그림자, 불투명 장면 및 스카이박스, 수면 합성, 일반 반투명 재질, 노출 및 톤 매핑의 순서로 렌더링한다. 물이 화면에 없으면 수면 합성 패스가 생기지 않는다.

환경 이미지는 2:1 Radiance HDR을 사용한다. 씬 설정에서 환경 이미지 경로를 비우면 내장 대기 하늘을 사용한다. 내장 하늘은 구형 대기의 Rayleigh 및 Mie 단일 산란, 오존 흡수, 지표 반사를 계산해 스카이박스와 확산·정반사 IBL에 공급한다. 스카이박스 표시와 IBL은 각각 제어할 수 있다. 기본 대기의 태양 방향은 씬 기본 방향광과 일치하며, 씬 방향광 편집은 직접광에 적용된다. C++에서 `BakeAtmospherePanorama`를 호출하면 태양 방향, 관측 고도, 에어로졸 밀도, 지표 반사율을 설정해 선형 RGB 파노라마를 생성할 수 있다.

HDR 내부 버퍼의 RGB는 노출을 적용한 뒤 색 공간 변환을 포함한 ACES SDR 근사와 sRGB 인코딩을 거친다. 자세한 재질과 맵 형식은 [재질 문서](Materials.md)에 정리했다.

`Mesh3D::UploadOcean`으로 만든 수면은 같은 그래픽 큐에서 행·열 역 FFT와 정점 생성 패스를 실행한다. 정점 버퍼를 수면 패스가 직접 읽으며, 여러 타일이 같은 버퍼를 공유할 수 있다. 사용 예시는 [FFT 바다 예제](../samples/OceanSpectrum/README.md)에 있다.

## 실행 파일과 SDK 배포

실행 파일 옆에 플랫폼별 `LuxForge` 라이브러리, `gpu.data`, `gpu.cfg`, API별 셰이더 폴더(`VULKAN/`, `DIRECT3D12/`, `MACOS/`)를 유지한다. Windows에서는 AMD AGS·DXC·PIX 런타임 DLL도 함께 배포한다. CMake 빌드와 설치 규칙이 이 파일을 복사한다.

설치 SDK를 사용하는 프로젝트는 `lux_copy_runtime(게임타깃)`을 호출한다. 이 함수는 렌더러·셰이더와 C# 코어·nethost도 실행 폴더에 복사한다. .NET 런타임 전체를 게임에 묶는 self-contained 배포는 아직 구현하지 않았다.

## 검증과 남은 범위

Forge 전용 회귀 테스트는 초기화·캡처, 프레임 반복, 드로잉·텍스처, 메시, 3D 씬을 검사한다. 실행 환경에 창 시스템이 없으면 종료 코드 `77`을 반환하며 CTest에서 건너뜀으로 표시한다. 실제 GPU 초기화 실패와 렌더링 오류를 성공으로 처리하지 않는다.

삭제한 Native Vulkan 구현의 오류 주입 API는 Forge의 검증 기능이 아니다. 기존 오류 주입 전용 테스트는 해당 기능이 없음을 명시하며, Forge에서 동일한 오류 복구 기능이 구현됐다고 간주하지 않는다. 렌더러 이관과 별개로 각 플랫폼의 GPU 손실 복구·메모리 계측·배포 도구 검증을 계속 확장해야 한다.

검증 계층은 기준 The Forge의 빌드 설정(`ENABLE_GRAPHICS_VALIDATION`)에 따라 프로세스 단위로 구성한다. 현재 Debug는 검증을 켜고 Release는 끈다. 공유 렌더러를 사용하는 각 뷰포트의 `GpuValidation::Off`/`Auto` 요청으로 이 모드를 실행 중에 전환하지는 못한다. 장치 정보는 요청값 대신 실제 빌드 모드를 반환한다.

## 감사 회귀와 RenderDoc

`framesInFlight`는 1~8로 제한한다. 실제 스왑체인 이미지 개수는 플랫폼이 별도로 선택한다. `GetCounters()`의 검증 오류·경고는 Forge 로그 콜백에서 수집하며, 공유 렌더러를 쓰는 장치마다 생성 이전의 이력은 제외한다. `GetMemoryStats()`는 사용자·내부 버퍼와 GPU 해제 대기 자원의 데이터 크기를 포함한다. 드라이버의 힙·메타데이터와 스왑체인 저장 공간을 측정한 값은 아니다.

`Forge_diagnostics`는 실제 로그 콜백과 C ABI를 통한 오류·경고 전달, 장치별 이력, 버퍼 생성·지연 해제의 통계를 검사한다. `Forge_lifecycle`는 1/2/3/4/8 슬롯을, `Forge_resize`는 크기 변경과 픽셀 출력을 검사한다. 최소화 복귀는 X11에서 검사하며, Wayland에서는 컴포지터의 사용자 활성화가 필요한 항목으로 명시한다. Wayland에서 API 호출 성공만으로 실제 창이 복귀했다고 판단하지 않는다.

Linux의 RenderDoc 1.42 주입과 실시간 Vulkan 검증을 겹치면 PRESENT/GENERAL 레이아웃에 관한 도구 상호작용 오류가 발생할 수 있다. `tools/CaptureRenderDoc.py`는 먼저 주입 없는 코어·동기화 검증이 성공했음을 확인하고, 별도 캡처 단계에서만 중복 검증 레이어를 비활성화한다. 검증 단계의 실패는 캡처로 덮어쓰지 않는다. 사용자 레이어 등록이나 시스템 설정을 바꾸지 않는다.

```sh
python3 tools/CaptureRenderDoc.py --build-dir out -- scene3d
```

필요하면 `--icd`로 Vulkan 드라이버 JSON, `--exe`로 다른 실행 파일을 지정한다. 게임에는 `--frames`처럼 종료 조건을 전달한다. 결과 디렉터리에는 단계별 로그, `.rdc`와 재생 결과 `result.json`이 남는다. 이 도구는 GPU 장치 손실·물리·Windows UI의 검증을 대신하지 않는다.
