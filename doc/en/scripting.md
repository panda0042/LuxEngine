---
_lang: en
---
# C# scripting guide

The game's `Scripts/GameScripts.csproj` targets .NET 10 and references `LuxEngine.ScriptCore.dll`. Create a public class derived from [EntityScript](xref:LuxEngine.EntityScript). Enter its namespace-qualified name in a C# Script component.

- `OnCreate` runs after the inspector fields are applied. Use it to record the starting state and check required references.
- `OnUpdate(double deltaTime)` receives elapsed time in seconds. `Input.IsKeyPressed` is true while a key is held.
- `OnDestroy` runs when the script instance is torn down. `OnAnimationEvent` receives Animator events.

`Entity.Transform` is a snapshot of local position, rotation, and scale. Coordinates use `Vector3d` and rotations use radians. Edit the local copy and assign it back. Use `Entity.SpriteRenderer.Color` to change the RGBA tint of an entity with a sprite. Call engine APIs from callbacks on the scene thread.

The inspector exposes public instance fields and private fields marked with `[SerializeField]`. Supported types include `bool`, `int`, `float`, `string`, `Vector2`, `Vector3`, `Vector4`, `Vector3d`, and `Entity`. Check `IsValid` when a referenced entity might have been removed.

Build the scripts and start a new play session to load the updated DLL. Click the code margin or press F9 to set a breakpoint. Start debugging in the script debugger to launch a separate LuxPlayer process. See [script development tools](https://github.com/panda0042/LuxEngine/blob/main/docs/ScriptDevelopment.md) for installation and setup.

[Make your first game](first-game.md) · [.NET API reference](xref:LuxEngine)
