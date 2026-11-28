# Frontier County

A LuxEngine outdoor rendering sample. Silverlake, the forest, a textured roadside streetscape, and a driveable pickup share a streamed 16,384 × 16,384 m terrain.

Build the SampleFrontierCounty target in your IDE and launch it. The sample opens by the lake at noon with the Dear ImGui Rendering Debug panel visible.

| Control | Action |
| --- | --- |
| Tab or the panel's close button | Switch between the rendering panel and gameplay |
| WASD, mouse | Walk or drive, look |
| Shift, Space, C | Jog or brake, jump, crouch |
| E, V | Enter or leave the pickup, change camera |
| T, G | Cycle weather, toggle hardware ray tracing when supported |
| F1, F3, P, Esc | Controls, frame statistics, pause, close the panel or exit |

Use the checkboxes to combine effects, the **Enable all**, **Disable all**, and **Reset defaults** buttons to compare configurations, and the view selector to inspect render buffers. Temporal AA and Edge AA are alternative filters. **Enable all** selects Temporal AA. The interface is composited after the scene, so the effects leave its text and controls readable.

Seeded understory patches use scanned ferns, pine saplings, dandelions, and fallen trunks. Three CC0 mossy rock scans sit in eroded soil contact patches. Terrain-clipped PBR decals add silt, damp patches, and asphalt cracks. Walk across soil or drive over the road to leave tread impressions.

[Visual quality validation](Documentation/VISUAL_QUALITY_VALIDATION.md) · [Renderer limits](Documentation/ENGINE_MISSING_FEATURES.md) · [Asset manifest](ASSET_MANIFEST.json) · [Third-party notices](THIRD_PARTY_NOTICES.md)
