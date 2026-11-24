# Third-party assets

**Powered by Poly Haven.** The asset tool reads official catalog and file metadata with a Frontier County user agent, verifies the downloads and stores the original bytes in a user-selected cache. [Poly Haven license](https://polyhaven.com/license) · [Public API](https://polyhaven.com/our-api)

- Rocky Gravel by Dimitrios Savva and Dario Barresi, CC0 1.0. Verified 8192 × 8192 originals supply three 1024 × 1024 runtime maps. Albedo is resized in linear light and encoded to sRGB. Normal and roughness are sampled as linear data. [Rocky Gravel](https://polyhaven.com/a/rocky_gravel)
- Lakeside Sunrise by Greg Zaal and Jarod Guest, CC0 1.0. Silverlake uses LuxEngine's existing 2048 × 1024 reflection derived from the 24576 × 12288 OpenEXR. The main sky uses the engine's baked noon atmosphere and the directional light follows the clock. [Lakeside Sunrise](https://polyhaven.com/a/lakeside_sunrise)
- Tevy Dog Walker, Hiker and Road Worker, CC0 1.0. The v1 LOD1 GLBs are verified against the provider's SHA-256 catalog. Runtime FBXs retain the meshes, skin and idle/walk curves. PBR textures are decoded from the packaged KTX2 images. [Dog Walker](https://tevy.dev/models/dog-walker) · [Hiker](https://tevy.dev/models/hiker) · [Road Worker](https://tevy.dev/models/road-worker)
- Poly Haven Asphalt 01 by Charlotte Baglioni and Dario Barresi, CC0 1.0. Road PBR maps come from verified 8192 × 8192 albedo, normal and roughness originals. [Asphalt 01](https://polyhaven.com/a/asphalt_01)
- Tevy roadside structures and pickup, CC0 1.0. FrontierProps contains model FBXs and material maps derived from the catalog-verified LOD2 GLBs. Version and source hashes are recorded per model. [Tevy catalog](https://tevy.dev/models)
- Tevy Norway Spruce, CC0 1.0. The verified v1 LOD1 GLB supplies near 3D trees. The LOD2 GLB supplies geometry and opacity for the distant foliage card. [Norway Spruce](https://tevy.dev/models/norway-spruce)
- Tevy Wild Grass Clump and Sedge Tuft, CC0 1.0. Actual v1 LOD1 geometry populates the meadow and lakeshore in nearby batches. [Wild Grass Clump](https://tevy.dev/models/grass-clump) · [Sedge Tuft](https://tevy.dev/models/sedge-tuft)
- Poly Haven Leafy Grass, CC0 1.0. The verified 8K originals supply the secondary terrain material. [Leafy Grass](https://polyhaven.com/a/leafy_grass)
- DungGeunMo by Kil Hyung-jin (orioncactus), Public Domain. Used by the optional diagnostics and controls overlay. [DungGeunMo](https://cactus.tistory.com/193)

[Asset manifest](ASSET_MANIFEST.json) records sources and runtime hashes. Rebuild assets with the following commands using OpenImageIO, Blender, glTF Transform with KTX Software, Assimp and Python Pillow/NumPy. CMake and the sample executable use the checked-in derivatives.

    python3 samples/FrontierCounty/Tools/prepare_tevy.py --cache /tmp/frontier-models
    python3 samples/FrontierCounty/Tools/prepare_props.py --cache /tmp/frontier-models
    python3 samples/FrontierCounty/Tools/prepare_props.py --cache /tmp/frontier-models --vegetation
    python3 samples/FrontierCounty/Tools/prepare_asphalt.py --cache /tmp/frontier-materials
    python3 samples/FrontierCounty/Tools/prepare_asphalt.py --cache /tmp/frontier-materials --asset leafy_grass
    python3 samples/FrontierCounty/Tools/prepare_gravel.py --cache /tmp/frontier-materials
