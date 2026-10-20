"""Parser for scene files (scenes\\c<chapter>l<level>s<scene>.scn).

Layout recovered from sb2.exe's scene loader (code at 0x437210) and the per-type stream readers.
Every record uses the same versioning scheme: a u16 version v, then fields 1..v of the type's
field table (a record written with an older version simply stops early).

    camera   'RMAC' u16 version, fields[RMAC]              reader 0x40EA40
    lights   up to 5 x ('ETIL' u16 version, fields[ETIL])  reader 0x423A30
    'TPTS'   optional: 10 x 16 bytes
    'DIRG'   optional walk grid: f32 half_size, 4 x f32, then (2*half_size)^2 bytes (row stride 30)
    'DIRG'   optional second grid, same layout
    objects  until end of file: u32 type, u16 version, fields[type]
             types: CHAR 2DOB ANIM SPOT ITEM TRIG (3LIG/3DOB/SCEN carry no data)

Usage:
    python scn_parse.py <file.scn>                 print JSON
    python scn_parse.py --tree <game_dir> <out>    parse every scene to <out>/scenes/*.json
"""
import json
import math
import os
import struct
import sys

# (struct offset in the game object, byte size) in read order, extracted from sb2.exe.
FIELDS = {
    "RMAC": [(0x174, 4), (0x178, 4), (0x17c, 4), (0x180, 2), (0x182, 2), (0x184, 2), (0x188, 4), (0x18c, 2), (0x18e, 2), (0x190, 2), (0x192, 32), (0x1b2, 32), (0x1d2, 32), (0x1f2, 32), (0x214, 4), (0x218, 32), (0x238, 2), (0x23a, 2), (0x240, 4), (0x23c, 2), (0x244, 32), (0x264, 32), (0x284, 2)],
    "ETIL": [(0x180, 2), (0x174, 4), (0x178, 4), (0x17c, 4), (0x184, 4), (0x188, 4), (0x18c, 4), (0x190, 4), (0x194, 4), (0x198, 4), (0x19c, 4), (0x1a0, 4)],
    "CHAR": [(0x1e4, 32), (0x230, 4), (0x234, 4), (0x238, 4), (0x240, 2), (0x224, 4), (0x27e, 2), (0x280, 2), (0x23c, 4), (0x20, 32), (0x40, 32), (0x60, 32), (0x204, 32), (0x120, 32), (0x14, 2), (0x18, 2), (0x16, 2), (0x1a, 2), (0x2a0, 2), (0x80, 32), (0xa0, 32), (0xc0, 32), (0xe0, 32), (0x100, 32), (0x246, 2), (0x976, 32), (0x16c, 2), (0x16e, 2), (0x996, 32), (0x9b6, 32), (0x9d6, 32), (0x2a4, 2)],
    "2DOB": [(0x174, 32), (0x194, 4), (0x198, 4), (0x19c, 4), (0x1a0, 2), (0x20, 32), (0x40, 32), (0x60, 32), (0x120, 32), (0x14, 2), (0x18, 2), (0x16, 2), (0x1a, 2), (0x100, 32), (0x140, 32), (0x16c, 2), (0x16e, 2)],
    "ANIM": [(0x174, 32), (0x194, 4), (0x198, 4), (0x19c, 4), (0x1a0, 2), (0x20, 32), (0x40, 32), (0x60, 32), (0x120, 32), (0x14, 2), (0x18, 2), (0x16, 2), (0x1a, 2), (0x1b0, 2), (0x1b2, 2), (0x1b6, 2), (0x1b8, 2), (0x1ba, 2), (0x1bc, 2), (0x1be, 2), (0x140, 32), (0x1b4, 2), (0x1c0, 2), (0x100, 32), (0x1ca, 2), (0x1d0, 2), (0x1d2, 2), (0x1d4, 2)],
    "SPOT": [(0x194, 4), (0x198, 4), (0x20, 32), (0x40, 32), (0x60, 32), (0x120, 32), (0x14, 2), (0x18, 2), (0x16, 2), (0x1a, 2), (0x1b0, 2), (0x140, 32), (0x16c, 2), (0x16e, 2)],
    "ITEM": [(0x1e4, 32), (0x230, 4), (0x234, 4), (0x238, 4), (0x240, 2), (0x224, 4), (0x278, 2), (0x27a, 2), (0x23c, 4), (0x20, 32), (0x40, 32), (0x60, 32), (0x204, 32), (0x120, 32), (0x14, 2), (0x18, 2), (0x16, 2), (0x1a, 2), (0x242, 2), (0x244, 2), (0x100, 32), (0x16c, 2), (0x16e, 2), (0x246, 2)],
    "TRIG": [(0x230, 4), (0x234, 4), (0x238, 4), (0x23c, 4), (0x278, 2), (0x20, 32), (0x40, 32), (0x60, 32), (0x120, 32), (0x14, 2), (0x18, 2), (0x16, 2), (0x1a, 2), (0x27a, 2), (0x140, 32), (0x27c, 2), (0x80, 32), (0xa0, 32), (0xc0, 32), (0xe0, 32), (0x27e, 2), (0x280, 32), (0x16c, 2), (0x16e, 2), (0x100, 32)],
    "3LIG": [], "3DOB": [], "SCEN": [],
}

# Names for fields whose meaning is clear from the data and the code that uses them.
NAMES = {
    # Screen rectangle of the clickable area, in 640x480 coordinates.
    ("*", 0x14): "rect_left", ("*", 0x16): "rect_top", ("*", 0x18): "rect_right", ("*", 0x1a): "rect_bottom",
    ("*", 0x20): "name", ("*", 0x40): "label", ("*", 0x60): "script",
    ("*", 0x120): "hover_text",
    # Where the player walks to before the action triggers.
    ("SPOT", 0x194): "walk_x", ("SPOT", 0x198): "walk_y",
    ("2DOB", 0x174): "image", ("ANIM", 0x174): "image",
    ("2DOB", 0x194): "image_x", ("2DOB", 0x198): "image_y",
    ("ANIM", 0x194): "image_x", ("ANIM", 0x198): "image_y",
    ("CHAR", 0x230): "pos_x", ("CHAR", 0x234): "pos_y", ("CHAR", 0x238): "pos_z",
    ("ITEM", 0x230): "pos_x", ("ITEM", 0x234): "pos_y", ("ITEM", 0x238): "pos_z",
    ("TRIG", 0x230): "pos_x", ("TRIG", 0x234): "pos_y", ("TRIG", 0x238): "pos_z",
    ("CHAR", 0x1e4): "model", ("ITEM", 0x1e4): "model",
    ("RMAC", 0x174): "cam_x", ("RMAC", 0x178): "cam_y", ("RMAC", 0x17c): "cam_z",
    ("RMAC", 0x18c): "bg_r", ("RMAC", 0x18e): "bg_g", ("RMAC", 0x190): "bg_b",
    ("RMAC", 0x192): "exit_1", ("RMAC", 0x1b2): "exit_2", ("RMAC", 0x1d2): "exit_3", ("RMAC", 0x1f2): "exit_4",
    ("RMAC", 0x218): "exit_5", ("RMAC", 0x244): "enter_script", ("RMAC", 0x264): "music",
    ("ETIL", 0x180): "light_type",
    # Shared by the 3D object types (CHAR / ITEM / TRIG).
    ("*", 0x16c): "grid_x", ("*", 0x16e): "grid_y",
    ("*", 0x224): "scale", ("*", 0x23c): "radius", ("*", 0x240): "angle",  # angle: 65536 = 360 degrees
    ("CHAR", 0x204): "shadow_texture", ("ITEM", 0x204): "shadow_texture",
    ("CHAR", 0x80): "accepts_item", ("CHAR", 0xa0): "item_script",
    ("CHAR", 0xc0): "dialogue_node", ("CHAR", 0xe0): "dialogue_node_2", ("CHAR", 0x100): "idle_script",
    ("CHAR", 0x976): "held_model", ("CHAR", 0x996): "held_bone", ("CHAR", 0x9b6): "hat_bone",
    ("ITEM", 0x100): "use_script", ("TRIG", 0x140): "door_object", ("TRIG", 0x280): "sound",
}


class Reader:
    def __init__(self, data):
        self.d, self.p = data, 0

    def left(self):
        return len(self.d) - self.p

    def take(self, n):
        if self.p + n > len(self.d):
            raise EOFError(f"read of {n} bytes at {self.p} past end ({len(self.d)})")
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def tag(self):
        return self.take(4)[::-1].decode("latin1")  # stored little-endian; return as written in code


def value(raw):
    if len(raw) == 32:
        return raw.split(b"\0", 1)[0].decode("latin1")
    if len(raw) == 2:
        return struct.unpack("<h", raw)[0]
    i = struct.unpack("<i", raw)[0]
    f = struct.unpack("<f", raw)[0]
    if i == 0:
        return 0
    if math.isfinite(f) and 1e-6 < abs(f) < 1e7:
        return round(f, 4)
    return i


def record(r, kind, version):
    out = {"type": kind, "version": version}
    table = FIELDS[kind]
    for i, (off, size) in enumerate(table[:version]):
        name = NAMES.get((kind, off)) or NAMES.get(("*", off)) or f"f_{off:x}"
        out[name] = value(r.take(size))
    return out


def grid(r):
    head = struct.unpack("<5f", r.take(20))
    n = int(head[0] * 2)
    rows = [list(r.take(n)) for _ in range(n)]
    return {"half_size": head[0], "params": [round(x, 4) for x in head[1:]], "cells": rows}


def parse(data):
    r = Reader(data)
    scene = {}
    if r.tag() != "CAMR":
        raise ValueError("not a scene file")
    scene["camera"] = record(r, "RMAC", r.u16())
    scene["lights"] = []
    for _ in range(5):
        if r.left() <= 0:
            break
        if r.tag() != "LITE":
            raise ValueError(f"expected light record at {r.p - 4}")
        scene["lights"].append(record(r, "ETIL", r.u16()))
    if r.left() >= 4:
        if r.d[r.p:r.p + 4][::-1] == b"TPTS"[::-1] or r.d[r.p:r.p + 4] == b"TPTS":
            r.take(4)
            scene["start_points"] = [list(struct.unpack("<4f", r.take(16))) for _ in range(10)]
    for key in ("walk_grid", "grid_2"):
        if r.left() >= 4 and r.d[r.p:r.p + 4] == b"DIRG":
            r.take(4)
            scene[key] = grid(r)
    scene["objects"] = []
    while r.left() > 0:
        kind = r.tag()
        if kind not in FIELDS:
            raise ValueError(f"unknown object type {kind!r} at {r.p - 4}")
        version = r.u16() if FIELDS[kind] else 0
        scene["objects"].append(record(r, kind, version))
    return scene


def convert_tree(game_dir, out_dir):
    src = os.path.join(game_dir, "scenes")
    dst = os.path.join(out_dir, "scenes")
    os.makedirs(dst, exist_ok=True)
    ok = 0
    for name in sorted(os.listdir(src)):
        if not name.lower().endswith(".scn"):
            continue
        scene = parse(open(os.path.join(src, name), "rb").read())
        with open(os.path.join(dst, name[:-4] + ".json"), "w", encoding="utf-8") as f:
            json.dump(scene, f, indent=1)
        ok += 1
    print(f"parsed {ok} scenes -> {dst}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--tree":
        convert_tree(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2:
        print(json.dumps(parse(open(sys.argv[1], "rb").read()), indent=1))
    else:
        print(__doc__)
        sys.exit(1)
