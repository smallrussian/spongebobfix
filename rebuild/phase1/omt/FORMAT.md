# OMT asset databases (`*.omt`)

The OMT engine ("Open Media Toolkit") started on the Mac, so every integer in these files is
**big-endian**, apart from the raw pixel/run memory noted below. The game ships 316 databases:
UI and backgrounds, sounds, model textures and materials. `omt_export.py` exports all of them.

| Objects | Exported as |
|---|---|
| 2808 canvases | PNG, colour keys become alpha |
| 2007 waves | WAV |
| 177 3D materials | JSON |

## Container

```
'0MF2' u32 directory_offset
... object data ...
directory @ directory_offset:
    u32 type_count
    per type: tag[4] u32 count
        per object: i32 id, u32 offset, u32 size, u8 name_length, name
```

Names are not unique inside a database. The exporter appends `_<id>` when a name repeats.

| Tag | Content |
|---|---|
| `Canv` | Bitmap (canvas). |
| `Wave` | A plain RIFF WAV, possibly after a small header. Exported from the `RIFF` marker. |
| `3DMa` | 3D material. |
| `mCTy`, `0HDR` | Bookkeeping, one per file. |

## Canvas (`Canv`)

Reader: OMT.dll `OMediaGfxWorld::read_class` (0x10066CB0). Unpacker: 0x10085790.

```
'OmCv' u32 flags
'OmGW' u16 width, u16 height, u16 depth (8/16/32), u8 pack_mode, u8 key_type, u16 row_bytes
u16 has_key
    key_type == 0: u16 r, g, b       (16-bit per channel; used only if has_key)
    key_type  > 0 and has_key: u32 packed key, compared with the stored pixel bytes
u16 has_palette
    'OPa2' u32 ?, u8 name_length + name, u32 count, count x (u16 r, g, b), 4096-byte inverse table
u32 packed_size
height x (u16 row_length, PackBits data)
```

**PackBits.** `pack_mode` 0, 1 and 2 select 8-, 16- and 32-bit units. Each run header is one unit
wide and is read as raw little-endian memory. 32-bit headers are truncated to a signed 16-bit
value.

| Header n | Meaning |
|---|---|
| 0 … 127 | n+1 literal units follow |
| -127 … -1 | 1 unit, repeated 1−n times |
| -128 | no-op |

**Pixels:**

| Depth | Pixel format |
|---|---|
| 8-bit | palette index |
| 16-bit | big-endian `xRRRRRGGGGGBBBBB` |
| 32-bit | raw x86 dword, bytes B, G, R, x |

## Wave (`Wave`)

Standard RIFF/WAVE data (8-bit or 16-bit PCM). It's playable as-is from the `RIFF` marker.

## 3D material (`3DMa`)

Reader: OMT.dll `OMedia3DMaterial::read_class` (0x1001D0D0). All records start with
`u16 version (1), u16 kind`.

**Kind 2, textured (52 bytes):**

```
+4   12 bytes, zero
+16  3 x (u16 r, g, b)        colours (white, then two unused)
+34  4 x u16 flags
+42  'Canv' u32 canvas_id u16 3     texture = canvas with that id in the same database
```

**Kind 0, flat (18 bytes):**

```
+4   f32 opacity
+8   u16 0
+10  u16 r, g, b
+16  u16 3
```

Kind 0 is used for SpongeBob's and Sandy's translucent helmets.

Material names match Granny texture names; see `../granny/README.md`. The material links to its
texture by canvas **id**, not by name, and the two can disagree. In `san_model`, material
`sandy1` uses canvas 0, whose name is `sandy2`.
