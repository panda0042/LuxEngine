import argparse
from pathlib import Path

import cv2
import numpy as np


def noise(x, z, seed):
    ix = np.floor(x).astype(np.int32)
    iz = np.floor(z).astype(np.int32)
    u, v = x - ix, z - iz
    u, v = u * u * (3 - 2 * u), v * v * (3 - 2 * v)

    def corner(dx, dz):
        bits = ((ix + dx).astype(np.uint32) * np.uint32(0x9E3779B1) ^
                (iz + dz).astype(np.uint32) * np.uint32(0x85EBCA77) ^ np.uint32(seed))
        bits = (bits ^ (bits >> 15)) * np.uint32(0xC2B2AE3D)
        return ((bits ^ (bits >> 16)) >> 8) / 16777216

    return ((1 - u) * corner(0, 0) + u * corner(1, 0)) * (1 - v) + (
        (1 - u) * corner(0, 1) + u * corner(1, 1)) * v


def clouds(x, z):
    warp_x = noise(x * 0.7, z * 0.7, 91) - 0.5
    warp_z = noise(x * 0.7, z * 0.7, 73) - 0.5
    x, z = x + warp_x * 0.7, z + warp_z * 0.7
    return sum(weight * noise(x * scale, z * scale, 3217 + octave * 2053)
               for octave, (weight, scale) in enumerate(
                   ((0.50, 1), (0.25, 2.07), (0.13, 4.19), (0.07, 8.39), (0.05, 16.79))))


def bake(source, output):
    clear = cv2.imread(str(source), cv2.IMREAD_UNCHANGED)
    if clear is None or clear.shape != (512, 1024, 3):
        raise ValueError("Expected the 1024 × 512 linear lake panorama")
    azimuth = ((np.arange(1024) + 0.5) / 1024 - 0.5) * 2 * np.pi
    polar = ((np.arange(512) + 0.5) / 512 * np.pi)[:, None]
    elevation = np.cos(polar)
    projection = np.sin(polar) / np.maximum(elevation, 0.07)
    x = 3.1 * np.cos(azimuth) * projection + 1.35
    z = 3.1 * np.sin(azimuth) * projection - 2.53
    density = np.maximum(clouds(x, z) - 0.53, 0.)
    toward_sun = np.maximum(clouds(x + 0.12, z - 0.17) - 0.53, 0.)
    cover = (1 - np.exp(-13 * density)) * np.clip((elevation - 0.075) / 0.24, 0, 1)
    shade = 0.37 + 0.25 * np.exp(-11 * density) + 0.44 * np.exp(-32 * toward_sun)
    shoulder = np.clip((density - toward_sun) * 9, 0, 0.35)
    luminance = (shade + shoulder) * (1.24 + 0.4 * np.clip(elevation, 0, 1))
    silver = luminance[..., None] * np.array([1.08, 1.04, 1.0])
    result = clear * (1 - cover[..., None]) + silver * cover[..., None]
    if not cv2.imwrite(str(output), result.astype(np.float32)):
        raise OSError(f"Could not write {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    bake(args.source, args.output)
