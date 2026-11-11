# Bellwether Engine production plan

The user selected **Bellwether Engine**. The final deliverable is a stable, deployable Windows/macOS/Ubuntu 24+ editor and runtime, plus the complete Bellwether survival game port. This document is a completion contract, not a claim that the work is done.

## Required stages

1. Reproducible C++/CMake static core; dependency lock; unit testing; scene/entity identity, serialization and agent CLI; Lua lifecycle.
2. GLFW surface, Vulkan adapter/queue/swapchain lifecycle, NVRHI device/validation, resize/minimize recovery, GPU readback/capture and feature test scene.
3. glTF meshes/materials/textures and animations; PBR, HDR/tonemapping, HDRI IBL, soft shadow maps, SSAO; render comparisons and GPU/frame metrics.
4. Editor hierarchy, inspector, asset browser, transform gizmos, undo/redo, scene/prefab workflows; every authoring command available through structured agent operations.
5. Physics components, queries, collision, controllable actors; Lua API for entities/components, update callbacks, creation/destruction and prefabs; miniaudio lifecycle and positional audio.
6. Complete game port: exact expanded terrain, snow/track simulation and save migration, authored village and wildlife, four weapons and original AI rules, all 34 recipes and queues, survival, injuries, equipment, storage, traps, projects, weather, entire Bellwether chapter, frost journal and settings/audio.
7. Export standalone games with editor code absent, asset validation, licenses and packaging; full empty-inventory-to-ending replay; crash/failure tests, input/motion/render/audio testing, measured performance; Windows/macOS/Ubuntu CI runs and release testing.

The earlier `../native` raylib code is superseded and has not been accepted into the selected engine. The source Godot game remains intact. The offline content bridge currently exports 1,021 meshes, 5,193 draw placements, 727 colliders, 771 resources, 25 houses and 24 actors; these counts alone do not prove gameplay parity.

## Platform risk

NVRHI upstream documents Windows and Linux, nowt macOS. Keep Vulkan as requested and investigate MoltenVK portability support; do not claim macOS support until real build and device tests succeed. Renderer quality must be established from actual images and measurements, not backend branding.

## Delivery destination

The original requested GitHub remote, `https://github.com/TheCherno/AIGameEngine`, returned repository-not-found/404. The user subsequently authorized creating a new repository. The new private repository is `https://github.com/Mayer700/bellwether-engine`, owned by the user's authenticated account. Only the reviewed engine repository is published; the parent Godot project, player saves, generated assets and builds remain local. Delivery is verified by comparing the pushed main branch with the local commit.

## Current verified foundation

- Static C++ core and separate agent/editor/runtime executables; locked dependencies, strict scene schema and transactional command batches; stable entity IDs and durable file replacement.
- Lua entity/prefab/position API with host-owned error handling, scene rollback, instruction budget and 16 MiB VM memory budget. No host filesystem/process libraries. Per-entity lifecycle and the rest of the scripting API remain pending.
- GLFW/Vulkan 1.3 device and presentation, two frames in flight, swapchain resize, depth buffers, NVRHI validation, SPIR-V build pipeline, indexed meshes, material constants, direct-light PBR equations and tone mapping.
- A standalone GPU test scene and a static render of the existing game bridge: 4,504 visible mesh placements including generated terrain. Skinned actors and hidden project props are not included in this static-render count.
- Windows RTX 3070 native frames, resize and GPU readback verified. This establishes basic rendering, not superior visual quality or cross-platform completion.

## HDR and sun-shadow milestone

Linear half-float scene rendering, two stabilized sun-shadow coverage levels with 5x5 PCF filtering, separate exposure/tone mapping, strict persisted renderer settings and native GPU readback tests are implemented. Shader resources now resolve beside the executable. Malformed imports validate before opening GPU upload commands; regression tests verify that the old scene remains renderable. Conservative mesh bounds keep the full terrain visible when viewing the outer region. See `Renderer.md` for evidence and limitations.

## Static models and physics foundation

Direct static glTF/GLB import, textured metallic/roughness materials, normal/occlusion/emissive maps, alpha-cutout color/shadow coverage, mipmaps and sampler reuse are implemented. Optional editable model components serialize through the shared commands, compose with node transforms and share GPU geometry. CPU and native GPU regressions cover malformed data, sparse/interleaved accessors, backside normals, color space and replacement rollback. The original cabin is captured through direct import. See `Models.md` and `ModelReview.md`.

The Jolt-backed fixed-step physics foundation now includes capsule colliders, terrain heightfields, `PhysicsComponent` scene serialization, `SceneSimulation` synchronization, and snow-coupled drag/track stamping (`PhysicsTests`).

## Deformable snow, Lua API, and Vulkan snowfall milestone

Deterministic grid snow simulation (`SnowField`, `.bwsn` CRC32 persistence, `depth.u8`/`ice.u8` bridge import), `BellwetherAgent` snow commands (`SetSnowSettings`, `inspect-snow`, `stamp-snow`, `step-snow`), Lua snow/weather/transform bindings (21 APIs with joint `Scene` and `SnowField` rollback), GPU snow displacement/ice shading/subsurface/sparkle, and depth-tested wind-driven HDR snowfall particles (`SnowParticle.vert`/`.frag`) are implemented and verified locally on Windows RTX 3070 Vulkan (`SnowSimulation`, `CoreAndScripting`, `AgentCommands`, and `VulkanLifecycle` `23/23` snow renderer checks). See `SnowTechnology.md`.

Animation/skin import, IBL/SSAO, editor panels/gizmos/undo, audio gameplay integration, cross-platform macOS/Ubuntu runtime verification, and the full playable survival game port remain immediate required work. `GamePortInventory.json` preserves the source-system audit. These milestones establish neither full production readiness nor gameplay parity.
