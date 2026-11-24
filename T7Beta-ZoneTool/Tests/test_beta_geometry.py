import argparse
import json
import struct
import sys
from pathlib import Path

import numpy as np

base = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(base / 'Src/Tools/MapPort'))
parser = argparse.ArgumentParser()
parser.add_argument('--prepared', type=Path, default=base.parent / 'Nuketown-Source-Prepared')
parser.add_argument('--stage', type=Path, default=base.parent / 'Nuketown-Native-Geometry-Stage')
parser.add_argument('--reference', type=Path, default=base / 'Examples/StrongholdLiveCapture/snapshot')
parser.add_argument('--zone', type=Path)
args = parser.parse_args()
from beta_snapshot import Snapshot

reference = args.reference
snapshot = Snapshot(reference)
report = snapshot.validate()
assert report['worldSurfaces'] == 2456
assert report['worldTriangles'] == 384612
assert report['unreferencedIndices'] == report['overlappingSurfaceIndices'] == 0
if args.zone:
    assert len(snapshot.compare_zone(args.zone)) == 7

# Reject actual bad pointers, counts, bounds, and truncated files rather than
# merely checking whether an encoder repeats its own output.
for label, offset, fmt, value in [
    ('clip_brushes', 32, '<Q', 1),
    ('gfxworld', 0x18, '<I', 1),
    ('world_surfaces', 0x2c, '<I', 0xffffffff),
    ('world_surfaces', 0x30, '<f', float('nan')),
]:
    original = snapshot.buffers[label]
    broken = bytearray(original)
    struct.pack_into(fmt, broken, offset, value)
    snapshot.buffers[label] = bytes(broken)
    try:
        snapshot.validate()
    except ValueError:
        pass
    else:
        raise AssertionError(f'Accepted corrupt {label} at {offset}')
    finally:
        snapshot.buffers[label] = original

stage = args.stage
world = json.loads((args.prepared / 'world.source.json').read_text())
blueprints = json.loads((stage / 'surface-blueprints.json').read_text())
positions = np.fromfile(stage / 'world_vertex_stream0.bin', dtype='<f4').reshape(-1, 3)
indices = np.fromfile(stage / 'world_indices.bin', dtype='<u2')
attributes = (stage / 'world_vertex_stream1.candidate.bin').read_bytes()
light_uv = np.fromfile(stage / 'world_lightmap_uv.source.bin', dtype='<f4').reshape(-1, 2)
vertex = index = 0
for source, recipe in zip(world['surfaces'], blueprints, strict=True):
    n, m = len(source['vertices']), len(source['indices'])
    assert recipe['positionByteOffset'] == vertex * 12
    assert recipe['attributeByteOffset'] == vertex * 20
    assert recipe['baseIndex'] == index
    assert np.array_equal(positions[vertex:vertex+n], np.asarray([v['position'] for v in source['vertices']], dtype='<f4'))
    assert np.array_equal(indices[index:index+m], source['indices'])
    assert np.array_equal(light_uv[vertex:vertex+n], np.asarray([v['lightmap_uv'] for v in source['vertices']], dtype='<f4'))
    for k, v in enumerate(source['vertices']):
        rgba_uv = struct.unpack_from('<4B2f', attributes, (vertex+k)*20)
        assert list(rgba_uv[:4]) == v['color']
        assert np.array_equal(np.asarray(rgba_uv[4:], dtype='<f4'), np.asarray(v['uv'], dtype='<f4'))
    vertex += n
    index += m
assert vertex == len(positions) == 83275
assert index == len(indices) == 160131
assert json.loads((stage/'geometry-stage.json').read_text())['playable'] is False
print('PASS: live geometry/brush references, optional zone comparison, corrupt-data rejection, all 1993 source surfaces preserved')
