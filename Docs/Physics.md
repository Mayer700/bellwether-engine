# Jolt rigid-body foundation

Bellwether has an independent C++ rigid-body world backed by the real Jolt Physics library. This milestone provides simulation and queries. It does **not** connect physics to the editor, entities, Lua, imported game colliders, the rendered runtime, player movement or saves yet. Those integration steps remain required for the full game port.

## Dependency and build contract

- Official source: [jrouwe/JoltPhysics](https://github.com/jrouwe/JoltPhysics).
- Selected release: **v5.5.0**, exact commit **23dadd0e603f1b321142d4c74df07fce85064989**. The upstream tag was resolved and the checkout verified locally.
- License: MIT, copyright 2021 Jorrit Rouwe; preserve the complete upstream `LICENSE` in exported dependency notices. The authoritative text is reproduced below.
- Build upstream's `Build/` subdirectory: its repository root has no `CMakeLists.txt`. The existing dependency helper can populate the locked source, followed by `add_subdirectory` of `jolt_SOURCE_DIR/Build`.
- Use `CROSS_PLATFORM_DETERMINISTIC=ON`, precise floating point arithmetic, floating point contraction disabled in the wrapper, and the matching transitive Jolt definitions. This option expresses build intent; it does not establish verified cross-platform determinism.
- Disable Jolt's override of project build flags, link-time optimization, installation targets and optional debug renderer/profiler. Keep C++ exceptions/RTTI enabled and use the same MSVC runtime choice as the engine. No AVX/SSE4/FMA requirement is introduced; the x86 baseline is SSE2.
- `PhysicsWorld.h` exposes GLM and standard types. Jolt headers are private implementation dependencies. Treat vendor headers as system headers; Bellwether source and tests retain strict warnings as errors.

The integration handoff was written to the ignored `Cache/PhysicsIntegration.cmake`; the main CMake file remains the authoritative checked-in configuration. Generated standalone drivers, third-party source, binaries and logs stay outside version control.

## Implemented behavior

`PhysicsWorld` owns one Jolt `PhysicsSystem`, a single-thread job system, a temporary malloc allocator and all bodies. Methods enforce the creating thread. Copy and move are disabled. Destruction removes and destroys every remaining body before destroying the system. A synchronized process registration owns the Jolt factory and type registry; its constructor unwinds partial registration and clears the factory pointer on an exception. Registration is released at process teardown after ordinary world lifetimes. Bellwether rejects an already-owned external Jolt factory rather than replacing it.

`CreateBody` accepts a static or dynamic box or sphere, position, unit quaternion, dimensions, mass, friction, restitution, initial linear velocity, sleeping preference and collision category/mask. Dynamic mass is supplied to Jolt's actual mass/inertia calculation. Velocity, impulse and transform setters update real rigid bodies. Static velocity/impulse requests fail explicitly.

Body handles are nonzero, monotonically allocated process-wide 64-bit values. A destroyed body or another world's handle cannot alias a reused Jolt slot. Failed creations may consume a handle; handles are never reused in the process. They are transient runtime identities and must not be written as persistent entity IDs in future saves.

Collision categories are indices 0 through 7. Both bodies' masks must admit the opposite category. Static/static pairs are omitted. Static and dynamic broadphase trees remain separate. Inserting, teleporting or removing a body wakes dynamic neighbors in the affected bounds, expanded by 0.1 metres for resting/speculative contacts. Teleports consider both old and new bounds. Neighbor candidates are sorted by backend body ID and checked against their actual bounds before activation so traversal order and widened broadphase boxes do not dictate activation order.

`Raycast(origin, displacement, categoryMask, ignoredBody)` queries a finite segment against Jolt's narrow phase. It returns the nearest owned body, hit position, normal and segment fraction. Equal-fraction hits choose the oldest stable body handle; backend slot reuse does not change that policy. The query mask selects body categories independently of the bodies' simulation masks. A zero mask returns no hit. An invalid ignored handle is an error. Convex shapes are solid: an origin inside a shape produces a zero-fraction hit. The normal in that case follows Jolt's interior convention and must not be interpreted as a measured surface contact.

## Time, bounds and failure behavior

`StepFixed()` advances exactly the configured tick; the default is 1/60 second. `Advance(elapsedSeconds)` retains fractional time and runs a bounded number of identical fixed ticks. It never passes variable frame time to Jolt. Zero time does nothing. Negative, NaN, infinite or excessive catch-up time is rejected before consuming the accumulator. The default catch-up limit is eight ticks per call. `GetInterpolationAlpha()` exposes the residual fraction; storing previous poses and rendering interpolated transforms remain separate integration work. Mixing manual ticks with `Advance` does not consume that residual fraction.

The supported input envelope uses positions within ±100000 metres per component, box half extents/radii from 0.01 to 10000 metres, mass from 0.001 to 1000000 kg, friction/restitution from zero to one, and total linear speed at most 500 m/s. An impulse is checked against the body's real inverse mass and current velocity before it can cause Jolt to clamp the resulting speed. The solver is still discrete; accepting a high speed is not a promise of continuous collision detection. Gravity components are bounded to ±1000 m/s². Fixed intervals must be between 1/240 and 1/15 second. Body capacity is 1 to 65536; body-pair/contact capacities are 4 to 1048576. Substep limits are 1 to 64.

The host must use round-to-nearest floating point mode; a step rejects a conflicting mode. Jolt's flush-denormal scope surrounds simulation. Following successful updates, dynamic positions, rotations and velocities are checked for finite values and the supported world envelope. Leaving the envelope faults the world instead of presenting a valid result.

Jolt returns `EPhysicsUpdateError` bits when its pair/contact/manifold capacities overflow. Jolt 5.5.0 Debug also asserts that this result is zero immediately before returning it. Bellwether's assertion callback logs the assertion and defers **only** that exact capacity-result assertion while the current thread is inside a guarded update. `StepFixed` then checks the real returned bits, reports them and permanently faults the world. Every unrelated Jolt assertion retains breakpoint behavior. This avoids disabling Debug checks while making documented capacity errors actionable. Unexpected assertions and allocation failure are not converted into fake success.

A capacity/state-envelope error can occur after the solver has advanced. There is no simulation rollback claim: the error explains that fact, and all subsequent public queries/mutations/steps fail. Destroy and recreate the world with corrected settings or content. Invalid user inputs and ordinary body-capacity exhaustion are rejected before changing existing simulation state. A full memory-budget/OOM policy and general transactional world loading remain pending.

## Verification and review

On Windows with MinGW GCC 16.1.0 and CMake 4.4.1, the isolated driver built the exact locked Jolt source and Bellwether wrapper in Debug and Release with Bellwether warnings as errors. `PhysicsFoundation` passed **105 checks, zero failures** in each configuration. The standalone commands were:

```powershell
cmake -S Engine/Cache/PhysicsDriver -B Engine/Cache/PhysicsDriver/Build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build Engine/Cache/PhysicsDriver/Build --parallel 6
ctest --test-dir Engine/Cache/PhysicsDriver/Build --output-on-failure
cmake -S Engine/Cache/PhysicsDriver -B Engine/Cache/PhysicsDriver/BuildRelease -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Engine/Cache/PhysicsDriver/BuildRelease --parallel 6
ctest --test-dir Engine/Cache/PhysicsDriver/BuildRelease --output-on-failure
```

The tests exercise gravity, box/sphere floor contact and sleeping, dynamic/dynamic contact, high collision category bits, both sides of collision masks, configured mass/impulse response, zero gravity, transforms, query hit/miss/mask/ignore/tie cases, finite segment length, interior rays, handle recycling/world isolation, invalid input preservation, wrong-thread rejection, fixed-time accumulation, host rounding, capacity recovery and solver-overflow faulting. A collision-stack replay includes frame batching, impulses, body destruction/replacement and support teleportation, checking all dynamic poses/velocities/sleep states against an independently constructed world. Support removal/movement and overlapping static insertion are verified with genuinely sleeping bodies. Repeated active-body teardown leaves other worlds usable.

Review identified and fixed these concrete issues before handoff: global factory pointer on registration failure; vector-speed/impulse clamp preflight; quaternion tolerance matching Jolt; too-small upstream hash capacities; the documented Debug capacity assertion preceding its error result; support changes leaving sleepers suspended; nondeterministic broadphase wake/query tie ordering; and unvalidated post-update state leaving the world envelope. Formatting and focused Debug/Release tests were repeated after fixes. This is foundation evidence on the tested Windows compiler, not a platform, gameplay or production-readiness claim.

## Still required

Entity/body components and serialization; structured authoring commands and editor tools; Lua access; transactional scene-to-world synchronization; per-entity lifecycles; runtime pose/render integration; original terrain and 727 exported collider shapes; heightfields/mesh colliders; character controller, slope/step handling and snow movement; contact events/triggers; CCD and sweep/shape queries; constraints and kinematic motion; save migration; measured game-scale memory/performance; cross-platform builds and replay verification. No physics APIs are declared implemented in the main feature scene or scripting API by this independent foundation alone.

Primary references: [Jolt architecture and determinism](https://jrouwe.github.io/JoltPhysics/), [the pinned HelloWorld integration](https://github.com/jrouwe/JoltPhysics/blob/23dadd0e603f1b321142d4c74df07fce85064989/HelloWorld/HelloWorld.cpp), and [the pinned update error contract](https://github.com/jrouwe/JoltPhysics/blob/23dadd0e603f1b321142d4c74df07fce85064989/Jolt/Physics/EPhysicsUpdateError.h).

## Upstream MIT notice

Copyright 2021 Jorrit Rouwe

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
