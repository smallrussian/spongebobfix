# Scene files (`scenes\c<chapter>l<level>s<scene>.scn`)

There are 123 scenes. `scn_parse.py` parses each one to JSON and consumes every byte of every
file. The layout comes from sb2.exe's scene loader (0x437210) and the per-type stream readers.
All values are **little-endian**, unlike the OMT databases.

## Layout

```
'CAMR' u16 version, camera fields            reader 0x40EA40
up to 5 x ('LITE' u16 version, light fields)  reader 0x423A30
optional 'TPTS'  10 x 16 bytes (4 floats each)  start points
optional 'DIRG'  walk grid: f32 half_size, 4 x f32, then (2*half_size)^2 bytes
optional 'DIRG'  second grid, same layout
objects until end of file: tag[4], u16 version, fields
```

**Tags.** Tags are stored reversed: the bytes `RMAC` mean `CAMR`. Object types are `CHAR`
(3D character), `2DOB` (2D image), `ANIM` (2D animation), `SPOT` (hotspot), `ITEM` (3D item)
and `TRIG` (trigger or door). `3LIG`, `3DOB` and `SCEN` carry no data.

**Versioning.** A record with version *v* contains the first *v* fields of its type's field
table. Older records simply stop early. The full tables, as (offset in the game object, size),
are `FIELDS` in `scn_parse.py`.

## Field types

| Size | Type |
|---|---|
| 32 bytes | NUL-terminated string |
| 2 bytes | i16 |
| 4 bytes | f32 or i32 (judged per value) |

## Common fields

| Field | Meaning |
|---|---|
| `rect_left/top/right/bottom` | Clickable screen rectangle, in 640×480 coordinates. |
| `name`, `label`, `script`, `hover_text` | Object name, display label, script run on use, and the cursor caption. |
| `pos_x/y/z` | Position of a 3D object, in the same space as the Granny models (1 unit = 1 cm, Z up). |
| `angle` | Rotation; 65536 = 360°. |
| `scale` | Scale. |
| `radius` | Radius. |
| `model` | Granny model name. |
| `grid_x`, `grid_y` | Walk-grid cell. |
| `walk_x`, `walk_y` (SPOT) | Where the player walks before the action fires. |
| `exit_1` … `exit_5`, `enter_script`, `music` (camera) | Scene exits, the entry script and the music. |

Some scenes contain leftover debug hotspots named "CHEAT To Chapter 2/3/4".

Scripts and dialogue (`*.sb2`) are documented in `SCRIPTS.md`.
