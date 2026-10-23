# HDR and filtered sun shadows

The renderer now performs three explicit stages: two depth-only sun coverage passes, a linear HDR scene pass, and a full-screen exposure/tone-mapping pass. This is implemented pass orchestration, not yet a general render-graph editor.

The scene target is RGBA16_FLOAT with a D32 depth buffer. Two 2048-square D32 shadow maps cover 48 and 180 metres around the camera target. Coverage blends near the inner map's edge. Light-space texel snapping stabilizes camera movement. A 5x5 PCF kernel filters shadow boundaries; this is a fixed filter, not physically varying area-light penumbrae. GGX direct lighting, hemisphere ambient and distance haze remain the lighting model. [Static glTF materials](Models.md) now provide PBR textures and matching alpha-cutout shadows. HDRI IBL, SSAO, animated import, local lights and a complete sky/environment remain pending.

## Scene and agent controls

`Renderer` is an optional scene property, so existing scenes receive defaults. `SetRendererSettings` updates specified fields while preserving other preferences. Numeric bounds and all fields are validated before committing a command batch. Saving/loading preserves the settings. The current property names are `Exposure`, `Shadows`, `FogDensity`, `SunDirection`, `SunRadiance`, `CameraTarget` and `CameraSize`.

```json
[
  {
    "Operation": "SetRendererSettings",
    "Settings": {
      "Exposure": 0.9,
      "Shadows": true,
      "CameraTarget": [0, 1, 10],
      "CameraSize": 24
    }
  }
]
```

Run scene preferences on bridge content with `BellwetherRuntime --world Cache/Bellwether --scene Project.bwscene`. `--scene Project.bwscene` alone renders authored model entities. For separate preferences, use `--settings RenderSettings.json`. Renderer Lua bindings remain future work; the existing eight Lua APIs retain their behavior.

## Verification

`Tools/Validate.ps1 -GPU` runs core, command-line and native Vulkan scenarios. The renderer fixture contains diffuse, metallic and bright emissive geometry. GPU readback checks linear radiance above one, independently projected shadow and lit locations, filtering, exposure behavior, mathematical tone-mapping output, resizing and frontier visibility. Corrupted assets exercise failure rollback and verify the previous image remains byte-identical afterward. `Evidence/Renderer/Verification.json` records the final run's results and source hashes.

The original village is captured separately through `Tools/Validate.ps1 -GPU -WorldDirectory Cache/Bellwether`. Local review images are `Evidence/Renderer/Before.png`, `After.png` and `Frontier.png`. The outer-region capture exposes remaining finite-world edge/background and terrain-material work; the native terrain does not yet reproduce the original ice/track materials. Do not treat a static-world image as game-port completion.

The GPU timings in the report describe this small controlled renderer fixture. They exclude asset loading and readback; they are not whole-game FPS or an improvement comparison against Godot. NVRHI validation is checked through teardown. A Khronos validation-layer run and other operating systems/devices are still unverified.

## Resource deployment

SPIR-V shaders load from `Shaders/` beside the running executable, independent of working directory or original build path. The runtime and shaders were copied to `Exports/RendererProof` and the same GPU replay executed there. This verifies renderer relocation; a complete game export/installer and its assets/licenses remain outstanding.
