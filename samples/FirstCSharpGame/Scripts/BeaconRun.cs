using System.Numerics;
using LuxEngine;

namespace FirstGame;

public sealed class BeaconRun : EntityScript {
    public Entity? Beacon;
    public Entity? Timer;
    public float Speed = 4;

    private static readonly Vector3d[] stops = [new(2.5, 1.5, 1), new(-2.5, 1.5, 1), new(0, -1.5, 1)];
    private const double TimeLimit = 10;
    private Transform start;
    private Transform timerStart;
    private Vector4 playerColor;
    private Vector4 beaconColor;
    private int collected;
    private double remaining;
    private bool spaceWasDown;

    protected override void OnCreate() {
        if (Beacon?.IsValid != true || Timer?.IsValid != true)
            throw new InvalidOperationException("Assign Beacon and Timer in the C# Script inspector.");

        start = Entity.Transform;
        playerColor = Entity.SpriteRenderer.Color;
        beaconColor = Beacon.SpriteRenderer.Color;
        timerStart = Timer.Transform;
        Restart();
    }

    protected override void OnUpdate(double deltaTime) {
        bool spaceDown = Input.IsKeyPressed(LuxKey.Space);
        bool restart = spaceDown && !spaceWasDown;
        spaceWasDown = spaceDown;
        if (collected == stops.Length || remaining <= 0) {
            if (restart)
                Restart();
            return;
        }

        Move(deltaTime);
        Vector3d offset = Entity.Transform.Position - Beacon!.Transform.Position;
        if (offset.X * offset.X + offset.Y * offset.Y < 0.55 * 0.55) {
            ++collected;
            if (collected == stops.Length) {
                Entity.SpriteRenderer.Color = new Vector4(0.3f, 1, 0.45f, 1);
                Beacon.SpriteRenderer.Color = new Vector4(beaconColor.X, beaconColor.Y, beaconColor.Z, 0);
                return;
            }
            PlaceBeacon();
        }

        remaining = Math.Max(0, remaining - deltaTime);
        Transform bar = timerStart;
        double width = timerStart.Scale.X * remaining / TimeLimit;
        bar.Scale.X = width;
        bar.Position.X -= (timerStart.Scale.X - width) / 2;
        Timer!.Transform = bar;
        if (remaining == 0)
            Entity.SpriteRenderer.Color = new Vector4(1, 0.35f, 0.3f, 1);
    }

    private void Move(double deltaTime) {
        int x = (Down(LuxKey.D, LuxKey.Right) ? 1 : 0) - (Down(LuxKey.A, LuxKey.Left) ? 1 : 0);
        int y = (Down(LuxKey.W, LuxKey.Up) ? 1 : 0) - (Down(LuxKey.S, LuxKey.Down) ? 1 : 0);
        if (x == 0 && y == 0)
            return;
        double step = Speed * deltaTime / Math.Sqrt(x * x + y * y);
        Transform player = Entity.Transform;
        player.Position.X = Math.Clamp(player.Position.X + x * step, -3.5, 3.5);
        player.Position.Y = Math.Clamp(player.Position.Y + y * step, -2, 2);
        Entity.Transform = player;
    }

    private static bool Down(LuxKey first, LuxKey second) {
        return Input.IsKeyPressed(first) || Input.IsKeyPressed(second);
    }

    private void Restart() {
        collected = 0;
        remaining = TimeLimit;
        Entity.Transform = start;
        Entity.SpriteRenderer.Color = playerColor;
        Beacon!.SpriteRenderer.Color = beaconColor;
        Timer!.Transform = timerStart;
        PlaceBeacon();
    }

    private void PlaceBeacon() {
        Transform beacon = Beacon!.Transform;
        beacon.Position = stops[collected];
        Beacon.Transform = beacon;
    }
}
