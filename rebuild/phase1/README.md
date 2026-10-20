# Phase 1: asset converters

These tools turn the game's 2002-era formats into modern, documented ones for the new engine.
Every tool reads the original game folder and writes its output somewhere else; output belongs in
`converted\`, which git ignores. None of them modify game files.

| Data | Tool | Output | Docs |
|---|---|---|---|
| Scripts and dialogue (`*.sb2`, 220 files) | `media/sb2text.py` | TSV/JSON, byte-exact round trip | `media/SCRIPTS.md` |
| Scenes (`scenes/*.scn`, 123) | `media/scn_parse.py` | JSON | `media/SCENES.md` |
| Movies (`Movies/*.bik`, 15) | `media/convert_movies.py` | H.264/AAC MP4 or AV1/Opus MKV | – |
| Asset databases (`*.omt`, 316) | `omt/omt_export.py` | PNG, WAV, material JSON | `omt/FORMAT.md` |
| 3D models (`models/**/*.grn`, 125 models, 549 clips) | `granny/grn_dump.exe` + `granny/grn_convert.py` | glTF 2.0 + raw `.grd` | `granny/README.md` |

Run everything from the game folder:

```
python rebuild/phase1/media/sb2text.py --tree . converted
python rebuild/phase1/media/scn_parse.py --tree . converted
python rebuild/phase1/media/convert_movies.py . converted/movies
python rebuild/phase1/omt/omt_export.py --tree . converted/omt
cmake -S rebuild/phase1/granny -B rebuild/phase1/granny/build -A Win32
cmake --build rebuild/phase1/granny/build --config Release
python rebuild/phase1/granny/grn_convert.py . converted/models
```

**Requirements:**

- Python 3.10+ with only the standard library.
- FFmpeg for the movie converter (default location `C:\ffmpeg\bin`).
- Visual Studio with CMake for the 32-bit `grn_dump`.
