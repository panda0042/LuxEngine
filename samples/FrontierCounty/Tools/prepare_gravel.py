#!/usr/bin/env python3
"""Verify/cache Poly Haven originals and prepare Frontier County's gravel maps."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

AGENT = "LuxEngine-FrontierCounty/0.1 (https://github.com/potatosl/LuxEngine)"
API = "https://api.polyhaven.com"
SAMPLE = Path(__file__).resolve().parents[1]
RUNTIME = SAMPLE.parent / "Assets" / "FrontierCounty"
MAPS = (
    ("Diffuse", "gravel_albedo.jpg", "sRGB", ["--colorconvert", "sRGB", "linear"], ["--colorconvert", "linear", "sRGB", "--quality", "91"]),
    ("nor_gl", "gravel_normal.png", "Linear tangent-space OpenGL", [], []),
    ("Displacement", "gravel_height.png", "Linear", [], ["--ch", "0"]),
    ("Rough", "gravel_roughness.png", "Linear", [], ["--ch", "0"]),
)


def request(url):
    return urlopen(Request(url, headers={"User-Agent": AGENT}), timeout=120)


def checksum(path, algorithm):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def original(path, record):
    url = record["url"]
    if urlsplit(url).scheme != "https" or urlsplit(url).netloc != "dl.polyhaven.org":
        raise ValueError(f"Unexpected asset CDN: {url}")

    def verified(candidate):
        return (candidate.is_file() and candidate.stat().st_size == record["size"]
                and checksum(candidate, "md5") == record["md5"])

    if verified(path):
        return
    temporary = path.with_suffix(".part")
    try:
        with request(url) as source, temporary.open("wb") as destination:
            shutil.copyfileobj(source, destination)
        if not verified(temporary):
            raise ValueError(f"Original does not match Poly Haven's checksum: {url}")
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def image_size(path):
    info = subprocess.check_output(["oiiotool", "--info", str(path)], text=True)
    match = re.search(r":\s+(\d+)\s*x\s*(\d+),", info)
    if not match:
        raise ValueError(f"Cannot determine image dimensions: {path}")
    return [int(value) for value in match.groups()]


def metadata(cache, path, endpoint):
    if cache and path.exists():
        return json.loads(path.read_text())
    with request(API + endpoint) as response:
        data = json.load(response)
    path.write_text(json.dumps(data, indent=2) + "\n")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__ + " Powered by Poly Haven.")
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--cached-metadata", action="store_true")
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    files = metadata(args.cached_metadata, args.cache / "rocky_gravel.json", "/files/rocky_gravel")
    details = metadata(args.cached_metadata, args.cache / "rocky_gravel_info.json", "/info/rocky_gravel")
    RUNTIME.mkdir(parents=True, exist_ok=True)
    outputs = []
    for key, filename, space, before, after in MAPS:
        resolutions = files[key]
        # Select from the live catalog. Entries other than JPEG are left untouched.
        tier = next(t for t in ("24k", "16k", "12k", "8k", "4k") if "jpg" in resolutions.get(t, {}))
        item = resolutions[tier]["jpg"]
        raw = args.cache / f"{key}.jpg"
        original(raw, item)
        dimensions = image_size(raw)
        if dimensions[0] != int(tier[:-1]) * 1024 or dimensions[1] != dimensions[0]:
            raise ValueError(f"Unexpected {tier} source dimensions for {key}: {dimensions}")
        asset = RUNTIME / filename
        subprocess.run(["oiiotool", "--threads", "4", str(raw), *before, "--resize", "1024x1024",
                        *after, "-d", "uint8", "-o", str(asset)], check=True)
        outputs.append({
            "map": key,
            "original": {"url": item["url"], "width": dimensions[0], "height": dimensions[1],
                         "format": "JPEG", "bytes": item["size"], "md5": item["md5"],
                         "sha256": checksum(raw, "sha256")},
            "processed": {"path": "samples/Assets/FrontierCounty/" + filename,
                          "width": 1024, "height": 1024, "bytes": asset.stat().st_size,
                          "sha256": checksum(asset, "sha256"), "colorSpace": space},
            "runtimePath": "assets/FrontierCounty/" + filename,
        })
    groups = [{
        "name": details["name"], "url": "https://polyhaven.com/a/rocky_gravel",
        "license": "CC0-1.0", "authors": details["authors"], "files": outputs,
    }]
    package = {"name": "FrontierCounty", "source": groups[0]["url"],
               "license": groups[0]["license"], "credits": groups[0]["authors"], "files": []}
    for filename in ("License.txt", *(item[1] for item in MAPS)):
        asset = RUNTIME / filename
        package["files"].append({"path": filename, "bytes": asset.stat().st_size,
                                 "sha256": checksum(asset, "sha256")})
    (RUNTIME / "source-manifest.json").write_text(json.dumps(package, indent=2) + "\n")

    used_files = {"LakesideSunrise": "lakeside_sunrise_water.hdr",
                  "DungGeunMo": "DungGeunMo.ttf"}
    for group, used in used_files.items():
        record = json.loads((RUNTIME.parent / group / "source-manifest.json").read_text())
        record["files"] = [entry | {"runtimePath": f"assets/{group}/{used}"} for entry in record["files"]
                           if entry["path"] == used]
        if not record["files"]:
            raise ValueError(f"Could not find {used} in the pinned manifest")
        asset = RUNTIME.parent / group / used
        entry = record["files"][0]
        if not entry.get("sha256") == checksum(asset, "sha256"):
            raise ValueError(f"Existing asset hash differs from the package: {used}")
        entry["processedPath"] = f"samples/Assets/{group}/{used}"
        if group == "LakesideSunrise":
            entry["width"], entry["height"] = image_size(asset)
            entry["format"] = "Radiance RGBE"
        else:
            entry["format"] = "TrueType"
        groups.append(record)
    for name in ("FrontierPeople", "FrontierFoliage", "FrontierProps", "FrontierAsphalt", "FrontierVegetation", "FrontierMeadow"):
        package = json.loads((RUNTIME.parent / name / "source-manifest.json").read_text())
        for entry in package["files"]:
            asset = RUNTIME.parent / name / entry["path"]
            if checksum(asset, "sha256") != entry["sha256"]:
                raise ValueError(f"Runtime asset hash differs from the package: {asset}")
            entry["runtimePath"] = f"assets/{name}/{entry['path']}"
            entry["processedPath"] = f"samples/Assets/{name}/{entry['path']}"
        groups.append(package)
    (SAMPLE / "ASSET_MANIFEST.json").write_text(json.dumps({"assets": groups}, indent=2) + "\n")


if __name__ == "__main__":
    main()
