---
_lang: en
---
# Make your first game — Beacon Run

Move the blue square and collect three yellow beacons in ten seconds. Beacons appear one at a time. The runner turns green when you win and red when time runs out. Press Space after the round to try again.

## Run the sample

You need the .NET 10 SDK and LuxPlayer and ScriptCore built from the same revision. See the [platform build notes](https://github.com/panda0042/LuxEngine/blob/main/README.md) to configure the engine. Run these commands from the repository root. They use a Linux build in `out`.

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

For a Visual Studio build on Windows, select `--config Debug` and use the executable and adjacent `managed` folder, typically under `out/bin/Debug`. Move with WASD or the arrow keys, and press Esc to quit.

## Build it in the editor

1. Create a scene in a LuxEditor 2D game project. Place the primary orthographic camera at the origin with an orthographic size of 3.
2. Create sprites named `Runner`, `Beacon`, and `Timer`. Use positions `(0, 0, 1)`, `(2.5, 1.5, 1)`, and `(0, 2.5, 1)` and scales `(0.48, 0.48, 1)`, `(0.36, 0.36, 1)`, and `(7, 0.12, 1)`, respectively. Set the background sprite's Z to 0. Make Runner blue, Beacon yellow, and Timer a light color.
3. Save the `BeaconRun.cs` below in the project's `Scripts/` folder. Build the scripts, then assign `FirstGame.BeaconRun` to Runner's C# Script component.
4. Read the fields from the built `GameScripts.dll` in the inspector. Assign the corresponding entities to `Beacon` and `Timer` and leave `Speed` at 4. Save the scene and play it in Game View.

## Read the script

`OnCreate` records the starting transforms. `OnUpdate` uses key direction and elapsed seconds to move the runner. Entering the beacon's radius advances the route. The bar width shows the remaining time and [SpriteRenderer.Color](xref:LuxEngine.SpriteRenderer.Color) shows the result.

[!code-csharp[](../../samples/FirstCSharpGame/Scripts/BeaconRun.cs)]

Try changing the time limit or the beacon positions. Stop play and rebuild the scripts after editing. See the [C# scripting guide](scripting.md) for more on fields and debugging.
