"""Validate typed August 21 map snapshots and compare immutable buffers with a zone."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np


class Snapshot:
    def __init__(self, folder):
        self.folder = Path(folder).resolve()
        self.manifest = json.loads((self.folder / 'map-snapshot.json').read_text())
        if self.manifest.get('build') != 'T7 August 21 2015 PC':
            raise ValueError('Unsupported snapshot build')
        self.records = {}
        self.buffers = {}
        for record in self.manifest['records']:
            label = record['label']
            if label in self.records:
                raise ValueError(f'Duplicate record {label}')
            self.records[label] = record
            if record['status'] != 'saved':
                continue
            path = (self.folder / record['file']).resolve()
            if not path.is_relative_to(self.folder):
                raise ValueError('Snapshot path escapes folder')
            data = path.read_bytes()
            if len(data) != record['count'] * record['stride']:
                raise ValueError(f'Wrong byte length for {label}')
            self.buffers[label] = data

    def data(self, label, stride):
        record = self.records.get(label)
        if record is None or record['stride'] != stride or label not in self.buffers:
            raise ValueError(f'Missing or incompatible {label}')
        return self.buffers[label]

    def pointer_index(self, label, address, count, stride):
        data = self.data(label, stride)
        offset = address - int(self.records[label]['address'], 16)
        if count and (offset < 0 or offset % stride or offset + count * stride > len(data)):
            raise ValueError(f'Pointer range outside {label}')
        return offset // stride if count else None

    def validate(self):
        world = self.data('gfxworld', 8256)
        clip = self.data('clipmap', 720)
        surfaces = self.data('world_surfaces', 96)
        vertices = np.frombuffer(self.data('world_vertex_stream0', 1), dtype='<f4').reshape(-1, 3)
        attributes = self.data('world_vertex_stream1', 1)
        indices = np.frombuffer(self.data('world_indices', 2), dtype='<u2')
        surface_count = len(surfaces) // 96
        if struct.unpack_from('<I', world, 0x18)[0] != surface_count:
            raise ValueError('World surface count mismatch')
        if len(attributes) % 20 or len(attributes) // 20 != len(vertices) or not np.isfinite(vertices).all():
            raise ValueError('Invalid vertex streams')
        for label, count_at, pointer_at, stride in [
            ('world_vertex_stream0', 0x278 + 0x14, 0x278 + 0x18, 1),
            ('world_vertex_stream1', 0x278 + 0x28, 0x278 + 0x30, 1),
            ('world_indices', 0x278 + 0x40, 0x278 + 0x48, 2),
        ]:
            count = struct.unpack_from('<I', world, count_at)[0]
            pointer = struct.unpack_from('<Q', world, pointer_at)[0]
            if count != self.records[label]['count']:
                raise ValueError(f'World draw count mismatch for {label}')
            self.pointer_index(label, pointer, count, stride)
        # A surface addresses a shared vertex batch. The u16 at +0x28 is often
        # zero and MUST NOT be used as a per-surface vertex range limit.
        covered = np.zeros(len(indices), dtype=np.uint32)
        batches = set()
        for i in range(surface_count):
            row = surfaces[i * 96:(i + 1) * 96]
            position_byte = struct.unpack_from('<I', row, 0x0c)[0]
            attribute_byte = struct.unpack_from('<I', row, 0x1c)[0]
            triangles, base = struct.unpack_from('<HI', row, 0x2a)
            if position_byte % 12 or attribute_byte % 20 or position_byte // 12 != attribute_byte // 20:
                raise ValueError(f'Surface {i} has incompatible stream offsets')
            end = base + triangles * 3
            if end > len(indices):
                raise ValueError(f'Surface {i} index range is invalid')
            selected = indices[base:end].astype(np.uint32) + position_byte // 12
            if selected.size and selected.max() >= len(vertices):
                raise ValueError(f'Surface {i} vertex range is invalid')
            lo = np.array(struct.unpack_from('<3f', row, 0x30))
            hi = np.array(struct.unpack_from('<3f', row, 0x3c))
            if not np.isfinite(np.r_[lo, hi]).all() or np.any(lo > hi):
                raise ValueError(f'Surface {i} has invalid bounds')
            points = vertices[selected]
            if points.size and (np.any(points.min(0) < lo - .1) or np.any(points.max(0) > hi + .1)):
                raise ValueError(f'Surface {i} vertices escape bounds')
            covered[base:end] += 1
            batches.add(position_byte // 12)
        clip_fields = [
            ('clip_planes', 0, 8, 20, 'I'),
            ('clip_materials', 0x10, 0x18, 16, 'I'),
            ('clip_brush_sides', 0x20, 0x28, 16, 'I'),
            ('clip_leaf_brush_nodes', 0x30, 0x38, 24, 'I'),
            ('clip_leaf_brushes', 0x40, 0x48, 2, 'I'),
            ('clip_vertices', 0x50, 0x58, 12, 'I'),
            ('clip_indices', 0x60, 0x68, 4, 'I'),
            ('clip_brushes', 0x70, 0x78, 112, 'H'),
        ]
        for label, count_at, pointer_at, stride, code in clip_fields:
            count = struct.unpack_from('<' + code, clip, 0x10 + count_at)[0]
            pointer = struct.unpack_from('<Q', clip, 0x10 + pointer_at)[0]
            if count != self.records[label]['count']:
                raise ValueError(f'ClipInfo count mismatch for {label}')
            if count:
                self.pointer_index(label, pointer, count, stride)
        brushes = self.data('clip_brushes', 112)
        sides = self.data('clip_brush_sides', 16)
        for at in range(0, len(sides), 16):
            self.pointer_index('clip_planes', struct.unpack_from('<Q', sides, at)[0], 1, 20)
        for at in range(0, len(brushes), 112):
            row = brushes[at:at + 112]
            lo = np.array(struct.unpack_from('<3f', row))
            hi = np.array(struct.unpack_from('<3f', row, 16))
            if not np.isfinite(np.r_[lo, hi]).all() or np.any(lo > hi):
                raise ValueError('Invalid brush bounds')
            count = struct.unpack_from('<I', row, 28)[0]
            self.pointer_index('clip_brush_sides', struct.unpack_from('<Q', row, 32)[0], count, 16)
            count = struct.unpack_from('<I', row, 88)[0]
            self.pointer_index('clip_vertices', struct.unpack_from('<Q', row, 96)[0], count, 12)
        external_leaf_ranges = []
        leaves = self.data('clip_leaf_brush_nodes', 24)
        for at in range(0, len(leaves), 24):
            count = struct.unpack_from('<h', leaves, at + 2)[0]
            if count > 0:
                pointer = struct.unpack_from('<Q', leaves, at + 8)[0]
                try:
                    self.pointer_index('clip_leaf_brushes', pointer, count, 2)
                except ValueError:
                    # Nested leaf arrays can be allocated outside the top-level
                    # ClipInfo leaf array. Record missing coverage explicitly.
                    external_leaf_ranges.append({'node': at // 24, 'address': hex(pointer), 'count': count})
        leaf_brushes = np.frombuffer(self.data('clip_leaf_brushes', 2), dtype='<u2')
        if leaf_brushes.size and leaf_brushes.max() >= len(brushes) // 112:
            raise ValueError('Leaf references nonexistent brush')
        terrain = np.frombuffer(self.data('terrain_vertices', 12), dtype='<f4').reshape(-1, 3)
        terrain_triangles = np.frombuffer(self.data('terrain_triangles', 12), dtype='<u4').reshape(-1, 3)
        if not np.isfinite(terrain).all() or (terrain_triangles.size and terrain_triangles.max() >= len(terrain)):
            raise ValueError('Invalid terrain collision')
        entity = self.data('entity_string', 1)
        from prepare_map import parse_entities
        entities = parse_entities(entity.rstrip(b'\0').decode('utf-8'))
        report = {
            'schema': 1, 'build': self.manifest['build'], 'map': self.manifest['map'],
            'capturedGeometryValidated': True, 'completeLeafPointerCoverage': not external_leaf_ranges,
            'externalLeafRanges': external_leaf_ranges, 'worldSurfaces': surface_count,
            'worldVertices': len(vertices), 'worldTriangles': int(sum(struct.unpack_from('<H', surfaces, at + 0x2a)[0] for at in range(0, len(surfaces), 96))),
            'sharedVertexBatches': len(batches), 'unreferencedIndices': int(np.count_nonzero(covered == 0)),
            'overlappingSurfaceIndices': int(np.count_nonzero(covered > 1)),
            'collisionBrushes': len(brushes) // 112, 'collisionSides': len(sides) // 16,
            'terrainTriangles': len(terrain_triangles), 'entities': len(entities),
            'unavailableRecords': [r for r in self.records.values() if r['status'] != 'saved'],
            'files': {label: {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()} for label, data in self.buffers.items()},
        }
        return report

    def compare_zone(self, zone):
        """Only match pointer-free immutable buffers. Never serialize live pointers."""
        data = Path(zone).read_bytes()
        result = {}
        for label in ['world_vertex_stream0', 'world_vertex_stream1', 'world_indices',
                      'clip_planes', 'clip_vertices', 'terrain_vertices', 'terrain_triangles']:
            buffer = self.buffers[label]
            offset = data.find(buffer)
            if offset < 0:
                raise ValueError(f'Zone does not contain exact live {label}')
            result[label] = {'offset': hex(offset), 'bytes': len(buffer), 'exactMatch': True}
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('--zone', type=Path, help='Decompressed matching beta fastfile')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    snapshot = Snapshot(args.snapshot)
    report = snapshot.validate()
    if args.zone:
        report['zoneMatches'] = snapshot.compare_zone(args.zone)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: v for k, v in report.items() if k not in ['files', 'unavailableRecords', 'externalLeafRanges']}, indent=2))


if __name__ == '__main__':
    main()
