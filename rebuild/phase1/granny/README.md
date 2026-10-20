# Granny models → glTF

The game's characters and items are RAD Granny **1.2b** files (`models\people\*.grn`,
`models\items\*.grn`). That format is undocumented and obfuscated, so instead of parsing it we
drive the game's own `granny.dll` with the same calls `sb2.exe` makes, and read back what it
hands to the renderer.

```
grn_dump.exe  (32-bit C++, loads granny.dll)  ->  <model>.grd  raw meshes + baked animation frames
grn_convert.py                                ->  <model>.gltf/.bin + textures/*.png
```

## Build and run

```
cmake -S rebuild/phase1/granny -B rebuild/phase1/granny/build -A Win32
cmake --build rebuild/phase1/granny/build --config Release
python rebuild/phase1/granny/grn_convert.py <game_dir> <out_dir> [--fps 30] [--no-anims] [--only NAME]
```

Result for this game: 125 models (69 characters, 56 items), 549 animation clips, all textures
resolved. Output is `<out>/people/*`, `<out>/items/*` and `<out>/models.json` (vertex/triangle
counts and clip lengths).

## How sb2.exe uses granny.dll

| sb2.exe | Calls |
|---|---|
| 0x40D2D0 | `GrannyOpenVersion("1.2b", "win32", "10-4-2000", copyright, &granny)`, `GrannyAllowTextureFormat(granny, 0xB)` |
| 0x403810 | `GrannyOpenModel(granny, "<name>.grn", &model)`; `GetToolCoordinateSystemForModel` → `RecommendTransform` → `TransformModel`; `OpenSequence(model, &seq)` |
| 0x403E10 | `GetNewTexturesLeft` / `LockNextNewTexture` / `UnlockNewTexture(&tex, material)`. Texture `+4` is the 3DStudio material name, which is looked up among the model `.omt` file's `3DMa` records |
| 0x4039C0 `BuildShape()` | `LockSequenceForRendering(seq, 0x3B, &r)`, then for each rendering state `LockNextRenderingState` → copy → `UnlockRenderingState`; `UnlockRendering` |
| 0x404200 | animations `models\people\<model>_<clip>.grn`: `OpenAnimation`, the same transform, `TransformAnimation` |
| 0x4046D0 | play: `AddControlledAnimation(seq, anim, &ctrl)`; `SetAnimationParameter` 3 (loop count, -1 → FLT_MAX), 6/8 (ease 0.4 s), 2 |
| 0x437C32 | each frame `GrannyAdvanceTime(granny, ms * 0.001)` |

Handles are pairs or triples of dwords passed by value (`{session, object}`, controls
`{session, sequence, control}`).

**Rendering state** (`LockNextRenderingState` output). Each array is described by a 0x18-byte
block: `+0` present, `+0xC` count, `+0x14` pointer.

| Offset | Content |
|---|---|
| 0x18 | the texture's user value (set by `UnlockNewTexture`) |
| 0x2C | positions, float3 |
| 0x74 | texture coordinates, float3 (u, v, unused) |
| 0x8C | normals, float3 |
| 0xA4 | triangles, 3 × u16 |

**Coordinate transform.** The game asks for origin 0 and axis rows `(-B0,0,0), (0,0,B5), (0,B7,0)`,
where `B` is the tool axis block. In practice this means Z up, X mirrored for Direct3D, and units
of 1 cm.

**Animation control parameters** (`GrannyGetAnimationParameter`):

| Parameter | Meaning |
|---|---|
| 0 | start of one play (seconds) |
| 1 | end of one play (seconds) |
| 2 | speed |
| 3 | loop count |
| 4 | weight |
| 5–8 | ease in/out |

Granny evaluates pose state incrementally on every `AdvanceTime` step, and a large jump in time
(for example starting at t=1000) collapses the mesh. `grn_dump` therefore opens a fresh session
for each clip and steps its clock from 0 in 1/fps increments.

## .grd format (grn_dump output, little-endian)

```
'GRD1'
u32 texture_count, per texture: str material, str source_file        (str = u16 length + bytes)
u32 set_count; per set (set 0 = "rest", then one per animation):
    str name, f32 fps, u32 frame_count, frames
frame: u32 state_count; per state:
    u32 texture_index, u32 vertex_count, f32 pos[n*3], f32 normal[n*3], f32 uv[n*3],
    u32 triangle_count, u16 index[t*3]
```

Coordinates are game space (the same space as the positions in `.scn` scene files).

## glTF output

- **Space:** Y up, metres, from game space as `(-x, z, -y) * 0.01`. Triangle winding is flipped
  back to counter-clockwise.
- **Materials:** one per Granny texture. Textured materials embed the `.omt` canvas as a PNG.
  Flat 18-byte `3DMa` records (the glass helmets) become `BLEND` materials with
  `baseColorFactor` taken from their colour and opacity. A material missing from the model's
  own database is taken from the database that defines it; for example, Leon holds the kelp item.
- **Animations:** Granny only exposes deformed vertices, so every sampled frame is a morph target
  (POSITION and NORMAL deltas). Each glTF animation steps the morph weights linearly through its
  own frames. The `.grd` files stay the lossless source for the new engine.

The Phase 2 engine can reload `.grd` directly. If it needs real skinning later, `granny.dll` also
exports `GetBoneCount`, `GetBoneState` and `GetBoneTreeState`.
