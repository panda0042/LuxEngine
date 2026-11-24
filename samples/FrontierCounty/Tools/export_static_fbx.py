"""Re-export a static glTF whose topology the Assimp FBX writer cannot represent."""
import sys
import bpy

incoming, outgoing = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=incoming)
bpy.ops.export_scene.fbx(filepath=outgoing, object_types={"MESH", "EMPTY"}, bake_anim=False,
                         axis_forward="-Z", axis_up="Y", path_mode="STRIP")
