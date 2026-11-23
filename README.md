# T7Beta-ZoneTool V1.0

A fastfile tool and weapon asset workflow for **T7MP-MOD**, targeting the **August 21, 2015 Black Ops III multiplayer prototype on PC**.

**By T5DW**

## Features

- Build stock-derived weapon `.ff` containers.
- Build experimental rawfile `.ff` containers.
- Inspect T7 beta fastfile headers.
- Register custom weapon variants through T7MP-MOD.
- Import converted weapon models, textures, and animations.
- Play custom weapon firing and reload sounds.
- Includes the FAMAS converters and current weapon integration source.

## Project Structure

```text
T7BetaZT/
├── Src/
│   ├── t7/
│   ├── thirdparty/
│   │   └── lz4/
│   ├── T7MP-Integration/
│   │   ├── offsets/
│   │   └── gsc/
│   └── Tools/
│       └── COD4-FAMAS/
├── xmake.lua
└── README.txt
```

## Requirements

- Windows x64
- Xmake
- Visual Studio with Desktop development with C++
- T7MP-MOD for in-game weapon loading

The Python converters also require Python, NumPy, SciPy, and Pillow.

```powershell
pip install numpy scipy pillow
```

## Building

Open PowerShell in the `T7BetaZT` folder.

```powershell
xmake f -m release -a x64
xmake build T7BetaZT
```

## Creating a Weapon Fastfile

```powershell
xmake run T7BetaZT build-weapon smg_standard_mp t5dw_msmc_mp 30 msmc.ff
```

| Argument | Purpose |
| --- | --- |
| `smg_standard_mp` | Registered donor weapon |
| `t5dw_msmc_mp` | Custom weapon ID |
| `30` | Magazine size |
| `msmc.ff` | Output fastfile |

Use a new output filename each time. The writer refuses to overwrite an existing file.

The weapon container stores the donor ID, custom ID, and magazine override. T7MP-MOD resolves the donor and loads the accompanying converted assets.

## Inspecting a Fastfile

```powershell
xmake run T7BetaZT inspect msmc.ff
```

Displays header information including the zone name, version, compression ID, encryption flag, and payload size.

## Creating a Rawfile Fastfile

Create `hello.txt`, then run:

```powershell
xmake run T7BetaZT build-rawfile test_zone t7mp/hello.txt hello.txt test_zone.ff
```

Rawfile serialization is experimental and requires further in-game validation.

## Runtime Integration

`Src/T7MP-Integration/` contains the weapon registration, model importer, animation importer, sound playback, offsets, and GSC scripts.

These modules compile inside the existing T7MP-MOD client project and depend on its host services. The standalone Xmake target builds the command-line tool.

## Current Status

V1.0 supports the current weapon-container workflow. Playable map conversion, complete map fastfile serialization, and general asset dependency linking remain future work.

The standalone source has passed compilation and a weapon-container build and inspection check. In-game compatibility depends on the matching T7MP-MOD runtime and game build.

## Credits

- **T5DW** — T7MP-MOD / T7Beta-ZoneTool
- **LZ4 contributors** — bundled compression library

The bundled LZ4 license is included under `Src/thirdparty/lz4/`.
