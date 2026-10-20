"""Exporter for the OMT engine's asset databases (*.omt).

Container (all integers big-endian; the engine came from the Mac):
    'OMF2'-style header: '0MF2' u32 directory_offset
    objects ...
    directory: u32 type_count, then per type: tag[4] u32 count, count x {i32 id, u32 offset, u32 size, pstring name}
Object types found in this game: Canv (bitmap), Wave (RIFF WAV), 3DMa (3D material), mCTy/0HDR (bookkeeping).

Canvas ('OmCv' u32 flags, then an 'OmGW' gfx world, read by OMT.dll OMediaGfxWorld::read_class 0x10066CB0):
    'OmGW' u16 width, height, depth  u8 pack_mode  u8 key_type  u16 row_bytes
    bool has_key (u16)   key_type==0: 3 x u16 RGB key   key_type>0 and has_key: u32 packed key
    bool has_palette (u16)   palette: 'OPa2' u32 ?, u8-length name, u32 count, count x 3 u16 (RGB), 4096-byte inverse table
    u32 packed_size, then per row: u16 row_length + PackBits data (OMT.dll 0x10085790),
        pack_mode 0/1/2 = runs of 8/16/32-bit units. Pixels: 8-bit palette index,
        16-bit xRRRRRGGGGGBBBBB, 32-bit xRGB. 16-bit pixels are big-endian, 32-bit are raw x86 BGRx; PackBits run headers
        are little-endian (the engine reads them as raw x86 memory).

Usage:
    python omt_export.py <file.omt> <out_dir>          export one database
    python omt_export.py --tree <game_dir> <out_dir>    export every .omt in the game
"""
import json
import os
import struct
import sys
import zlib


def directory(d):
    off = struct.unpack_from(">I", d, 4)[0]
    p = off
    (ntypes,) = struct.unpack_from(">I", d, p)
    p += 4
    out = {}
    for _ in range(ntypes):
        tag = d[p:p + 4].decode("latin1")
        (n,) = struct.unpack_from(">I", d, p + 4)
        p += 8
        entries = []
        for _ in range(n):
            i, o, s = struct.unpack_from(">iII", d, p)
            p += 12
            ln = d[p]
            name = d[p + 1:p + 1 + ln].decode("latin1")
            p += 1 + ln
            entries.append({"id": i, "offset": o, "size": s, "name": name})
        out[tag] = entries
    return out


def unpack_bits(src, unit, out_units):
    """PackBits with 8/16/32-bit units; header is one unit wide (signed)."""
    out = bytearray()
    p = 0
    need = out_units * unit
    # Run headers are raw native (little-endian) memory; the engine truncates them to 16 bits.
    fmt = {1: "b", 2: "<h", 4: "<i"}[unit]
    while len(out) < need and p < len(src):
        (n,) = struct.unpack_from(fmt, src, p)
        if unit == 4:
            n = ((n & 0xFFFF) ^ 0x8000) - 0x8000
        p += unit
        if n >= 0:
            count = (n + 1) * unit
            out += src[p:p + count]
            p += count
        elif n != -128:
            out += src[p:p + unit] * (1 - n)
            p += unit
    return bytes(out[:need])


def png(path, w, h, rgba):
    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


_LUT555 = [bytes((((v >> 10) & 31) << 3 | ((v >> 10) & 31) >> 2, ((v >> 5) & 31) << 3 | ((v >> 5) & 31) >> 2,
                  (v & 31) << 3 | (v & 31) >> 2, 255)) for v in range(65536)]


def decode_canvas(d):
    """Returns (width, height, depth, rgba bytes, info dict)."""
    if d[0:4] != b"OmCv" or d[8:12] != b"OmGW":
        raise ValueError(f"not an OMT canvas ({d[:4]!r})")
    p = 12
    w, h, depth = struct.unpack_from(">HHH", d, p)
    p += 6
    mode, key_type = d[p], d[p + 1]
    p += 2
    (row_bytes,) = struct.unpack_from(">H", d, p)
    p += 2
    (has_key,) = struct.unpack_from(">H", d, p)
    p += 2
    key = None      # packed key, compared against the raw pixel bytes
    key_rgb = None  # 8-bit RGB key, compared after decoding
    if key_type == 0:
        r, g, b = struct.unpack_from(">HHH", d, p)
        p += 6
        if has_key:
            key_rgb = (r >> 8, g >> 8, b >> 8)
    elif has_key:
        (key,) = struct.unpack_from(">I", d, p)
        p += 4
    (has_palette,) = struct.unpack_from(">H", d, p)
    p += 2
    palette = None
    if has_palette:
        if d[p:p + 4] != b"OPa2":
            raise ValueError("palette tag missing")
        p += 4
        p += 1 + d[p]  # name (u8 length + chars)
        (count,) = struct.unpack_from(">I", d, p)
        p += 4
        palette = []
        for _ in range(count):
            r, g, b = struct.unpack_from(">HHH", d, p)
            p += 6
            palette.append((r >> 8, g >> 8, b >> 8))
        p += 0x1000  # 'OPa2' palettes carry a 4096-byte inverse colour table (OMediaPalette::read_class)
    (packed_size,) = struct.unpack_from(">I", d, p)
    p += 4
    packed = d[p:p + packed_size]

    unit = {0: 1, 1: 2, 2: 4}[mode]
    bpp = depth // 8
    rows = []
    q = 0
    for _ in range(h):
        (n,) = struct.unpack_from(">H", packed, q)
        q += 2
        rows.append(unpack_bits(packed[q:q + n], unit, (w * bpp) // unit))
        q += n

    rgba = bytearray(w * h * 4)
    if depth == 32:
        src = b"".join(rows)
        # 32-bit pixels are raw x86 dwords: bytes B, G, R, x.
        rgba[0::4] = src[2::4]
        rgba[1::4] = src[1::4]
        rgba[2::4] = src[0::4]
        alpha = bytearray([255]) * (w * h)
        if key is not None:
            # The engine compares the packed key with pixel memory byte for byte.
            kb = struct.pack(">I", key)
            for i in range(w * h):
                if src[i * 4:i * 4 + 4] == kb:
                    alpha[i] = 0
        rgba[3::4] = alpha
    elif depth == 16:
        lut = _LUT555
        src = b"".join(rows)
        vals = struct.unpack(f">{w * h}H", src)
        rgba = bytearray(b"".join(lut[v] for v in vals))
        if key is not None:
            for i, v in enumerate(vals):
                if v == key:
                    rgba[i * 4 + 3] = 0
    elif depth == 8:
        pal = [bytes((*(palette[i] if palette and i < len(palette) else (i, i, i)), 255)) for i in range(256)]
        if key is not None and key < 256:
            pal[key] = pal[key][:3] + b"\x00"
        rgba = bytearray(b"".join(pal[i] for i in b"".join(rows)))
    else:
        raise ValueError(f"unsupported depth {depth}")
    if key_rgb is not None:
        # 16/32-bit keys are 5/8 bits per channel; compare at the canvas's own precision.
        shift = 3 if depth == 16 else 0
        kr, kg, kb_ = (c >> shift for c in key_rgb)
        for i in range(w * h):
            o = i * 4
            if rgba[o] >> shift == kr and rgba[o + 1] >> shift == kg and rgba[o + 2] >> shift == kb_:
                rgba[o + 3] = 0
    info = {"width": w, "height": h, "depth": depth, "pack_mode": mode, "key": key, "key_rgb": key_rgb,
            "palette": palette is not None}
    return w, h, bytes(rgba), info


def decode_material(d):
    """3DMa records: 52 bytes in this game (OMT.dll OMedia3DMaterial::read_class 0x1001D0D0)."""
    out = {"header": list(struct.unpack_from(">HH", d, 0))}
    if len(d) < 52:
        # 3 short (18-byte) records exist with an older layout; keep them raw.
        out["raw"] = d.hex()
        return out
    out["raw_12"] = d[4:16].hex()
    out["colors"] = [[c >> 8 for c in struct.unpack_from(">HHH", d, 16 + i * 6)] for i in range(3)]
    out["flags"] = list(struct.unpack_from(">HHHH", d, 34))
    if d[42:46] == b"Canv":
        out["texture"] = {"type": "Canv", "database": struct.unpack_from(">I", d, 46)[0], "id": struct.unpack_from(">H", d, 50)[0]}
    return out


def export(path, out_dir):
    d = open(path, "rb").read()
    dr = directory(d)
    os.makedirs(out_dir, exist_ok=True)
    index = {"source": os.path.basename(path), "objects": []}
    used = set()
    for tag, entries in dr.items():
        for e in entries:
            obj = d[e["offset"]:e["offset"] + e["size"]]
            label = e["name"] or f"{tag}_{e['id']}"
            safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in label)
            if (tag, safe.lower()) in used:  # names are not unique inside a database
                safe = f"{safe}_{e['id']}"
            used.add((tag, safe.lower()))
            rec = {"type": tag, **e}
            if tag == "Canv":
                w, h, rgba, info = decode_canvas(obj)
                fn = f"{safe}.png"
                png(os.path.join(out_dir, fn), w, h, rgba)
                rec.update(info, file=fn)
            elif tag == "Wave":
                start = obj.find(b"RIFF")
                fn = f"{safe}.wav"
                open(os.path.join(out_dir, fn), "wb").write(obj[start:])
                rec["file"] = fn
            elif tag == "3DMa":
                rec["material"] = decode_material(obj)
            index["objects"].append(rec)
    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=1)
    return index


def export_tree(game_dir, out_dir):
    total = {"files": 0, "Canv": 0, "Wave": 0, "3DMa": 0}
    for root, _, names in os.walk(game_dir):
        if os.path.relpath(root, game_dir).split(os.sep)[0] in ("converted", "rebuild", "_dgvoodoo_backup"):
            continue
        for name in sorted(names):
            if not name.lower().endswith(".omt"):
                continue
            src = os.path.join(root, name)
            rel = os.path.splitext(os.path.relpath(src, game_dir))[0]
            idx = export(src, os.path.join(out_dir, rel))
            total["files"] += 1
            for o in idx["objects"]:
                if o["type"] in total:
                    total[o["type"]] += 1
            print(f"{rel}: {len(idx['objects'])} objects", flush=True)
    print("done", total)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--tree":
        export_tree(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 3:
        idx = export(sys.argv[1], sys.argv[2])
        print(f"{len(idx['objects'])} objects")
    else:
        print(__doc__)
        sys.exit(1)
