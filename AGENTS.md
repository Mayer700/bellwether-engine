# Bellwether Engine

## Architecture and style

- C++20, CMake; a static core library, separate editor, runtime and agent CLI. GLFW + NVRHI/Vulkan, GLM, miniaudio, Lua. Physics selection must include deterministic tests and component/script access.
- Follow Hazel naming: PascalCase namespaces, classes, functions, enums and source filenames; camelCase locals and parameters; private fields `m_Name`, static variables `s_Name`. Use descriptive names, Allman braces and tabs. Do not copy Hazel source or its license assumptions.
- Keep platform and GPU code behind explicit interfaces. Own resources with RAII. Check failures and return actionable diagnostics; no silent fallback or swallowed validation errors. No undefined behavior, unbounded waits, resource leaks or unchecked data conversions.
- Pin dependencies; preserve licenses. Keep fetched vendor/build/generated content out of Git. Do not add paid or online runtime dependencies.

## Test and delivery gates

- Each component and Lua API must have automated tests and appear in `Scenes/FeatureTest.bwscene` or its test script. Adding an API also adds its regression test. Test invalid handles, destruction/spawning during iteration, invalid input, failure rollback and serialization.
- Run configure, compile and CTest. For graphics, also run a real Vulkan test with NVRHI validation, readback/capture and clean shutdown. Headless core tests do not prove rendering.
- Register pending features in `Docs/ProductionPlan.md`; never make an unimplemented API return fake success. Unsupported behavior must produce a clear error.
- Changes must receive a documented code review, fixes, and revalidation before commit. Stage explicit paths, inspect the staged diff, scan for credentials and generated files. Never commit secrets, player saves, binaries or vendor repositories.
- Do not force push, rewrite remote history, overwrite another project or create substitute GitHub repositories. The requested remote is `https://github.com/TheCherno/AIGameEngine`; record access failure and continue locally if unavailable.
- Windows, macOS and Ubuntu 24+ are required targets. A platform is verified only after its build and tests run there. CI configuration alone is not evidence. NVRHI upstream currently documents Windows/Linux; macOS/MoltenVK needs explicit compatibility work and verification.

## Agent authoring

The editor and CLI must share the same scene commands and validation. Scene files and structured command batches are the supported automation interface; no UI-only authoring capabilities. Commands must validate first, apply transactionally, and report stable entity IDs and errors.
