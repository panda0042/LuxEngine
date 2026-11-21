---
_lang: ja
---
# はじめてのゲーム制作 — ビーコンラン

青い四角形を動かし、10 秒以内に黄色いビーコンを 3 個集めます。ビーコンは一つずつ現れます。プレイヤーが緑色になれば成功、赤色になれば時間切れです。ラウンド終了後に Space キーでやり直せます。

## サンプルを実行する

.NET 10 SDK と、同じリビジョンからビルドした LuxPlayer および ScriptCore が必要です。エンジンの構成は[プラットフォーム別ビルドの案内](https://github.com/panda0042/LuxEngine/blob/main/README.md)を参照してください。リポジトリのルートで以下を実行します。例では Linux の `out` ビルドを使用します。

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

Windows の Visual Studio ビルドでは `--config Debug` を指定し、通常は `out/bin/Debug` にある実行ファイルと同じ場所の `managed` フォルダーを使います。WASD または矢印キーで移動し、Esc で終了します。

## エディターで作る

1. LuxEditor の 2D ゲームプロジェクトでシーンを作ります。原点にメインの正投影カメラを置き、正投影サイズを 3 に設定します。
2. スプライト付きの `Runner`、`Beacon`、`Timer` を作ります。位置は順に `(0, 0, 1)`、`(2.5, 1.5, 1)`、`(0, 2.5, 1)`、スケールは `(0.48, 0.48, 1)`、`(0.36, 0.36, 1)`、`(7, 0.12, 1)` です。背景スプライトの Z は 0 に設定します。 Runner を青、Beacon を黄、Timer を明るい色に設定します。
3. 以下の `BeaconRun.cs` をプロジェクトの `Scripts/` に保存します。スクリプトをビルドし、Runner の C# Script コンポーネントに `FirstGame.BeaconRun` を指定します。
4. インスペクターでビルド済みの `GameScripts.dll` からフィールドを読み込みます。`Beacon` と `Timer` に対応するエンティティを割り当て、`Speed` は 4 のままにします。シーンを保存し、Game View で実行します。

## スクリプトを読む

`OnCreate` は開始時の Transform を記録します。`OnUpdate` は入力の方向と経過秒数から移動量を計算します。ビーコンの範囲に入ると次の位置へ移ります。残り時間はバーの幅、結果は [SpriteRenderer.Color](xref:LuxEngine.SpriteRenderer.Color) で表示します。

[!code-csharp[](../../samples/FirstCSharpGame/Scripts/BeaconRun.cs)]

制限時間やビーコンの位置を変えてみましょう。編集後はプレイを停止し、スクリプトを再ビルドします。フィールドとデバッグについては [C# スクリプトガイド](scripting.md) を参照してください。
