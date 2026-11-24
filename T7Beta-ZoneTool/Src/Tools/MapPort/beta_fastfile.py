"""Inspect and decompress unencrypted T7 beta 0x1FB fastfiles."""
import argparse
from collections import Counter
import hashlib
import json
import struct
from pathlib import Path
import zlib

HEADER_SIZE = 0x248
INLINE_POINTER = (1 << 64) - 1
MAX_OUTPUT = 1024 * 1024 * 1024


def decode(path):
    raw = path.read_bytes()
    if len(raw) < HEADER_SIZE or raw[:8] != b"TAff0000":
        raise ValueError("Invalid T7 fastfile header")
    version, = struct.unpack_from("<I", raw, 8)
    if version != 0x1FB:
        raise ValueError(f"Unsupported fastfile version {version:#x}")
    encrypted, compression = raw[12:14]
    if encrypted:
        raise ValueError("This reader supports unencrypted beta fastfiles")
    size, = struct.unpack_from("<Q", raw, 144)
    if not size or size > MAX_OUTPUT:
        raise ValueError("Invalid decompressed byte count")
    output = bytearray()
    offset = HEADER_SIZE
    blocks = []
    while len(output) < size:
        if offset + 16 > len(raw):
            raise ValueError("Truncated block header")
        compressed, expanded, aligned, file_offset = struct.unpack_from("<IIII", raw, offset)
        if file_offset != offset:
            raise ValueError(f"Block offset mismatch at {offset:#x}")
        if not expanded:
            offset = (offset + 16 + 0x7FFFFF) & ~0x7FFFFF
            continue
        if (not compressed or aligned < compressed or aligned % 4
                or offset + 16 + aligned > len(raw) or len(output) + expanded > size):
            raise ValueError(f"Invalid block bounds at {offset:#x}")
        payload = raw[offset + 16:offset + 16 + compressed]
        if compression == 0:
            data = payload
        elif compression == 1:
            inflater = zlib.decompressobj()
            data = inflater.decompress(payload, expanded + 1)
            if not inflater.eof or inflater.unused_data or inflater.unconsumed_tail:
                raise ValueError("Incomplete or oversized zlib block")
        elif compression == 3:
            try:
                import lz4.block
            except ImportError as error:
                raise ValueError("Install lz4 to read compression ID 3") from error
            data = lz4.block.decompress(payload, uncompressed_size=expanded)
        else:
            raise ValueError(f"Unsupported compression ID {compression}")
        if len(data) != expanded:
            raise ValueError(f"Block size mismatch at {offset:#x}")
        blocks.append({"fileOffset": offset, "sourceOffset": len(output),
                       "compressedBytes": compressed, "expandedBytes": expanded})
        output.extend(data)
        offset += 16 + aligned
    name = raw[248:312].split(b"\0", 1)[0].decode("ascii")
    return output, {"name": name, "version": version, "compression": compression,
                    "decompressedBytes": size, "blockCount": len(blocks),
                    "blockAllocations": list(struct.unpack_from("<10Q", raw, 168)),
                    "sha256": hashlib.sha256(raw).hexdigest(),
                    "blocks": blocks}


def read_asset_list(data):
    if len(data) < 48:
        raise ValueError("Truncated asset list")
    strings, string_pointer, unknown, unknown_pointer, count, assets_pointer = struct.unpack_from("<6Q", data)
    if unknown or unknown_pointer:
        raise ValueError("Extra asset-list data requires another layout")
    if string_pointer not in (0, INLINE_POINTER) or assets_pointer != INLINE_POINTER:
        raise ValueError("Unsupported asset-list pointer layout")
    if strings > (len(data) - 48) // 8:
        raise ValueError("Script-string table exceeds the stream")
    position = 48 + strings * 8
    values = []
    for i in range(strings):
        pointer, = struct.unpack_from("<Q", data, 48 + i * 8)
        if not pointer:
            values.append(None)
        elif pointer == INLINE_POINTER:
            end = data.find(0, position)
            if end < 0:
                raise ValueError("Unterminated script string")
            values.append(data[position:end].decode("utf-8"))
            position = end + 1
        else:
            values.append({"packedPointer": hex(pointer)})
    if count > (len(data) - position) // 16:
        raise ValueError("Asset table exceeds the stream")
    definitions = json.loads((Path(__file__).parent / "formats/t7beta-asset-types.json").read_text())
    types = {r["type"]: r["name"] for r in definitions}
    assets = []
    for index in range(count):
        kind, pointer = struct.unpack_from("<QQ", data, position + index * 16)
        if kind not in types:
            raise ValueError(f"Unknown beta asset type {kind} at index {index}")
        assets.append({"index": index, "type": kind, "name": types[kind],
                       "headerPointer": hex(pointer)})
    return {"scriptStrings": values, "assets": assets,
            "assetTableOffset": position, "assetDataOffset": position + count * 16,
            "counts": dict(Counter(a["name"] for a in assets))}


def map_entities(data):
    # A MapEnts root stores name, entityString, char count and trigger arrays.
    position = data.find(b'"worldspawn"')
    if position < 0:
        raise ValueError("No worldspawn entity found")
    start = data.rfind(b"{", 0, position)
    header = start - 0x48
    if header < 0:
        raise ValueError("Entity root precedes stream")
    pointer, = struct.unpack_from("<Q", data, header + 8)
    length, = struct.unpack_from("<I", data, header + 16)
    if pointer != INLINE_POINTER or not length or start + length > len(data):
        raise ValueError("MapEnts header did not validate")
    text = bytes(data[start:start + length])
    if not text.endswith(b"\0"):
        raise ValueError("Entity character count excludes a valid terminator")
    from prepare_map import parse_entities
    records = parse_entities(text[:-1].decode("utf-8"))
    if dict(records[0]).get("classname") != "worldspawn":
        raise ValueError("First entity is not worldspawn")
    return {"headerOffset": header, "charCount": length, "entities": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fastfile", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--entities", action="store_true")
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("Choose a fresh output directory")
    data, container = decode(args.fastfile)
    assets = read_asset_list(data)
    entities = map_entities(data) if args.entities else None
    args.output.mkdir(parents=True)
    (args.output / "decompressed.bin").write_bytes(data)
    (args.output / "container.json").write_text(json.dumps(container, indent=2))
    (args.output / "asset-list.json").write_text(json.dumps(assets, indent=2))
    if entities:
        (args.output / "map-entities.json").write_text(json.dumps(entities, indent=2))
    print(json.dumps({key: value for key, value in container.items() if key != "blocks"}, indent=2))
    print(json.dumps(assets["counts"], indent=2))


if __name__ == "__main__":
    main()
