#!/usr/bin/env python3
"""Bake the licensed near spruce into three lit impostor views with matching tangent normals and depth."""

import argparse
import ctypes
import ctypes.util
import json
from pathlib import Path
import shlex
import tempfile

import numpy as np
from PIL import Image


def flatten_fbx(source, folder):
    assimp = ctypes.CDLL(ctypes.util.find_library("assimp") or "libassimp.so")
    assimp.aiImportFile.argtypes = [ctypes.c_char_p, ctypes.c_uint]
    assimp.aiImportFile.restype = ctypes.c_void_p
    assimp.aiExportScene.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_uint,
    ]
    assimp.aiReleaseImport.argtypes = [ctypes.c_void_p]
    scene = assimp.aiImportFile(
        bytes(source), 0x8 | 0x100
    )  # triangulate, pretransform vertices
    if not scene:
        raise ValueError(f"Cannot read {source}")
    path = folder / "flat.gltf"
    try:
        if assimp.aiExportScene(scene, b"gltf2", bytes(path), 0) != 0:
            raise ValueError("Assimp could not flatten spruce geometry")
    finally:
        assimp.aiReleaseImport(scene)
    return json.loads(path.read_text())


def bake(folder, destination):
    with tempfile.TemporaryDirectory() as conversion:
        temp = Path(conversion)
        scene = flatten_fbx(folder / "model.fbx", temp)
        buffers = [(temp / b["uri"]).read_bytes() for b in scene["buffers"]]
    materials = {
        row[0]: row
        for row in map(shlex.split, (folder / "materials.txt").read_text().splitlines())
    }

    def attribute(index):
        a = scene["accessors"][index]
        v = scene["bufferViews"][a["bufferView"]]
        dtype = np.dtype({5123: "<u2", 5125: "<u4", 5126: "<f4"}[a["componentType"]])
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
        return np.ndarray(
            (a["count"], n),
            dtype,
            buffers[v["buffer"]],
            offset=v.get("byteOffset", 0) + a.get("byteOffset", 0),
            strides=(v.get("byteStride", n * dtype.itemsize), dtype.itemsize),
        )

    parts = []
    for mesh in scene["meshes"]:
        for primitive in mesh["primitives"]:
            row = materials[scene["materials"][primitive["material"]]["name"]]
            attr = primitive["attributes"]
            texture = np.asarray(Image.open(folder / row[1]).convert("RGBA")) / 255
            mapped = np.asarray(Image.open(folder / row[2]).convert("RGB")) / 127.5 - 1
            indices = attribute(primitive["indices"]).reshape(-1, 3)
            parts.append(
                (
                    attribute(attr["POSITION"]),
                    attribute(attr["NORMAL"]),
                    attribute(attr["TEXCOORD_0"]),
                    indices,
                    texture,
                    mapped,
                    np.array(row[4:7], dtype=float),
                    float(row[-1]) if row[-2] == "MASK" else 0,
                )
            )
    width, height = 384, 640
    pos = np.concatenate([p[0] for p in parts])
    top, bottom = np.max(pos[:, 1]), np.min(pos[:, 1])
    center = (np.min(pos, axis=0) + np.max(pos, axis=0)) / 2
    # All views share the same dimensions, origin and depth scale.
    diameter = max(
        2 * np.linalg.norm((pos - center)[:, [0, 2]], axis=1).max(),
        (top - bottom) * 0.60,
    )
    scale = min((width - 10) / diameter, (height - 10) / (top - bottom))
    colors, surfaces = [], []
    for angle in np.radians([0, 60, 120]):
        right = np.array([np.cos(angle), 0, np.sin(angle)])
        front = np.array([-np.sin(angle), 0, np.cos(angle)])
        down = np.array([0, -1, 0])
        basis = np.column_stack((right, down, front))
        depth = np.full((height, width), -np.inf)
        color = np.zeros((height, width, 4), dtype=np.uint8)
        surface = np.zeros_like(color)
        for vertices, normals, uv, triangles, texture, mapped, tint, cutoff in parts:
            points = (vertices - center) @ basis
            points[:, :2] = points[:, :2] * scale + [width * 0.5, height * 0.5]
            for tri in triangles:
                a, b, c = points[tri]
                ab, ac = b - a, c - a
                denom = ab[0] * ac[1] - ab[1] * ac[0]
                if abs(denom) < 1e-6:
                    continue
                xy = points[tri, :2]
                x0, y0 = np.maximum(np.floor(xy.min(0)).astype(int), 0)
                x1, y1 = np.minimum(
                    np.ceil(xy.max(0)).astype(int), [width - 1, height - 1]
                )
                y, x = np.mgrid[y0 : y1 + 1, x0 : x1 + 1]
                dx, dy = x + 0.5 - a[0], y + 0.5 - a[1]
                u, v = (
                    (dx * ac[1] - dy * ac[0]) / denom,
                    (ab[0] * dy - ab[1] * dx) / denom,
                )
                w = 1 - u - v
                z = w * a[2] + u * b[2] + v * c[2]
                hit = (u >= 0) & (v >= 0) & (w >= 0) & (z >= depth[y, x])
                if not hit.any():
                    continue
                coord = (
                    w[hit, None] * uv[tri[0]]
                    + u[hit, None] * uv[tri[1]]
                    + v[hit, None] * uv[tri[2]]
                ) % 1
                texel = texture[
                    (coord[:, 1] * texture.shape[0]).astype(int),
                    (coord[:, 0] * texture.shape[1]).astype(int),
                ]
                keep = texel[:, 3] >= max(1 / 255, cutoff)
                if not keep.any():
                    continue
                yy, xx = y[hit][keep], x[hit][keep]
                rgb = texel[keep, :3]
                linear = (
                    np.where(
                        rgb <= 0.04045, rgb / 12.92, ((rgb + 0.055) / 1.055) ** 2.4
                    )
                    * tint
                )
                color[yy, xx, :3] = np.rint(
                    np.clip(
                        np.where(
                            linear <= 0.0031308,
                            linear * 12.92,
                            1.055 * linear ** (1 / 2.4) - 0.055,
                        ),
                        0,
                        1,
                    )
                    * 255
                )
                color[yy, xx, 3] = 255
                n = (
                    w[hit, None] * normals[tri[0]]
                    + u[hit, None] * normals[tri[1]]
                    + v[hit, None] * normals[tri[2]]
                )[keep]
                n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-8)
                tu, tv = uv[tri[1]] - uv[tri[0]], uv[tri[2]] - uv[tri[0]]
                det = tu[0] * tv[1] - tu[1] * tv[0]
                if abs(det) > 1e-8:
                    e1, e2 = (
                        vertices[tri[1]] - vertices[tri[0]],
                        vertices[tri[2]] - vertices[tri[0]],
                    )
                    tangent = (e1 * tv[1] - e2 * tu[1]) / det
                    tangents = tangent - n * (n @ tangent)[:, None]
                    tangents /= np.maximum(
                        np.linalg.norm(tangents, axis=1, keepdims=True), 1e-8
                    )
                    bitangents = np.cross(n, tangents)
                    bitangents *= np.sign(
                        bitangents @ ((e2 * tu[0] - e1 * tv[0]) / det)
                    )[:, None]
                    tc = coord[keep]
                    nm = mapped[
                        (tc[:, 1] * mapped.shape[0]).astype(int),
                        (tc[:, 0] * mapped.shape[1]).astype(int),
                    ]
                    n = tangents * nm[:, :1] + bitangents * nm[:, 1:2] + n * nm[:, 2:3]
                n = n @ basis
                n *= np.where(n[:, 2:] < 0, -1.0, 1.0)
                n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-8)
                surface[yy, xx, :3] = np.rint((n * 0.5 + 0.5) * 255)
                zz = z[hit][keep]
                surface[yy, xx, 3] = np.rint(np.clip(zz / diameter + 0.5, 0, 1) * 255)
                depth[yy, xx] = zz
        # Extend the RGB, normals and depth underneath transparent borders before mip generation.
        known = color[:, :, 3] != 0
        for _ in range(6):
            for dx, dy in [(1, 0), (-1, 0), (0, 1), (0, -1)]:
                fill = np.roll(known, (dy, dx), (0, 1)) & ~known
                surface[fill] = np.roll(surface, (dy, dx), (0, 1))[fill]
                color[:, :, :3][fill] = np.roll(color[:, :, :3], (dy, dx), (0, 1))[fill]
                known |= fill
        surface[~known] = [128, 128, 255, 128]
        colors.append(color)
        surfaces.append(surface)
    for files, suffix in [(colors, "color"), (surfaces, "normal-depth")]:
        Image.fromarray(np.concatenate(files, axis=1)).save(
            destination / f"spruce-views-{suffix}.png", optimize=True
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    bake(args.source, args.destination)
