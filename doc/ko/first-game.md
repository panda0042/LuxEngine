---
_lang: ko
---
# 첫 게임 만들기 — 비콘 달리기

파란 사각형을 움직여 10초 안에 노란 비콘 세 개를 모읍니다. 비콘은 하나씩 나타납니다. 캐릭터가 초록색으로 바뀌면 성공, 빨간색으로 바뀌면 시간 초과입니다. 라운드가 끝나면 Space로 다시 시작합니다.

## 예제 실행

.NET 10 SDK와 같은 리비전에서 빌드한 LuxPlayer 및 ScriptCore가 필요합니다. 엔진은 [플랫폼별 빌드 안내](https://github.com/panda0042/LuxEngine/blob/main/README.md)를 참고해 구성합니다. 저장소 루트에서 다음 명령을 실행합니다. 예시는 Linux의 `out` 빌드 기준입니다.

```sh
cmake -S . -B out -DCMAKE_BUILD_TYPE=Debug
cmake --build out --target LuxPlayer
dotnet build samples/FirstCSharpGame/Scripts/GameScripts.csproj \
  --output out/first-game --artifacts-path out/first-game-artifacts \
  -p:RestoreConfigFile="$PWD/Engine/Scripting/Managed/NuGet.Config" \
  -p:LuxEngineScriptCore="$PWD/out/bin/managed/LuxEngine.ScriptCore.dll"
out/bin/LuxPlayer samples/FirstCSharpGame/Scene.lux \
  --size 960 640 --scripts out/first-game/GameScripts.dll
```

Windows의 Visual Studio 빌드에서는 `--config Debug`를 지정하고 보통 `out/bin/Debug/LuxPlayer.exe`와 그 옆 `managed` 폴더를 사용합니다. WASD 또는 방향키로 이동하고 Esc로 종료합니다.

## 에디터에서 직접 만들기

1. LuxEditor의 2D 게임 프로젝트에서 씬을 만듭니다. 원점에 주 직교 카메라를 놓고 직교 크기를 3으로 설정합니다.
2. Sprite를 가진 `Runner`, `Beacon`, `Timer`를 만듭니다. 각각의 위치는 `(0, 0, 1)`, `(2.5, 1.5, 1)`, `(0, 2.5, 1)`이고, 스케일은 `(0.48, 0.48, 1)`, `(0.36, 0.36, 1)`, `(7, 0.12, 1)`입니다. 배경용 Sprite는 Z를 0으로 설정합니다. Runner는 파란색, Beacon은 노란색, Timer는 밝은 색으로 설정합니다.
3. 아래 `BeaconRun.cs`를 프로젝트의 `Scripts/`에 저장합니다. 스크립트를 빌드하고 Runner의 C# Script 컴포넌트에 `FirstGame.BeaconRun`을 지정합니다.
4. Inspector에서 빌드된 `GameScripts.dll`의 필드를 읽습니다. `Beacon`과 `Timer`에 해당 엔티티를 각각 연결하고 `Speed`는 4로 둡니다. 씬을 저장하고 Game View에서 실행합니다.

## 코드 읽기

`OnCreate`는 시작 Transform을 저장합니다. `OnUpdate`는 키 방향과 경과 초로 이동 거리를 계산하고, 비콘 반경에 들어오면 다음 목표로 옮깁니다. 남은 시간은 막대 너비, 결과는 [SpriteRenderer.Color](xref:LuxEngine.SpriteRenderer.Color)로 표시합니다.

[!code-csharp[](../../samples/FirstCSharpGame/Scripts/BeaconRun.cs)]

시간 제한이나 비콘 위치를 바꿔 보세요. 코드를 변경한 뒤 플레이를 멈추고 스크립트를 다시 빌드합니다. [C# 스크립트 사용법](scripting.md)에서 필드와 디버거를 더 살펴볼 수 있습니다.
