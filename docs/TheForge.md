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

`LUX_ENABLE_FORGE`나 `LUX_RENDER_BACKEND`를 선택할 필요가 없다. 렌더러 파일이 없거나 엔진과 ABI가 맞지 않으면 초기화 오류를 반환한다. ABI 버전은 현재 `23`이며 엔진과 렌더러를 함께 다시 빌드해야 한다.

## 프레임과 자원 수명

각 창은 별도 스왑체인·명령 링·프레임 자원을 사용한다. The Forge의 렌더러·리소스 로더·정적 샘플러 루트는 프로세스 안에서 공유하며 마지막 장치가 종료될 때 해제한다. 화면 획득 세마포어는 프레임 슬롯과, 제출 완료를 기다리는 프레젠테이션 세마포어는 스왑체인 이미지와 수명을 맞췄다.

Vulkan에서도 `INIT_RS_DESC`와 `initRootSignature`를 호출해 기본 정적 샘플러 디스크립터를 생성한다. 이 초기화가 없으면 첫 파이프라인 바인딩에서 assertion이 발생했다.

기준 리비전의 리소스 로더는 업로드용 전송 큐에서 셰이더 접근 배리어를 실행한다. Vulkan 검증 계층과 호환되도록 Linux 빌드에서만 생성한 리소스 로더 사본의 업로드 큐를 그래픽 큐로 바꿨다. 외부 소스 체크아웃은 수정하지 않는다. 이 호환 처리는 CMake에 주석과 적용 범위를 기록했다.

## 3D 장면 합성

에디터와 독립 Player, 생성 게임은 모두 동일한 PBR 씬 패스를 사용한다. 방향광 그림자, 불투명 장면 및 스카이박스, 수면 합성, 일반 반투명 재질, 노출 및 톤 매핑의 순서로 렌더링한다. 물이 화면에 없으면 수면 합성 패스가 생기지 않는다.

환경 이미지는 2:1 Radiance HDR을 사용한다. 씬 설정에서 환경 이미지 경로를 비우면 내장 대기 하늘을 사용한다. 내장 하늘은 구형 대기의 Rayleigh 및 Mie 단일 산란, 오존 흡수, 지표 반사를 계산해 스카이박스와 확산·정반사 IBL에 공급한다. 스카이박스 표시와 IBL은 각각 제어할 수 있다. 기본 대기의 태양 방향은 씬 기본 방향광과 일치하며, 씬 방향광 편집은 직접광에 적용된다. C++에서 `BakeAtmospherePanorama`를 호출하면 태양 방향, 관측 고도, 에어로졸 밀도, 지표 반사율을 설정해 선형 RGB 파노라마를 생성할 수 있다.

런타임에서 너비 8192픽셀과 인코딩 파일 128 MiB까지의 `.hdr`를 로드할 수 있다. OpenEXR 원본은 Python의 OpenImageIO와 NumPy를 설치한 뒤 `tools/PrepareHdrEnvironment.py`로 가져온다. 도구는 최대 24,576×12,288 원본을 스캔라인 또는 타일 단위로 읽고 선형 RGB를 평균해 8K 이하의 게임용 Radiance HDR을 생성한다.

```sh
python3 tools/PrepareHdrEnvironment.py lakeside_sunrise_24k.exr lakeside_8k.hdr --width 8192 --exposure -6
```

`--exposure`는 변환 시 적용하는 EV다. 위 예제의 −6 EV를 되돌리려면 해당 환경의 강도에 64를 곱한다. `Render3DSettings::waterReflection.faceSize`로 수면 반사 큐브맵 면 크기를 128·256·512픽셀 중 선택한다.

HDR 내부 버퍼의 RGB는 노출을 적용한 뒤 색 공간 변환을 포함한 ACES SDR 근사와 sRGB 인코딩을 거친다. 자세한 재질과 맵 형식은 [재질 문서](Materials.md)에 정리했다.

`Mesh3D::UploadOcean`으로 만든 수면은 같은 그래픽 큐에서 행·열 역 FFT와 정점 생성 패스를 실행한다. 정점 버퍼를 수면 패스가 직접 읽으며, 여러 타일이 같은 버퍼를 공유할 수 있다. 사용 예시는 [FFT 바다 예제](../samples/OceanSpectrum/README.md)에 있다.

씬별 하드웨어 레이 쿼리와 프러스텀 복셀 포그는 `Render3DSettings`에서 켠다. 적용 범위와 지원 여부 확인 방법은 [레이 조명과 포그](RayLighting.md)에 있다.

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

### 화면 공간 에지 안티앨리어싱

Render3DSettings::screenSpaceAntialiasing을 켜면 HDR 합성의 SDR 톤 매핑 결과에 대비 적응형 에지 필터를 적용한다. 주변 네 픽셀의 휘도를 비교하고, 대비가 큰 방향의 픽셀을 혼합한다. 화면 밖 샘플 좌표를 제한하고 HDR 렌더링 영역 밖의 투명 픽셀은 혼합에서 제외한다. 합성 이후 UI는 원래 해상도로 그린다. 기본값은 꺼짐이며 씬 JSON에서는 screen_space_antialiasing으로 저장된다. Frontier County에서 H로 비교할 수 있다.

### 지형 재질 블렌딩과 TAA

Material3D::terrainLayer는 불투명 커스텀 Mesh3D에서 두 번째 베이스 컬러, 접선 공간 노멀 및 거칠기 지도를 지정한다. Vertex3D::color.a가 보조 층의 가중치다. 두 층은 같은 UV와 RGB 정점 색을 사용한다. 베이스 컬러는 선형화한 후 혼합하고 노멀은 공통 접선 공간에서 보간한다.

Render3DSettings::temporalAntialiasing은 프레임의 첫 3D 패스에 Halton 서브픽셀 지터를 적용한다. HDR 합성 이후와 안개·톤 매핑 이전 사이에서 깊이 기반 카메라 리프로젝션과 이웃 색 범위 제한을 실행한다. 수면이 있는 패스는 불투명 배경과 수면에 TAA를 적용한 뒤 현재 프레임의 수면 깊이를 기준으로 투명 재질을 합성한다. 수면 픽셀은 반사나 파도 모양이 빠르게 바뀌면 히스토리의 가중치를 줄인다. 히스토리는 뷰 교체, 카메라 컷, 프레임 단절, 크기 변경, 하늘 환경 교체에 따라 초기화된다. 수면 없이 블렌드 재질이 포함된 패스와 후속 3D 패스는 기본 경로를 쓴다. UI는 합성 후 렌더링된다.


씬 엔티티는 프레임 사이에 같은 식별자를 유지한다. 직접 메시를 제출할 때는 `Material3D::motionInstance`에 배치별 고유한 값을 주면 회전·이동·크기 변화까지 TAA가 추적한다. 모션 패스는 현재 깊이와 알파 마스크를 검사한 픽셀에 이전 UV와 현재 UV의 차이 및 이전 뷰 깊이를 기록한다. 식생은 현재와 이전 프레임의 바람 변형을 평가한다. 최초 등장, 메시 버퍼 교체, CPU 정점 갱신 시 해당 표면의 히스토리를 버린다. TAA가 적용되는 마스크 재질은 텍스처 좌표와 화면상 표본 크기로 알파 커버리지를 정한다. 겹친 잎과 잔가지에서 같은 픽셀이 반복해서 비는 현상을 줄인다. 본별 스키닝 속도와 수면 시뮬레이션 속도는 제공되지 않는다.

시간적 리졸브는 모션을 제출한 표면에 11비트 식별 태그를 부여한다. 이전 픽셀의 깊이와 태그를 함께 확인해 비슷한 깊이의 다른 물체가 지나간 자리에서 잔상을 줄인다. 이 태그는 기존 R32 깊이의 하위 가수 비트를 사용한다. 현재 표면에 속한 주변 픽셀로 YCoCg 색 범위와 분산을 구하고, 조명 변화나 모션이 클 때 이전 색의 가중치를 낮춘다. `Render3DSettings::temporalSharpness`는 현재 프레임의 휘도 디테일 복원 강도다. 범위는 0–1, 기본값은 0.3이며 출력 휘도는 관측한 이웃의 범위 안에 둔다.

알파 마스크 텍스처의 첫 축소 밉은 재질의 실제 컷오프를 통과한 원본 텍셀 면적의 비율을 알파에 기록한다. 이후 밉은 이 피복률을 면적으로 가중 평균하며 RGB는 덮인 텍셀의 색으로 계산한다. 원본 밉은 유지된다. 렌더러는 축소율에 따라 알파 테스트를 분산 임계값으로 전환해 잎처럼 가는 형상의 평균 면적을 보존한다. TAA 사용 시 텍스처 좌표 기반 해시를, 그 외에는 화면 기준 Bayer 패턴을 사용한다. 그림자·호수 가림·모션 패스는 동일한 픽셀 분류 함수를 사용한다. 불투명·알파 블렌드 재질에는 마스크용 밉 처리가 적용되지 않는다.

`Render3DSettings::motionBlurShutter`는 이전 프레임 간격에 대한 셔터 개방 비율이다. 0이면 꺼지고 0.5는 180도 셔터에 해당한다. 첫 번째 HDR 뷰에서 오브젝트·카메라 속도와 깊이를 사용해 노출 구간을 적분한 뒤 투명 오버레이와 UI를 합성한다. TAA를 함께 켜면 지터를 속도에서 제거하고 TAA 히스토리에는 선명한 결과를 보관한다. 한 픽셀의 탐색 반경은 24픽셀로 제한한다. 씬 JSON 필드는 `motion_blur_shutter`다.

### 시간적 ReSTIR와 레이 추적 디노이저

Vulkan 레이 쿼리 컴퓨트 경로에서 `rayTracing.temporalRestir`를 켜면 ReSTIR DI/GI가 이전 프레임의 리저버를 카메라로 재투영한다. 수신 표면의 위치, 노멀, 인스턴스 및 재질을 확인하고 현재 표면에서 후보 기여도를 다시 평가한다. 유효 표본 수에 상한을 두어 이전 프레임의 조명이 새 조명으로 교체되도록 한다.

`rayTracing.denoiser`는 불투명 표면의 거친 반사와 간접 확산광에 적용된다. 시야 광선으로 만든 격자에서 노멀·깊이·거칠기가 비슷한 이웃을 사용하고 재투영한 조명 이력을 누적한다. 대응되는 격자 표면이 없는 픽셀은 기존 픽셀 단위 광선 추적을 사용한다. 굴절과 투명 수면 반사는 기존 시그널을 사용한다. 두 옵션은 TAA와 독립적으로 동작한다. `GpuFrameStats`의 `objectMotionDraws`, `temporalRestirPasses`, `rayDenoiserPasses`에서 모션 드로 수, 유효한 이전 프레임을 읽을 수 있는 리저버 패스 수, 디노이저 패스 수를 확인할 수 있다.

### 조명과 반사

| Render3DSettings 필드 | 단위와 동작 |
| --- | --- |
| `gtaoRadius` | 미터. 네 방향의 화면 공간 단면에서 깊이 지평선을 찾아 간접광 가시성을 적분한다. |
| `shadowLightAngularRadius` | 라디안. PCSS가 차폐물과 수신면의 깊이 차이로 그림자 필터 폭을 정한다. 태양은 약 0.00465를 쓴다. |
| `contactShadowDistance` | 미터. 수신면에서 태양 방향으로 화면 깊이를 추적한다. |
| `hierarchicalReflections` | 불투명 HDR 장면과 1, 4, 16픽셀 깊이 계층을 사용한다. 광택 재질과 수면에서 유효한 교차점을 찾고 화면 경계에서는 환경 반사로 혼합한다. |

GTAO, 접촉 그림자, SSR은 불투명 장면의 스냅샷을 공유한다. 화면 밖 물체에는 환경맵이나 지원 장치의 레이 트레이싱 반사를 사용할 수 있다.

### HDR 후처리

기본 블룸은 `bloomThreshold`를 넘는 선형 HDR 하이라이트를 여러 크기로 흐린 뒤 합성한다. `physicallyBasedBloom`을 켜면 장면의 모든 빛을 같은 세 가지 반경으로 산란시킨다. `bloomIntensity`가 커질수록 원본에서 흐린 영상으로 이동하는 빛의 비율이 증가하며 균일한 표면 밝기는 보존된다.

사진 노출은 `physicalCamera.exposure`로 켠다. `N = fNumber`, `t = shutterSeconds`, `S = iso`에서 `EV100 = log2(N²/t × 100/S)`이다. HDR 휘도에 `luminanceScale`(장면 휘도 단위당 cd/m²)을 곱한 뒤 `1.2 × 2^EV100`으로 나누고 기존 씬 노출 배율을 적용한다. 자동·지역 노출도 이 보정값을 기준으로 동작한다. [Filament 사진 노출 수식](https://github.com/google/filament/blob/main/filament/src/Exposure.cpp)

`physicalCamera.depthOfField`는 사진 노출과 별도로 켤 수 있다. 센서 세로 길이 `sensorHeightMm`와 실제 세로 시야각으로 초점거리 `f`를 정한다. 화면 높이가 `H` 픽셀이고 센서 높이가 `h`일 때 거리 `z`에서 흐림 반경은 `H/(2h) × f²/[N(Sf−f)] × |1−Sf/z|`이다. `Sf`는 미터 단위 `focusDistance`이고 계산에 쓰이는 렌즈 길이도 미터로 환산한다. 초점면은 선명하며 심도에 따라 앞뒤의 HDR 점광원이 넓어진다. 샘플링 반경은 1080p에서 최대 32픽셀로 제한한다.

`autoExposure`는 로그 평균 휘도에 시간적으로 적응한다. `localExposure`(0~1)는 가까운 표면의 깊이와 로그 휘도를 함께 비교해 국소 노출을 최대 ±1.5스톱 보정한다. `focusDistance`는 미터 단위 초점 거리, `depthOfFieldRadius`는 1080p 기준 최대 흐림 반경이다. `apertureBlades`를 0으로 두면 원형, 3~12로 설정하면 해당 날 수의 다각형 보케로 HDR 빛을 모은다. 안개는 HDR에서 먼저 합성한다. `colorGradingLut`는 톤 매핑 후 디스플레이용 3D `.cube`를 적용한다. LUT는 2³~64³와 입력 도메인을 지원하며 `colorGradingStrength`로 원본과 혼합한다.

### 하늘, 식생과 표면

`dynamicAtmosphere`는 현재 방향광에서 태양 고도를 구해 파장별 레일리 산란과 에어로졸 산란을 계산한다. `aerosolDensity`로 연무를 조절한다. `cloudCoverage`는 고도 1,250~2,400m의 구름층을 제어한다. 구름 밀도와 햇빛의 감쇠를 시선 방향으로 적분한다. 스카이 패스가 그리는 구름이며, 지형에 드리워지는 구름 그림자와 IBL 큐브의 실시간 갱신은 별도 작업이다.

`windVelocity`의 두 성분은 월드 X와 Z의 초속 미터다. 애플리케이션이 `weatherTime`에 시뮬레이션 초를 전달한다. `Vertex3D::windWeight`를 뿌리에서 0, 끝에서 1로 지정하고 `Material3D::windAmplitude`에 최대 변위(미터)를 준다. 컬러·그림자·모션 패스가 같은 변형을 사용한다. `subsurfaceRadius`는 RGB별 확산 길이(미터)다. 국소 곡률로 입사 확산광을 적분하고 `subsurfaceThickness`가 양수이면 얇은 재질의 역광 투과도 계산한다.

`AppendProjectedDecal`은 수신 메시 좌표로 정의한 투영 상자에 삼각형을 클리핑해 UV와 보간 노멀이 있는 정점·인덱스를 추가한다. 결과를 `Mesh3D::Upload`로 한 번 업로드하면 일반 PBR 재질로 그릴 수 있다. Frontier County는 이 API로 노면에 밀착된 중앙 차선을 생성한다.

### 렌즈 효과

`Render3DSettings`에서 다음 효과의 강도를 개별 조절한다. 기본값은 모두 0이다. 3D 뷰를 합성한 뒤 UI를 그리므로 글자와 조작 요소는 렌즈의 영향을 받지 않는다.

| 필드 | 범위와 동작 |
| --- | --- |
| `lensFlareStrength` | 0~2. 화면 안의 밝은 HDR 광원에서 광축 반대편의 고스트와 헤일로를 만든다. 블룸을 꺼도 작동한다. |
| `vignette` | 0~1. 센서 중앙에서 모서리로 갈수록 노출을 낮춘다. |
| `filmGrain` | 0~1. 최종 색 보정 후 중간 밝기 영역에 매 프레임 다른 입자를 더한다. |
| `chromaticAberration` | 1080p에서 0~10픽셀. 광축에서 멀어질수록 빨강·파랑 채널의 표본 위치를 벌린다. |
| `lensDistortion` | −0.35~+0.35. 부호로 방사 왜곡 방향을 바꾸며, 화면비에 맞춰 소스 영상을 샘플링한다. |
