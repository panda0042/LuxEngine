---
_lang: zh-CN
---
# 制作第一个游戏 — 信标竞速

移动蓝色方块，在 10 秒内收集三个黄色信标。信标依次出现。角色变绿表示成功，变红表示超时。回合结束后按空格键重试。

## 运行示例

请准备 .NET 10 SDK，并使用同一源码版本构建 LuxPlayer 和 ScriptCore。引擎的配置方式见[各平台构建说明](https://github.com/panda0042/LuxEngine/blob/main/README.md)。在仓库根目录运行以下命令。示例使用 Linux 上的 `out` 构建目录。

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

在 Windows 上使用 Visual Studio 构建时，指定 `--config Debug`，并使用对应的可执行文件和同目录的 `managed` 文件夹，通常位于 `out/bin/Debug`。用 WASD 或方向键移动，按 Esc 退出。

## 在编辑器中制作

1. 在 LuxEditor 的 2D 游戏项目中创建场景。将主正交相机放在原点，正交尺寸设为 3。
2. 创建三个精灵实体 `Runner`、`Beacon` 和 `Timer`。位置依次为 `(0, 0, 1)`、`(2.5, 1.5, 1)`、`(0, 2.5, 1)`，缩放依次为 `(0.48, 0.48, 1)`、`(0.36, 0.36, 1)`、`(7, 0.12, 1)`。背景精灵的 Z 设为 0。 将 Runner 设为蓝色、Beacon 设为黄色、Timer 设为浅色。
3. 将下方的 `BeaconRun.cs` 保存到项目的 `Scripts/` 目录。构建脚本，然后在 Runner 的 C# Script 组件上指定 `FirstGame.BeaconRun`。
4. 在检查器中读取构建生成的 `GameScripts.dll` 字段。将对应实体分别赋给 `Beacon` 和 `Timer`，保留 `Speed` 为 4。保存场景并在 Game View 中运行。

## 阅读脚本

`OnCreate` 记录初始变换。`OnUpdate` 根据按键方向与经过的秒数移动角色。进入信标的判定范围后，信标会移到下一处。计时条的宽度表示剩余时间，[SpriteRenderer.Color](xref:LuxEngine.SpriteRenderer.Color) 表示回合结果。

[!code-csharp[](../../samples/FirstCSharpGame/Scripts/BeaconRun.cs)]

试着修改时间限制或信标的位置。修改代码后，停止运行并重新构建脚本。字段与调试方法见 [C# 脚本指南](scripting.md)。
