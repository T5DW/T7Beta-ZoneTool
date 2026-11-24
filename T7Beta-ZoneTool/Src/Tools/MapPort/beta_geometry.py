"""Stage IW3 world geometry against a validated T7 beta buffer layout.

This produces compiler inputs, NOT an engine-loadable zone or GfxWorld asset.
Material linking, visibility, lighting and unknown surface fields remain unresolved.
"""
import argparse
import hashlib
import json
import shutil
import struct
import tempfile
from pathlib import Path

import numpy as np

from beta_snapshot import Snapshot


def pack_direction(vector):
    vector = np.asarray(vector, dtype=np.float64)
    if vector.shape != (3,) or not np.isfinite(vector).all():
        raise ValueError('Invalid source tangent-space vector')
    length = np.linalg.norm(vector)
    if length < 1e-6:
        raise ValueError('Zero source tangent-space vector')
    vector /= length
    fields = np.clip(np.rint(vector * 511 + 512), 0, 1023).astype(np.uint32)
    return int(fields[0] | fields[1] << 10 | fields[2] << 20)


def stage(prepared, snapshot, output):
    prepared, output = Path(prepared), Path(output)
    if output.exists():
        raise ValueError('Choose a fresh output folder')
    reference = Snapshot(snapshot)
    checked = reference.validate()
    world = json.loads((prepared / 'world.source.json').read_text())
    if world.get('schema') != 1 or not world.get('surfaces'):
        raise ValueError('Unsupported source geometry schema')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='t7geom-', dir=output.parent) as tmp:
        root = Path(tmp)
        blueprints = []
        vertex_total = index_total = 0
        with (root / 'world_vertex_stream0.bin').open('wb') as positions, \
                (root / 'world_vertex_stream1.candidate.bin').open('wb') as attributes, \
                (root / 'world_indices.bin').open('wb') as indices, \
                (root / 'world_lightmap_uv.source.bin').open('wb') as light_uv:
            for number, surface in enumerate(world['surfaces']):
                vertices = surface['vertices']
                if not vertices or len(vertices) > 65535:
                    raise ValueError(f'Surface {number} needs vertex partitioning')
                original_indices = surface['indices']
                if any(type(i) is not int for i in original_indices):
                    raise ValueError(f'Surface {number} has non-integer indices')
                local_indices = np.asarray(original_indices, dtype=np.int64)
                if not local_indices.size or local_indices.size % 3 or local_indices.min() < 0 or local_indices.max() >= len(vertices):
                    raise ValueError(f'Invalid source indices on surface {number}')
                if len(local_indices) // 3 > 65535:
                    raise ValueError(f'Surface {number} needs triangle partitioning')
                points = np.asarray([v['position'] for v in vertices], dtype='<f4')
                if points.shape != (len(vertices), 3) or not np.isfinite(points).all():
                    raise ValueError(f'Invalid source positions on surface {number}')
                recipes = {
                    'sourceSurface': number, 'material': surface['material'],
                    'positionByteOffset': positions.tell(), 'attributeByteOffset': attributes.tell(),
                    'baseIndex': index_total, 'triangleCount': len(local_indices) // 3,
                    'sourceVertexCount': len(vertices),
                    'mins': points.min(0).tolist(), 'maxs': points.max(0).tolist(),
                    'inflatedMins': (points.min(0) - 1).tolist(),
                    'inflatedMaxs': (points.max(0) + 1).tolist(),
                    'sourceLightmap': surface['lightmap'],
                    'sourceReflectionProbe': surface['reflection_probe'],
                    'unresolvedNativeFields': ['0x20', '0x24', '0x28', '0x48 material relocation', '0x50 flags'],
                }
                blueprints.append(recipes)
                positions.write(points.tobytes())
                # Existing custom-weapon converters use the same 10-bit unit
                # vector encoding. The world stream interpretation is provisional.
                for vertex in vertices:
                    color = vertex['color']
                    uv = vertex['uv']
                    luv = vertex['lightmap_uv']
                    sign = vertex['binormal_sign']
                    if len(color) != 4 or any(type(c) is not int or not 0 <= c <= 255 for c in color):
                        raise ValueError('Invalid vertex color')
                    if len(uv) != 2 or len(luv) != 2 or not np.isfinite([*uv, *luv, sign]).all():
                        raise ValueError('Invalid vertex UV or handedness')
                    normal = pack_direction(vertex['normal'])
                    tangent = pack_direction(vertex['tangent'])
                    if sign < 0:
                        tangent |= 0x80000000
                    attributes.write(struct.pack('<4B2f2I', *color, *uv, normal, tangent))
                    light_uv.write(struct.pack('<2f', *luv))
                # Indices remain local to each surface batch. Native draw offsets
                # address the batch. Converting them to global u16 would overflow.
                indices.write(local_indices.astype('<u2').tobytes())
                vertex_total += len(vertices)
                index_total += len(local_indices)
        (root / 'surface-blueprints.json').write_text(json.dumps(blueprints, indent=2) + '\n')
        report = {
            'schema': 1, 'name': world['name'], 'stage': 'native-geometry-staging',
            'targetBuild': reference.manifest['build'], 'playable': False,
            'surfaces': len(blueprints), 'vertices': vertex_total, 'triangles': index_total // 3,
            'positionStride': 12, 'candidateAttributeStride': 20, 'indexStride': 2,
            'referenceMap': checked['map'], 'referenceGeometryValidated': True,
            'sourceCoordinates': 'COD axes, inches; unchanged',
            'candidateAttributeStatus': 'Provisional RGBA8, float2 UV, packed normal and tangent; handedness needs engine validation',
            'surfaceStatus': 'Blueprints only; unresolved fields prevent native surface serialization',
            'lightingStatus': 'Source lightmap UVs preserved separately; target lighting conversion pending',
            'pending': ['Complete GfxWorld and visibility serialization',
                        'Resolve surface flags and material links', 'Validate world tangent handedness in engine',
                        'Convert materials, lightmaps and static models', 'Serialize collision and triggers',
                        'Integrate entities, scripts and bot navigation', 'Write native zone relocation graph',
                        'Register and test map in game'],
            'files': {f.name: {'bytes': f.stat().st_size, 'sha256': hashlib.sha256(f.read_bytes()).hexdigest()}
                      for f in sorted(root.iterdir()) if f.is_file()},
        }
        (root / 'geometry-stage.json').write_text(json.dumps(report, indent=2) + '\n')
        (root / 'README.txt').write_text(
            'T7 beta geometry compiler inputs\n\n'
            'These files cannot be loaded in the game. They are not a fastfile.\n'
            'Positions and local indices use the live beta buffer layout.\n'
            'The attribute stream is provisional and explicitly named candidate.\n'
            'Surface blueprints retain material names and source lighting references.\n'
            'Source world JSON remains authoritative for unconverted attributes.\n'
            'See geometry-stage.json for pending native compiler work.\n')
        shutil.move(str(root), str(output))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True, help='Live dumpmap folder')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = stage(args.prepared, args.reference, args.output)
    print(json.dumps({k: v for k, v in report.items() if k != 'files'}, indent=2))


if __name__ == '__main__':
    main()
