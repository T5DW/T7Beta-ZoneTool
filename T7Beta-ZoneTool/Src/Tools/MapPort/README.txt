IW3 map source preparation for T7Beta-ZoneTool

This stage extracts and validates compiled COD4 map assets for a T7 beta port.
The generated world.bin uses the portable T7MAPSRC interchange format.
Native T7 GfxWorld serialization, collision serialization, native registration
and in-game validation are pending. map-port.json records this status.

Requirements
Python 3.10+, NumPy and Pillow.
Install with python -m pip install numpy pillow.
OpenAssetTools v0.33.0, commit 7d027e8f89118196713e955b0e11f8404149c54d.
Install OAT/configure_exporters.ps1 into that checkout and build Unlinker.
The supplied OAT extension exports original map geometry and collision.

Extract and prepare
python prepare_map.py --fastfile mp_nuketown.ff --unlinker Unlinker.exe --output prepared_nuketown

Pass --search-path C:/Games/COD4/main to resolve stock images from IWDs.
A sibling map IWD is searched automatically by Unlinker.
For an existing complete extraction, use
python prepare_map.py --source exported --output prepared_nuketown

Choose a fresh output directory. Extraction and preparation use temporary
staging, and the final output appears only after validation succeeds.

Contents
world.bin contains the source world surfaces.
surfaces.json records material, lightmap and byte ranges.
collision.source.json preserves original brushes and collision triangles.
entities.json preserves original entity key/value pairs.
world.source.json and source/ preserve source attributes, models and lighting.
source-preview.glb contains source geometry and first-LOD static models.
Its preview materials use diffuse textures. Shader effects and baked lightmaps
are retained in source/ for later target conversion.
map-port.json reports counts, missing models and the remaining native stages.

world.bin layout
8 bytes T7MAPSRC, uint32 version 1, uint32 surface count.
For each surface, uint32 vertex count, uint32 index count, then vertices.
A vertex contains ten float32 values: position XYZ, normal XYZ, UV, lightmap UV.
Each surface ends with its uint32 local indices. All values are little-endian.
Coordinates retain the COD source axes and inch units.

Source credits
Nuketown COD4 remake by >>N.B.Z.I.<<, version 1.0 (2011).
Original Nuketown design by Treyarch. COD4 tools by Infinity Ward.
OpenAssetTools by Laupetin and contributors (GPL-3.0).
Exporter extension from IDARETW/IW8-zonetool, with its included GPL license.
Retain these credits when distributing the source preparation work.

Restoring stock dependencies
Build ImageConverter from the patched OAT checkout, then run
python restore_images.py --source exported --iwd-dir C:/Games/COD4/main --image-converter C:/Tools/OAT/ImageConverter.exe
The helper fills absent material image dependencies and saves their archive
provenance. Its fresh conversions include IWI6 wavelet images.
Run prepare_map.py after restoring images.

Beta fastfile inspection
python beta_fastfile.py mp_stronghold.ff --entities --output stronghold_inspected
The reader validates the header and each compressed block, decompresses the
stream, reads script-string slots and the native asset directory, and locates
the entity root. It supports stored, zlib and optional LZ4 compression.
Install lz4 with python -m pip install lz4 for compression ID 3.

Live native map snapshots
The accompanying DLL update adds dumpmap to the T7MP-MOD console.
Start a local Stronghold match on August 21, spawn, and run dumpmap.
The console prints the created folder under .t7mp-mod/assets/maps/dumps/.
Zip that entire folder, including map-snapshot.json and its binary records.
The command captures typed world/collision buffers and entity/trigger records.
The snapshot is needed to continue native mesh and collision layout checks.
The command passed offline fixture checks and produced the supplied live Stronghold capture.

Integration source
Copy map_dump.cpp, map_dump.hpp and offsets/maps.hpp into the host client tree.
Add map_dump.cpp to the host CMake/xmake source list.
The supplied console.cpp registers dumpmap, and weaponzone.cpp pumps its queued
request on the existing server-frame callback.

Live capture validation and geometry staging
The supplied StrongholdLiveCapture example includes the actual typed snapshot.
Run these commands from the T7BetaZT folder after extracting the work package.

python Src/Tools/MapPort/beta_snapshot.py Examples/StrongholdLiveCapture/snapshot --output stronghold-validation.json

Optionally add --zone path/to/decompressed.bin to byte-match immutable buffers
with the decompressed matching Stronghold fastfile. The reader confirmed all
seven immutable geometry/collision buffers, about 17 MB, exactly.

python Src/Tools/MapPort/beta_geometry.py --prepared ../Nuketown-Source-Prepared --reference Examples/StrongholdLiveCapture/snapshot --output ../Nuketown-Geometry-New

The geometry output is compiler input, not a playable fastfile. Positions use
the verified 12-byte stream and indices use per-batch local uint16 values.
The 20-byte candidate attributes have provisional tangent/handedness encoding.
Native surface flags, shader/material links, visibility and lighting still need
conversion. Unknown fields are listed in surface-blueprints.json.
All source lighting UVs are retained in a separate float2 buffer.

python Tests/test_beta_geometry.py

This checks the included live snapshot, rejects corrupted counts/pointers/
bounds, and compares every staged Nuketown position, index, color, UV and
lightmap UV with the original export. --zone adds the optional exact FF checks.
The tests do not validate rendering or map loading in the game.

The live capture contains 107 leaf nodes whose nested brush lists extend beyond
the top-level captured leaf array. validation.json records these missing ranges.
They do not invalidate the captured world geometry or brush-side/vertex checks.
The optional separate brush bounds/contents pointers are null in this capture.
Native brush records already contain those values.

Experimental native zone writer
Run from the T7BetaZT directory:
python Src/Tools/MapPort/native_zone.py build-probe --output t7zt_probe_new.ff
python Src/Tools/MapPort/native_zone.py build-mapents-reference --snapshot Examples/StrongholdLiveCapture/snapshot --output t7zt_entities_new.ff --zlib

The writer supports native RawFile and the captured MapEnts subset. It emits
inline references, keeps stream alignment in logical blocks, and handles
8-MiB container page boundaries. Allocation capacities include scratch space.
It refuses existing output files, reads its output back, and records an
explicit playableMap=false and inGameValidated=false in the sidecar report.

python Tests/test_native_zone.py
python Tests/test_native_zone_client.py

The second test compiles the actual client module with mocked engine memory
and requires a native g++ toolchain. It checks the verified-build/match gates,
probe payload, command queuing, resident asset detection and file preservation.
The test does not execute the real engine.
The reference MapEnts zone preserves all entity and trigger bytes, contains no
GfxWorld or clipMap, and is not installed by the loader probe.
