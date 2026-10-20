# spongebobfix

This project makes **SpongeBob SquarePants: Employee of the Month** (2002, THQ / AWE) run
properly on modern Windows. It also rebuilds the game for modern hardware.

**This repository contains no game files.** Use your own installed copy.

## Use it now (v0.1.0)

Download `sb2modern-v0.1.0.zip` from Releases. Copy `ddraw.dll`, `dsound.dll` and
`sb2modern.ini` into your installed game folder, next to `sb2.exe`. `INSTALL.txt` in the zip has
the details.

It gives you:

- A Direct3D 11 renderer, borderless fullscreen or a scaled window, with correct 4:3.
- WASAPI audio that follows your default output device.
- No CD or ISO needed.
- Precise timing and low CPU use.

## Layout

| Path | What |
|---|---|
| `rebuild/ANALYSIS.md` | Reverse-engineering notes and the plan. |
| `rebuild/phase0/` | Compatibility layer: replacement `ddraw.dll` + `dsound.dll` (source). |
| `rebuild/phase1/` | Converters from the game's formats (scripts, scenes, OMT databases, Granny models, Bink movies) to PNG/WAV/JSON/glTF/MP4, with format docs. |
| `rebuild/releases/` | Release packaging. |

**Next phases:**

- **Phase 2:** a new 64-bit engine.
- **Phase 3:** port the game logic onto it.
