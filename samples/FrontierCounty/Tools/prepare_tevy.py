#!/usr/bin/env python3
"""Convert pinned CC0 tevy models to Frontier County runtime assets."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from urllib.request import urlopen

from bake_spruce import bake

SAMPLE_ASSETS = Path(__file__).resolve().parents[2] / "Assets"
ASSETS = {"dog-walker": 1, "hiker": 1, "road-worker": 1, "norway-spruce": 2}


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(cache, catalog):
    packages = {"FrontierPeople": [], "FrontierFoliage": []}
    for name, lod in ASSETS.items():
        item = catalog[name]
        if item["license"] != "CC0-1.0" or item["version"] != 1:
            raise ValueError(f"Expected CC0 version 1 for {name}")
        original = next(f for f in item["files"] if f["lod"] == lod)
        url = f"https://tevy.dev/models/{name}/v1/lod{lod}.glb"
        if original["path"] != f"/models/{name}/v1/lod{lod}.glb":
            raise ValueError(f"Unexpected catalog download for {name}")
        raw = cache / f"{name}.glb"
        if not raw.is_file() or sha(raw) != original["sha256"]:
            download = raw.with_suffix(".part")
            try:
                with urlopen(url, timeout=120) as response, download.open("wb") as stream:
                    shutil.copyfileobj(response, stream)
                if download.stat().st_size != original["bytes"] or sha(download) != original["sha256"]:
                    raise ValueError(f"Checksum or size mismatch for {name}")
                download.replace(raw)
            finally:
                download.unlink(missing_ok=True)
        stage = cache / name
        packed, decoded = stage / "packed", stage / "decoded"
        for path in (packed, decoded):
            path.mkdir(parents=True, exist_ok=True)
        packed_model, decoded_model = packed / "model.gltf", decoded / "model.gltf"
        subprocess.run(["gltf-transform", "ktxdecompress", raw, packed_model], check=True)
        subprocess.run(["gltf-transform", "dequantize", packed_model, decoded_model], check=True)
        package = "FrontierFoliage" if lod == 2 else "FrontierPeople"
        dst = SAMPLE_ASSETS / package
        dst.mkdir(parents=True, exist_ok=True)
        runtime = []
        if package == "FrontierFoliage":
            path = dst / "spruce.png"
            bake(decoded_model, path)
            runtime.append(path)
        else:
            target = dst / name
            target.mkdir(parents=True, exist_ok=True)
            data = json.loads(decoded_model.read_text())
            data["animations"] = [a for a in data["animations"] if a["name"] in ("idle", "walk")]
            filtered = decoded / "runtime.gltf"
            filtered.write_text(json.dumps(data))
            command = ["blender", "--background", "--factory-startup", "--python",
                       Path(__file__).with_name("export_human_fbx.py"), "--", filtered, target / "model.fbx"]
            if name == "dog-walker":
                command.append(target / "motion.fbx")
                runtime.append(target / "motion.fbx")
            subprocess.run(command, check=True)
            runtime.append(target / "model.fbx")
            for incoming, outgoing in (("baseColor_1.png", "albedo.png"), ("normal_1.png", "normal.png")):
                shutil.copyfile(decoded / incoming, target / outgoing)
                runtime.append(target / outgoing)
            subprocess.run(["oiiotool", decoded / "metallicRoughness_1.png", "--ch", "G",
                            "-o", target / "roughness.png"], check=True)
            runtime.append(target / "roughness.png")
        packages[package].append((item, {**original, "url": url, "format": "GLB"}, runtime))
    return packages


def manifest(packages):
    from PIL import Image
    for package, records in packages.items():
        root = SAMPLE_ASSETS / package
        license_file = root / "License.txt"
        license_file.write_text("Models by tevy, licensed CC0 1.0 Universal.\n"
                                "https://creativecommons.org/publicdomain/zero/1.0/\n")
        files = [{"path": "License.txt", "bytes": license_file.stat().st_size, "sha256": sha(license_file)}]
        sources = []
        for item, original, paths in records:
            sources.append({"name": item["name"], "source": "https://tevy.dev/models/" + item["slug"],
                            "original": original, "files": [p.relative_to(root).as_posix() for p in paths]})
            for p in paths:
                entry = {"path": p.relative_to(root).as_posix(), "bytes": p.stat().st_size,
                         "sha256": sha(p), "format": p.suffix[1:].upper()}
                if p.suffix == ".png":
                    with Image.open(p) as image:
                        entry["width"], entry["height"] = image.size
                    entry["colorSpace"] = "sRGB" if p.stem.startswith(("albedo", "spruce")) else "Linear"
                files.append(entry)
        data = {"name": package, "author": "tevy", "source": "https://tevy.dev/models",
                "license": "CC0-1.0", "sources": sources, "files": files}
        (root / "source-manifest.json").write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, type=Path)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    with urlopen("https://tevy.dev/models.json", timeout=120) as response:
        catalog = {item["slug"]: item for item in json.load(response)["models"]}
    manifest(prepare(args.cache, catalog))
