---
_lang: ja
---
# C# スクリプトガイド

ゲームの `Scripts/GameScripts.csproj` は .NET 10 を対象とし、`LuxEngine.ScriptCore.dll` を参照します。[EntityScript](xref:LuxEngine.EntityScript) を継承する公開クラスを作り、C# Script コンポーネントに名前空間を含むクラス名を入力します。

- `OnCreate` はインスペクターのフィールド適用後に呼ばれます。初期状態の記録や必須参照の確認に使います。
- `OnUpdate(double deltaTime)` が受け取る経過時間の単位は秒です。キーが押されている間は `Input.IsKeyPressed` が true を返します。
- `OnDestroy` はスクリプトインスタンスの終了時に呼ばれます。`OnAnimationEvent` は Animator のイベントを受け取ります。

`Entity.Transform` はローカル位置・回転・スケールのコピーです。座標には `Vector3d` を使い、回転の単位はラジアンです。コピーを変更したら再代入します。`Entity.SpriteRenderer.Color` はスプライトを持つエンティティの RGBA 色を変更します。エンジン API はシーンスレッドのコールバックから呼び出します。

インスペクターには公開インスタンスフィールドと `[SerializeField]` を付けた非公開フィールドが表示されます。対応する型は `bool`、`int`、`float`、`string`、`Vector2`、`Vector3`、`Vector4`、`Vector3d`、`Entity` です。参照先のエンティティが削除される可能性がある場合は `IsValid` を確認します。

スクリプトをビルドして新しいプレイセッションを開始すると更新した DLL が読み込まれます。コード左側の余白をクリックするか F9 を押してブレークポイントを設定し、スクリプトデバッガーから別の LuxPlayer プロセスを起動します。導入と設定は[スクリプト開発ツール](https://github.com/panda0042/LuxEngine/blob/main/docs/ScriptDevelopment.md)を参照してください。

[はじめてのゲーム制作](first-game.md) · [.NET API リファレンス](xref:LuxEngine)
