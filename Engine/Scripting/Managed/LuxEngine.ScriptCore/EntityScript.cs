using System.Numerics;
using System.Runtime.InteropServices;

namespace LuxEngine {

[StructLayout(LayoutKind.Sequential)]
/// <summary>A three-component vector with double-precision coordinates.</summary>
public struct Vector3d {
    public double X, Y, Z;

    public Vector3d(double x, double y, double z) {
        X = x;
        Y = y;
        Z = z;
    }

    public static Vector3d operator +(Vector3d a, Vector3d b) {
        return new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    }

    public static Vector3d operator -(Vector3d a, Vector3d b) {
        return new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    }

    public static Vector3d operator*(Vector3d a, double scale) {
        return new(a.X * scale, a.Y * scale, a.Z * scale);
    }

    public static implicit operator Vector3d(Vector3 value) {
        return new(value.X, value.Y, value.Z);
    }

    public static explicit operator Vector3(Vector3d value) {
        return new((float)value.X, (float)value.Y, (float)value.Z);
    }
}

[StructLayout(LayoutKind.Sequential)]
/// <summary>A snapshot of an entity’s local transform relative to its parent.</summary>
public struct Transform {
    public Vector3d Position;
    /// <summary>Local Euler angles in radians.</summary>
    public Vector3d Rotation;
    public Vector3d Scale;
}

/// <summary>A handle to an entity in the running scene.</summary>
public sealed class Entity {
    private readonly ulong world;
    public uint Id { get; }

    internal Entity(ulong world, uint id) {
        this.world = world;
        Id = id;
    }

    /// <summary>Whether the referenced entity still exists in its scene.</summary>
    public bool IsValid {
        get { return NativeBridge.IsAlive(world, Id); }
    }

    /// <summary>Gets a handle to the sprite renderer on this entity.</summary>
    public SpriteRenderer SpriteRenderer {
        get { return new SpriteRenderer(world, Id); }
    }

    public Animator Animator {
        get { return new Animator(world, Id); }
    }

    /// <summary>Gets or replaces the local transform. Assign a modified snapshot back to save changes.</summary>
    public Transform Transform {
        get { return NativeBridge.GetTransform(world, Id); }
        set { NativeBridge.SetTransform(world, Id, value); }
    }
}

/// <summary>Controls the sprite renderer attached to an entity.</summary>
public sealed class SpriteRenderer {
    private readonly ulong world;
    private readonly uint entity;

    internal SpriteRenderer(ulong world, uint entity) {
        this.world = world;
        this.entity = entity;
    }

    /// <summary>Gets or sets the sprite's RGBA tint. Values from 0 to 1 represent normal channels.</summary>
    /// <exception cref="System.InvalidOperationException">
    /// The component is unavailable, the call is off the scene thread, or a channel is not finite.
    /// </exception>
    public Vector4 Color {
        get { return NativeBridge.GetSpriteColor(world, entity); }
        set { NativeBridge.SetSpriteColor(world, entity, value); }
    }
}

public sealed class Animator {
    private readonly ulong world;
    private readonly uint entity;

    internal Animator(ulong world, uint entity) {
        this.world = world;
        this.entity = entity;
    }

    public bool Play(string state, double fade = 0.2) {
        return NativeBridge.AnimationCommand(world, entity, 0, state, fade) != 0;
    }

    public bool SetFloat(string name, double value) {
        return NativeBridge.AnimationCommand(world, entity, 1, name, value) != 0;
    }

    public bool SetInteger(string name, int value) {
        return SetFloat(name, value);
    }

    public bool SetBoolean(string name, bool value) {
        return SetFloat(name, value ? 1 : 0);
    }

    public double GetFloat(string name) {
        return NativeBridge.AnimationCommand(world, entity, 3, name, 0);
    }

    public bool SetTrigger(string name) {
        return NativeBridge.AnimationCommand(world, entity, 2, name, 0) != 0;
    }

    public double Speed {
        get { return NativeBridge.AnimationCommand(world, entity, 7, string.Empty, 0); }
        set { NativeBridge.AnimationCommand(world, entity, 6, string.Empty, value); }
    }

    public void Pause() {
        NativeBridge.AnimationCommand(world, entity, 4, string.Empty, 0);
    }

    public void Resume() {
        NativeBridge.AnimationCommand(world, entity, 5, string.Empty, 0);
    }
}

/// <summary>Base class for game scripts attached to entities.</summary>
public abstract class EntityScript {
    public Entity Entity { get; internal set; } = null!;

    /// <summary>Called once after authored fields are applied to this instance.</summary>
    protected virtual void OnCreate() {}

    /// <summary>Called when the scene updates this script.</summary>
    /// <param name="deltaTime">Elapsed time in seconds.</param>
    protected virtual void OnUpdate(double deltaTime) {}

    /// <summary>Called when the engine releases this script instance.</summary>
    protected virtual void OnDestroy() {}

    internal void Create() {
        OnCreate();
    }

    /// <summary>Called when the entity’s Animator delivers an event.</summary>
    protected virtual void OnAnimationEvent(string name, string payload) {}

    internal void AnimationEvent(string name, string payload) {
        OnAnimationEvent(name, payload);
    }

    internal void Update(double deltaTime) {
        OnUpdate(deltaTime);
    }

    internal void Destroy() {
        OnDestroy();
    }
}

public static class Input {
    /// <summary>Returns whether the specified key is currently held.</summary>
    public static bool IsKeyPressed(LuxKey key) {
        return NativeBridge.IsKeyPressed((int)key);
    }
}

// These are engine key codes, matching the public C++ LuxKey API.
public enum LuxKey {
    A = 'a',
    B = 'b',
    C = 'c',
    D = 'd',
    E = 'e',
    F = 'f',
    G = 'g',
    H = 'h',
    I = 'i',
    J = 'j',
    K = 'k',
    L = 'l',
    M = 'm',
    N = 'n',
    O = 'o',
    P = 'p',
    Q = 'q',
    R = 'r',
    S = 's',
    T = 't',
    U = 'u',
    V = 'v',
    W = 'w',
    X = 'x',
    Y = 'y',
    Z = 'z',
    Space = 32,
    Enter = 13,
    Escape = 27,
    Right = 0x4000004f,
    Left = 0x40000050,
    Down = 0x40000051,
    Up = 0x40000052
}
}
