T7BetaZT source for the August 21, 2015 BO3 PC prototype

All source files are under T7BetaZT/Src.

Build the standalone command-line tool from this folder with xmake.
  xmake f -m release -a x64
  xmake build T7BetaZT
  xmake run T7BetaZT inspect path/to/zone.ff
  xmake run T7BetaZT build-weapon smg_standard_mp t5dw_msmc_mp 30 msmc.ff
  xmake run T7BetaZT build-rawfile test_zone t7mp/hello.txt hello.txt test_zone.ff

build-weapon writes the stock-derived weapon container used by the T7MP-MOD
runtime. Its parameters are donor ID, custom ID, magazine size and output.
Use a fresh output filename. The writer refuses to overwrite existing files.
The rawfile writer is experimental. inspect checks the beta header.

Src/T7MP-Integration contains the current weapon registration, definitions,
model importer, animation importer, sound playback, offsets and GSC scripts.
These modules compile inside the existing T7MP-MOD client source tree using
its Lua, camo, build profile, server frame, hook and other host services.
The standalone xmake target builds Src/t7 and Src/thirdparty only.
Copy integration modules and offsets into matching paths in the host project.
The included animation and audio modules include the latest FAMAS fixes.

Src/Tools/COD4-FAMAS contains the model and animation converters.
Install their Python dependencies with pip install numpy scipy pillow.
Run from that folder with the COD4 asset folder and output folder as arguments.
  python convert.py SOURCE OUTPUT
  python motion.py SOURCE OUTPUT
The source folder must contain images, materials, xmodel, xmodelparts,
xmodelsurfs and xanim from the supplied port. OUTPUT holds the converted assets.

Current implemented scope includes weapon containers and runtime weapon assets.
Playable map conversion and map serialization still require implementation.
The standalone tool uses the bundled LZ4 source with its included license.
