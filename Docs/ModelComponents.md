# Editable static model components

An entity may contain an optional `Model` object with one required `Asset` string. The native field is `Entity::ModelAsset`. Empty strings represent an absent component; serialization omits absent components. Existing scenes without `Model` continue to load. The scene format remains version 1.

```json
{"Model": {"Asset": "Models/Cabin.glb"}}
```

The shared structured command service sets or removes the component transactionally:

```json
[
  {"Operation": "SetModel", "ID": 2, "Asset": "Models/Cabin.glb"}
]
```

Set `Asset` to `""` to remove it. Commands require an existing entity and reject unknown fields, wrong types and malformed paths. A failed command rolls back the entire batch. Prefab spawning copies model references while allocating a new entity identity; failed spawning preserves the previous scene and ID allocation. Transform edits preserve the model reference. This change adds no Lua API.

`void ValidateModelAssetPath(const std::string& asset)` is available to import/render code. It accepts empty references and relative forward-slash paths of at most 1,024 bytes with segments of at most 255 bytes, encoded as valid UTF-8, ending in `.gltf` or `.glb` (case insensitive). Absolute paths, drive paths, URLs, backslashes, empty/dot/parent segments, control characters, invalid UTF-8, Windows forbidden characters, reserved device names and segment-ending dots/spaces are rejected. No asset existence check occurs in pure scene loading: save/load and offline authoring must work before asset delivery. Runtime resolution must additionally enforce containment after filesystem canonicalization and validate the imported asset's dependencies.

The component stores a static model reference; render integration is responsible for resolving the root, importing geometry/materials/textures and composing entity and model-node transforms. It does not establish animation, skinning, hierarchy editing or full game parity.

`Scenes/FeatureTest.bwscene` references `ModelFixtures/Materials.glb` on entity 2. The generated file belongs to the configured build's `ModelFixtures` directory, not the checked-in `Scenes` directory. Runtime feature testing must copy the scene into the build content root (or explicitly use that root for resolution) so this reference resolves to the real generated fixture. Core scene tests intentionally do not fabricate or open that asset.

`CoreTests.cpp` exercises command edits/removal, in-memory and disk roundtrips, compatibility with absent components, malformed schemas and paths, transform preservation, prefab identity/copy/failure and whole-batch rollback. Native import/render validation remains a separate required gate.

On 2026-10-23, the dedicated `BuildModelComponents` Debug configuration (graphics disabled, GNU 16.1.0, warnings as errors) compiled `BellwetherTests`; `ctest --test-dir BuildModelComponents -R CoreAndScripting --output-on-failure` passed with 129 core assertions and zero failures. The scene and tests passed clang-format 19.1.7 dry-run and `git diff --check`. Review covered ID preservation, old-scene compatibility, UTF-8/path boundaries, strict field/type validation and failed-edit/prefab/deserialization rollback. Review caught and fixed the need to preserve `ModelAsset` through `SetTransform`'s entity reconstruction. These are Windows core results; GPU rendering and other operating systems require their own evidence.
