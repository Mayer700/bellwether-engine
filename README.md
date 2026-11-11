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

The native Debug build (`Build/`) produces the agent, editor, runtime (`Build\BellwetherRuntime.exe`), core tests, asset tests, snow tests and physics tests. Launching `BellwetherRuntime.exe` directly opens the interactive real-time Snow Sandbox; passing `--scene`, `--gltf` or `--world` previews authored assets instead. The editor is currently a window/device shell; authoring panels, gizmos and the full survival game port remain pending.

To configure and build an optimized Windows Release `.exe` and run all Release CTest targets (requires free workspace disk space for a separate `BuildRelease/` tree):

```powershell
cmake -S . -B BuildRelease -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBELLWETHER_BUILD_GRAPHICS=ON -DBELLWETHER_GPU_TESTS=ON
cmake --build BuildRelease --parallel 6
ctest --test-dir BuildRelease --output-on-failure
```

`BellwetherRuntime.exe` links statically against the C++ runtime and loads SPIR-V shaders from `Shaders/` next to the executable (verified standalone after relocating `Build\BellwetherRuntime.exe` + `Build\Shaders/`).

The test scene and Lua API exercise are in `Scenes/FeatureTest.bwscene` and `Scenes/FeatureTest.lua`. The GPU fixture is `Scenes/FeatureWorld.json`; the build converts its original test geometry into the renderer's intermediate mesh format. The Vulkan replay presents frames, resizes, renders the fixture and checks GPU readback and NVRHI diagnostics. These tests do not replace future glTF animation, audio and full-game scenarios.

## Real-time Snow Sandbox (`BellwetherRuntime.exe`)

Run `Build\BellwetherRuntime.exe` (or `BuildRelease\BellwetherRuntime.exe` after a Release build) with no arguments to launch the real-time Vulkan Snow Sandbox (`--help` prints all options):

```powershell
.\Build\BellwetherRuntime.exe
.\Build\BellwetherRuntime.exe --snow-sandbox --scenario melt
```

Available procedural test scenarios:

- `tracks` (`F1`): Side-by-side powder sled vs slick ice-lane sled physics (`SnowDrag` and friction coupling), pre-carved trails, and launchable sleds/boulders.
- `melt` (`F2`): Sub-zero campfire hearth melt zone, thermal snow reduction (`MeltAt`), meltwater wetness, and cold-snap refreezing into ice.
- `blizzard` (`F3`): Heavy crosswind storm, depth-tested 4096-flake GPU snowfall particles, wind advection, and progressive track healing.
- `sparkle` (`F4`): Low winter sun, steep cliff snow shedding, carved tracks, subsurface scattering, and HDR crystal sparkle glints.

Interactive controls:

| Input | Action |
| --- | --- |
| `WASD` / Arrow keys | Pan camera across alpine arena |
| `Q` / `E` | Zoom camera out / in |
| Left Mouse drag | Raycast and carve continuous snow tracks under cursor |
| Right Mouse hold / `M` | Apply localized thermal snow melt at probe |
| `Space` | Stamp / drag trench under active probe |
| `F` | Trigger sub-zero cold snap (`-22C`) to refreeze wet slush into ice |
| `G` / `Enter` | Launch alternating dynamic snow sled or boulder |
| `1` / `2` | Decrease / increase snowfall intensity |
| `3` / `4` | Cycle wind direction/speed presets |
| `5` / `6` | Cycle snow sparkle and subsurface scatter presets |
| `F1`–`F4` / `T` / `R` | Jump to scenario / cycle next scenario / reset current scenario |
| `H` | Print live probe depth, wetness, ice and ambient temperature to console |

## Snow tests

Automated CPU, physics, scripting, CLI and Vulkan GPU snow tests run via CTest:

```powershell
ctest --test-dir Build -R "SnowSimulation|SnowSandboxRenderer|PhysicsFoundation|CoreAndScripting|AgentCommands|VulkanLifecycle" --output-on-failure
```

| Test suite | Target / Command | Scope covered |
| --- | --- | --- |
| `SnowSimulation` (`45` checks) | [`Tests/SnowTests.cpp`](Tests/SnowTests.cpp) (`BellwetherSnowTests.exe`) | `SnowField` compression and displaced rim berms, thermal melting, sub-zero moisture-to-ice refreeze, wind drift and track healing, steep boundary slope shedding, CRC32 `.bwsn` save/load and rollback, `depth.u8`/`ice.u8` resampling, invalid input rejection, plus headless `SnowSandbox` twin-sled drag separation, 10-launch entity pool recycling, melt/freeze cycles, blizzard healing and heightfield raycasting. |
| `PhysicsFoundation` (`133` total checks) | [`Tests/PhysicsTests.cpp`](Tests/PhysicsTests.cpp) (`BellwetherPhysicsTests.exe`) | Jolt snow-coupled linear drag and friction (`SnowTracking`, `SnowDrag`), vertical AABB contact probing (tall capsules vs hovering bodies), deterministic replay under snow coupling, `SceneSimulation::SyncFromScene()`, validated `SetAmbientCelsius()`, and `SceneSnowSettings` hot reconfiguration. |
| `CoreAndScripting` (`245` total checks) | [`Tests/CoreTests.cpp`](Tests/CoreTests.cpp) (`BellwetherTests.exe`) | 25 Lua API functions (`GetSnowDepth`, `GetSnowCompression`, `GetSnowSpeedFactor`, `GetSnowIce`, `GetSnowFriction`, `StampSnow`, `StampSnowSegment`, `MeltSnow`, `StepSnowWeather`, `SetWind`, `GetWind`, `SetSnowfall`, entity transforms), joint `Scene` + `SnowField` error/budget rollback, `.bwsn` path security, and `SceneSnowSettings` roundtrips. |
| `AgentCommands` | [`Tests/AgentTests.cmake`](Tests/AgentTests.cmake) (`BellwetherAgent.exe`) | `inspect-snow`, `stamp-snow`, `step-snow`, and transactional `SetSnowSettings` / `SetPhysics` scene command validation and failure rollback. |
| `VulkanLifecycle` (`29` snow checks) | `BellwetherRuntime --gpu-test --renderer-test` | Vulkan 1.3 + NVRHI validation (`VerifySnowRenderer` in `RendererVerification.cpp`): terrain vertex displacement, central-difference track normals and luminance delta, HDR sparkle peak, animated `SnowParticle` wind streaks, `depth.u8`/`ice.u8` bridge rendering, `.bwscene` `RenderGround` + `.bwsn` `TracksAsset` rollback, and `ClearSnowField` exact recovery (`Evidence/Renderer/Snow.json`). |
| `SnowSandboxRenderer` (`23` checks) | `BellwetherRuntime --gpu-test --snow-test` | End-to-end procedural Vulkan sandbox rendering (`VerifySnowSandbox` in `RendererVerification.cpp`) across `TracksAndPhysics`, `ThermalMeltAndIce`, `BlizzardDrift`, and `SunnySparkle`, readback pixel deltas, HDR peaks, raycast bounds, and API error rejection (`Evidence/SnowSandbox/SnowSandbox.json`). |

Run the standalone GPU snow sandbox test directly (including from a relocated executable + `Shaders/` folder):

```powershell
.\Build\BellwetherRuntime.exe --gpu-test --snow-test --capture Build/Evidence/SnowSandbox.ppm --readback Build/Evidence/SnowSandboxReadback.ppm
```

## Agent commands

```text
BellwetherAgent create Project.bwscene
BellwetherAgent inspect Project.bwscene
BellwetherAgent apply Project.bwscene Commands.json
BellwetherAgent validate Project.bwscene
BellwetherAgent inspect-snow Tracks.bwsn
BellwetherAgent stamp-snow Tracks.bwsn 0 0 2.0 0.35 128 64.0 0.45
BellwetherAgent step-snow Tracks.bwsn 1.0 0.8 4.0 -1.5 -8.0
```

`apply` accepts `Create`, `Destroy`, `Rename`, `SetTransform`, `SetModel`, `SetPhysics`, `SetRendererSettings` and `SetSnowSettings` command arrays. The entire batch validates before replacing the scene file. Responses are JSON and entity IDs remain stable. Unsupported fields/components/commands reject. Lua exposes 25 tested entity/transform/prefab/snow/weather methods and `OnUpdate(timestep)` with atomic `Scene` and `SnowField` rollback; component physics scripting, audio APIs and UI authoring panels remain pending.

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

The game content is not bundled into the engine repository. The bridge exports geometry, transforms, material constants, terrain, `depth.u8`/`ice.u8` snow layers and gameplay metadata. The native renderer displays static scenes with deformable snow displacement, ice/powder PBR blending, HDR crystal sparkle, subsurface scatter, and depth-tested wind-driven snowfall particles ([snow technology](Docs/SnowTechnology.md)); actors, interactions and chapter logic remain unported. Lighting uses linear RGBA16_FLOAT, two filtered sun-shadow maps and separate exposure/tone mapping. Direct static glTF import supports textured PBR materials; HDRI IBL, SSAO and animated import remain required. See [renderer verification](Docs/Renderer.md) for scope and limits.

Renderer preferences are authored with `SetRendererSettings` and persisted in `.bwscene`. Combining `--world` or `--gltf` with `--scene` applies only that scene's renderer settings to the explicit content source. Using `--scene` alone renders its model entities and configured `SnowField`. `--settings RenderSettings.json` accepts standalone preferences. Shaders load from `Shaders/` beside the executable, tested after runtime relocation.

A Jolt-backed [physics foundation](Docs/Physics.md) supplies fixed-step rigid bodies, capsule shapes, heightfields, `PhysicsComponent` scene sync, collision filters, snow-coupled track drag, and ray queries. Controllable actors, Lua physics bindings, and gameplay integration remain required work.

Windows Debug core and native GPU snow/world checks (`9/9` CTest) have run locally; Release GPU parity was verified on earlier milestones prior to the Snow Sandbox additions. The private source repository is [Mayer700/bellwether-engine](https://github.com/Mayer700/bellwether-engine). CI contains core build/test jobs for Windows, macOS and Ubuntu 24.04; remote results must be checked separately. macOS Vulkan/NVRHI compatibility is unverified.
