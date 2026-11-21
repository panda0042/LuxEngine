---
_lang: ko
---
# C# 스크립트 사용법

게임 프로젝트의 `Scripts/GameScripts.csproj`는 .NET 10을 대상으로 하며 `LuxEngine.ScriptCore.dll`을 참조합니다. 스크립트마다 공개 클래스를 만들고 [EntityScript](xref:LuxEngine.EntityScript)를 상속합니다. 씬에서 C# Script 컴포넌트에 네임스페이스를 포함한 클래스 이름을 입력합니다.

- `OnCreate`는 Inspector 필드가 적용된 뒤에 호출됩니다. 시작 상태를 저장하거나 필수 참조를 확인할 때 사용합니다.
- `OnUpdate(double deltaTime)`의 시간 단위는 초입니다. `Input.IsKeyPressed`는 키를 누르고 있는 동안 참입니다.
- `OnDestroy`는 스크립트 인스턴스가 종료될 때 호출됩니다. `OnAnimationEvent`는 Animator의 이벤트를 전달받습니다.

`Entity.Transform`은 부모 기준의 위치·회전·스케일을 담은 값입니다. 좌표는 `Vector3d`이며 회전은 라디안입니다. 읽어 온 값을 수정한 뒤 다시 대입합니다. `Entity.SpriteRenderer.Color`는 Sprite가 붙은 엔티티의 RGBA 색을 변경합니다. 엔진 API는 씬 스레드의 콜백에서 사용합니다.

Inspector에는 public 인스턴스 필드와 `[SerializeField]`가 붙은 비공개 필드가 표시됩니다. 지원 형식은 `bool`, `int`, `float`, `string`, `Vector2`, `Vector3`, `Vector4`, `Vector3d`, `Entity`입니다. 씬에서 참조하는 엔티티가 사라질 수 있으면 `IsValid`를 확인합니다.

스크립트 빌드 후 새 플레이 세션을 시작하면 새 DLL이 로드됩니다. 코드 여백이나 F9로 중단점을 설정하고 스크립트 디버거에서 별도 LuxPlayer 디버그 실행을 시작합니다. 도구 설치와 설정은 [스크립트 개발 도구](https://github.com/panda0042/LuxEngine/blob/main/docs/ScriptDevelopment.md)를 참고하세요.

[첫 게임 만들기](first-game.md) · [.NET API 레퍼런스](xref:LuxEngine)
