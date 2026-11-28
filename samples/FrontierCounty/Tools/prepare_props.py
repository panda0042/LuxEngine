#!/usr/bin/env python3
"""Convert catalog-verified tevy props and their glTF materials to FBX and PBR maps."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from urllib.request import urlopen

from prepare_tevy import SAMPLE_ASSETS, manifest, sha

NAMES = ("corner-shop-block", "village-shop", "roadside-diner", "workshop-building",
         "village-house", "abandoned-fuel-station", "boathouse", "hay-barn", "pickup-truck")


VEGETATION = ("norway-spruce", "grass-clump", "sedge-tuft")


def export(decoded, target):
    target.mkdir(parents=True, exist_ok=True)
    data = json.loads(decoded.read_text())
    model = target / "model.fbx"
    if target.name == "pickup-truck":
        subprocess.run(["blender", "--background", "--factory-startup", "--python-exit-code", "7",
                        "--python", Path(__file__).with_name("export_static_fbx.py"), "--", decoded,
                        model.resolve()], check=True)
    else:
        subprocess.run(["assimp", "export", decoded, model, "-ffbx"], check=True, stdout=subprocess.DEVNULL)
    files, lines = [model], []
    for index, material in enumerate(data["materials"]):
        pbr = material.get("pbrMetallicRoughness", {})
        paths = []
        for prop, kind in ((pbr.get("baseColorTexture"), "albedo"),
                           (material.get("normalTexture"), "normal"),
                           (pbr.get("metallicRoughnessTexture"), "roughness")):
            if prop is None:
                paths.append("")
                continue
            image = data["images"][data["textures"][prop["index"]]["source"]]
            incoming = decoded.parent / image["uri"]
            out = target / f"{kind}-{index}.png"
            if kind == "roughness":
                subprocess.run(["oiiotool", incoming, "--ch", "G", "-o", out], check=True)
            else:
                shutil.copyfile(incoming, out)
            files.append(out)
            paths.append(out.name)
        fields = [*pbr.get("baseColorFactor", [1, 1, 1, 1]), pbr.get("metallicFactor", 1),
                  pbr.get("roughnessFactor", 1), material.get("alphaMode", "OPAQUE"),
                  material.get("alphaCutoff", .5)]
        lines.append(" ".join([*(json.dumps(v, ensure_ascii=False) for v in [material["name"], *paths]),
                               *(str(v) for v in fields)]))
    table = target / "materials.txt"
    table.write_text("\n".join(lines) + "\n")
    return files + [table]


def prepare(cache, catalog, decoded_cache=None, vegetation=False):
    records = []
    package = "FrontierVegetation" if vegetation else "FrontierProps"
    lod = 1 if vegetation else 2
    for name in VEGETATION if vegetation else NAMES:
        item = catalog[name]
        src = next(f for f in item["files"] if f["lod"] == lod)
        version = 1 if vegetation or name == "pickup-truck" else 2
        if item["license"] != "CC0-1.0" or item["version"] != version or src["path"] != f"/models/{name}/v{version}/lod{lod}.glb":
            raise ValueError(f"Unexpected catalog entry: {name}")
        url = "https://tevy.dev" + src["path"]
        raw = cache / (f"{name}-lod1.glb" if vegetation else f"{name}.glb")
        if not raw.is_file() or sha(raw) != src["sha256"]:
            tentative = raw.with_suffix(".part")
            try:
                with urlopen(url, timeout=120) as response, tentative.open("wb") as stream:
                    shutil.copyfileobj(response, stream)
                if tentative.stat().st_size != src["bytes"] or sha(tentative) != src["sha256"]:
                    raise ValueError(f"Prop checksum differs from the catalog: {name}")
                tentative.replace(raw)
            finally:
                tentative.unlink(missing_ok=True)
        if decoded_cache:
            decoded = decoded_cache / name / "decoded" / "asset.gltf"
        else:
            packed_dir, decoded_dir = cache / name / "expanded", cache / name / "decoded"
            packed_dir.mkdir(parents=True, exist_ok=True)
            decoded_dir.mkdir(parents=True, exist_ok=True)
            packed, decoded = packed_dir / "asset.gltf", decoded_dir / "asset.gltf"
            subprocess.run(["gltf-transform", "ktxdecompress", raw, packed], check=True)
            subprocess.run(["gltf-transform", "dequantize", packed, decoded], check=True)
        target = SAMPLE_ASSETS / package / name
        files = export(decoded, target)
        if name == "norway-spruce":
            from bake_tree_views import bake
            bake(target,target)
            files.extend(target / f"spruce-views-{suffix}.png" for suffix in ("color","normal-depth"))
        records.append((item, {**src, "url": url, "format": "GLB"}, files))
    manifest({package: records})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("--decoded-cache", type=Path)
    parser.add_argument("--vegetation", action="store_true")
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    with urlopen("https://tevy.dev/models.json", timeout=120) as response:
        catalog = {item["slug"]: item for item in json.load(response)["models"]}
    prepare(args.cache, catalog, args.decoded_cache, args.vegetation)
