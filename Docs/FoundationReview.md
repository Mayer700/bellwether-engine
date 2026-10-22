# Foundation review

Scope: C++ scene core, agent CLI, Lua API, Vulkan/NVRHI lifecycle, intermediate static-world rendering, dependency/bootstrap and current tests. This is a source review of the foundation, not approval of the unfinished engine as a production release.

## Findings addressed before commit

- Replaced the superseded raylib direction with the requested GLFW/NVRHI/Vulkan stack and a separate authoritative repository.
- Scene loading now rejects unknown fields/components, duplicate IDs, fractional IDs, non-finite vectors and singular transforms. Commands validate against strict schemas and apply transactionally.
- Agent `create` refuses to overwrite an existing scene. CLI integration checks real saved output and verifies that failed batches preserve the previous file hash.
- File writes use unique, exclusively-created temporary files, checked partial writes, durable flush and platform replacement. A concurrent writer regression verifies complete documents.
- Lua errors roll back scene mutations and fault the VM. Instruction and memory budgets are tested. Scripts cannot intercept budget errors or access host file/process libraries. C++ exception scopes end before Lua error propagation can longjmp.
- Fixed duplicate push-constant bindings, the volatile constant-buffer layout mismatch, undefined final image states and query reuse after swapchain recreation. Both depth and color formats participate in the pipeline key.
- GPU tests read actual images, including a separate material/transform/depth fixture and the original static world; passing startup alone is insufficient. Shutdown is idempotent and rendering after shutdown rejects explicitly.
- Mesh/terrain inputs have size, index, finite-value and material validation. Unsupported framebuffer configurations fail explicitly.
- Dependency fetching checks exact commits, refuses modified local checkouts and verifies TLS. A separate Release tree fetches clean sources through Git without relying on local Vendor overrides. Certificate verification was not disabled when the supplied CMake build lacked CA trust anchors.
- C++ was formatted with clang-format 19.1.7 under the committed configuration. Include order is preserved because Vulkan declarations must precede GLFW's Vulkan API declarations. Engine sources build with warnings as errors.

## Required follow-up

The renderer currently displays material constants and static geometry with direct-light PBR equations and tone mapping. It does not provide the requested HDR render graph, shadows, IBL, SSAO, textures or animated glTF workflow yet. Editor authoring UI/gizmos, miniaudio, physics, per-entity scripting lifecycle, export packaging and the full survival-game port remain required work. GPU-loss recovery and broader platform/device tests are not complete. NVRHI diagnostics are enabled; this does not establish a clean Khronos validation-layer run.

Only Windows/RTX 3070 has been executed locally. The macOS and Ubuntu CI definitions are unexecuted. The requested GitHub repository returns 404, so a local reviewed commit does not imply publication.

See `Evidence/Foundation.json` for the matching source hashes, build/test outcomes and capture results recorded after this review's fixes.
