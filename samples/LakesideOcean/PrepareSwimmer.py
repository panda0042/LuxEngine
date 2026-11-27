import math
import sys
from pathlib import Path

import bpy
from mathutils import Quaternion, Vector


def add_weave(directory):
    size = 512
    image = bpy.data.images.new("Uniform weave · tangent normal", size, size, alpha=False)
    image.colorspace_settings.name = "Non-Color"
    pixels = []
    for y in range(size):
        v = math.tau * 16 * (y + 0.5) / size
        for x in range(size):
            u = math.tau * 16 * (x + 0.5) / size
            nx = 0.21 * math.sin(u) * (0.8 + 0.2 * math.cos(v * 0.5))
            ny = 0.16 * math.sin(v) * (0.8 - 0.2 * math.cos(u * 0.5))
            nx += 0.048 * math.sin(5 * u + 3 * v) + 0.029 * math.sin(9 * u - 2 * v)
            ny += 0.042 * math.sin(2 * u - 5 * v) + 0.024 * math.sin(3 * u + 9 * v)
            pixels.extend((0.5 + nx * 0.5, 0.5 + ny * 0.5,
                           0.5 + 0.5 * math.sqrt(1 - nx * nx - ny * ny), 1))
    image.pixels.foreach_set(pixels)
    image.filepath_raw = str(directory / "weave.png")
    image.file_format = "PNG"
    image.save()
    image.pack()
    for material in (bpy.data.materials["해병대 오색 물결무늬"], bpy.data.materials["회녹색 능직"]):
        nodes = material.node_tree.nodes
        uv = nodes.new("ShaderNodeTexCoord")
        scale = nodes.new("ShaderNodeVectorMath")
        scale.operation = "SCALE"
        scale.inputs["Scale"].default_value = 4
        texture = nodes.new("ShaderNodeTexImage")
        texture.image = image
        normal = nodes.new("ShaderNodeNormalMap")
        normal.inputs["Strength"].default_value = 0.42
        shader = nodes.get("Principled BSDF")
        for node, position in zip((uv, scale, texture, normal), ((-900, -240), (-690, -240),
                                                               (-480, -240), (-180, -240))):
            node.location = position
        links = material.node_tree.links
        links.new(uv.outputs["UV"], scale.inputs[0])
        links.new(scale.outputs["Vector"], texture.inputs["Vector"])
        links.new(texture.outputs["Color"], normal.inputs["Color"])
        links.new(normal.outputs["Normal"], shader.inputs["Normal"])


def add_sole_tread(rig):
    rubber = bpy.data.materials["고무 밑창"]
    raised = rubber.copy()
    raised.name = "Boot tread · raised rubber"
    shader = raised.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (0.021, 0.024, 0.023, 1)
    shader.inputs["Roughness"].default_value = 0.9
    for side in ("L", "R"):
        boot = bpy.data.objects[f"Combat boot · {side}"]
        pieces = []
        for direction in (-1, 1):
            for index in range(9):
                y = -0.02 + 0.104 * index
                z = 0.089
                inv = boot.matrix_world.inverted()
                origin = inv @ Vector((-1.4 * direction, y, z))
                ray = inv.to_3x3() @ Vector((direction, 0, 0))
                hit, location, normal, _ = boot.ray_cast(origin, ray)
                if not hit:
                    continue
                position = boot.matrix_world @ location
                world_normal = (boot.matrix_world.inverted().transposed().to_3x3() @ normal).normalized()
                bpy.ops.mesh.primitive_cube_add(size=2, location=position + world_normal * 0.002)
                piece = bpy.context.object
                piece.rotation_euler.z = math.atan2(world_normal.y, world_normal.x)
                piece.dimensions = (0.029, 0.056, 0.045)
                bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
                pieces.append(piece)
        if not pieces:
            raise RuntimeError(f"No tread contacts on boot {side}")
        bpy.ops.object.select_all(action="DESELECT")
        for piece in pieces:
            piece.select_set(True)
        bpy.context.view_layer.objects.active = pieces[0]
        bpy.ops.object.join()
        tread = bpy.context.object
        tread.name = f"Boot sole grip · {side}"
        tread.data.materials.append(raised)
        bevel = tread.modifiers.new("Rounded rubber edges", "BEVEL")
        bevel.width = 0.009
        bevel.segments = 2
        bpy.ops.object.modifier_apply(modifier=bevel.name)
        tread.parent = rig
        group = tread.vertex_groups.new(name=f"Foot.{side}")
        group.add(list(range(len(tread.data.vertices))), 1.0, "REPLACE")
        tread.modifiers.new("Armature", "ARMATURE").object = rig


def aim(rig, name, direction):
    bone = rig.pose.bones[name]
    rest = bone.bone.matrix_local
    base = bone.parent.matrix @ bone.parent.bone.matrix_local.inverted() @ rest
    local = base.to_3x3().inverted() @ Vector(direction)
    bone.rotation_quaternion = Vector((0, 1, 0)).rotation_difference(local.normalized())
    bpy.context.view_layer.update()


def swim_pose(rig, phase):
    drive = math.sin(phase)
    sweep = (1 - math.cos(phase)) * 0.5
    recovery = max(0, -drive)
    pitch = math.radians(70 - 9 * drive)
    rig.pose.bones["Root"].location.y = 0.11 * drive
    rig.pose.bones["Hips"].rotation_quaternion = Quaternion((1, 0, 0), pitch)
    bpy.context.view_layer.update()
    aim(rig, "Head", (0, -0.16 - 0.11 * drive, 1))
    for side, sign in (("L", -1), ("R", 1)):
        aim(rig, f"UpperArm.{side}", (sign * (0.33 + 0.69 * sweep), -0.95 + 1.32 * sweep,
                                      -0.16 - 0.44 * sweep))
        aim(rig, f"Forearm.{side}", (sign * (0.2 + 0.65 * drive), -0.95 + 0.91 * sweep,
                                     -0.24 - 0.55 * max(0, drive)))
        aim(rig, f"Thigh.{side}", (sign * (0.12 + 0.37 * recovery), 1, -0.12 - 0.55 * recovery))
        aim(rig, f"Shin.{side}", (sign * (0.04 + 0.5 * recovery), 1 - 1.57 * recovery,
                                  -0.13 - 0.45 * recovery))
        aim(rig, f"Foot.{side}", (sign * (0.07 + 0.5 * recovery), 0.85, -0.26 - 0.46 * recovery))


def tread_pose(rig, phase):
    rig.pose.bones["Root"].location.y = 0.07 * math.cos(phase * 2)
    rig.pose.bones["Hips"].rotation_quaternion = Quaternion((1, 0, 0), math.radians(8))
    bpy.context.view_layer.update()
    aim(rig, "Head", (0, -0.08, 1))
    for side, sign in (("L", -1), ("R", 1)):
        cycle = phase + (math.pi if sign > 0 else 0)
        stroke = math.sin(cycle)
        aim(rig, f"UpperArm.{side}", (sign * (0.83 + 0.17 * math.cos(cycle)), -0.24, -0.33))
        aim(rig, f"Forearm.{side}", (sign * (0.55 + 0.33 * stroke), -0.60 - 0.25 * stroke, -0.22))
        aim(rig, f"Thigh.{side}", (sign * (0.25 + 0.24 * stroke), -0.24, -1))
        aim(rig, f"Shin.{side}", (sign * (0.18 - 0.40 * stroke), 0.46 + 0.32 * stroke, -1))
        aim(rig, f"Foot.{side}", (sign * 0.16, -0.72, -0.32 - 0.33 * stroke))


def make_action(scene, rig, name, length, set_pose):
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    rig.animation_data.action = action
    for bone in rig.pose.bones:
        bone.rotation_mode = "QUATERNION"
        bone.location = (0, 0, 0)
        bone.rotation_quaternion = (1, 0, 0, 0)
        bone.scale = (1, 1, 1)
    for frame in range(1, length + 2, 3):
        scene.frame_set(frame)
        phase = 0 if frame == length + 1 else math.tau * (frame - 1) / length
        set_pose(rig, phase)
        for bone in rig.pose.bones:
            bone.keyframe_insert("rotation_quaternion", frame=frame, group=bone.name)
        rig.pose.bones["Root"].keyframe_insert("location", frame=frame, group="Root")
    for layer in action.layers:
        for strip in layer.strips:
            for channelbag in strip.channelbags:
                for curve in channelbag.fcurves:
                    for key in curve.keyframe_points:
                        key.interpolation = "LINEAR"
    return action


def main():
    blend, runtime = map(lambda name: Path(name).resolve(), sys.argv[sys.argv.index("--") + 1:])
    blend.parent.mkdir(parents=True, exist_ok=True)
    runtime.parent.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    rig = bpy.data.objects["Character_Rig"]
    rig.hide_set(False)
    idle = rig.animation_data.action
    rig.animation_data.action = None
    for bone in rig.pose.bones:
        bone.matrix_basis.identity()
    bpy.context.view_layer.update()
    for name in ("camo.png", "name.png"):
        image = bpy.data.images[name]
        (runtime.parent / name).write_bytes(bytes(image.packed_file.data))
    add_weave(runtime.parent)
    add_sole_tread(rig)
    swim = make_action(scene, rig, "swim_breaststroke", 84, swim_pose)
    tread = make_action(scene, rig, "swim_tread", 66, tread_pose)
    rig.animation_data.action = idle
    scene.frame_set(1)
    bpy.ops.object.select_all(action="DESELECT")
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=str(blend), compress=True)

    runtime_actions = {idle, bpy.data.actions["walk"], swim, tread}
    for action in list(bpy.data.actions):
        if action not in runtime_actions:
            bpy.data.actions.remove(action)
    for child in rig.children:
        if child.type == "MESH":
            child.select_set(True)
    bpy.ops.export_scene.fbx(filepath=str(runtime), use_selection=True,
                             object_types={"ARMATURE", "MESH"}, add_leaf_bones=False,
                             axis_forward="-Z", axis_up="Y", path_mode="COPY", embed_textures=True,
                             bake_anim_use_nla_strips=False, bake_anim_use_all_actions=True,
                             bake_anim_step=2.0, bake_anim_simplify_factor=0.1)


main()
