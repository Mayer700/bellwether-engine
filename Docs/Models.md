# Static glTF models and materials

Bellwether loads static glTF 2.0 JSON and GLB scenes directly. The original game's cabin has been imported and captured through this path. The complete survival game still uses the original Godot runtime; animation, gameplay and save migration remain separate required work.

## Authoring and preview

An entity's optional `Model: {"Asset": "Models/Cabin.glb"}` references an asset relative to its `.bwscene` file. `SetModel` adds, changes or removes the component through the shared command service; `SetTransform` preserves it. See `ModelComponents.md` for schema and path validation. Runtime scene loading composes entity and glTF node transforms, shares uploaded geometry between repeated asset references, and applies the scene's renderer preferences. Script fields remain metadata here; this does not yet execute a game lifecycle.

```text
BellwetherAgent inspect-model Models/Cabin.glb
BellwetherRuntime --gltf Models/Cabin.glb --settings Camera.json
BellwetherRuntime --scene Village.bwscene
```

`inspect-model` uses the same importer as rendering and returns validated resource counts or a JSON error. `--gltf` and `--world` select different content sources and cannot be combined. For compatibility, combining `--scene` with either explicit content source applies that scene's renderer settings; it does not also append its model entities. Using `--scene` alone renders its model entities.

## Implemented import

- Selected scene, node hierarchy, TRS/matrices, repeated mesh placements, negative and nonuniform scale.
- Triangle lists with indexed or nonindexed geometry, interleaved accessors, normalized integer attributes, sparse float attributes, optional vertex colors and generated flat normals.
- Existing tangent frames or MikkTSpace generation at face corners. Sparse replacements use tightly packed elements independently of an interleaved base stride.
- PNG/JPEG from local files, base64 data URIs or GLB buffer views; glTF texture-coordinate/image orientation.
- Metallic/roughness PBR factors and all five texture roles: base color, metallic/roughness, tangent normal, occlusion and emissive. Base/emissive images use hardware sRGB decoding; data maps stay linear. Vertex colors and material factors remain linear.
- Per-texture wrap/filter state, full mip chains where requested, linear-space color mip filtering, sampler-state reuse, alpha masks and double-sided rendering in lighting and sun shadows. Back faces reverse the final mapped normal. `KHR_materials_emissive_strength` is supported.

Unsupported features report errors: animations, skins, morph targets, non-triangle-list topology, Draco/meshopt compression, additional UV sets, texture transforms, alpha blending, advanced material lobes, camera/light nodes and GPU instancing extensions. Required unknown extensions reject. Animated actors are not silently flattened. HDRI IBL, SSAO and environment rendering remain pending.

## Failure handling and limits

CPU validation precedes non-immediate GPU upload transactions. Failed imports/scenes retain previous state. Upload completion checks device-loss failure before committing; hardware-loss recovery has not been implemented or injected in tests.

Dependencies resolve below the model directory after URI decoding and canonicalization. Scene references remain below their scene asset root. Absolute/network/parent traversal and escaping links reject. Files are limited to 64 MiB, images to 4096 square, parsed dependency allocations to 256 MiB, decoded images to 256 MiB, and geometry to 256 MiB per asset. These are independent categories, not a total process-memory guarantee. Hierarchy depth is checked in linear time before upstream validation. Combined scenes cap model count, placements, images, textures, materials, primitives and resource bytes.

This is a synchronous development import path; cooking, streaming, compression and animation remain outstanding. Mips use box reduction; specialized normal/alpha-coverage mip production is future work.

## Verification

`Tools/BuildModelFixtures.py` generates original fixtures into the build directory. `ModelImport` tests positive/negative CPU paths. `ModelRenderer` compares real Vulkan HDR readback against independent expectations for color space, UVs, normal maps, GGX channels, mipmaps and cutout coverage/shadow depth. It also checks equivalent encodings, many-material sampler reuse, authored transforms, shared geometry, component removal and rejection rollback. Original renderer regressions remain enabled.

The build copies the canonical feature scene and script next to `ModelFixtures/` so `FeatureSceneRenderer` loads the real model reference. `Evidence/Models/Verification.json` records final checks and source hashes. Cabin captures are local generated evidence; original game assets are excluded from the engine source repository. Windows is the executed graphics platform; others need their own GPU evidence.
