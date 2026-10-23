"""Create original, deterministic glTF/GLB import and rendering fixtures."""
import base64
import copy
import json
import pathlib
import struct
import sys
import zlib


def png(width, height, pixels):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))
    scanlines = b"".join(b"\0" + bytes(pixels[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def uri(data, mime="application/octet-stream"):
    return "data:" + mime + ";base64," + base64.b64encode(data).decode("ascii")


def glb(document, binary):
    document = copy.deepcopy(document)
    document["buffers"][0].pop("uri", None)
    encoded = json.dumps(document, separators=(",", ":")).encode("utf-8")
    encoded += b" " * (-len(encoded) % 4)
    binary += b"\0" * (-len(binary) % 4)
    return (struct.pack("<III", 0x46546C67, 2, 28 + len(encoded) + len(binary))
            + struct.pack("<II", len(encoded), 0x4E4F534A) + encoded
            + struct.pack("<II", len(binary), 0x004E4942) + binary)


def build(destination):
    destination.mkdir(parents=True, exist_ok=True)
    # Position, normal, UV; stride intentionally differs from the engine layout.
    vertices = [(-1, 0, -1, 0, 1, 0, 0, 0), (1, 0, -1, 0, 1, 0, 1, 0),
                (1, 0, 1, 0, 1, 0, 1, 1), (-1, 0, 1, 0, 1, 0, 0, 1)]
    binary = b"".join(struct.pack("<8f", *vertex) for vertex in vertices) + struct.pack("<6H", 0, 2, 1, 0, 3, 2)
    quadrants = png(2, 2, [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 128, 128, 128, 255])
    flat = png(1, 1, [128, 128, 255, 255])
    tilted = png(1, 1, [218, 128, 218, 255])
    orm = png(1, 1, [128, 64, 255, 255])
    alpha = png(2, 2, [255, 255, 255, 0, 255, 255, 255, 255] * 2)
    document = {
        "asset": {"version": "2.0", "generator": "Bellwether deterministic fixture"},
        "buffers": [{"byteLength": len(binary), "uri": uri(binary)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 128, "byteStride": 32},
                        {"buffer": 0, "byteOffset": 128, "byteLength": 12}],
        "accessors": [
            {"bufferView": 0, "byteOffset": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1, 0, -1], "max": [1, 0, 1]},
            {"bufferView": 0, "byteOffset": 12, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 0, "byteOffset": 24, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR"}],
        "images": [{"uri": uri(image, "image/png")} for image in [quadrants, flat, tilted, orm, alpha]],
        "samplers": [{"magFilter": 9728, "minFilter": 9728, "wrapS": 33071, "wrapT": 33071}],
        "textures": [{"source": index, "sampler": 0} for index in range(5)],
        "materials": [], "meshes": [], "nodes": [], "scenes": [{"nodes": []}], "scene": 0,
    }
    materials = [
        {"name": "BaseColor", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0}},
        {"name": "Emissive", "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 0, 1], "metallicFactor": 0},
         "emissiveTexture": {"index": 0}, "emissiveFactor": [1, 1, 1]},
        {"name": "Occlusion", "pbrMetallicRoughness": {"metallicFactor": 0}, "occlusionTexture": {"index": 3}},
        {"name": "FlatNormal", "pbrMetallicRoughness": {"metallicFactor": 0}, "normalTexture": {"index": 1}},
        {"name": "TiltedNormal", "pbrMetallicRoughness": {"metallicFactor": 0}, "normalTexture": {"index": 2}},
        {"name": "MetalRough", "pbrMetallicRoughness": {"metallicRoughnessTexture": {"index": 3}}},
    ]
    for index, material in enumerate(materials):
        # Explicitly single sided: the GPU check also verifies glTF winding.
        document["materials"].append(material)
        document["meshes"].append({"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": index}]})
        document["nodes"].append({"name": material["name"], "mesh": index,
                                  "translation": [-4 + 4 * (index % 3), 0, 10 + 4 * (index // 3)]})
        document["scenes"][0]["nodes"].append(index)

    def write(name, value):
        (destination / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    write("Materials.gltf", document)
    (destination / "Materials.glb").write_bytes(glb(document, binary))
    external = copy.deepcopy(document)
    external["buffers"][0]["uri"] = "geometry%20buffer.bin"
    (destination / "geometry buffer.bin").write_bytes(binary)
    external["images"][0] = {"uri": "quadrants.png"}
    (destination / "quadrants.png").write_bytes(quadrants)
    write("External.gltf", external)
    embedded = copy.deepcopy(document)
    while len(binary) % 4:
        binary += b"\0"
    embedded["bufferViews"].append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(quadrants)})
    embedded["images"][0] = {"bufferView": 2, "mimeType": "image/png"}
    binary += quadrants
    embedded["buffers"][0]["byteLength"] = len(binary)
    (destination / "Embedded.glb").write_bytes(glb(embedded, binary))

    def variant(name, edit):
        value = copy.deepcopy(document)
        edit(value)
        write(name + ".gltf", value)

    variant("NoNormals", lambda d: d["meshes"][0]["primitives"][0]["attributes"].pop("NORMAL"))
    variant("Hierarchy", lambda d: (d["nodes"].append({"translation": [2, 3, 4], "children": [0]}),
                                    d["scenes"][0].update(nodes=[6, 1, 2, 3, 4, 5])))
    variant("Mirrored", lambda d: d["nodes"][0].update(scale=[-1, 2, 1]))
    variant("Repeat", lambda d: d["samplers"][0].update(wrapS=10497, wrapT=33648, minFilter=9987, magFilter=9729))
    variant("InvalidIndex", lambda d: d["accessors"][3].update(count=7))
    variant("InvalidView", lambda d: d["bufferViews"][0].update(byteOffset=99999))
    variant("HugeAccessor", lambda d: d["accessors"][0].update(count=2**63))
    variant("RequiredExtension", lambda d: d.update(extensionsRequired=["KHR_draco_mesh_compression"]))
    variant("Blend", lambda d: d["materials"][0].update(alphaMode="BLEND"))
    variant("MissingUV", lambda d: d["meshes"][0]["primitives"][0]["attributes"].pop("TEXCOORD_0"))
    variant("BadImage", lambda d: d["images"][0].update(uri=uri(b"invalid", "image/png")))
    variant("Traversal", lambda d: d["buffers"][0].update(uri="../escape.bin"))
    variant("EncodedTraversal", lambda d: d["buffers"][0].update(uri="%2e%2e/escape.bin"))
    variant("Absolute", lambda d: d["buffers"][0].update(uri="/escape.bin"))
    variant("Network", lambda d: d["buffers"][0].update(uri="https://example.com/file.bin"))
    variant("NulURI", lambda d: d["buffers"][0].update(uri="geometry%00buffer.bin"))
    variant("MalformedURI", lambda d: d["buffers"][0].update(uri="geometry%Q0buffer.bin"))
    variant("ShortBase64", lambda d: d["buffers"][0].update(uri="data:application/octet-stream;base64,AA=="))
    variant("InvalidBase64", lambda d: d["buffers"][0].update(uri="data:application/octet-stream;base64,!AAA"))
    variant("Singular", lambda d: d["nodes"][0].update(scale=[0, 1, 1]))
    variant("Cycle", lambda d: d["nodes"][0].update(children=[0]))
    variant("NegativeFactor", lambda d: d["materials"][0]["pbrMetallicRoughness"].update(metallicFactor=-1))
    variant("UnsupportedUV", lambda d: d["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"].update(texCoord=1))
    variant("Morph", lambda d: d["meshes"][0]["primitives"][0].update(targets=[{"POSITION": 0}]))
    variant("Animated", lambda d: d.update(animations=[{"channels": [], "samplers": []}]))
    variant("Skinned", lambda d: d.update(skins=[{"joints": [0]}]))
    variant("ManyMaterials", lambda d: d["materials"].extend(copy.deepcopy(d["materials"][0]) for _ in range(1200)))
    geometry = base64.b64decode(document["buffers"][0]["uri"].split(",")[1])
    colors = copy.deepcopy(document)
    colors["bufferViews"].append({"buffer": 0, "byteOffset": len(geometry), "byteLength": 16})
    colors["accessors"].append({"bufferView": 2, "componentType": 5121, "normalized": True, "count": 4, "type": "VEC4"})
    colors["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"] = 4
    color_data = geometry + bytes([128, 255, 0, 255] * 4)
    colors["buffers"][0].update(byteLength=len(color_data), uri=uri(color_data))
    write("VertexColors.gltf", colors)
    sparse = copy.deepcopy(document)
    sparse["bufferViews"] += [{"buffer": 0, "byteOffset": len(geometry), "byteLength": 4},
                              {"buffer": 0, "byteOffset": len(geometry) + 4, "byteLength": 48}]
    sparse["accessors"][0].pop("bufferView")
    sparse["accessors"][0]["sparse"] = {"count": 4, "indices": {"bufferView": 2, "componentType": 5121}, "values": {"bufferView": 3}}
    sparse_data = geometry + bytes(range(4)) + b"".join(struct.pack("<3f", *vertex[:3]) for vertex in vertices)
    sparse["buffers"][0].update(byteLength=len(sparse_data), uri=uri(sparse_data))
    write("Sparse.gltf", sparse)
    sparse["accessors"][0]["bufferView"] = 0
    write("InterleavedSparse.gltf", sparse)
    invalid_sparse = copy.deepcopy(sparse)
    invalid_sparse_data = geometry + bytes([0, 0, 2, 3]) + sparse_data[len(geometry) + 4:]
    invalid_sparse["buffers"][0]["uri"] = uri(invalid_sparse_data)
    write("UnsortedSparse.gltf", invalid_sparse)
    invalid_sparse["accessors"][0]["sparse"]["values"]["byteOffset"] = 2
    write("MisalignedSparse.gltf", invalid_sparse)
    deep = copy.deepcopy(document)
    deep["nodes"] = [{"children": [index + 1]} for index in range(9999)] + [{"mesh": 0}]
    deep["scenes"][0]["nodes"] = [0]
    write("Deep.gltf", deep)
    nan_data = struct.pack("<f", float("nan")) + geometry[4:]
    variant("NaNVertex", lambda d: d["buffers"][0].update(uri=uri(nan_data)))
    bad_index_data = geometry[:-2] + struct.pack("<H", 65535)
    variant("OutOfRangeIndex", lambda d: d["buffers"][0].update(uri=uri(bad_index_data)))

    mask = copy.deepcopy(document)
    mask["materials"] = [{"name": "Cutout", "pbrMetallicRoughness": {"baseColorTexture": {"index": 4}, "metallicFactor": 0},
                          "alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True}]
    mask["meshes"] = [copy.deepcopy(document["meshes"][0])]
    mask["nodes"] = [{"name": "Cutout", "mesh": 0, "translation": [0, 2, 10], "scale": [3, 1, 3]}]
    mask["scenes"][0]["nodes"] = [0]
    write("Masked.gltf", mask)
    mask["materials"][0]["alphaMode"] = "OPAQUE"
    write("Opaque.gltf", mask)
    minified = copy.deepcopy(document)
    checker = []
    for y in range(256):
        for x in range(256):
            value = 255 if (x + y) % 2 else 0
            checker.extend([value, value, value, 255])
    minified["images"][0] = {"uri": uri(png(256, 256, checker), "image/png")}
    minified["samplers"][0].update(minFilter=9987, magFilter=9729)
    minified["nodes"] = [{"name": "LinearMipTest", "mesh": 1, "translation": [0, 0, 12]}]
    minified["scenes"][0]["nodes"] = [0]
    write("Minified.gltf", minified)
    backside = copy.deepcopy(document)
    backside["materials"][4]["doubleSided"] = True
    backside["nodes"] = [{"name": "BacksideNormal", "mesh": 4, "translation": [0, 0, 14], "rotation": [1, 0, 0, 0]}]
    backside["scenes"][0]["nodes"] = [0]
    write("Backside.gltf", backside)
    settings = {"Shadows": False, "FogDensity": 0, "SunRadiance": [0, 0, 0], "CameraTarget": [0, 0, 12], "CameraSize": 18}
    def entity(identifier, model="Materials.glb", translation=None):
        return {"ID": identifier, "Name": "Model instance", "Model": {"Asset": model}, "Script": "",
                "Transform": {"Translation": translation or [0, 0, 0], "Rotation": [0, 0, 0], "Scale": [1, 1, 1]}}
    write("Scene.bwscene", {"Format": 1, "NextID": 2, "Renderer": settings, "Entities": [entity(1)]})
    write("MultiScene.bwscene", {"Format": 1, "NextID": 3, "Renderer": settings, "Entities": [entity(1), entity(2, translation=[4, 0, 0])]})
    write("Fixture.json", {"Fixture": "BellwetherModels_v1"})
    (destination / "Truncated.glb").write_bytes((destination / "Materials.glb").read_bytes()[:-8])
    (destination.parent / "escape.bin").write_bytes(bytes(range(64)))
    write("Settings.json", settings)
    print("BELLWETHER_MODEL_FIXTURES", destination)


if __name__ == "__main__":
    build(pathlib.Path(sys.argv[1]))
