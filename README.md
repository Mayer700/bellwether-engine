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

The native build produces the agent, editor, runtime, core tests, asset tests and physics tests. The editor is currently a window/device shell; authoring panels and gizmos are pending. The runtime previews static worlds and authored model scenes; the survival game port is not yet playable.

The test scene and Lua API exercise are in `Scenes/FeatureTest.bwscene` and `Scenes/FeatureTest.lua`. The GPU fixture is `Scenes/FeatureWorld.json`; the build converts its original test geometry into the renderer's intermediate mesh format. The Vulkan replay presents frames, resizes, renders the fixture and checks GPU readback and NVRHI diagnostics. These tests do not replace future physics, glTF, sound and full-game scenarios.

## Agent commands

```text
BellwetherAgent create Project.bwscene
BellwetherAgent inspect Project.bwscene
BellwetherAgent apply Project.bwscene Commands.json
BellwetherAgent validate Project.bwscene
```

`apply` accepts `Create`, `Destroy`, `Rename`, `SetTransform`, `SetModel` and `SetRendererSettings` command arrays. The entire batch validates before replacing the scene file. Responses are JSON and entity IDs remain stable. Unsupported fields/components/commands reject. Lua exposes eight tested entity/prefab/position methods and `OnUpdate(timestep)`; component lifecycle and the wider physics/render/audio API remain pending.

Static glTF/GLB assets can be validated with `BellwetherAgent inspect-model Models/Cabin.glb`, previewed with `BellwetherRuntime --gltf Models/Cabin.glb --settings Camera.json`, or attached to entities through `Model: {"Asset": "Models/Cabin.glb"}`. `BellwetherRuntime --scene Village.bwscene` resolves models relative to that scene, shares geometry and composes transforms. See [model import and materials](Docs/Models.md) for supported PBR texture roles, diagnostics and limits. The copied build `FeatureTest.bwscene` resolves its generated model fixture.

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

The game content is not bundled into the engine repository. The bridge exports geometry, transforms, material constants, terrain and gameplay metadata. The native renderer displays the static world; actors, interactions, snow compression and chapter logic remain unported. Lighting uses linear RGBA16_FLOAT, two filtered sun-shadow maps and separate exposure/tone mapping. Direct static glTF import now supports textured PBR materials; HDRI IBL, SSAO and animated import remain required. See [renderer verification](Docs/Renderer.md) for scope and limits.

Renderer preferences are authored with `SetRendererSettings` and persisted in `.bwscene`. Combining `--world` or `--gltf` with `--scene` applies only that scene's renderer settings to the explicit content source. Using `--scene` alone renders its model entities. `--settings RenderSettings.json` accepts standalone preferences. Shaders load from `Shaders/` beside the executable, tested after runtime relocation.

A Jolt-backed [physics foundation](Docs/Physics.md) supplies fixed-step rigid bodies, collision filters and ray queries. Scene components, Lua bindings, character control and gameplay integration remain required work.

Windows Debug and Release core and native GPU checks have run locally. The private source repository is [Mayer700/bellwether-engine](https://github.com/Mayer700/bellwether-engine). CI contains core build/test jobs for Windows, macOS and Ubuntu 24.04; remote results must be checked separately. macOS Vulkan/NVRHI compatibility is unverified.
