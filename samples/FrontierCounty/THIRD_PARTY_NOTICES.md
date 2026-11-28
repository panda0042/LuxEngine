# Third-party assets

**Powered by Poly Haven.** The asset tool reads official catalog and file metadata with a Frontier County user agent, verifies the downloads and stores the original bytes in a user-selected cache. [Poly Haven license](https://polyhaven.com/license) · [Public API](https://polyhaven.com/our-api)

- Dandelion 01 by Rob Tuytel and Rico Cilliers, CC0 1.0. A flowering rosette was converted from its scanned glTF mesh. Verified 8192 × 8192 diffuse, alpha, normal and roughness originals supply 1024 × 1024 runtime maps. [Dandelion 01](https://polyhaven.com/a/dandelion_01)
- The forest floor uses three Rainy Creek scans under CC0 1.0. Rob Tuytel and Rico Cilliers created Fern 02 and Pine Sapling Small. Rob Tuytel created Dead Tree Trunk. [Fern 02](https://polyhaven.com/a/fern_02) · [Pine Sapling Small](https://polyhaven.com/a/pine_sapling_small) · [Dead Tree Trunk](https://polyhaven.com/a/dead_tree_trunk)
- Rock Moss Set 01 by Kless Gyzen, CC0 1.0. Three scanned shoreline meshes reuse the verified Rainy Creek package and its diffuse texture. [Rock Moss Set 01](https://polyhaven.com/a/rock_moss_set_01)
- Rocky Gravel by Dimitrios Savva and Dario Barresi, CC0 1.0. Verified 8192 × 8192 originals supply four 1024 × 1024 runtime maps, including measured displacement. Albedo is resized in linear light and encoded to sRGB. Normal and roughness are sampled as linear data. [Rocky Gravel](https://polyhaven.com/a/rocky_gravel)
- Lakeside Sunrise by Greg Zaal and Jarod Guest, CC0 1.0. Silverlake uses LuxEngine's existing 2048 × 1024 reflection derived from the 24576 × 12288 OpenEXR. The main sky follows the simulation sun through the engine's dynamic atmosphere. The baked noon environment still supplies image-based lighting. [Lakeside Sunrise](https://polyhaven.com/a/lakeside_sunrise)
- Tevy Dog Walker, Hiker and Road Worker, CC0 1.0. The v1 LOD1 GLBs are verified against the provider's SHA-256 catalog. Runtime FBXs retain the meshes, skin and idle/walk curves. PBR textures are decoded from the packaged KTX2 images. [Dog Walker](https://tevy.dev/models/dog-walker) · [Hiker](https://tevy.dev/models/hiker) · [Road Worker](https://tevy.dev/models/road-worker)
- Poly Haven Asphalt 01 by Charlotte Baglioni and Dario Barresi, CC0 1.0. Road PBR maps come from verified 8192 × 8192 albedo, normal and roughness originals. [Asphalt 01](https://polyhaven.com/a/asphalt_01)
- Tevy roadside structures and pickup, CC0 1.0. FrontierProps contains model FBXs and material maps derived from the catalog-verified LOD2 GLBs. Version and source hashes are recorded per model. [Tevy catalog](https://tevy.dev/models)
- Tevy Norway Spruce, CC0 1.0. The verified v1 LOD1 GLB supplies near 3D trees. The LOD2 GLB supplies geometry and opacity for the distant foliage card. [Norway Spruce](https://tevy.dev/models/norway-spruce)
- Tevy Wild Grass Clump and Sedge Tuft, CC0 1.0. Actual v1 LOD1 geometry populates the meadow and lakeshore in nearby batches. [Wild Grass Clump](https://tevy.dev/models/grass-clump) · [Sedge Tuft](https://tevy.dev/models/sedge-tuft)
- Poly Haven Leafy Grass, CC0 1.0. The verified 8K albedo, normal, roughness and displacement originals supply the secondary terrain material. [Leafy Grass](https://polyhaven.com/a/leafy_grass)
- DungGeunMo by Kil Hyung-jin (orioncactus), Public Domain. Used by the optional diagnostics and controls overlay. [DungGeunMo](https://cactus.tistory.com/193)

[Asset manifest](ASSET_MANIFEST.json) records sources and runtime hashes. Rebuild assets with the following commands using OpenImageIO, Blender, glTF Transform with KTX Software, Assimp and Python Pillow/NumPy. CMake and the sample executable use the checked-in derivatives.

    python3 samples/FrontierCounty/Tools/prepare_tevy.py --cache /tmp/frontier-models
    python3 samples/FrontierCounty/Tools/prepare_props.py --cache /tmp/frontier-models
    python3 samples/FrontierCounty/Tools/prepare_props.py --cache /tmp/frontier-models --vegetation
    python3 samples/FrontierCounty/Tools/prepare_asphalt.py --cache /tmp/frontier-materials
    python3 samples/FrontierCounty/Tools/prepare_asphalt.py --cache /tmp/frontier-materials --asset leafy_grass
    python3 samples/FrontierCounty/Tools/prepare_gravel.py --cache /tmp/frontier-materials

## Silverlake terrain palette

- Marble Cliff 01 — Amal Kumar — https://polyhaven.com/a/marble_cliff_01
- Lichen Rock — Rico Cilliers — https://polyhaven.com/a/lichen_rock
- Withered Grass — Charlotte Baglioni — https://polyhaven.com/a/withered_grass
- Leafy Grass — Charlotte Baglioni — https://polyhaven.com/a/leafy_grass
- Coast Sand 01 — Rob Tuytel — https://polyhaven.com/a/coast_sand_01
- Rocky Gravel — Dario Barresi, Dimitrios Savva — https://polyhaven.com/a/rocky_gravel
- Forest Floor — eye-candy.xyz — https://polyhaven.com/a/forest_floor
- Brown Mud — Rob Tuytel — https://polyhaven.com/a/brown_mud

All eight scans are CC0 1.0. The package manifest records channel sources, original dimensions and hashes.

## In-game rendering controls

Frontier County uses Dear ImGui 1.90.0 from The Forge's vendored sources under the MIT license. See its [license](../../Engine/ThirdParty/TheForge/Common_3/Application/ThirdParty/OpenSource/imgui/LICENSE.txt).
