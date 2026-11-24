"""Restore material image dependencies from COD4 IWD archives."""
import argparse
import json
from pathlib import Path
import subprocess
import zipfile


def restore(source, archives, converter):
    names = set()
    for material in (source / "materials").rglob("*.json"):
        definition = json.loads(material.read_text())
        names.update(t["image"].lstrip(",") for t in definition.get("textures", []) if t.get("image"))
    wanted = {n for n in names if not (source / "images" / (n.replace("*", "_") + ".dds")).is_file()}
    entries = {}
    for archive in sorted(archives.glob("*.iwd")):
        with zipfile.ZipFile(archive) as package:
            index = {n.casefold(): n for n in package.namelist()}
            for name in wanted:
                key = "images/" + name + ".iwi"
                if key.casefold() in index:
                    entries[name] = (archive, index[key.casefold()])
    restored = []
    for name, (archive, entry) in sorted(entries.items()):
        raw = source / "images" / (name.replace("*", "_") + ".iwi")
        if not raw.resolve().is_relative_to(source.resolve()):
            raise ValueError("Image path escapes extraction")
        raw.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(archive) as package:
            payload = package.read(entry)
        raw.write_bytes(payload)
        result = subprocess.run([str(converter.resolve()), "--no-color", str(raw.resolve())],
                                capture_output=True, text=True)
        dds = raw.with_suffix(".dds")
        if result.returncode or not dds.is_file():
            raise RuntimeError(f"Image conversion failed for {name}: {result.stdout} {result.stderr}")
        restored.append({"image": name, "archive": archive.name, "entry": entry,
                         "iwiFormat": payload[4]})
    report = {"restored": restored, "missing": sorted(wanted - entries.keys())}
    (source / "image-restoration.json").write_text(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--iwd-dir", type=Path, required=True)
    parser.add_argument("--image-converter", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(restore(args.source, args.iwd_dir, args.image_converter), indent=2))


if __name__ == "__main__":
    main()
