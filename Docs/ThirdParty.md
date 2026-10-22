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

Miniaudio and a physics backend are selected/planned but not yet linked. Runtime/export packaging must copy the complete applicable dependency notices, rather than relying on this abbreviated table.
