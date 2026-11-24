"""Offline native container, MapEnts fidelity and bad-input checks."""
import argparse
import json
import struct
import sys
import tempfile
from pathlib import Path

base = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(base / 'Src/Tools/MapPort'))
from native_zone import (ZoneStream, container, rawfile_stream, mapents_stream,
                         read_single_mapents, validate_triggers, write_output)
from beta_fastfile import decode

parser = argparse.ArgumentParser()
parser.add_argument('--reference', type=Path, default=base/'Examples/StrongholdLiveCapture/snapshot')
args = parser.parse_args()
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    stream = mapents_stream(args.reference)
    name, entities, arrays = read_single_mapents(stream.body)
    manifest = json.loads((args.reference/'map-snapshot.json').read_text())
    assert name == manifest['map']
    assert entities == (args.reference/'entity_string.bin').read_bytes()
    for data, file in zip(arrays, ['trigger_models.bin','trigger_hulls.bin','trigger_slabs.bin']):
        assert data == (args.reference/file).read_bytes()
    for compressed in [False, True]:
        output = root / ('native_zlib.ff' if compressed else 'native_stored.ff')
        write_output('t7zt_roundtrip', stream, output, compressed)
        data, _ = decode(output)
        assert data == stream.body
        # A second write must preserve the original artifact.
        original = output.read_bytes()
        try:
            write_output('t7zt_roundtrip', stream, output, compressed)
        except ValueError:
            pass
        else:
            raise AssertionError('Existing file overwritten')
        assert output.read_bytes() == original
    broken = bytearray(stream.body)
    # Oversized count points outside the entity string / trigger arrays.
    struct.pack_into('<I', broken, 72+16, 0xffffffff)
    try:
        read_single_mapents(broken)
    except ValueError:
        pass
    else:
        raise AssertionError('Corrupt entity string accepted')
    try:
        validate_triggers(struct.pack('<IHH',1,2,0), bytes(32), b'')
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid trigger hull range accepted')
    probe = rawfile_stream('t7zt/probe.txt', b'T7Beta-ZoneTool native fastfile probe - T5DW\n')
    write_output('t7zt_probe', probe, root/'probe.ff')
    # Exercise an actual 8-MiB stored-file page boundary, independent of map data.
    large = ZoneStream(46)
    large.write('container-boundary-test', bytes(range(256)) * 37000)
    raw, _, _ = container('t7zt_boundary', large)
    file = root/'boundary.ff';file.write_bytes(raw)
    decoded, meta = decode(file)
    assert decoded == large.body
    assert any(block['fileOffset'] == 0x800000 for block in meta['blocks'])
    file.write_bytes(raw[:-100])
    try:
        decode(file)
    except ValueError:
        pass
    else:
        raise AssertionError('Truncated container accepted')
print('PASS: native stored/zlib zones, 8-MiB page boundary, exact entity/trigger fidelity, corrupt data and overwrite rejection')
