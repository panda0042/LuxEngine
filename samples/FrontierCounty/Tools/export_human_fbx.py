"""Export a decoded glTF to rigged FBX and optionally an animation-only FBX."""
import sys
import bpy

args = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=args[0])
# The importer's armatures can reference a generated Icosphere as a bone display shape.
# Selection includes only skinned render meshes and their rig, never bone widgets.
for obj in bpy.context.scene.objects:
    obj.select_set(obj.type in {"ARMATURE", "EMPTY"} or
                   (obj.type == "MESH" and obj.find_armature() is not None))
options = dict(use_selection=True, axis_forward="-Z", axis_up="Y", add_leaf_bones=False,
               bake_anim_use_nla_strips=False, bake_anim_use_all_actions=True,
               bake_anim_simplify_factor=.8, path_mode="STRIP")
bpy.ops.export_scene.fbx(filepath=args[1], object_types={"ARMATURE", "MESH", "EMPTY"}, **options)
if len(args) == 3:
    bpy.ops.export_scene.fbx(filepath=args[2], object_types={"ARMATURE", "EMPTY"}, **options)
