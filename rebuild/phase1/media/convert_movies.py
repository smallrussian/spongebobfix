"""Converts the game's Bink 1 movies (Movies\\*.bik, 640x480 @ 12 fps) to modern video files.

Default: H.264 (CRF 16) + AAC in .mp4, playable everywhere.
    --av1        SVT-AV1 + Opus in .mkv instead (smaller, newer players)
    --scale N    upscale N times with Lanczos (e.g. 2 -> 1280x960)
Each output is checked with ffprobe: duration must match the source within 0.25 s.

Usage: python convert_movies.py <game_dir> <out_dir> [--av1] [--scale N] [--ffmpeg <bin_dir>]
"""
import argparse
import json
import os
import subprocess
import sys


def probe(ffprobe, path):
    out = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration:stream=codec_type,nb_frames,width,height",
                          "-of", "json", path], capture_output=True, text=True, check=True).stdout
    info = json.loads(out)
    video = next((s for s in info["streams"] if s["codec_type"] == "video"), {})
    return float(info["format"]["duration"]), video.get("width"), video.get("height")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--av1", action="store_true")
    ap.add_argument("--scale", type=int, default=1)
    ap.add_argument("--ffmpeg", default=r"C:\ffmpeg\bin")
    a = ap.parse_args()
    ffmpeg = os.path.join(a.ffmpeg, "ffmpeg.exe")
    ffprobe = os.path.join(a.ffmpeg, "ffprobe.exe")
    src_dir = os.path.join(a.game_dir, "Movies")
    os.makedirs(a.out_dir, exist_ok=True)

    vf = []
    if a.scale > 1:
        vf = ["-vf", f"scale=iw*{a.scale}:ih*{a.scale}:flags=lanczos"]
    if a.av1:
        codec = ["-c:v", "libsvtav1", "-crf", "24", "-preset", "6", "-pix_fmt", "yuv420p10le", "-c:a", "libopus", "-b:a", "160k"]
        ext = ".mkv"
    else:
        codec = ["-c:v", "libx264", "-crf", "16", "-preset", "slow", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "192k",
                 "-movflags", "+faststart"]
        ext = ".mp4"

    failures = 0
    for name in sorted(os.listdir(src_dir)):
        if not name.lower().endswith(".bik"):
            continue
        src = os.path.join(src_dir, name)
        dst = os.path.join(a.out_dir, os.path.splitext(name)[0] + ext)
        subprocess.run([ffmpeg, "-y", "-v", "error", "-i", src, *vf, *codec, dst], check=True)
        d_src, w, h = probe(ffprobe, src)
        d_dst, w2, h2 = probe(ffprobe, dst)
        ok = abs(d_src - d_dst) < 0.25 and w2 == w * a.scale and h2 == h * a.scale
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name}: {d_src:.2f}s {w}x{h} -> {d_dst:.2f}s {w2}x{h2}  {os.path.getsize(dst) // 1024} KB")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
