# Static model and physics foundation review

Static import/material and editable-model changes received separate subagent review, parent fixes and native regressions. Review covered ownership, bounds, invalid data, containment, scene composition, bindings, color spaces and teardown.

Findings addressed:

- Kept shader/sampler handles alive through pipeline/binding creation. Registered frame and material descriptor spaces explicitly and consistently.
- Matched glTF counterclockwise winding; preserved the bridge's older color convention explicitly. Corrected backside tangent normals by reversing the final mapped normal.
- Avoided an upstream sparse-unpack stride assumption: packed replacements overlay validated base data separately. Added interleaved sparse, unsorted and misaligned fixtures.
- Added early linear hierarchy depth/cycle validation before dependency validation.
- Shared identical sampler states. A native test exercises more than 1,200 materials with two material sampler states. Combined imports enforce resource-count limits.
- Checked upload `waitForIdle()` results before replacing state. Device-loss injection/recovery remain unverified.
- Distinguished geometry from the tone-mapped clear color in capture validation. Sparse bright scenes are valid even with few dark pixels; semantic readback tests check material behavior separately.
- Reviewed optional model serialization, portable UTF-8 references, prefab/transform preservation and batch rollback. Shared commands remain the authoring interface.

Physics is a separate Jolt-backed foundation, pending playable actor, scene and Lua integration. Its review and executed limits are documented in `Physics.md`. The full contract remains in `ProductionPlan.md`. The unrelated existing Godot UID deletion is left untouched.
