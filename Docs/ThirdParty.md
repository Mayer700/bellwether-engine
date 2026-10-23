# Third party dependencies

All dependencies are fetched into the ignored Vendor/ or build dependency directory and retain their own notices. They are not authored engine code. The CMake file pins revisions; changes must receive review and revalidation.

| Dependency | Revision | License / purpose |
|---|---|---|
| GLFW | 7b6aead9fb88b3623e3b3725ebb42670cbe4c579 | zlib/libpng; window, input, Vulkan surfaces |
| NVRHI | 6b96fb03e07539f08327aea76c56d55f1de9d906 | MIT; graphics abstraction and validation |
| Vulkan-Headers | 015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9 | Upstream Apache-2.0/MIT notices; Vulkan declarations |
| GLM | 0af55ccecd98d4e5a8d1fad7de25ba429d60e863 | Upstream MIT/Happy Bunny options; math |
| Lua | 6e22fedb74cf0c9b6656e9fce8b7331db847c605 | MIT; scripting |
| nlohmann/json | 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 | MIT; scene/command interchange |
| glslang | 1062752a891c95b2bfeed9e356562d88f9df84ac | Preserve upstream LICENSE.txt and component notices; build-time SPIR-V compilation |

Miniaudio remains planned and is not linked. Jolt Physics is now linked for the isolated rigid-body foundation. Runtime/export packaging must copy complete applicable notices, rather than relying on this abbreviated list.

Static model import adds three pinned upstream dependencies, fetched with the same verified-TLS process:

- cgltf, `jkuhlmann/cgltf` at `85cd62382dfea638278962690cf515023f33ed00`: MIT; retain its `LICENSE` (Johannes Kuhlmann copyright).
- stb, `nothings/stb` at `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`: MIT alternative selected; retain its `LICENSE`. Only `stb_image` PNG/JPEG memory decoding is built.
- MikkTSpace, `mmikk/MikkTSpace` at `3e895b49d05ea07e4c2133156cfa94369e19e409`: upstream zlib-style notice in `mikktspace.h` and `mikktspace.c`, copyright Morten S. Mikkelsen. Both files remain unmodified. Retain the full notices in source distributions and exported license bundles.
- Jolt Physics, `jrouwe/JoltPhysics` v5.5.0 at `23dadd0e603f1b321142d4c74df07fce85064989`: MIT, copyright Jorrit Rouwe; retain upstream `LICENSE`. The build disables optional host-specific AVX features, enables the deterministic configuration and keeps matching dependency definitions on the wrapper.

`Source/ThirdParty/AssetDependencies.c` defines the upstream implementation macros; it contains no copied dependency implementation. Generated model/texture fixtures are original test content authored by `Tools/BuildModelFixtures.py`.
