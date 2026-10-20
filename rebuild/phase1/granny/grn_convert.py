"""Converts the game's Granny models (models\\people, models\\items) to glTF 2.0.

Pipeline per model:
    grn_dump.exe (32-bit, drives the game's granny.dll) -> <name>.grd   raw meshes + baked animation frames
    this script                                         -> <name>.gltf + <name>.bin + textures (PNG)

Characters are <prefix>_model.grn plus animations <prefix>_<clip>.grn in the same folder. Granny only
exposes deformed vertices, so each animation is baked: every sampled frame becomes a morph target and
the glTF animation steps the morph weights. The .grd files are kept as the lossless source for the
new engine.

Coordinates: the game uses Granny's tool space (Z up) mirrored on X for Direct3D; glTF is Y up,
right-handed, metres. Granny reports 39.37 units per metre-ish tool scale; the models are authored so
that 1 unit = 1 cm, so positions are scaled by 0.01.

Textures: each Granny texture names a 3D material ('3DMa') in the model's .omt database, whose
texture link is a canvas ('Canv') id in the same database.

Usage:
    python grn_convert.py <game_dir> <out_dir> [--fps N] [--no-anims] [--only NAME]
"""
import argparse
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "omt"))
import omt_export  # noqa: E402

SCALE = 0.01


def read_grd(path):
    d = open(path, "rb").read()
    if d[:4] != b"GRD1":
        raise ValueError(f"{path}: not a grn_dump file")
    p = 4

    def u32():
        nonlocal p
        v = struct.unpack_from("<I", d, p)[0]
        p += 4
        return v

    def s():
        nonlocal p
        n = struct.unpack_from("<H", d, p)[0]
        v = d[p + 2:p + 2 + n].decode("latin1")
        p += 2 + n
        return v

    textures = [(s(), s()) for _ in range(u32())]
    sets = []
    for _ in range(u32()):
        name = s()
        fps = struct.unpack_from("<f", d, p)[0]
        p += 4
        frames = []
        for _ in range(u32()):
            states = []
            for _ in range(u32()):
                tex, nv = u32(), u32()
                arrays = [d[p + i * nv * 12:p + (i + 1) * nv * 12] for i in range(3)]
                p += nv * 36
                nt = u32()
                idx = d[p:p + nt * 6]
                p += nt * 6
                states.append({"tex": tex, "nv": nv, "pos": arrays[0], "nrm": arrays[1], "uv": arrays[2], "idx": idx})
            frames.append(states)
        sets.append({"name": name, "fps": fps, "frames": frames})
    return textures, sets


def to_gltf_vec(raw, scale):
    """Game space (x mirrored, z up) -> glTF (y up): (-x, z, -y)."""
    n = len(raw) // 12
    v = struct.unpack(f"<{n * 3}f", raw)
    out = [0.0] * (n * 3)
    out[0::3] = [-x * scale for x in v[0::3]]
    out[1::3] = [z * scale for z in v[2::3]]
    out[2::3] = [-y * scale for y in v[1::3]]
    return out


class Builder:
    def __init__(self):
        self.bin = bytearray()
        self.g = {"asset": {"version": "2.0", "generator": "sb2 rebuild grn_convert.py"},
                  "buffers": [], "bufferViews": [], "accessors": [], "meshes": [], "nodes": [],
                  "scenes": [{"nodes": [0]}], "scene": 0}

    def view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target:
            bv["target"] = target
        self.bin += data
        self.g["bufferViews"].append(bv)
        return len(self.g["bufferViews"]) - 1

    def accessor(self, data, comp, count, typ, minmax=None, target=None):
        a = {"bufferView": self.view(data, target), "componentType": comp, "count": count, "type": typ}
        if minmax:
            a["min"], a["max"] = minmax
        self.g["accessors"].append(a)
        return len(self.g["accessors"]) - 1

    def vec3(self, values, with_bounds=False):
        n = len(values) // 3
        mm = None
        if with_bounds and n:
            mm = ([min(values[i::3]) for i in range(3)], [max(values[i::3]) for i in range(3)])
        return self.accessor(struct.pack(f"<{len(values)}f", *values), 5126, n, "VEC3", mm, 34962)


def load_materials(omt_path, tex_dir, rel_dir):
    """Returns {material name lower: image uri} using the model's .omt database."""
    if not os.path.exists(omt_path):
        return {}
    d = open(omt_path, "rb").read()
    dr = omt_export.directory(d)
    canv = {e["id"]: e for e in dr.get("Canv", [])}
    out = {}
    for e in dr.get("3DMa", []):
        m = omt_export.decode_material(d[e["offset"]:e["offset"] + e["size"]])
        t = m.get("texture")
        if "opacity" in m:
            out[e["name"].lower()] = {"color": [c / 255 for c in m["color"]] + [m["opacity"]]}
            continue
        if not t or t["id"] not in canv:
            continue
        c = canv[t["id"]]
        w, h, rgba, info = omt_export.decode_canvas(d[c["offset"]:c["offset"] + c["size"]])
        fn = f"{os.path.splitext(os.path.basename(omt_path))[0]}_{c['name'] or c['id']}.png"
        fn = "".join(ch if ch.isalnum() or ch in "-_." else "_" for ch in fn)
        omt_export.png(os.path.join(tex_dir, fn), w, h, rgba)
        out[e["name"].lower()] = {"uri": f"{rel_dir}/{fn}" if rel_dir else fn, "alpha": info["key"] is not None or info["key_rgb"] is not None}
    return out


def build_gltf(name, textures, sets, materials, out_dir, anims=True):
    b = Builder()
    g = b.g
    rest = sets[0]["frames"][0]
    clips = [s for s in sets[1:] if s["frames"]] if anims else []
    # Morph targets: every frame of every clip.
    targets_of = []  # (clip index, frame index)
    for ci, c in enumerate(clips):
        for fi in range(len(c["frames"])):
            targets_of.append((ci, fi))

    g["images"], g["textures"], g["materials"], g["samplers"] = [], [], [], [{"magFilter": 9729, "minFilter": 9987}]
    mat_index = {}

    def material_for(tex):
        if tex in mat_index:
            return mat_index[tex]
        mname = textures[tex][0] if tex < len(textures) else ""
        m = {"name": mname or f"material_{tex}", "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
        info = materials.get(mname.lower())
        if info and "color" in info:
            m["pbrMetallicRoughness"]["baseColorFactor"] = info["color"]
            m["alphaMode"] = "BLEND"
        elif info:
            g["images"].append({"uri": info["uri"]})
            g["textures"].append({"source": len(g["images"]) - 1, "sampler": 0})
            m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(g["textures"]) - 1}
            if info["alpha"]:
                m["alphaMode"] = "MASK"
        g["materials"].append(m)
        mat_index[tex] = len(g["materials"]) - 1
        return mat_index[tex]

    prims = []
    for si, st in enumerate(rest):
        if not st["nv"] or not st["idx"]:
            continue
        pos = to_gltf_vec(st["pos"], SCALE)
        nrm = to_gltf_vec(st["nrm"], 1.0)
        uvw = struct.unpack(f"<{st['nv'] * 3}f", st["uv"])
        uv = [0.0] * (st["nv"] * 2)
        uv[0::2], uv[1::2] = uvw[0::3], uvw[1::3]
        idx = struct.unpack(f"<{len(st['idx']) // 2}H", st["idx"])
        # The X mirror flips handedness; swap two indices per triangle to keep front faces CCW.
        tri = []
        for t in range(0, len(idx), 3):
            tri += [idx[t], idx[t + 2], idx[t + 1]]
        prim = {"attributes": {"POSITION": b.vec3(pos, True), "NORMAL": b.vec3(nrm),
                               "TEXCOORD_0": b.accessor(struct.pack(f"<{len(uv)}f", *uv), 5126, st["nv"], "VEC2", None, 34962)},
                "indices": b.accessor(struct.pack(f"<{len(tri)}H", *tri), 5123, len(tri), "SCALAR", None, 34963),
                "material": material_for(st["tex"])}
        if targets_of:
            prim["targets"] = []
            for ci, fi in targets_of:
                fst = clips[ci]["frames"][fi]
                if si >= len(fst) or fst[si]["nv"] != st["nv"]:
                    raise ValueError(f"{name}: frame layout differs from rest pose")
                fp = to_gltf_vec(fst[si]["pos"], SCALE)
                fn = to_gltf_vec(fst[si]["nrm"], 1.0)
                prim["targets"].append({"POSITION": b.vec3([a - r for a, r in zip(fp, pos)], True),
                                        "NORMAL": b.vec3([a - r for a, r in zip(fn, nrm)])})
        prims.append(prim)

    mesh = {"name": name, "primitives": prims}
    if targets_of:
        mesh["weights"] = [0.0] * len(targets_of)
        mesh["extras"] = {"targetNames": [f"{clips[ci]['name']}#{fi}" for ci, fi in targets_of]}
    g["meshes"].append(mesh)
    g["nodes"].append({"name": name, "mesh": 0})

    if targets_of:
        g["animations"] = []
        nt = len(targets_of)
        base = 0
        for c in clips:
            nf = len(c["frames"])
            times = [i / c["fps"] for i in range(nf)]
            weights = []
            for i in range(nf):
                w = [0.0] * nt
                w[base + i] = 1.0
                weights += w
            t_acc = b.accessor(struct.pack(f"<{nf}f", *times), 5126, nf, "SCALAR", ([times[0]], [times[-1]]))
            w_acc = b.accessor(struct.pack(f"<{len(weights)}f", *weights), 5126, len(weights), "SCALAR")
            g["animations"].append({"name": os.path.splitext(c["name"])[0],
                                    "samplers": [{"input": t_acc, "output": w_acc, "interpolation": "LINEAR"}],
                                    "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}}]})
            base += nf

    g["buffers"].append({"uri": f"{name}.bin", "byteLength": len(b.bin)})
    with open(os.path.join(out_dir, f"{name}.bin"), "wb") as f:
        f.write(b.bin)
    with open(os.path.join(out_dir, f"{name}.gltf"), "w", encoding="utf-8") as f:
        json.dump(g, f, indent=1)
    return {"vertices": sum(s["nv"] for s in rest), "triangles": sum(len(s["idx"]) // 6 for s in rest),
            "clips": {os.path.splitext(c["name"])[0]: len(c["frames"]) / c["fps"] for c in clips}}


_material_owner = None


def shared_material(game_dir, name):
    """Path of a model .omt database that defines the named 3D material, or None."""
    global _material_owner
    if _material_owner is None:
        _material_owner = {}
        for sub in ("items", "people"):
            folder = os.path.join(game_dir, "models", sub)
            for n in sorted(os.listdir(folder)):
                if n.lower().endswith(".omt"):
                    path = os.path.join(folder, n)
                    for e in omt_export.directory(open(path, "rb").read()).get("3DMa", []):
                        _material_owner.setdefault(e["name"].lower(), path)
    return _material_owner.get(name.lower())


def find_models(game_dir):
    """Yields (folder, model file, [animation files]) for every model in models\\people and models\\items."""
    for sub in ("people", "items"):
        folder = os.path.join(game_dir, "models", sub)
        names = sorted(os.listdir(folder))
        grn = [n for n in names if n.lower().endswith(".grn")]
        for n in grn:
            stem = n[:-4]
            if not stem.lower().endswith("_model"):
                continue
            prefix = stem[:-6].lower() + "_"
            anims = [a for a in grn if a.lower().startswith(prefix) and a != n]
            yield sub, n, anims


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--no-anims", action="store_true")
    ap.add_argument("--only")
    ap.add_argument("--dump-exe", default=os.path.join(HERE, "build", "Release", "grn_dump.exe"))
    a = ap.parse_args()
    game = os.path.abspath(a.game_dir)
    summary = {}
    failures = 0
    for sub, model, anims in find_models(game):
        name = model[:-4]
        if a.only and a.only.lower() not in name.lower():
            continue
        out = os.path.join(os.path.abspath(a.out_dir), sub)
        tex_dir = os.path.join(out, "textures")
        os.makedirs(tex_dir, exist_ok=True)
        grd = os.path.join(out, f"{name}.grd")
        rel = lambda f: os.path.join("models", sub, f)
        cmd = [a.dump_exe, os.path.join(game, "granny.dll"), rel(model), grd, "--fps", str(a.fps)]
        cmd += [] if a.no_anims else [rel(x) for x in anims]
        r = subprocess.run(cmd, cwd=game, capture_output=True, text=True)
        if r.returncode not in (0, 7):
            print(f"FAIL {name}: grn_dump exit {r.returncode} {r.stderr.strip()}")
            failures += 1
            continue
        if r.returncode == 7:
            print(f"warn {name}: {r.stderr.strip()}")
        textures, sets = read_grd(grd)
        materials = load_materials(os.path.join(game, "models", sub, name + ".omt"), tex_dir, "textures")
        for t in textures:
            # Some models borrow a material from another model's database (e.g. Leon holds the kelp item).
            if t[0].lower() not in materials and t[0] != "<null texture>":
                donor = shared_material(game, t[0])
                if donor:
                    materials.update(load_materials(donor, tex_dir, "textures"))
        missing = [t[0] for t in textures if t[0].lower() not in materials]
        info = build_gltf(name, textures, sets, materials, out, not a.no_anims)
        info["missing_textures"] = missing
        summary[f"{sub}/{name}"] = info
        print(f"ok   {sub}/{name}: {info['vertices']} verts, {info['triangles']} tris, {len(info['clips'])} clips"
              + (f", missing textures {missing}" if missing else ""), flush=True)
    with open(os.path.join(a.out_dir, "models.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    print(f"done: {len(summary)} models, {failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
