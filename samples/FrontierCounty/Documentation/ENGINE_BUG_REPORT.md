# Engine findings

## Zero-length skinned normals

- **Condition** A valid bind normal is present, but the skin evaluation produces a zero-length normal at an individual vertex. Detected during early conversions of the Dog Walker and Hiker FBXs.
- **Previous behavior** Evaluating the pose rejected the entire model with “FBX 법선을 정규화할 수 없습니다.”
- **Change** Pose extraction retains the validated node-local bind normal when the deformed vector has zero length and continues to reject non-finite normals. Degenerate source conversions that produced non-finite poses were replaced by Blender exports.
- **Verification** Frontier County evaluates the final FBXs with the runtime importer, and the smoke test checks skin deformation at two times.

## Import pipeline and road surface

- The Blender glTF importer generated an Icosphere for bone display. The export previously included that object alongside the character. The exporter now selects only the skeleton and skinned render objects. The sample rejects any unskinned submesh in a humanoid package.
- The old road was elevated about 28 cm above the terrain and bridged independently across heightfield edges. Each asphalt and marking polygon is now clipped per heightfield triangle and uses the same triangle interpolation, with a centimeter-scale depth bias.

## Benchmark framebuffer resize

The X11 window was resized during a first measurement, producing data at two framebuffer sizes in the same run. The application now exposes a resizable-window option. The benchmark locks its window and rejects an in-run framebuffer dimension change.

## Physics limits

The Jolt adapter selects a single-threaded job system. Scene::AddImpulse3D also narrows the pre-step velocity to float. Those paths were observed in code and are separate from the sample renderer changes.

## Local tooling

The sandbox initially denied the FSL forkserver socket. The approved local compile path completed the shader build. The packaged workstation Blender had a MaterialX symbol mismatch. The portable Blender 4.5.17 binary exported the final meshes and motion clips.
