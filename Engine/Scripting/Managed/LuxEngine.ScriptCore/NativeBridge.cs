using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;

namespace LuxEngine {

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeApi {
    public uint Version;
    public uint Size;
    public delegate* unmanaged[Cdecl]<ulong, uint, int> IsAlive;
    public delegate* unmanaged[Cdecl]<ulong, uint, Transform*, int> GetTransform;
    public delegate* unmanaged[Cdecl]<ulong, uint, Transform*, int> SetTransform;
    public delegate* unmanaged[Cdecl]<ulong, uint, Vector4*, int> GetSpriteColor;
    public delegate* unmanaged[Cdecl]<ulong, uint, Vector4*, int> SetSpriteColor;
    public delegate* unmanaged[Cdecl]<int, int> IsKeyPressed;
    public delegate* unmanaged[Cdecl]<ulong, uint, int, byte*, int, double, double> Animation;
    public delegate* unmanaged[Cdecl]<ulong, uint, byte*, int, int> AnimationEvent;
    public delegate* unmanaged[Cdecl]<ulong, uint, Vector3d*, double, int> CharacterMotion;
    public delegate* unmanaged[Cdecl]<ulong, uint, CharacterState3D*, int> GetCharacterState;
    public delegate* unmanaged[Cdecl]<ulong, uint, Vector3d*, int> GetVelocity3D;
    public delegate* unmanaged[Cdecl]<ulong, uint, Vector3d*, int> SetVelocity3D;
    public delegate* unmanaged[Cdecl]<ulong, uint, Vector3d*, int> AddImpulse3D;
    public delegate* unmanaged[Cdecl]<ulong, uint, int> IsGrounded3D;
    public delegate* unmanaged[Cdecl]<ulong, uint, Vector3d*, Vector3d*, float, int, RaycastHit3D*, int>
        Raycast3D;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ManagedApi {
    public uint Version;
    public uint Size;
    public delegate* unmanaged[Cdecl]<ulong, byte*, int, ulong> Load;
    public delegate* unmanaged[Cdecl]<ulong, uint, byte*, int, byte*, int, int> Attach;
    public delegate* unmanaged[Cdecl]<ulong, uint, byte*, int, int> SetFields;
    public delegate* unmanaged[Cdecl]<ulong, uint, int> Detach;
    public delegate* unmanaged[Cdecl]<ulong, double, int, int> Update;
    public delegate* unmanaged[Cdecl]<ulong, void> Unload;
    public delegate* unmanaged[Cdecl]<ulong, byte*, int, byte*, int, int> Describe;
    public delegate* unmanaged[Cdecl]<byte*, int, int> LastError;
}

internal sealed class ScriptAssembly : AssemblyLoadContext {
    private readonly AssemblyDependencyResolver resolver;

    internal ScriptAssembly(string path) : base(isCollectible: true) {
        resolver = new AssemblyDependencyResolver(path);
    }

    protected override Assembly? Load(AssemblyName name) {
        if (name.Name == typeof(EntityScript).Assembly.GetName().Name)
            return typeof(EntityScript).Assembly;
        string? path = resolver.ResolveAssemblyToPath(name);
        return path is null ? null : LoadFromAssemblyPath(path);
    }

    protected override nint LoadUnmanagedDll(string name) {
        string? path = resolver.ResolveUnmanagedDllToPath(name);
        return path is null ? 0 : LoadUnmanagedDllFromPath(path);
    }
}

// This is the only managed/native entry surface. Exceptions never cross its C ABI.
public static unsafe class NativeBridge {
    private sealed record Instance(EntityScript Script, ScriptFieldReflection.Binding Fields) {
        internal bool UpdateFaulted { get; set; }
    }

    private sealed class World {
        internal required ulong NativeId;
        internal required ScriptAssembly Loader;
        internal required Assembly Assembly;
        internal readonly SortedDictionary<uint, Instance> Instances = new();
    }

    private static NativeApi native;
    private const uint ScriptAbiVersion = 8;
    private static readonly Dictionary<ulong, World> worlds = new();
    private static ulong nextId;
    private static string lastError = string.Empty;

    internal static double AnimationCommand(ulong world, uint entity, int operation, string name,
                                            double value) {
        byte[] bytes = Encoding.UTF8.GetBytes(name);
        fixed(byte* data = bytes) return native.Animation(world, entity, operation, data,
                                                          bytes.Length, value);
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    public static int GetApi(void* nativeApi, void* managedApi) {
        try {
            if (nativeApi == null || managedApi == null)
                return 0;
            // Check the fixed header before reading callbacks from a potentially older host.
            uint* header = (uint*)nativeApi;
            if (header[0] != ScriptAbiVersion || header[1] != sizeof(NativeApi))
                return 0;
            native = *(NativeApi*)nativeApi;
            *(ManagedApi*)managedApi =
                new ManagedApi { Version = ScriptAbiVersion, Size = (uint)sizeof(ManagedApi),
                                 Load = &Load,          Attach = &Attach,
                                 SetFields = &SetFields, Detach = &Detach, Update = &Update,
                                 Unload = &Unload,      Describe = &Describe,
                                 LastError = &LastError };
            return 1;
        } catch (Exception error) {
            lastError = error.ToString();
            return 0;
        }
    }

    internal static bool IsAlive(ulong world, uint entity) {
        return native.IsAlive(world, entity) != 0;
    }

    internal static Transform GetTransform(ulong world, uint entity) {
        Transform value;
        if (native.GetTransform(world, entity, &value) == 0)
            throw new InvalidOperationException(
                "The entity is unavailable or the script is running off the scene thread.");
        return value;
    }

    internal static void SetTransform(ulong world, uint entity, Transform value) {
        if (native.SetTransform(world, entity, &value) == 0)
            throw new InvalidOperationException("The entity transform is invalid or unavailable.");
    }

    internal static bool SetCharacterMotion3D(ulong world, uint entity, Vector3d horizontalVelocity,
                                               double jumpSpeed) {
        return native.CharacterMotion(world, entity, &horizontalVelocity, jumpSpeed) != 0;
    }

    internal static bool TryGetCharacterState3D(ulong world, uint entity, out CharacterState3D state) {
        CharacterState3D value = new();
        bool found = native.GetCharacterState(world, entity, &value) != 0;
        state = found ? value : default;
        return found;
    }

    internal static bool TryGetVelocity3D(ulong world, uint entity, out Vector3d velocity) {
        Vector3d value;
        bool found = native.GetVelocity3D(world, entity, &value) != 0;
        velocity = found ? value : default;
        return found;
    }

    internal static bool SetVelocity3D(ulong world, uint entity, Vector3d velocity) {
        return native.SetVelocity3D(world, entity, &velocity) != 0;
    }

    internal static bool AddImpulse3D(ulong world, uint entity, Vector3d impulse) {
        return native.AddImpulse3D(world, entity, &impulse) != 0;
    }

    internal static bool IsGrounded3D(ulong world, uint entity) {
        return native.IsGrounded3D(world, entity) != 0;
    }

    internal static bool Raycast3D(ulong world, uint entity, Vector3d origin, Vector3d direction,
                                   float maxDistance, bool includeTriggers, out RaycastHit3D hit) {
        RaycastHit3D value;
        bool found = native.Raycast3D(world, entity, &origin, &direction, maxDistance,
                                     includeTriggers ? 1 : 0, &value) != 0;
        hit = found ? value : default;
        return found;
    }

    internal static Vector4 GetSpriteColor(ulong world, uint entity) {
        Vector4 value;
        if (native.GetSpriteColor(world, entity, &value) == 0)
            throw new InvalidOperationException("The entity's sprite renderer is unavailable.");
        return value;
    }

    internal static void SetSpriteColor(ulong world, uint entity, Vector4 value) {
        if (native.SetSpriteColor(world, entity, &value) == 0)
            throw new InvalidOperationException("The sprite renderer is unavailable or its color is not finite.");
    }

    internal static bool IsKeyPressed(int key) {
        return native.IsKeyPressed(key) != 0;
    }

    private static World Find(ulong id) {
            if (!worlds.TryGetValue(id, out World? world)) {
                throw new InvalidOperationException("The script world has stopped.");
            }
            return world;
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static ulong Load(ulong nativeWorld, byte* path, int length) {
        ScriptAssembly? loader = null;
        try {
            string assemblyPath =
                Path.GetFullPath(Encoding.UTF8.GetString(new ReadOnlySpan<byte>(path, length)));
            loader = new ScriptAssembly(assemblyPath);
            Assembly assembly = loader.LoadFromAssemblyPath(assemblyPath);
            // Only a separately launched debug player opts in; editor PIE must never block its UI.
            if (Environment.GetEnvironmentVariable("LUX_SCRIPT_DEBUG_WAIT") == "1" &&
                !System.Diagnostics.Debugger.IsAttached) {
                Console.WriteLine("LUX_SCRIPT_DEBUG_READY");
                Console.Out.Flush();
                var timeout = System.Diagnostics.Stopwatch.StartNew();
                while (!System.Diagnostics.Debugger.IsAttached) {
                    if (timeout.Elapsed > TimeSpan.FromSeconds(30))
                        throw new TimeoutException(
                            "The script debugger did not attach within 30 seconds.");
                    System.Threading.Thread.Sleep(20);
                }
            }
            ulong id = ++nextId;
            worlds.Add(id,
                       new World { NativeId = nativeWorld, Loader = loader, Assembly = assembly });
            return id;
        } catch (Exception error) {
            loader?.Unload();
            lastError = error.ToString();
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static int Attach(ulong id, uint entity, byte* name, int length, byte* fields,
                              int fieldLength) {
        try {
            World world = Find(id);
            string typeName = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(name, length));
            Type type = world.Assembly.GetType(typeName, throwOnError: true)!;
            if (type.IsAbstract || !typeof(EntityScript).IsAssignableFrom(type))
                throw new InvalidOperationException(
                    $"{typeName} must be a concrete EntityScript class.");
            if (world.Instances.ContainsKey(entity))
                throw new InvalidOperationException("A script is already attached to this entity.");
            EntityScript script = (EntityScript)Activator.CreateInstance(type)!;
            script.Entity = new Entity(world.NativeId, entity);
            var binding = new ScriptFieldReflection.Binding(script, world.NativeId);
            binding.Apply(Encoding.UTF8.GetString(new ReadOnlySpan<byte>(fields, fieldLength)));
            script.Create();
            world.Instances.Add(entity, new Instance(script, binding));
            return 1;
        } catch (Exception error) {
            lastError = error.ToString();
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static int Describe(ulong id, byte* name, int length, byte* destination, int capacity) {
        try {
            World world = Find(id);
            Type type = world.Assembly.GetType(
                Encoding.UTF8.GetString(new ReadOnlySpan<byte>(name, length)), throwOnError: true)!;
            if (type.IsAbstract || !typeof(EntityScript).IsAssignableFrom(type))
                throw new InvalidOperationException(
                    "The type must be a concrete EntityScript class.");
            byte[] bytes = Encoding.UTF8.GetBytes(ScriptFieldReflection.Describe(type));
            if (bytes.Length > 65536)
                throw new InvalidOperationException(
                    "Script metadata exceeds the 65536-byte UTF-8 limit.");
            if (destination != null && capacity > bytes.Length) {
                bytes.CopyTo(new Span<byte>(destination, capacity));
                destination[bytes.Length] = 0;
            }
            return bytes.Length;
        } catch (Exception error) {
            lastError = error.ToString();
            return -1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static int SetFields(ulong id, uint entity, byte* fields, int length) {
        try {
            Find(id).Instances[entity].Fields.Apply(
                Encoding.UTF8.GetString(new ReadOnlySpan<byte>(fields, length)));
            return 1;
        } catch (Exception error) {
            lastError = error.ToString();
            return 0;
        }
    }

    private static void Remove(World world, uint entity) {
            if (!world.Instances.Remove(entity, out Instance? instance))
                return;
            instance.Script.Destroy();
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static int Detach(ulong id, uint entity) {
        try {
            Remove(Find(id), entity);
            return 1;
        } catch (Exception error) {
            lastError = error.ToString();
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static int Update(ulong id, double deltaTime, int isolateFailures) {
        try {
            World world = Find(id);
            var errors = new StringBuilder();
            foreach ((uint entity, Instance instance) in world.Instances.ToArray()) {
                if (!IsAlive(world.NativeId, entity)) {
                    Remove(world, entity);
                    continue;
                }
                if (instance.UpdateFaulted)
                    continue;
                EntityScript script = instance.Script;
                var callback = "OnAnimationEvent";
                try {
                    for (int count = 0; count < 4096; ++count) {
                        int length = native.AnimationEvent(world.NativeId, entity, null, 0);
                        if (length <= 0)
                            break;
                        if (length > 1024 * 1024)
                            throw new InvalidOperationException(
                                "Animation event exceeds the size limit.");
                        byte[] data = new byte[length];
                        fixed (byte* destination = data)
                            native.AnimationEvent(world.NativeId, entity, destination, length);
                        using JsonDocument value = JsonDocument.Parse(data);
                        script.AnimationEvent(
                            value.RootElement.GetProperty("name").GetString()!,
                            value.RootElement.GetProperty("payload").GetString()!);
                    }
                    callback = "OnUpdate";
                    script.Update(deltaTime);
                } catch (Exception error) when (isolateFailures != 0) {
                    instance.UpdateFaulted = true;
                    if (errors.Length < 12000)
                        errors.AppendLine($"Entity #{entity} {script.GetType().FullName}.{callback}: {error}");
                }
            }
            if (errors.Length == 0)
                return 1;
            lastError = errors.ToString();
            return 2;
        } catch (Exception error) {
            lastError = error.ToString();
            return 0;
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static void Unload(ulong id) {
            if (!worlds.Remove(id, out World? world))
                return;
            try {
                foreach (uint entity in world.Instances.Keys.ToArray()) {
                    try {
                        Remove(world, entity);
                    } catch (Exception error) {
                        lastError = error.ToString();
                    }
                }
            } finally {
                world.Instances.Clear();
                world.Loader.Unload();
            }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]

    private static int LastError(byte* destination, int capacity) {
        try {
            byte[] bytes = Encoding.UTF8.GetBytes(lastError);
            if (destination != null && capacity > 0)
                bytes.AsSpan(0, Math.Min(bytes.Length, capacity - 1))
                    .CopyTo(new Span<byte>(destination, capacity));
            if (destination != null && capacity > 0)
                destination[Math.Min(bytes.Length, capacity - 1)] = 0;
            return bytes.Length;
        } catch {
            return 0;
        }
    }
}
}
