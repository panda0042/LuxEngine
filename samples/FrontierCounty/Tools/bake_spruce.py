#!/usr/bin/env python3
"""Bake a depth-tested foliage card from the decoded tevy Norway Spruce glTF."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def bake(source, output):
    gltf = json.loads(source.read_text())
    buffers = [(source.parent / item["uri"]).read_bytes() for item in gltf["buffers"]]

    def read(index):
        acc = gltf["accessors"][index]
        view = gltf["bufferViews"][acc["bufferView"]]
        dtype = np.dtype({5121: "u1", 5123: "<u2", 5125: "<u4", 5126: "<f4"}[acc["componentType"]])
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        stride = view.get("byteStride", width * dtype.itemsize)
        array = np.ndarray((acc["count"], width), dtype, buffers[view["buffer"]], offset,
                           strides=(stride, dtype.itemsize))
        if acc.get("normalized"):
            return array.astype(np.float32) / np.iinfo(dtype).max
        return array

    width, height = 432, 768
    angle = np.radians(26)
    right = np.array([np.cos(angle), 0, -np.sin(angle)])
    front = np.array([np.sin(angle), 0, np.cos(angle)])
    parts = []
    for node in gltf["nodes"]:
        for primitive in gltf["meshes"][node["mesh"]]["primitives"]:
            attrs = primitive["attributes"]
            pos = read(attrs["POSITION"]) * np.asarray(node.get("scale", [1, 1, 1]))
            pos += np.asarray(node.get("translation", [0, 0, 0]))
            plane = np.column_stack((pos @ right, -pos[:, 1], pos @ front))
            uv = read(attrs["TEXCOORD_0"])
            material = gltf["materials"][primitive["material"]]
            texture = material["pbrMetallicRoughness"]["baseColorTexture"]["index"]
            image = gltf["images"][gltf["textures"][texture]["source"]]
            texels = np.array(Image.open(source.parent / image["uri"]).convert("RGBA"))
            parts.append((plane, uv, read(primitive["indices"]).reshape(-1, 3),
                          texels, material.get("alphaCutoff", .0)))

    planes = np.concatenate([part[0] for part in parts])
    lo = planes.min(axis=0)
    hi = planes.max(axis=0)
    pixels_per_meter = min((width - 10) / (hi[0] - lo[0]), (height - 10) / (hi[1] - lo[1]))
    origin = (lo[:2] + hi[:2]) * .5
    depth = np.full((height, width), -np.inf)
    rgba = np.zeros((height, width, 4), dtype=np.uint8)
    for plane, uv, indices, texture, cutoff in parts:
        plane[:, :2] = (plane[:, :2] - origin) * pixels_per_meter + np.array([width, height]) * .5
        for tri in indices:
            a, b, c = plane[tri]
            ab, ac = b[:2] - a[:2], c[:2] - a[:2]
            signed_area = ab[0] * ac[1] - ab[1] * ac[0]
            if abs(signed_area) < 1e-5:
                continue
            bounds = plane[tri, :2]
            x0, y0 = np.maximum(np.floor(bounds.min(axis=0)).astype(int), 0)
            x1, y1 = np.minimum(np.ceil(bounds.max(axis=0)).astype(int), [width - 1, height - 1])
            y, x = np.mgrid[y0:y1 + 1, x0:x1 + 1]
            dx, dy = x + .5 - a[0], y + .5 - a[1]
            u = (dx * ac[1] - dy * ac[0]) / signed_area
            v = (ab[0] * dy - ab[1] * dx) / signed_area
            w = 1 - u - v
            z = w * a[2] + u * b[2] + v * c[2]
            take = (w >= 0) & (u >= 0) & (v >= 0) & (z >= depth[y, x])
            if not take.any():
                continue
            t = w[take, None] * uv[tri[0]] + u[take, None] * uv[tri[1]] + v[take, None] * uv[tri[2]]
            th, tw = texture.shape[:2]
            tx = (t[:, 0] % 1 * tw).astype(int)
            ty = (t[:, 1] % 1 * th).astype(int)
            color = texture[ty, tx]
            opaque = color[:, 3] >= max(1, round(cutoff * 255))
            yy, xx = y[take][opaque], x[take][opaque]
            rgba[yy, xx, :3] = color[opaque, :3]
            rgba[yy, xx, 3] = 255
            depth[yy, xx] = z[take][opaque]

    # Fill transparent RGB from opaque neighbours to avoid dark rims in coarse mips.
    known = rgba[:, :, 3] > 0
    for _ in range(8):
        for dy, dx in ((0, 1), (1, 0), (0, -1), (-1, 0)):
            neighbour = np.roll(known, (dy, dx), axis=(0, 1))
            target = neighbour & ~known
            rgba[:, :, :3][target] = np.roll(rgba[:, :, :3], (dy, dx), axis=(0, 1))[target]
            known |= target
    Image.fromarray(rgba).save(output, optimize=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    bake(args.source, args.output)
