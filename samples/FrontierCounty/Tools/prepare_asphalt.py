#!/usr/bin/env python3
"""Prepare a verified 8K Poly Haven surface for road or terrain PBR."""
import argparse
import json
from pathlib import Path
import subprocess

from prepare_gravel import RUNTIME, MAPS, checksum, image_size, metadata, original


SURFACES = {"asphalt_01": ("asphalt", "FrontierAsphalt"),
            "leafy_grass": ("terrain-grass", "FrontierMeadow")}


def prepare(cache, cached, asset):
    prefix, package = SURFACES[asset]
    files = metadata(cached, cache / f"{prefix}-files.json", f"/files/{asset}")
    details = metadata(cached, cache / f"{prefix}-info.json", f"/info/{asset}")
    root = RUNTIME.parent / package
    root.mkdir(parents=True, exist_ok=True)
    source = f"https://polyhaven.com/a/{asset}"
    license_file = root / "License.txt"
    authors = ", ".join(details["authors"])
    license_file.write_text(f"{details['name']} by {authors}, Poly Haven.\n"
                            "Released under CC0 1.0 Universal.\n"
                            "https://polyhaven.com/license\n")
    gathered = [{"path": license_file.name, "bytes": license_file.stat().st_size,
                 "sha256": checksum(license_file, "sha256")}]
    for key, filename, color, before, after in MAPS:
        if key == "Displacement" and asset != "leafy_grass":
            continue
        entry = files[key]["8k"]["jpg"]
        raw = cache / f"{prefix}-{key}.jpg"
        original(raw, entry)
        dimensions = image_size(raw)
        if dimensions != [8192, 8192]:
            raise ValueError(f"Source has unexpected dimensions: {dimensions}")
        out = root / filename.removeprefix("gravel_")
        subprocess.run(["oiiotool", "--threads", "4", raw, *before, "--resize", "1024x1024",
                        *after, "-d", "uint8", "-o", out], check=True)
        gathered.append({"path": out.name, "bytes": out.stat().st_size, "sha256": checksum(out, "sha256"),
                         "width": 1024, "height": 1024, "colorSpace": color,
                         "original": {"url": entry["url"], "width": dimensions[0], "height": dimensions[1],
                                      "format": "JPEG", "bytes": entry["size"], "md5": entry["md5"],
                                      "sha256": checksum(raw, "sha256")}})
    data = {"name": package, "title": details["name"], "source": source,
            "authors": details["authors"], "license": "CC0-1.0", "files": gathered}
    (root / "source-manifest.json").write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("--cached-metadata", action="store_true")
    parser.add_argument("--asset", choices=SURFACES, default="asphalt_01")
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    prepare(args.cache, args.cached_metadata, args.asset)
