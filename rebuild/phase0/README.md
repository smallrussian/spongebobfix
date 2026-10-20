# Phase 0: sb2modern compatibility layer

Two drop-in 32-bit DLLs that run the original `sb2.exe` / `OMT.dll` on modern Windows without
dgVoodoo, CD, or exclusive fullscreen. No game files are modified on disk.

| DLL | Replaces | What it does |
|---|---|---|
| `ddraw.dll` | DirectDraw 4 + Direct3D 6 | Emulates exactly the DirectDraw/D3D6 surface `OMT.dll` uses, rendered with **Direct3D 11** (flip-model swap chain, tearing support, vsync, frame limiter). Also installs the compatibility fixes below at load time. |
| `dsound.dll` | DirectSound | Software float mixer over **WASAPI shared mode**. Follows the Windows default output device live, survives unplug/replug, can be pinned to a named device, keeps time while no device exists. Serves both the engine and Bink movie audio. |

## Compatibility fixes (applied in memory when ddraw.dll loads)
- **No CD check**: `sb2.exe` 0x40E470 (scans CD drives for `Movies\mSpongeBob.bik`) returns "found"; all data loads from the game folder.
- **Borderless / windowed** instead of exclusive 640x480x16: the mode switch is emulated, the window is resized to the monitor (or a scaled window) and the image is scaled with correct 4:3 aspect. Alt+Enter toggles. The engine's always-on-top flag is removed.
- **Mouse** coordinates are mapped between the scaled window and the game's 640x480 space (window messages, `GetCursorPos`/`SetCursorPos`, `GetClientRect`).
- **Precise timer**: the engine's `GetTickCount` (~15.6 ms steps) is replaced with a QueryPerformanceCounter clock.
- **CPU usage**: the engine's main loop (`OMediaPeriodical::devote_time`, OMT.dll+0x83090) spun a core at 100 %; iterations that produce no frame now wait 1 ms.
- **Per-monitor DPI awareness**, so Windows does not blur-scale the window.
- **Movies**: DirectDraw page flips are presented with copy semantics so Bink's changed-blocks-only copies stay intact.

## Settings — `sb2modern.ini` (created next to sb2.exe on first run)
```
[Display]  WindowMode=0 (borderless) | 1 (window)   WindowScale=2   Scaling=0 (4:3) | 1 (stretch) | 2 (integer)
           Filter=0 (nearest) | 1 (bilinear) | 2 (sharp)   VSync=1   FrameLimit=60   TextureFilter=-1|0|1|2
[Audio]    Device=   (empty = follow Windows default; else part of a device name)   LatencyMs=20   MasterVolume=100
[Compat]   NoCD=1   PreciseTimer=1   ReduceCPU=1
[Debug]    LogLevel=1   (log goes to sb2modern.log)
```
Debug aid: create an empty file `sb2modern_capture` in the game folder to dump one presented
frame per second to `capture_NNNN.bmp`.

## Build & install
```
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
powershell -File deploy.ps1
```
`deploy.ps1` moves dgVoodoo's DLLs into `_dgvoodoo_backup\` (first run only) and copies the two DLLs into the game folder.

## Known limits
- 3D is rendered at the game's native 640x480 and scaled up; higher internal resolution belongs to the Phase 2 engine.
- Only the API surface OMT.dll and Bink use is implemented; anything else is logged as `UNIMPLEMENTED` in `sb2modern.log`.
