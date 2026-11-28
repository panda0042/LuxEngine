#!/usr/bin/env python3
"""Build the Silverlake terrain atlases from Poly Haven's CC0 scans."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import shutil
from urllib.request import Request, urlopen

import numpy as np
from PIL import Image

AGENT = "LuxEngine-FrontierCounty/1.0 (https://github.com/panda0042/LuxEngine)"
API = "https://api.polyhaven.com"
# Dry and wet members for rock, meadow, shore and forest-floor weights.
LAYERS = ("marble_cliff_01", "lichen_rock", "withered_grass", "leafy_grass",
          "coast_sand_01", "rocky_gravel", "forest_floor", "brown_mud")
CHANNELS = ("Diffuse", "nor_gl", "Rough", "Displacement")


def request(url):
    return urlopen(Request(url, headers={"User-Agent": AGENT}), timeout=120)


def digest(path, kind):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, kind).hexdigest()


def cache_source(cache, entry):
    target = cache / Path(entry["url"]).name
    if not target.exists() or digest(target, "md5") != entry["md5"]:
        tmp = target.with_suffix(".part")
        with request(entry["url"]) as stream, tmp.open("wb") as out:
            shutil.copyfileobj(stream, out)
        if tmp.stat().st_size != entry["size"] or digest(tmp, "md5") != entry["md5"]:
            raise ValueError(f"Invalid download for {target.name}")
        tmp.replace(target)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--tile", type=int, default=1536, choices=(512, 1024, 1536, 2048, 4096))
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    args.destination.mkdir(parents=True, exist_ok=True)
    with request(API + "/assets?t=textures") as response:
        catalog = json.load(response)
    tile = args.tile
    border = tile // 32
    pitch = tile + 2 * border
    albedo = Image.new("RGB", (4*pitch, 2*pitch))
    packed = Image.new("RGBA", albedo.size)
    layers = []
    for index, asset in enumerate(LAYERS):
        info = catalog[asset]
        with request(API + "/files/" + asset) as response:
            files = json.load(response)
        maps = {channel: next(files[channel][size]["jpg"] for size in ("8k","4k")
                              if "jpg" in files[channel].get(size, {})) for channel in CHANNELS}
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            sources = list(executor.map(lambda channel: cache_source(args.cache, maps[channel]), CHANNELS))
        metadata = []
        pixels = []
        for channel, path in zip(CHANNELS, sources):
            with Image.open(path) as im:
                metadata.append(dict(channel=channel, width=im.width, height=im.height,
                                     format=im.format, bytes=path.stat().st_size,
                                     sha256=digest(path,"sha256"), url=maps[channel]["url"]))
                mode = "RGB" if channel in ("Diffuse","nor_gl") else "L"
                converted = im.convert(mode)
                if channel == "Diffuse":
                    # Filter color in linear space. Process channels separately to bound peak RAM.
                    linear = np.where(np.arange(256)/255 <= .04045, np.arange(256)/3294.6,
                                      ((np.arange(256)/255+.055)/1.055)**2.4).astype("float32")
                    rgb = []
                    for band in converted.split():
                        values = Image.fromarray(linear[np.asarray(band)]).resize((tile,tile),Image.Resampling.BOX)
                        x = np.asarray(values)
                        rgb.append(np.rint(255*np.where(x<=.0031308,12.92*x,1.055*x**(1/2.4)-.055)).astype("uint8"))
                    pixels.append(np.stack(rgb,axis=-1))
                else:
                    pixels.append(np.asarray(converted.resize((tile,tile),Image.Resampling.BOX)))
        n = pixels[1].astype("float32") / 127.5 - 1
        n /= np.maximum(np.linalg.norm(n,axis=-1,keepdims=True),1e-6)
        xy = np.rint(127.5*(n[:,:,:2]+1)).astype("uint8")
        surface = np.dstack((xy, pixels[2],pixels[3]))
        offset = ((index % 4)*pitch,(index // 4)*pitch)
        for atlas, data in ((albedo,pixels[0]),(packed,surface)):
            atlas.paste(Image.fromarray(np.pad(data, ((border,border),(border,border),(0,0)),mode="wrap")), offset)
        layers.append(dict(asset=asset,name=info["name"],authors=info["authors"],license="CC0-1.0",
                           source="https://polyhaven.com/a/"+asset, files=metadata))
        print(asset,flush=True)
    albedo.save(args.destination/"albedo.jpg",quality=93,subsampling=0)
    packed.save(args.destination/"surface.png",compress_level=5)
    files = [dict(path=p.name,sha256=digest(p,"sha256"),bytes=p.stat().st_size,
                  width=albedo.width,height=albedo.height,runtimePath="assets/FrontierTerrain/"+p.name)
             for p in (args.destination/"albedo.jpg", args.destination/"surface.png")]
    notice = "Terrain scans from Poly Haven, released under CC0 1.0.\nhttps://polyhaven.com/license\n\n"
    notice += "".join(l["name"]+" — "+", ".join(l["authors"])+" — "+l["source"]+"\n" for l in layers)
    license_path = args.destination / "License.txt"
    license_path.write_text(notice)
    files.append(dict(path=license_path.name, bytes=license_path.stat().st_size, sha256=digest(license_path,"sha256")))
    (args.destination/"source-manifest.json").write_text(json.dumps(
        dict(name="FrontierTerrain",license="CC0-1.0",tile=tile,border=border,layers=layers,files=files),indent=2)+"\n")


if __name__ == "__main__":
    main()
