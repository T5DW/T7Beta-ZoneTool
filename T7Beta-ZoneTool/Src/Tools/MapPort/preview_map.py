"""Export the extracted source map to GLB for geometry inspection."""
import io
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image


def export(source, output, world):
    data = bytearray()
    gltf = {
        "asset": {"version": "2.0", "generator": "T7Beta-ZoneTool source map preview"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "COD4 inches to glTF meters", "children": [],
                   "matrix": [0, 0, -.0254, 0, -.0254, 0, 0, 0,
                              0, .0254, 0, 0, 0, 0, 0, 1]}],
        "meshes": [], "materials": [], "accessors": [], "bufferViews": [],
        "textures": [], "images": [],
    }
    excluded_sky = set()
    materials = {}
    model_meshes = {}
    missing = {"models": set(), "materials": set(), "images": set()}

    def append(payload):
        data.extend(bytes((-len(data)) % 4))
        view = len(gltf["bufferViews"])
        gltf["bufferViews"].append({"buffer": 0, "byteOffset": len(data), "byteLength": len(payload)})
        data.extend(payload)
        return view

    def accessor(array, kind, component):
        array = np.ascontiguousarray(array)
        entry = {"bufferView": append(array.tobytes()), "componentType": component,
                 "count": len(array), "type": kind}
        if kind == "VEC3":
            entry.update(min=array.min(axis=0).tolist(), max=array.max(axis=0).tolist())
        result = len(gltf["accessors"])
        gltf["accessors"].append(entry)
        return result

    def safe_file(folder, name, extension):
        name = name.lstrip(",")
        if folder == "materials" and name.startswith("*"):
            name = "generated/" + name.replace("*", "_").split("(")[0]
        elif folder == "images":
            name = name.replace("*", "_")
        result = (source / folder / (name + extension)).resolve()
        if not result.is_relative_to(source.resolve()):
            raise ValueError("Asset path escapes extraction")
        return result

    def material(name):
        if name in materials:
            return materials[name]
        result = {"name": name, "doubleSided": True,
                  "pbrMetallicRoughness": {"metallicFactor": 0, "roughnessFactor": 1}}
        definition = safe_file("materials", name, ".json")
        if definition.exists():
            doc = json.loads(definition.read_text())
            states = doc.get("stateBits", [])
            if states:
                state = states[0]
                if state.get("srcBlendRgb") == "srcalpha" and state.get("dstBlendRgb") == "invsrcalpha":
                    result["alphaMode"] = "BLEND"
                elif state.get("alphaTest") in ("ge128", "gt0"):
                    result["alphaMode"] = "MASK"
                    result["alphaCutoff"] = .5 if state["alphaTest"] == "ge128" else .01
            for texture in doc.get("textures", []):
                if texture["semantic"] != "colorMap":
                    continue
                image_file = safe_file("images", texture["image"], ".dds")
                if image_file.exists():
                    with Image.open(image_file) as image:
                        stream = io.BytesIO()
                        image.convert("RGBA").save(stream, "PNG")
                    image_index = len(gltf["images"])
                    gltf["images"].append({"bufferView": append(stream.getvalue()), "mimeType": "image/png"})
                    gltf["textures"].append({"source": image_index})
                    result["pbrMetallicRoughness"]["baseColorTexture"] = {"index": image_index}
                else:
                    missing["images"].add(texture["image"])
                break
        else:
            missing["materials"].add(name)
        index = len(gltf["materials"])
        gltf["materials"].append(result)
        materials[name] = index
        return index

    def mesh(surfaces, name):
        primitives = []
        for surface in surfaces:
            definition = safe_file("materials", surface["material"], ".json")
            if definition.exists() and "sky" in json.loads(definition.read_text()).get("techniqueSet", "").lower():
                excluded_sky.add(surface["material"])
                continue
            vertices = surface["vertices"]
            p = np.asarray([v["position"] for v in vertices], dtype="<f4")
            n = np.asarray([v["normal"] for v in vertices], dtype="<f4")
            uv = np.asarray([v["uv"] for v in vertices], dtype="<f4")
            indices = np.asarray(surface["indices"], dtype="<u4")
            if not len(p) or indices.size % 3 or indices.max() >= len(p):
                raise ValueError("Invalid geometry")
            # IW3 front faces use clockwise winding. glTF uses counterclockwise.
            indices = indices.reshape(-1, 3)[:, ::-1].copy().reshape(-1)
            # glTF UV V increases downward. The exported IW3 UV uses the same convention.
            primitives.append({"attributes": {"POSITION": accessor(p, "VEC3", 5126),
                                                "NORMAL": accessor(n, "VEC3", 5126),
                                                "TEXCOORD_0": accessor(uv, "VEC2", 5126)},
                               "indices": accessor(indices, "SCALAR", 5125),
                               "material": material(surface["material"])})
        index = len(gltf["meshes"])
        gltf["meshes"].append({"name": name, "primitives": primitives})
        return index

    def node(name, mesh_index, matrix=None):
        entry = {"name": name, "mesh": mesh_index}
        if matrix is not None:
            entry["matrix"] = matrix.T.reshape(-1).tolist()
        gltf["nodes"][0]["children"].append(len(gltf["nodes"]))
        gltf["nodes"].append(entry)

    node("world", mesh(world["surfaces"], "world"))
    for row in world["models"]:
        name = row["model"]
        if name not in model_meshes:
            model = safe_file("xmodel", name, ".replay.json")
            if not model.exists():
                missing["models"].add(name)
                continue
            doc = json.loads(model.read_text())
            model_meshes[name] = mesh(doc["lods"][0]["surfaces"], name)
        transform = np.eye(4)
        transform[:3, :3] = np.asarray(row["axis"]).T * row["scale"]
        transform[:3, 3] = row["origin"]
        node(name, model_meshes[name], transform)
    data.extend(bytes((-len(data)) % 4))
    gltf["extras"] = {"sourceSkyMaterials": sorted(excluded_sky)}
    gltf["buffers"] = [{"byteLength": len(data)}]
    metadata = json.dumps(gltf, separators=(",", ":")).encode()
    metadata += b" " * ((-len(metadata)) % 4)
    with output.open("wb") as stream:
        stream.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(metadata) + 8 + len(data)))
        stream.write(struct.pack("<II", len(metadata), 0x4E4F534A))
        stream.write(metadata)
        stream.write(struct.pack("<II", len(data), 0x004E4942))
        stream.write(data)
    return {key: sorted(value) for key, value in missing.items()}
