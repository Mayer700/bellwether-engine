# Bellwether Engine

C++ engine and editor under active development, targeting Windows, macOS and Ubuntu 24+. Selected stack: CMake, GLFW, NVRHI/Vulkan, GLM, Lua and miniaudio. See [the production plan](Docs/ProductionPlan.md) for the complete required engine and game port scope and current limits.

This is not yet a production-ready engine or a complete port. The original Godot game remains the playable reference in the parent workspace. The earlier raylib experiment is superseded.

Code style follows [Hazel's naming guide](https://docs.hazelengine.com/HazelForEngineers/DeveloperGuide#naming). Dependencies are separately licensed; no Hazel source is included.

## Build and test

Requires a C++20 compiler, CMake 3.24+, Ninja, Git and Python 3. Vulkan execution additionally requires a Vulkan 1.3 device/driver and display. Dependencies are fetched at exact locked commits with TLS verification. On Ubuntu, install the GLFW X11/Wayland development packages appropriate to its build configuration.

On Windows, from this directory:

```powershell
./Tools/Validate.ps1 -GPU
```

For portable core-only checks (no window/device required):

```text
cmake --preset core
cmake --build --preset core
ctest --preset core
```

The native build produces `BellwetherAgent`, `BellwetherRuntime`, `BellwetherEditor` and `BellwetherTests`. The editor is currently a window/device shell; authoring panels and gizmos are pending. The runtime is a renderer test host, not the survival game port.

The test scene and Lua API exercise are in `Scenes/FeatureTest.bwscene` and `Scenes/FeatureTest.lua`. The GPU fixture is `Scenes/FeatureWorld.json`; the build converts its original test geometry into the renderer's intermediate mesh format. The Vulkan replay presents frames, resizes, renders the fixture and checks GPU readback and NVRHI diagnostics. These tests do not replace future physics, glTF, sound and full-game scenarios.

## Agent commands

```text
BellwetherAgent create Project.bwscene
BellwetherAgent inspect Project.bwscene
BellwetherAgent apply Project.bwscene Commands.json
BellwetherAgent validate Project.bwscene
```

`apply` accepts an array of `Create`, `Destroy`, `Rename` and `SetTransform` commands. It validates the entire batch and replaces the scene file only after success. Responses are JSON. Stable entity IDs are preserved. Unsupported fields/components/commands produce errors instead of being discarded. Lua exposes eight currently tested entity/prefab/position methods and `OnUpdate(timestep)`; per-entity script components and the wider physics/render/audio API are still pending.

## Render the existing game world

In the original parent Godot workspace, run the installed Godot executable with:

```text
--headless --path . --script res://Engine/Tools/ExportGodot.gd
```

This read-only bridge resolves the original scene into `Engine/Cache/Bellwether`. From Engine, run:

```powershell
./Tools/Validate.ps1 -GPU -WorldDirectory Cache/Bellwether
py -3 Tools/ConvertCapture.py Evidence/World.ppm Evidence/World.png
```

The game content is not bundled into the engine repository. The bridge exports geometry, transforms, material constants, terrain and gameplay metadata. The current native renderer displays the static world; it does not yet run its actors, interactions, snow compression or chapter. Lighting now renders into a linear RGBA16_FLOAT target, with two filtered sun-shadow maps and a separate exposure/tone-mapping pass. HDRI IBL, SSAO, textures and animated glTF import remain required work. See [renderer verification](Docs/Renderer.md) for the exact scope, controls and limits.

Renderer preferences can be authored through `SetRendererSettings` agent commands and saved in a `.bwscene`. Run `BellwetherRuntime --world Cache/Bellwether --scene Project.bwscene` to preview those renderer preferences. This currently consumes the scene's renderer settings; rendering its entity components remains pending. `--settings RenderSettings.json` accepts a standalone settings object instead. Shaders load from a `Shaders/` directory beside the executable, and this resource lookup has been tested after relocating the runtime.

Windows Debug and Release core and native GPU checks have run locally. The private source repository is [Mayer700/bellwether-engine](https://github.com/Mayer700/bellwether-engine). CI contains core build/test jobs for Windows, macOS and Ubuntu 24.04; remote results must be checked separately. macOS Vulkan/NVRHI compatibility is unverified.
