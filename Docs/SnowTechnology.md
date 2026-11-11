# Bellwether Snow Technology

Bellwether Engine includes a deterministic CPU/physics snow simulation, binary track persistence, and a Vulkan/NVRHI HDR snow, ice, and snowfall rendering pipeline tailored for winter survival environments.

## 1. Deformable SnowField (`SnowField`)

- **Grid state (`SnowCell`)**: Tracks `BaseDepth`, `Compression`, lateral berm `Displacement`, surface `Ice`, and `Moisture` per cell across square grids from `4x4` up to `2048x2048`.
- **Deformation & thermal coupling**:
  - `StampCircle` and `StampSegment` compress snow beneath actors/sleds with conservation of displaced rim berms (`rimFactor`) and speed/friction penalties (`Sample().SpeedFactor`, `Sample().FrictionFactor`).
  - `ApplyHeatSource` models campfires, lanterns, and torches: melts snow depth, converts meltwater into slick `Ice` once cooled, and updates effective traction.
  - `StepWeather` accumulates wind-drifted fresh snow, heals light tracks (`TrackHealRate`), freezes wet snow below freezing, and responds deterministically to wind velocity (`SnowTests`).
- **Persistence & external bridges**:
  - Binary `.bwsn` format (`SerializeBinary` / `DeserializeBinary` / `SaveTracks` / `LoadTracks`) with magic header, version validation, bounded dimensions, and CRC32 checksums verified via `SnowTests` and `BellwetherAgent`.
  - Interoperability with 8-bit `depth.u8` + `ice.u8` maps from the Godot export bridge (`snow_size` validated in `world.json`).

## 2. Physics & Scene Simulation (`PhysicsWorld`, `SceneSimulation`)

- Jolt-backed `CreateHeightField` terrain shapes and `PhysicsShape::Capsule` character/prop colliders integrated with `Scene` (`PhysicsComponent` with friction, restitution, `SnowTracking`, and `SnowDrag`).
- `ApplySnowInteraction` feeds dynamic velocity-dependent snow drag and linear damping back into Jolt bodies while stamping continuous trail segments into the `SnowField`.
- `SceneSimulation` deterministically synchronizes authored entities between `Scene`, `PhysicsWorld`, and `SnowField` (`PhysicsTests`).

## 3. Vulkan/NVRHI Snow, Ice, & Snowfall Rendering

- **Snow map texture binding (`set=0, binding=2` + `binding=129`)**: `RGBA8_UNORM` packed from `SnowField::PackRGBA8` (`R = EffectiveDepth / MaxDepth`, `G = Compression / MaxDepth`, `B = Ice`, `A = Coverage`).
- **Surface & vertex shading (`World.vert`, `World.frag`)**:
  - Vertex displacement on marked terrain instances (`SurfaceFlags.x`) proportional to effective snow depth (`textureLod` in vertex stage).
  - Central-difference normal reconstruction along stamped track trenches and rim berms.
  - Slope-aware snow coverage accumulation on static meshes (`SnowCoverage`).
  - Blends bare terrain, powder albedo/roughness, compacted trail shading, low-roughness ice speculars, icy wrap subsurface scattering (`SnowSubsurface`), HDR 3D world-cell crystal glints (`SnowSparkle`), and blizzard haze (`SnowfallIntensity`).
- **GPU snowfall particle pipeline (`SnowParticle.vert`, `SnowParticle.frag`)**:
  - Up to `4096` camera-target toroidal instanced billboard flakes sheared by `WindVelocity` with curl flutter, velocity-aligned streaking, alpha blending, and hardware depth testing (`Less`, depth-write disabled) against `m_HDRDepth`.
- **Verification**: Verified natively on Windows Vulkan 1.3 (`NVIDIA GeForce RTX 3070`) with NVRHI validation enabled via `VulkanLifecycle` (`23/23` checks passed in `Evidence/Renderer/Snow.json`, verifying luminance, track normals/displacement deltas, HDR sparkle peaks > 13.7, wind-sheared particle motion across `SetTimeSeconds`, bridge loading, and exact pixel-identical recovery when disabled).

## 4. Authoring, Scripting, & CLI

- **Lua API (`ScriptRuntime`, 21 functions)**: Exposes entity CRUD, `SetRotation`/`GetRotation`, `SetScale`/`GetScale`, `GetEntities` (safe under spawn/destroy iteration), `GetSnowDepth`, `GetSnowCompression`, `GetSnowSpeedFactor`, `StampSnow`, `MeltSnow`, `SetWind`, `GetWind`, and `SetSnowfall` with atomic `Scene` + `SnowField` rollback on script errors or instruction/memory budget overflow.
- **Agent CLI (`BellwetherAgent`)**: Shares `Scene` and `SnowField` validation for `SetSnowSettings`, `SetPhysics`, `inspect-snow`, `stamp-snow`, and `step-snow`.
- **Interactive preview (`BellwetherRuntime`)**: Non-test mode supports real-time snowfall animation, `WASD`/Arrow pan, `Q`/`E` zoom, `1`/`2` snowfall adjustment, and `Space` interactive snow footprint stamping without affecting `--gpu-test` determinism.
