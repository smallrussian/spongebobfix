# SpongeBob SquarePants: Employee of the Month (PC, 2002) — modernization analysis

Static analysis using REA (Ghidra 12.1.4 backend). Evidence: `rea-evidence.json` (21 records),
OMT.dll analysis snapshot: `omt-dll.rea-snapshot`. Everything below is **static** — nothing was executed.

## 1. What ships

| File | Role | Notes |
|---|---|---|
| `sb2.exe` (356 KB, x86 PE) | Game logic | 1,245 functions, MSVC 6-era CRT. Build path `C:\THQ\Bob2\GAME\Awe.cpp`. Registry `HKCU\Software\THQ\SpongeBob 2\Settings`. |
| `OMT.dll` (4 MB, x86) | Engine: "Open Media Toolkit" (window class `OPEN_MEDIA_TOOLKIT_DISPLAY`) | ~840 C++ classes (`OMedia*`), exported by mangled name. Portable design ("Rtg" = retarget layer; Win/DX backends are `OMediaWin*` / `OMediaDX*`). |
| `granny.dll` | RAD Granny (old 1.x/2 API: `GrannyOpenModel`, `GrannyLockRenderingState`…) | 32-bit only, proprietary. |
| `binkw32.dll` | Bink 1.5J video | 32-bit only, proprietary. |
| `D3D8/D3D9/DDraw/D3DImm.dll` + `dgVoodoo.conf` | **dgVoodoo 2.87.5** wrapper, not original | Already translates DDraw/D3D6 → D3D11/12. |

`sb2.exe` imports **208 OMT exports** across ~55 classes (2D/3D elements, worlds, anims, sound, input, DB, file streams), 33 Granny and 10 Bink functions. That set is the API surface a rebuild has to reimplement.

## 2. Findings (observed in the code)

### Rendering — DirectX 6 era
- `OMediaDirectX::install(7)` (sb2 `0x4097d0`) installs the DX gfx, sound and input backends.
- `OMediaDXScreenDevice::setmode` (`0x1005ac30`): `DirectDrawCreate` → QI `IDirectDraw4` → `SetCooperativeLevel` → `SetDisplayMode`. **Exclusive fullscreen with a mode switch.** After every mode set, it busy-waits 1000 ms calling `devote_time`.
- `d3d_create_device` (`0x1005bbe0`) returns an **`IDirect3DDevice3`** (D3D6). It tries three device GUIDs in order (HAL → fallbacks).
- `OMediaDXRasterizer::rasterize` (`0x10059a40`) makes **one `DrawPrimitive` call per polygon**, with FVF `0x1C4` = `XYZRHW|DIFFUSE|SPECULAR|TEX1` (pre-transformed screen-space vertices). Transform, lighting and fog are all done on the CPU by OMT. It also changes render state and texture-stage state per polygon. → On modern GPUs the cost is draw-call overhead, not GPU work, and resolution scaling has to happen in the engine's viewport math.
- There is also a full **software rasterizer** (`OMediaOMTRasterizer`, hundreds of generated span-filler `switch` tables). `sb2.exe` selects it when the command line contains `sw` (`strstr(cmdline,"sw")`; `sb2safe.bat` = `sb2.exe -sw`). Software mode forces Shadows off.

### Resolution / window
- `sb2` `FUN_0040e2c0` (SetVideoMode) asks for **640×480×16**. If that exact mode isn't enumerated, it scans for 640×480×16 again and otherwise aborts. The only configurable values are Music/Sound/Voice/Shadows/two more DWORDs in the registry. Resolution is not among them.
- `OMediaDisplay::OMediaDisplay` (`0x1038f8a0`): `CreateWindowExA(WS_EX_TOPMOST, …, WS_POPUP|WS_SYSMENU, 0,0, screenW, screenH)`. The window is a topmost popup sized to the *mode*, so it's never a real windowed mode.
- `page_flip` (`0x1005eb00`) calls `Flip` (or `Blt` in a non-flip path). With `wait=true` it **spin-waits** on `is_page_flip_done`.

### Timing / CPU
- `omt_GetMilliSecsCounter` (`0x10395370`) = `GetTickCount() - base` (~15.6 ms granularity), unless a trap function is installed. **`omt_SetMilliSecsCounterTrap` is exported**, so a QPC-based clock can be injected without patching code.
- `OMediaApplication::start` (`0x10037390`) is `while(!quit) OMediaPeriodical::devote_time();`. It never sleeps or waits, so it **pins one core at 100%**. Every game system is an `OMediaPeriodical` ticked from that loop.
- A coarse clock plus an unthrottled loop is the classic source of jitter and speed problems on fast CPUs.

### Other legacy dependencies
- **CD check** (`FUN_0040e470`): scans every `DRIVE_CDROM` for `Movies\mSpongeBob.bik`. Without it you get "You must insert the game CD".
- Single-instance guard: named file mapping `Employee of the Month`.
- DirectSound (`DirectSoundCreate`); the game aborts if DirectSound init fails. DirectInput 1-era `DirectInputCreateA`. OMT also links WinSock/IPX/serial/modem code that the game never uses.
- 8-bit palette paths (GDI palette APIs) still exist in OMT but aren't used at 16 bpp.

### Data formats
| Ext | Format | Status |
|---|---|---|
| `.omt` (sb2.omt, sceneart, sounds, music, Font.omt) | OMT database, magic `0MF2`, chunk tags (`OmCv` canvas, `OmGW`, `OPa2` palette, `Canv`, `3DMa`, `Wave`) | Needs a parser. The OMT class streamers (`OMediaClassStreamer`, `*::builder`) document the layout. |
| `.grn` (674 models) | Granny 1/2 (compressed) | Proprietary. Practical route: an offline converter that drives the 32-bit `granny.dll` and exports glTF. |
| `.bik` (15 movies) | Bink 1 | FFmpeg decodes Bink 1, so convert offline to modern video or decode in-engine with libavcodec. |
| `scripts/*.sb2`, `dialogue/*.sb2` | **Plain tab-separated text, XOR `0x30`**, with a 2-byte header per line | Confirmed by decoding (e.g. `talk\tBAA`, `add\tiub1\tUmbrella`). Trivial to load. |
| `scenes/*.scn` | Binary, magic `RMAC` | Needs RE (loader is in sb2.exe). |

## 3. Constraints that drive the plan
1. **x64 / ARM64 rules out in-process reuse of `granny.dll` and `binkw32.dll`**: both are 32-bit and closed. Their assets have to be converted, or the formats reimplemented.
2. OMT exports C++ classes by name, and sb2 depends on their **object layouts and vtables**. A drop-in OMT replacement therefore has to be ABI-compatible (32-bit MSVC6 layout), which is only workable as a 32-bit compatibility layer.
3. The game logic is small (≈300 KB of code, much of it CRT), and its scripts and dialogue are plain text. A real source-level rebuild of sb2 is feasible. The engine (OMT) is the larger job.

## 4. Recommended phased plan

**Phase 0 — Compatibility shim (32-bit, quick wins, no asset work).** A small `version.dll`/`dinput.dll` proxy loaded by `sb2.exe` that:
- installs a QPC clock via `omt_SetMilliSecsCounterTrap`;
- hooks `OMediaApplication::start` / `devote_time` to add frame pacing (`Sleep(0)` / waitable timer), and removes the busy-wait in `page_flip` and the 1 s spin in `setmode`;
- turns exclusive fullscreen into **borderless windowed**: make `SetCooperativeLevel` use `DDSCL_NORMAL`, skip `SetDisplayMode`, size a `WS_POPUP` to the monitor, and present 640×480 scaled with correct aspect;
- bypasses the CD check by patching `FUN_0040e470` to return 1 and pointing `DAT_00457314` at the install dir;
- keeps dgVoodoo (or a custom D3D6→D3D11 layer) for the API translation.
→ Result: smooth, borderless, low-CPU play on Win10/11 with the original binaries. The image stays 640×480 upscaled.

**Phase 1 — Asset pipeline (offline, any arch).** Tools to: parse `0MF2` `.omt` → PNG/WAV/JSON; convert `.grn` → glTF (32-bit helper process using `granny.dll`); convert `.bik` → AV1/H.264 (FFmpeg); decode `.sb2` text and `.scn` scenes → JSON.

**Phase 2 — New engine (x64/ARM64, C++20).** Reimplement only the OMT surface sb2 uses (the 208 exports): scene graph (2D/3D elements, worlds, viewport), canvases/sprites, 3D shapes and materials, sound, input and file/DB. Render with **D3D12 or D3D11** (or SDL3 GPU for portability), with batched draws instead of per-polygon calls. Use resolution-independent coordinates (the 640×480 logical space maps to any output) and a fixed-timestep simulation with interpolated rendering.

**Phase 3 — Game logic port.** Decompile sb2.exe's ~1,245 functions by subsystem (app/scene manager, script VM, inventory/verbs, dialogue, save slots `Saved\slotN.sb2`, movie playback, Granny animation driver) and rewrite them against the new engine. Use the original as the oracle: compare script execution and save-file round-trips.

**Phase 4 — Polish.** Widescreen handling (pillarbox vs extended backgrounds), higher-res UI, settings UI (resolution, vsync, window mode), save compatibility, and an ARM64 build.

## 5. Open questions / unknowns
- `.scn` (`RMAC`) and `0MF2` internal layouts are not yet mapped. Next step: decompile the `OMediaDataBase` / `*::builder` streamers and sb2's scene loader (`scenes\c%dl%ds%d.scn`).
- The Granny version isn't confirmed (the API looks like Granny 1.x / early 2).
- Whether game logic is tick- or delta-based isn't confirmed. Inspect sb2's `OMediaPeriodical` subclass at vtable `0x44db10`.
- The registry values at `DAT_00454480` / `DAT_00454478` (two unnamed settings) haven't been read.
- Behavior isn't verified at runtime. Every finding above comes from static analysis.
