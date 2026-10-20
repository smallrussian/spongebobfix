"""Builds the release zip: rebuild/releases/sb2modern-<version>.zip

Contents: the Phase 0 DLLs (from rebuild/phase0/build/Release), a default sb2modern.ini generated
from the defaults compiled into config.cpp, and INSTALL.txt. No game files.

Usage: python make_release.py <version>      e.g. v0.1.0
"""
import hashlib
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PHASE0 = os.path.join(HERE, "..", "phase0")


def default_ini():
    src = open(os.path.join(PHASE0, "common", "config.cpp"), encoding="utf-8").read()
    block = src[src.index("kDefaultIni"):]
    block = block[:block.index(";\n")]
    parts = re.findall(r'L"((?:[^"\\]|\\.)*)"', block)
    return "".join(parts).encode("latin1").decode("unicode_escape")


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else "v0.1.0"
    root = f"sb2modern-{version}"
    out = os.path.join(HERE, f"{root}.zip")
    files = {
        "ddraw.dll": open(os.path.join(PHASE0, "build", "Release", "ddraw.dll"), "rb").read(),
        "dsound.dll": open(os.path.join(PHASE0, "build", "Release", "dsound.dll"), "rb").read(),
        # The DLL reads the ini as UTF-16 (it writes it that way on first run).
        "sb2modern.ini": default_ini().encode("utf-16"),
        "INSTALL.txt": open(os.path.join(HERE, "INSTALL.txt"), "rb").read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"),
    }
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(f"{root}/{name}", data)
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    print(out)
    for name, data in files.items():
        print(f"  {name:14} {len(data):8} bytes  sha256 {hashlib.sha256(data).hexdigest()[:16]}")
    print(f"zip sha256 {digest}")


if __name__ == "__main__":
    main()
