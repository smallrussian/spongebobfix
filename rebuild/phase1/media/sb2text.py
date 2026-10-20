"""Decoder for the game's .sb2 text files (scripts\\c*\\*.sb2, dialogue\\*.sb2).

Layout (little-endian):
    u16 line_count
    line_count x { u16 length; byte text[length] }
Each text byte is XOR 0x30. Decoded lines are tab-separated fields, usually ending in '\\n'.

Usage:
    python sb2text.py <file.sb2>                 print decoded text
    python sb2text.py --tree <game_dir> <out>    decode every .sb2 under scripts/ and dialogue/
                                                 to .txt and .json, plus a verb census
"""
import json
import os
import struct
import sys
from collections import Counter, defaultdict

KEY = 0x30


def decode(data: bytes) -> list[list[str]]:
    """Returns the file as a list of rows (each a list of tab-separated fields)."""
    return decode_full(data)[0]


def decode_full(data: bytes) -> tuple[list[list[str]], list[bool]]:
    """Returns the rows plus, per row, whether the original line ended in a newline."""
    (count,) = struct.unpack_from("<H", data, 0)
    pos, rows, newline = 2, [], []
    for _ in range(count):
        (length,) = struct.unpack_from("<H", data, pos)
        pos += 2
        raw = bytes(b ^ KEY for b in data[pos:pos + length])
        pos += length
        text = raw.decode("cp1252")
        newline.append(text.endswith("\n"))
        rows.append(text.rstrip("\n").split("\t"))
    if pos != len(data):
        raise ValueError(f"{len(data) - pos} trailing bytes")
    return rows, newline


def encode(rows: list[list[str]], newline: list[bool] | None = None) -> bytes:
    """Inverse of decode_full(); lines end in a newline unless `newline` says otherwise."""
    out = bytearray(struct.pack("<H", len(rows)))
    for i, row in enumerate(rows):
        nl = "\n" if newline is None or newline[i] else ""
        raw = ("\t".join(row) + nl).encode("cp1252")
        out += struct.pack("<H", len(raw)) + bytes(b ^ KEY for b in raw)
    return bytes(out)


def convert_tree(game_dir: str, out_dir: str) -> None:
    verbs = Counter()
    arity = defaultdict(Counter)
    examples = {}
    files = 0
    for sub in ("scripts", "dialogue"):
        for root, _, names in os.walk(os.path.join(game_dir, sub)):
            for name in sorted(names):
                if not name.lower().endswith(".sb2"):
                    continue
                src = os.path.join(root, name)
                rel = os.path.relpath(src, game_dir)
                rows, newline = decode_full(open(src, "rb").read())
                files += 1
                dst = os.path.join(out_dir, os.path.splitext(rel)[0])
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst + ".txt", "w", encoding="utf-8") as f:
                    f.write("\n".join("\t".join(r) for r in rows) + "\n")
                with open(dst + ".json", "w", encoding="utf-8") as f:
                    json.dump({"source": rel.replace("\\", "/"), "rows": rows, "newline": newline}, f, indent=1, ensure_ascii=False)
                if sub == "scripts":
                    for r in rows:
                        verb = r[0]
                        verbs[verb] += 1
                        arity[verb][len(r) - 1] += 1
                        examples.setdefault(verb, "\t".join(r))
    census = {
        v: {"count": n, "arg_counts": dict(arity[v]), "example": examples[v]}
        for v, n in verbs.most_common()
    }
    with open(os.path.join(out_dir, "script_verbs.json"), "w", encoding="utf-8") as f:
        json.dump(census, f, indent=1, ensure_ascii=False)
    print(f"decoded {files} files, {len(verbs)} distinct script verbs -> {out_dir}")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--tree":
        convert_tree(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2:
        for row in decode(open(sys.argv[1], "rb").read()):
            print("\t".join(row))
    else:
        print(__doc__)
        sys.exit(1)
