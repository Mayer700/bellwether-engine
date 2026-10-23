# Renderer source review

Scope: HDR targets, depth-only sun passes, tone mapping, renderer settings, shader resource discovery, static-mesh bounds, asset failure handling and the associated tests. This review approves the bounded change for a local development commit, not the unfinished engine as a production release.

Findings fixed before commit:

- Removed in-fragment tone mapping from the lighting pass. Scene radiance is retained in a half-float target and sampled by an explicit output pass; readback verifies values greater than one and the output transfer.
- Used separate shadow-only bindings so a depth attachment is not simultaneously sampled. Shadow maps have explicit tracked entry/exit states and are initialized even when the setting is disabled.
- Matched light projection to NVRHI's inverted Vulkan viewport. Independent ground-point checks catch a flipped or displaced shadow projection.
- Renderer settings reject unknown fields, invalid numeric ranges, zero or overflowing sun vectors, and malformed data. Partial agent updates preserve unrelated preferences; whole-batch rollback is retained.
- Replaced origin-only mesh culling with conservative transformed bounds. A frontier view now retains the terrain even when its mesh origin is far from the camera.
- Corrupted-asset tests exposed an abandoned immediate command list after import failure. Import now completes CPU validation before recording a non-immediate upload transaction. Later frames are checked against the previous image after all rejection cases.
- Rejected non-affine/overflowing transforms, non-finite geometry and invalid HDR material values. Relative asset paths also reject rooted paths and canonical link escapes.
- Checked resource creation before dereference and committed resized HDR targets only after their bindings and pipeline are valid.
- Removed the compiled absolute shader directory. Platform executable-location APIs supply the resource root; a relocated runtime passes the native replay.

Remaining limits: fixed camera/render host, incomplete material/animation import, finite terrain edges, no IBL/SSAO/local-light production pipeline, no complete editor/physics/audio/export or game port. Windows RTX 3070 is the executed platform. GPU allocation failure/device-loss recovery and Khronos validation are not proven by these tests. The existing deleted Godot UID sidecar is unrelated to this renderer change and is left untouched.
