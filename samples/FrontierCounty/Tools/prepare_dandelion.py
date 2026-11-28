"""Bake a masked, UV-preserving flowering dandelion from the locked Poly Haven 8K scan."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
from urllib.request import Request, urlopen

import numpy as np
from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / 'tools'))
from PrepareRainyCreekAssets import read_gltf, simplify, write_obj


def verify(path, metadata):
    with path.open('rb') as stream:
        match = hashlib.file_digest(stream, 'sha256').hexdigest() == metadata['sha256']
    if not match or path.stat().st_size != metadata['bytes']:
        raise ValueError(f'Unexpected content for {path.name}')


def bake_material(cache, dest):
    size = 1024
    with Image.open(cache / 'alpha.png') as alpha, Image.open(cache / 'diff.jpg') as color:
        # Filter the authored sRGB color in linear light, weighted by the leaf mask.
        # A small row strip bounds temporary float storage for the 8192-square source.
        scale = color.width // size
        if color.size != alpha.size or color.size != (8192, 8192):
            raise ValueError('Expected matching 8192-square diffuse and alpha')
        lookup = np.linspace(0, 1, 256, dtype=np.float32)
        lookup = np.where(lookup <= .04045, lookup / 12.92, ((lookup + .055) / 1.055) ** 2.4)
        out = np.empty((size, size, 4), dtype=np.uint8)
        for row in range(0, size, 16):
            bounds = (0, row * scale, color.width, (row + 16) * scale)
            rgb = lookup[np.asarray(color.crop(bounds).convert('RGB'))]
            a = np.asarray(alpha.crop(bounds).convert('L')).astype(np.float32) / 255
            a = a.reshape(16, scale, size, scale)
            coverage = a.mean(axis=(1, 3))
            rgb = rgb.reshape(16, scale, size, scale, 3)
            filtered = (rgb * a[..., None]).mean(axis=(1, 3))
            linear = np.where((coverage > 0)[..., None],
                              filtered / np.maximum(coverage[..., None], 1e-8), rgb.mean(axis=(1, 3)))
            srgb = np.where(linear < .0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - .055)
            out[row:row+16] = np.rint(255 * np.dstack((srgb, coverage))).clip(0, 255)
        Image.fromarray(out).save(dest / 'albedo.png')
    with Image.open(cache / 'normal.jpg') as src:
        normals = np.asarray(src.resize((size, size), Image.Resampling.BOX)).astype(np.float32) / 127.5 - 1
        normals /= np.maximum(np.linalg.norm(normals, axis=-1, keepdims=True), .001)
        Image.fromarray(np.rint((normals + 1) * 127.5).clip(0, 255).astype('uint8')).save(dest / 'normal.png')
    with Image.open(cache / 'rough.jpg') as src:
        src.convert('L').resize((size, size), Image.Resampling.BOX).save(dest / 'roughness.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cache', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--fetch', action='store_true', help='Fetch missing originals into the cache')
    args = parser.parse_args()
    lock = json.loads((HERE / 'DandelionSources.json').read_text())
    args.cache.mkdir(parents=True, exist_ok=True)
    for name, meta in lock['sourceFiles'].items():
        path = args.cache / name
        if not path.exists() and args.fetch:
            request = Request(meta['url'], headers={'User-Agent': 'LuxEngine-FrontierCounty/1.0'})
            part = path.with_suffix('.part')
            with urlopen(request, timeout=90) as stream, part.open('wb') as target:
                shutil.copyfileobj(stream, target)
            verify(part, meta)
            part.replace(path)
        verify(path, meta)
    args.destination.mkdir(parents=True, exist_ok=True)
    doc, accessor = read_gltf(args.cache / 'dandelion.gltf')
    node = next(n for n in doc['nodes'] if n['name'] == 'dandelion_01_a_LOD0')
    primitive, = doc['meshes'][node['mesh']]['primitives']
    positions, normals, uvs = (np.ascontiguousarray(accessor(primitive['attributes'][key]), dtype=np.float32)
                              for key in ('POSITION', 'NORMAL', 'TEXCOORD_0'))
    indices = np.ascontiguousarray(accessor(primitive['indices']).reshape(-1), dtype=np.uint32)
    reduced, error = simplify(positions, normals, uvs, indices, 1400, True)
    mesh = write_obj(args.destination / 'flower.obj', positions, normals, uvs, reduced)
    bake_material(args.cache, args.destination)
    (args.destination / 'License.txt').write_text(
        'Dandelion 01 by Rico Cilliers and Rob Tuytel. CC0 1.0.\n'
        'https://polyhaven.com/a/dandelion_01\nhttps://polyhaven.com/license\n')
    paths = ('flower.obj', 'albedo.png', 'normal.png', 'roughness.png', 'License.txt')
    files = []
    for name in paths:
        path = args.destination / name
        with path.open('rb') as stream:
            files.append({'path':name, 'bytes':path.stat().st_size,
                          'sha256':hashlib.file_digest(stream,'sha256').hexdigest()})
    lock.update(sourceNode=node['name'], sourceTriangles=len(indices)//3, mesh=mesh,
                geometricError=error, runtimeTextureDimensions=[1024,1024], files=files)
    (args.destination / 'source-manifest.json').write_text(json.dumps(lock,indent=2)+'\n')


if __name__ == '__main__':
    main()
