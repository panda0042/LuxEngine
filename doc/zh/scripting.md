---
_lang: zh-CN
---
# C# 脚本指南

游戏的 `Scripts/GameScripts.csproj` 以 .NET 10 为目标，并引用 `LuxEngine.ScriptCore.dll`。创建继承 [EntityScript](xref:LuxEngine.EntityScript) 的公开类，在 C# Script 组件中填写带命名空间的类名。

- `OnCreate` 在检查器字段赋值后运行。可在这里记录初始状态或检查必需的引用。
- `OnUpdate(double deltaTime)` 接收以秒为单位的经过时间。按住按键时，`Input.IsKeyPressed` 返回 true。
- `OnDestroy` 在脚本实例销毁时运行。`OnAnimationEvent` 接收 Animator 事件。

`Entity.Transform` 是局部位置、旋转和缩放的值快照。坐标使用 `Vector3d`，旋转单位为弧度。修改副本后将它赋回属性。使用 `Entity.SpriteRenderer.Color` 修改带有精灵组件的实体的 RGBA 颜色。请在场景线程的回调中调用引擎 API。

检查器显示公开实例字段，以及带有 `[SerializeField]` 的非公开字段。支持 `bool`、`int`、`float`、`string`、`Vector2`、`Vector3`、`Vector4`、`Vector3d` 和 `Entity`。如果引用的实体可能已被删除，请检查 `IsValid`。

构建脚本并开启新的运行会话以加载更新后的 DLL。点击代码边栏或按 F9 设置断点，然后从脚本调试器启动独立的 LuxPlayer 进程。安装及配置方法见[脚本开发工具](https://github.com/panda0042/LuxEngine/blob/main/docs/ScriptDevelopment.md)。

[制作第一个游戏](first-game.md) · [.NET API 参考](xref:LuxEngine)
