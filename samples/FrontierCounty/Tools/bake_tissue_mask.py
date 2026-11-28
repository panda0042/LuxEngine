#!/usr/bin/env python3
"""Author a soft skin/cloth mask for tevy's single-atlas characters using sampled color swatches."""

import argparse
from pathlib import Path
import numpy as np
from PIL import Image

# UV-space samples on each source atlas' exposed skin. Coordinates refer to a 300 x 300 preview.
SKIN = {
    "dog-walker": [(68, 269), (102, 245), (36, 256), (122, 142)],
    "hiker": [(84, 161), (105, 156), (60, 164), (132, 154), (10, 210)],
    "road-worker": [(73, 265), (103, 245), (119, 246), (47, 271)],
}
CLOTH = {
    "dog-walker": [(81, 226), (173, 194), (229, 174), (189, 86), (172, 270)],
    "hiker": [(242, 88), (49, 208), (94, 105), (178, 246), (135, 27)],
    "road-worker": [(76, 118), (87, 78), (119, 102), (200, 110), (172, 169)],
}


def bake(folder):
    name = folder.name
    color = Image.open(folder / "albedo.png").convert("RGB")
    preview = color.resize((300, 300), Image.Resampling.BOX)
    pixels = np.asarray(color).astype(np.float32)

    def distance(swatches):
        return np.minimum.reduce(
            [
                np.sum((pixels - np.array(preview.getpixel(uv))) ** 2, axis=2)
                for uv in swatches
            ]
        )

    skin, cloth = distance(SKIN[name]), distance(CLOTH[name])
    affinity = np.clip((cloth - skin) / np.maximum(cloth + skin, 1) * 4, 0, 1)
    # Bound classification to artist-selected skin swatches, including their light/shadow variation.
    affinity *= np.clip((64 - np.sqrt(skin)) / 30, 0, 1)
    mask = np.empty((*skin.shape, 3), dtype=np.uint8)
    mask[:, :, 0] = np.rint(
        255 * (0.30 + 0.70 * affinity)
    )  # relative optical thickness
    mask[:, :, 1] = np.rint(255 * affinity)
    mask[:, :, 2] = 255 - mask[:, :, 1]
    path = folder / "tissue.png"
    Image.fromarray(mask).save(path, optimize=True)
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("character", type=Path)
    bake(parser.parse_args().character)
