# Bellwether Snow Technology

Bellwether Engine includes a deterministic CPU/physics snow simulation, binary track persistence, and a Vulkan/NVRHI HDR snow, ice, and snowfall rendering pipeline tailored for winter survival environments.

## 1. Deformable SnowField (`SnowField`)

- **Grid state (`SnowCell`)**: Tracks `BaseDepth`, `Compression`, lateral berm `Displacement`, surface `Ice`, and `Moisture` per cell across square grids from `4x4` up to `1024x1024`.
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
  - Physics body contact probing (`ApplySnowInteraction`) measures the collider's world-space AABB vertical extent so tall capsules ground accurately and hovering actors stay clear of phantom track carving while coupling `Sample().FrictionFactor` back to the Jolt body.
- `SceneSimulation` deterministically synchronizes authored entities (including external transform teleports, `PhysicsComponent` mutations, and `SceneSnowSettings` changes) between `Scene`, `PhysicsWorld`, and `SnowField` (`PhysicsTests`).

## 3. Vulkan/NVRHI Snow, Ice, & Snowfall Rendering

- **Snow map texture binding (`set=0, binding=2` + `binding=129`)**: `RGBA8_UNORM` packed from `SnowField::PackRGBA8` (`R = EffectiveDepth / MaxDepth`, `G = Compression / MaxDepth`, `B = Ice`, `A = Coverage`).
- **Surface & vertex shading (`World.vert`, `World.frag`)**:
  - World-space vertex displacement on marked terrain and `SceneSnowSettings::RenderGround` instances (`SurfaceFlags.x`) with half-texel-aligned sampling proportional to effective snow depth (`textureLod` in vertex stage).
  - Central-difference normal reconstruction, trench cavity ambient occlusion, and pushed berm highlights along stamped trails.
  - Slope- and windward-aware snow coverage accumulation on static meshes (`SnowCoverage`).
  - Blends bare terrain, powder albedo/roughness, compacted trail shading, low-roughness ice speculars, icy wrap subsurface scattering (`SnowSubsurface`), sparse HDR micro-facet crystal glints (`SnowSparkle`), and blizzard haze (`SnowfallIntensity`).
- **GPU snowfall particle pipeline (`SnowParticle.vert`, `SnowParticle.frag`)**:
  - Up to `4096` camera-target toroidal instanced billboard flakes scaled by orthographic `CameraSize`, sheared by `WindVelocity` with curl flutter, velocity-aligned streaking, alpha blending, and hardware depth testing (`Less`, depth-write disabled) against `m_HDRDepth`.
- **Verification**: Verified natively on Windows Vulkan 1.3 (`NVIDIA GeForce RTX 3070`) with NVRHI validation enabled via `VulkanLifecycle` (`29/29` checks passed in `Evidence/Renderer/Snow.json`, verifying luminance, track normals/displacement deltas, HDR sparkle peaks > 24.0, wind-sheared particle motion across `SetTimeSeconds`, `depth.u8`/`ice.u8` bridge loading, `.bwscene` `RenderGround` + `.bwsn` `TracksAsset` rendering and transactional rollback on missing/corrupt/escaping track paths, and exact pixel-identical recovery when cleared) and `SnowSandboxRenderer` (`23/23` checks passed in `Evidence/SnowSandbox/SnowSandbox.json` across `TracksAndPhysics`, `ThermalMeltAndIce`, `BlizzardDrift`, and `SunnySparkle`).

## 4. Authoring, Scripting, CLI, & Interactive Sandbox

- **Lua API (`ScriptRuntime`, 25 functions)**: Exposes entity CRUD, `SetRotation`/`GetRotation`, `SetScale`/`GetScale`, `GetEntities` (safe under spawn/destroy iteration), `GetSnowDepth`, `GetSnowCompression`, `GetSnowSpeedFactor`, `GetSnowIce`, `GetSnowFriction`, `StampSnow`, `StampSnowSegment`, `MeltSnow`, `StepSnowWeather`, `SetWind`, `GetWind`, and `SetSnowfall` with atomic `Scene` + `SnowField` rollback on script errors or instruction/memory budget overflow.
- **Agent CLI (`BellwetherAgent`)**: Shares `Scene` and `SnowField` validation for `SetSnowSettings` (including `RenderGround`), `SetPhysics`, `inspect-snow`, `stamp-snow`, and `step-snow`.
- **Real-time Snow Sandbox (`SnowSandbox`, `BellwetherRuntime.exe`)**: Launching `BellwetherRuntime.exe` directly (or with `--snow-sandbox [--scenario <tracks|melt|blizzard|sparkle>]`) builds a self-contained procedural alpine heightfield bowl, cabin, glowing campfire hearth, twin powder-vs-ice comparison sleds, and pool-recycled user sled/boulder entities wired into `SceneSimulation`, supporting cursor raycast carving/melting, cold-snap refreezing, live snowfall/wind/sparkle presets, and `F1`-`F4`/`T`/`R` scenario switching without affecting `--gpu-test` determinism.
