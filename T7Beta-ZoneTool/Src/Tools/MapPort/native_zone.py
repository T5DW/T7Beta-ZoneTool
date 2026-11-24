"""Write experimental unencrypted August 21 native fastfiles.

Supported assets: RawFile and MapEnts rebuilt from a native dumpmap reference.
A full map cannot be compiled until GfxWorld/clipMap/material serializers exist.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
import zlib

from beta_fastfile import decode, read_asset_list
from beta_snapshot import Snapshot

INLINE = 0xffffffffffffffff
HEADER = 0x248
PAGE = 0x800000
MAX_BODY = 256 * 1024 * 1024


class ZoneStream:
    """Serialized bytes are contiguous; alignment belongs to logical blocks.

    Only inline references are currently emitted. No live/foreign virtual
    addresses or cross-zone packed aliases may enter the generated stream.
    """
    def __init__(self, kind):
        self.body = bytearray(struct.pack('<6Q', 1, INLINE, 0, 0, 1, INLINE))
        self.body += struct.pack('<Q2Q', 0, kind, INLINE)
        self.trace = []
        self.logical = [0] * 10
        # Reserve conservative loader working space. These are capacities,
        # not file offsets. Inline-only records do not depend on their values.
        self.logical[5] = 72

    def write(self, label, data, block=5, alignment=1):
        if alignment <= 0 or alignment & (alignment - 1):
            raise ValueError('Alignment must be a power of two')
        logical = (self.logical[block] + alignment - 1) & -alignment
        self.trace.append({'label': label, 'fileOffset': len(self.body), 'block': block,
                           'logicalOffset': logical, 'bytes': len(data), 'alignment': alignment})
        self.logical[block] = logical + len(data)
        self.body.extend(data)


def valid_name(name):
    return bool(re.fullmatch(r't7zt_[a-z0-9_]{1,50}', name))


def container(name, stream, stored=True):
    if not valid_name(name):
        raise ValueError('Experimental zone names must start t7zt_ and use lowercase letters/digits/underscores')
    body = stream.body
    if not body or len(body) > MAX_BODY:
        raise ValueError('Invalid zone body size')
    raw = bytearray(HEADER)
    struct.pack_into('<8sI', raw, 0, b'TAff0000', 0x1fb)
    raw[13] = 0 if stored else 1
    struct.pack_into('<Q', raw, 144, len(body))
    capacities = list(stream.logical)
    # XAssetList and loader bookkeeping capacity is intentionally generous.
    capacities[0] += 8192
    capacities[5] += 8192
    struct.pack_into('<10Q', raw, 168, *capacities)
    raw[248:248 + len(name)] = name.encode('ascii')
    blocks = 0
    for offset in range(0, len(body), 0x10000):
        source = body[offset:offset + 0x10000]
        packed = bytes(source) if stored else zlib.compress(source, 9)
        aligned = (len(packed) + 3) & ~3
        page_end = ((len(raw) // PAGE) + 1) * PAGE
        if len(raw) + 16 + aligned + 16 > page_end:
            sentinel = struct.pack('<4I', 0, 0, 0, len(raw))
            raw.extend(sentinel)
            raw.extend(bytes(page_end - len(raw)))
        raw.extend(struct.pack('<4I', len(packed), len(source), aligned, len(raw)))
        raw.extend(packed)
        raw.extend(bytes(aligned - len(packed)))
        blocks += 1
    raw.extend(struct.pack('<4I', 0, 0, 0, len(raw)))
    raw.extend(bytes(64))
    return bytes(raw), capacities, blocks


def validate_triggers(models, hulls, slabs):
    if len(models) % 8 or len(hulls) % 32 or len(slabs) % 20:
        raise ValueError('Invalid trigger buffer length')
    hull_count, slab_count = len(hulls) // 32, len(slabs) // 20
    for at in range(0, len(models), 8):
        _, count, first = struct.unpack_from('<IHH', models, at)
        if first + count > hull_count:
            raise ValueError('Trigger model references unavailable hulls')
    for at in range(0, len(hulls), 32):
        values = struct.unpack_from('<6fIHH', hulls, at)
        import math
        if not all(math.isfinite(x) for x in values[:6]) or any(x < 0 for x in values[3:6]):
            raise ValueError('Invalid trigger bounds')
        if values[8] + values[7] > slab_count:
            raise ValueError('Trigger hull references unavailable slabs')
    if slabs:
        import math
        for values in struct.iter_unpack('<5f', slabs):
            if not all(math.isfinite(x) for x in values) or values[4] < 0:
                raise ValueError('Invalid trigger slab')


def rawfile_stream(asset, text):
    if not re.fullmatch(r't7zt/[a-z0-9_./-]{1,180}', asset) or '..' in asset:
        raise ValueError('RawFile asset must be under t7zt/')
    if len(text) > 512 * 1024 or b'\0' in text:
        raise ValueError('Probe RawFile must be non-NUL text up to 512 KiB')
    stream = ZoneStream(46)
    stream.write('RawFile', struct.pack('<QQQ', INLINE, len(text), INLINE), 0, 8)
    stream.write('RawFile.name', asset.encode('ascii') + b'\0')
    stream.write('RawFile.buffer', text + b'\0')
    return stream


def mapents_stream(snapshot):
    snapshot = Snapshot(snapshot)
    snapshot.validate()
    root = snapshot.data('mapents', 72)
    text = snapshot.data('entity_string', 1)
    if not text.endswith(b'\0') or struct.unpack_from('<I', root, 16)[0] != len(text):
        raise ValueError('Invalid native entity character count')
    models = snapshot.data('trigger_models', 8)
    hulls = snapshot.data('trigger_hulls', 32)
    slabs = snapshot.data('trigger_slabs', 20)
    validate_triggers(models, hulls, slabs)
    for offset, data, stride in [(24, models, 8), (40, hulls, 32), (56, slabs, 20)]:
        if struct.unpack_from('<I', root, offset)[0] != len(data) // stride:
            raise ValueError('MapEnts trigger count mismatch')
    root = bytearray(72)
    struct.pack_into('<QQI', root, 0, INLINE, INLINE, len(text))
    for offset, data, stride in [(24, models, 8), (40, hulls, 32), (56, slabs, 20)]:
        struct.pack_into('<I', root, offset, len(data) // stride)
        struct.pack_into('<Q', root, offset + 8, INLINE if data else 0)
    stream = ZoneStream(15)
    stream.write('MapEnts', root, 0, 8)
    stream.write('MapEnts.name', snapshot.manifest['map'].encode('ascii') + b'\0')
    stream.write('MapEnts.entityString', text)
    for name, data in [('models', models), ('hulls', hulls), ('slabs', slabs)]:
        if data:
            stream.write('MapTriggers.' + name, data, alignment=4)
    return stream


def read_single_mapents(body):
    """Independent field reader for the inline-only writer's native subset."""
    listing = read_asset_list(body)
    if len(listing['assets']) != 1 or listing['assets'][0]['type'] != 15:
        raise ValueError('Expected exactly one native MapEnts asset')
    at = listing['assetDataOffset']
    if at + 72 > len(body):
        raise ValueError('Truncated MapEnts root')
    root = bytes(body[at:at+72]);at += 72
    if struct.unpack_from('<QQ', root) != (INLINE, INLINE):
        raise ValueError('Unsupported MapEnts string references')
    end = body.find(0, at)
    if end < 0:
        raise ValueError('Unterminated map name')
    name = bytes(body[at:end]).decode('ascii');at = end + 1
    count = struct.unpack_from('<I', root, 16)[0]
    if not count or at + count > len(body) or body[at+count-1] != 0:
        raise ValueError('Invalid entity string range')
    text = bytes(body[at:at+count]);at += count
    arrays = []
    for offset, stride in [(24, 8), (40, 32), (56, 20)]:
        count = struct.unpack_from('<I', root, offset)[0]
        pointer = struct.unpack_from('<Q', root, offset+8)[0]
        if pointer != (INLINE if count else 0) or at + count*stride > len(body):
            raise ValueError('Invalid trigger array reference')
        arrays.append(bytes(body[at:at+count*stride]));at += count*stride
    if at != len(body):
        raise ValueError('Unconsumed MapEnts asset data')
    validate_triggers(*arrays)
    return name, text, arrays


def write_output(name, stream, output, compressed=False, purpose='native-loader-probe'):
    output = Path(output)
    sidecar = output.with_suffix('.json')
    if output.exists() or sidecar.exists():
        raise ValueError('Output or sidecar already exists')
    data, capacities, blocks = container(name, stream, not compressed)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive create protects an existing compiled file even across a race.
    with output.open('xb') as file:
        file.write(data)
    try:
        decoded, _ = decode(output)
        if decoded != stream.body:
            raise ValueError('Native container round trip failed')
        listing = read_asset_list(decoded)
        if listing['assets'][0]['type'] == 15:
            read_single_mapents(decoded)
        report = {'schema': 1, 'target': 'T7 August 21 2015 PC', 'zone': name,
                  'purpose': purpose, 'playableMap': False, 'inGameValidated': False,
                  'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
                  'compression': 'zlib' if compressed else 'stored', 'blocks': blocks,
                  'allocationCapacities': capacities, 'assets': listing['assets'], 'writes': stream.trace}
        with sidecar.open('x') as file:
            json.dump(report, file, indent=2)
            file.write('\n')
    except Exception:
        output.unlink(missing_ok=True)
        raise
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    probe = sub.add_parser('build-probe')
    probe.add_argument('--output', type=Path, required=True)
    probe.add_argument('--zone', default='t7zt_probe')
    probe.add_argument('--zlib', action='store_true')
    entities = sub.add_parser('build-mapents-reference')
    entities.add_argument('--snapshot', type=Path, required=True)
    entities.add_argument('--output', type=Path, required=True)
    entities.add_argument('--zone', default='t7zt_stronghold_entities')
    entities.add_argument('--zlib', action='store_true')
    args = parser.parse_args()
    if args.command == 'build-probe':
        stream = rawfile_stream('t7zt/probe.txt', b'T7Beta-ZoneTool native fastfile probe - T5DW\n')
        purpose = 'rawfile-loader-probe'
    else:
        stream = mapents_stream(args.snapshot)
        purpose = 'native-mapents-reference-roundtrip; not a complete map'
    print(json.dumps(write_output(args.zone, stream, args.output, args.zlib, purpose), indent=2))


if __name__ == '__main__':
    main()
