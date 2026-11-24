"""Extract an IW3 map and preserve its source data for the T7 beta map port."""
import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def parse_entities(text):
    """Read Quake entity blocks while preserving every original key/value."""
    tokens = re.findall(r'//[^\n]*|"(?:\\.|[^"\\])*"|[{}]|\S+', text)
    entities = []
    current = None
    key = None
    for token in tokens:
        if token.startswith("//"):
            continue
        if token == "{":
            if current is not None:
                raise ValueError("Nested entity block")
            current = []
        elif token == "}":
            if current is None or key is not None:
                raise ValueError("Incomplete entity block")
            entities.append(current)
            current = None
        elif current is None:
            raise ValueError("Token outside entity block")
        elif not token.startswith('"'):
            raise ValueError("Unquoted entity field")
        else:
            # COD entity strings may contain backslashes that are not JSON escapes.
            value = token[1:-1].replace(r'\"', '"')
            if key is None:
                key = value
            else:
                current.append([key, value])
                key = None
    if current is not None:
        raise ValueError("Unclosed entity block")
    return entities


def source_file(root, suffix):
    found = sorted(root.rglob("*" + suffix))
    if len(found) != 1:
        raise ValueError(f"Expected one {suffix} export, found {len(found)}")
    return found[0]


def prepare(source, output):
    world_file = source_file(source, ".replay-world.json")
    collision_file = source_file(source, ".replay-collision.json")
    world = read_json(world_file)
    collision = read_json(collision_file)
    if world.get("schema") != 1 or collision.get("schema") != 1:
        raise ValueError("Unsupported source exporter schema")
    surfaces = world["surfaces"]
    if not surfaces:
        raise ValueError("Source world contains no geometry")
    missing_models = sorted({
        row["model"] for row in world["models"]
        if not (source / "xmodel" / (row["model"] + ".replay.json")).is_file()
    })
    index_count = vertex_count = 0
    groups = []
    with (output / "world.bin").open("wb") as binary:
        # Portable source interchange. Native GfxWorld serialization is a later stage.
        binary.write(b"T7MAPSRC")
        binary.write(struct.pack("<II", 1, len(surfaces)))
        for number, surface in enumerate(surfaces):
            vertices = surface["vertices"]
            positions = np.asarray([v["position"] for v in vertices], dtype="<f4")
            normals = np.asarray([v["normal"] for v in vertices], dtype="<f4")
            uv = np.asarray([v["uv"] for v in vertices], dtype="<f4")
            light_uv = np.asarray([v["lightmap_uv"] for v in vertices], dtype="<f4")
            indices = np.asarray(surface["indices"], dtype="<u4")
            if (positions.shape != (len(vertices), 3) or not len(vertices)
                    or indices.size % 3 or not indices.size
                    or indices.max() >= len(vertices)):
                raise ValueError(f"Invalid source world surface {number}")
            packed = np.column_stack((positions, normals, uv, light_uv)).astype("<f4")
            if not np.isfinite(packed).all():
                raise ValueError(f"Non-finite vertex on surface {number}")
            groups.append({
                "sourceSurface": number, "material": surface["material"],
                "lightmap": surface["lightmap"], "offset": binary.tell(),
                "vertices": len(vertices), "indices": indices.size,
            })
            binary.write(struct.pack("<II", len(vertices), indices.size))
            binary.write(packed.tobytes())
            binary.write(indices.tobytes())
            vertex_count += len(vertices)
            index_count += indices.size
    collision_vertices = np.asarray(collision["vertices"], dtype=float)
    triangles = np.asarray(collision["triangles"], dtype=np.int64)
    if triangles.size and (triangles.min() < 0 or triangles.max() >= len(collision_vertices)):
        raise ValueError("Invalid source collision triangle")
    entity_file = source_file(source, ".ents")
    entities = parse_entities(entity_file.read_text(encoding="utf-8", errors="strict"))
    spawn_counts = {}
    for entity in entities:
        classname = dict(entity).get("classname", "")
        if "spawn" in classname or classname.startswith("mp_"):
            spawn_counts[classname] = spawn_counts.get(classname, 0) + 1
    from preview_map import export
    preview_missing = export(source, output / "source-preview.glb", world)
    source_images = set()
    for material_file in (source / "materials").rglob("*.json"):
        for texture in read_json(material_file).get("textures", []):
            name = texture.get("image", "").lstrip(",")
            if name and not (source / "images" / (name.replace("*", "_") + ".dds")).is_file():
                source_images.add(name)
    report = {
        "schema": 1, "name": world["name"], "stage": "source-prepared",
        "target": "T7 August 21 2015 PC", "playable": False,
        "worldBounds": world["bounds"], "worldSurfaces": len(surfaces),
        "vertices": vertex_count, "triangles": index_count // 3,
        "staticModelInstances": len(world["models"]), "missingModels": missing_models, "previewMissing": preview_missing,
        "collisionBrushes": len(collision["brushes"]),
        "collisionTriangles": len(collision["triangles"]),
        "entityCount": len(entities), "spawnCounts": spawn_counts,
        "pending": ["T7 beta GfxWorld serialization", "T7 beta collision serialization",
                    "T7 map materials, lighting and static-model conversion",
                    "T7 map script and game-mode entity integration",
                    "T7 bot navigation conversion",
                    "T7 native map registration and loading", "In-game validation"],
        "missingSourceImages": sorted(source_images),
        "sourceHashes": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in (world_file, collision_file)},
    }
    for filename, data in [("surfaces.json", groups), ("entities.json", entities),
                           ("map-port.json", report)]:
        (output / filename).write_text(json.dumps(data, indent=2), encoding="utf-8")
    shutil.copyfile(collision_file, output / "collision.source.json")
    shutil.copyfile(world_file, output / "world.source.json")
    print(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Existing patched OAT export")
    parser.add_argument("--fastfile", type=Path, help="IW3 .ff to extract")
    parser.add_argument("--unlinker", type=Path, help="OAT v0.33.0 with map exporters")
    parser.add_argument("--search-path", action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        raise ValueError("Choose a fresh output directory")
    if bool(args.source) == bool(args.fastfile):
        raise ValueError("Choose --source or --fastfile")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=output.name + "-", dir=output.parent) as work:
        staging = Path(work)
        source = args.source
        if args.fastfile:
            if not args.unlinker or not args.unlinker.is_file():
                raise ValueError("--unlinker must point to the patched OAT executable")
            source = staging / "extracted"
            source.mkdir()
            searches = [str(args.fastfile.resolve().parent), *args.search_path]
            command = [str(args.unlinker.resolve()), "--no-color", "--image-format", "DDS",
                       "--model-format", "GLB", "--search-path", ";".join(searches),
                       "-o", str(source), str(args.fastfile.resolve())]
            with (staging / "extraction.log").open("w", encoding="utf-8") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                diagnostic = (staging / "extraction.log").read_text(encoding="utf-8", errors="replace")
                raise RuntimeError(f"Unlinker exited {result.returncode}:\n{diagnostic[-8000:]}")
        result = staging / "prepared"
        result.mkdir()
        prepare(source.resolve(), result)
        # Keep original exports, images, lighting, model meshes and scripts together.
        shutil.copytree(source, result / "source")
        if (staging / "extraction.log").exists():
            shutil.copyfile(staging / "extraction.log", result / "extraction.log")
        result.rename(output)


if __name__ == "__main__":
    main()
