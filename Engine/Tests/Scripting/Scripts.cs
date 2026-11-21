using LuxEngine;
using System.Numerics;

namespace ScriptTests {
public sealed class Mover : EntityScript {
    protected override void OnCreate() {
        Transform value = Entity.Transform;
        value.Position.Y += 10;
        Entity.Transform = value;
    }

    protected override void OnUpdate(double deltaTime) {
        Transform value = Entity.Transform;
        value.Position.X += (float)deltaTime;
        Entity.Transform = value;
    }

    protected override void OnDestroy() {
        if (Entity.IsValid) {
            Transform value = Entity.Transform;
            value.Position.Z += 1;
            Entity.Transform = value;
        }
    }
}

public sealed class Fields : EntityScript {
    public int Health = 5;
    public float Speed = 1;
    public bool Enabled;
    public string Title = "default";
    public Vector3 Offset;
    public Vector4 Tint = Vector4.One;
    public Entity? Target;
    [SerializeField]
    private Vector2 window = new(2, 3);

    protected override void OnCreate() {
        if (Health != 7 || Speed != 3.5f || !Enabled || Title != "필드 회귀" ||
            Offset != new Vector3(1, 2, 3) || Tint != new Vector4(1, 0.5f, 0.25f, 1) ||
            window != new Vector2(8, 9) || Target is null || !Target.IsValid)
            throw new InvalidOperationException(
                "Serialized fields were not applied before OnCreate.");
        Transform value = Entity.Transform;
        value.Position = new Vector3(Health, Speed, Target.Id);
        Entity.Transform = value;
    }
}

public sealed class MetadataOnce : EntityScript {
    private static int creations;
    public string Text = new('x', ++creations);
}

public sealed class LargeMetadata : EntityScript {
    public string Text = new('x', 65536);
}

public sealed class UnsupportedField : EntityScript {
    public double Unsupported;
}

public sealed class Throwing : EntityScript {
    protected override void OnUpdate(double deltaTime) {
        throw new InvalidOperationException("managed regression exception");
    }
}

public sealed class AnimationDriver : EntityScript {
    protected override void OnCreate() {
        Transform value = Entity.Transform;
        if (value.Position.X != 1000000000000.125)
            throw new InvalidOperationException("64-bit transform ABI lost precision.");
        value.Position.X += 0.125;
        Entity.Transform = value;
        if (!Entity.Animator.SetFloat("speed", 2) || !Entity.Animator.SetTrigger("jump"))
            throw new InvalidOperationException("Animator parameters were not connected.");
        Entity.Animator.Speed = 1;
    }

    protected override void OnAnimationEvent(string name, string payload) {
        if (name != "step" || payload != "right")
            throw new InvalidOperationException("Animation event payload changed.");
        Transform value = Entity.Transform;
        value.Scale.Z += 1;
        Entity.Transform = value;
    }
}

public sealed class LiveFields : EntityScript {
    public float Amount = 2;
    public float Other = 4;
    private int updates;

    protected override void OnCreate() {
        Transform value = Entity.Transform;
        value.Position.Y += 1;
        Entity.Transform = value;
    }

    protected override void OnUpdate(double deltaTime) {
        Transform value = Entity.Transform;
        value.Position.X = ++updates;
        value.Scale.X = Amount;
        value.Scale.Y = ++Other;
        Entity.Transform = value;
    }

    protected override void OnDestroy() {
        if (Entity.IsValid) {
            Transform value = Entity.Transform;
            value.Position.Z += 1;
            Entity.Transform = value;
        }
    }
}

public sealed class AnimatorPlayProbe : EntityScript {
    public string State = "";

    protected override void OnCreate() {
        Entity.Animator.Pause();
        if (State.Length == 0)
            return;
        var result = Entity.Animator.Play(State, 0);
        Transform value = Entity.Transform;
        value.Position.Z = result ? 1 : -1;
        Entity.Transform = value;
    }
}

public sealed class AnimationQueueProbe : EntityScript {
    protected override void OnAnimationEvent(string name, string payload) {
        Transform value = Entity.Transform;
        if (name == "clip")
            value.Position.X += 1;
        else if (name == "step")
            value.Position.Y += 1;
        Entity.Transform = value;
    }
}

public sealed class ControllerSwitchProbe : EntityScript {
    protected override void OnUpdate(double deltaTime) {
        Transform value = Entity.Transform;
        value.Position.Z = Entity.Animator.Play("NewMove", 0) ? 1 : -1;
        Entity.Transform = value;
    }
}

public sealed class DebugProbe : EntityScript {
    public int Counter = 0;

    protected override void OnUpdate(double deltaTime) {
        Counter += 1; // LuxEditor debugger integration breakpoint
        int snapshot = Counter;
        if (snapshot < 0)
            throw new InvalidOperationException("Unexpected counter value.");
    }
}
}
