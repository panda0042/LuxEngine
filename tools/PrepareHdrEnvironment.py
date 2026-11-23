#!/usr/bin/env python3
"""Convert a linear OpenEXR panorama to a Radiance HDR file usable by LuxEngine."""

import argparse
import os
from pathlib import Path
import tempfile

MAX_SOURCE_WIDTH = 24_576
MAX_OUTPUT_WIDTH = 8_192
MAX_ENCODED_BYTES = 128 << 20
MAX_RADIANCE = 65_504.0


def read_rows(image, spec, first, last, channels, oiio):
    ybegin, yend = spec.y + first, spec.y + last
    channel_end = max(channels) + 1
    if not spec.tile_width:
        rows = image.read_scanlines(ybegin, yend, spec.z, 0, channel_end, oiio.FLOAT)
        if rows is None:
            raise ValueError(image.geterror())
        return rows

    tile_first = (first // spec.tile_height) * spec.tile_height
    tile_end = min(
        spec.height, ((last + spec.tile_height - 1) // spec.tile_height) * spec.tile_height
    )
    rows = image.read_tiles(
        spec.x, spec.x + spec.width, spec.y + tile_first, spec.y + tile_end,
        spec.z, spec.z + 1, 0, channel_end, oiio.FLOAT
    )
    if rows is None:
        raise ValueError(image.geterror())
    return rows[first - tile_first:last - tile_first]


def convert(source, destination, width, exposure, np, oiio):
    if source.suffix.lower() != ".exr" or destination.suffix.lower() != ".hdr":
        raise ValueError("Expected an OpenEXR input and a Radiance .hdr destination")
    if destination.exists():
        raise ValueError(f"Destination already exists: {destination}")

    image = oiio.ImageInput.open(str(source))
    if not image:
        raise ValueError(oiio.geterror())
    temporary = None
    writer = None
    try:
        spec = image.spec()
        if width is None:
            width = min(spec.width, MAX_OUTPUT_WIDTH)
        if (
            image.format_name() != "openexr"
            or spec.deep
            or spec.depth != 1
            or not 2 <= spec.width <= MAX_SOURCE_WIDTH
            or spec.width != spec.height * 2
            or not 2 <= width <= min(spec.width, MAX_OUTPUT_WIDTH)
            or width % 2
        ):
            raise ValueError("Expected a flat RGB 2:1 panorama up to 24576 pixels wide")
        names = list(spec.channelnames)
        if not all(channel in names for channel in "RGB"):
            raise ValueError("The EXR has no R, G and B channels")
        channels = [names.index(channel) for channel in "RGB"]
        if max(channels) >= 4:
            raise ValueError("Expected RGB or RGBA in the first four channels")
        if spec.tile_width and spec.tile_height > 256:
            raise ValueError("EXR tile height exceeds the bounded import size of 256 rows")

        height = width // 2
        x_edges = np.arange(width + 1, dtype=np.int64) * spec.width // width
        y_edges = np.arange(height + 1, dtype=np.int64) * spec.height // height
        x_counts = np.diff(x_edges)
        # Keep source batches near 64 rows even for large panoramas.
        batch_rows = max(1, 64 * height // spec.height)
        gain = 2.0 ** exposure

        with tempfile.NamedTemporaryFile(suffix=".hdr", dir=destination.parent, delete=False) as tmp:
            temporary = Path(tmp.name)
        writer = oiio.ImageOutput.create(str(temporary))
        if not writer or not writer.open(str(temporary), oiio.ImageSpec(width, height, 3, oiio.FLOAT)):
            raise ValueError(oiio.geterror())
        for first in range(0, height, batch_rows):
            last = min(first + batch_rows, height)
            source_first, source_last = int(y_edges[first]), int(y_edges[last])
            rows = read_rows(image, spec, source_first, source_last, channels, oiio)
            if not np.isfinite(rows).all():
                raise ValueError(f"Nonfinite source sample near row {source_first}")
            # Area average in linear light with integer source bounds. Exact box filtering for
            # integer reductions, including Poly Haven's 24K to 8K and 4K conversions.
            horizontal = np.add.reduceat(rows[..., channels], x_edges[:-1], axis=1, dtype=np.float64)
            vertical_start = y_edges[first:last] - source_first
            pixels = np.add.reduceat(horizontal, vertical_start, axis=0)
            pixels *= gain
            pixels /= np.diff(y_edges[first:last + 1])[:, None, None] * x_counts[None, :, None]
            np.maximum(pixels, 0, out=pixels)
            if pixels.max() > MAX_RADIANCE:
                raise ValueError("Radiance exceeds the engine range; decrease --exposure")
            if not writer.write_scanlines(first, last, 0, pixels.astype(np.float32)):
                raise ValueError(writer.geterror())
        if not writer.close():
            raise ValueError(writer.geterror())
        writer = None
        if temporary.stat().st_size > MAX_ENCODED_BYTES:
            raise ValueError("Encoded HDR exceeds 128 MiB; select a smaller output width")
        os.replace(temporary, destination)
        temporary = None
        return width
    finally:
        if writer:
            writer.close()
        if temporary:
            temporary.unlink(missing_ok=True)
        image.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--width", type=int,
                        help="Even output width, defaulting to the source width capped at 8192")
    parser.add_argument("--exposure", type=float, default=0,
                        help="Linear exposure adjustment in stops, default 0")
    args = parser.parse_args()
    if not -32 <= args.exposure <= 32:
        parser.error("--exposure must be finite and between -32 and 32")
    try:
        import numpy as np
        import OpenImageIO as oiio
    except ImportError:
        parser.exit(1, "Install the Python OpenImageIO and numpy packages to import EXR.\n")
    try:
        width = convert(args.source, args.destination, args.width, args.exposure, np, oiio)
    except (OSError, RuntimeError, ValueError) as error:
        parser.exit(1, f"HDR import failed: {error}\n")
    print(f"{args.destination} ({width}×{width // 2})")


if __name__ == "__main__":
    main()
