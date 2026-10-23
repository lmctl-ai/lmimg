#!/usr/bin/env python3
"""Check the committed lmimg gallery without third-party image packages."""
from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAX_PNG = 300 * 1024
MAX_GIF = 3 * 1024 * 1024


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("invalid PNG signature or IHDR")
    if data.rfind(b"IEND") < 12:
        raise ValueError("truncated PNG without IEND")
    return struct.unpack(">II", data[16:24])


def gif_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if data[:6] not in (b"GIF87a", b"GIF89a"):
        raise ValueError("invalid GIF signature")
    return struct.unpack("<HH", data[6:10])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated", action="store_true")
    args = parser.parse_args()
    examples = sorted(p.stem for p in (ROOT / "examples").glob("*.lms"))
    if not examples:
        raise SystemExit("no examples found")
    gallery = ROOT / "docs" / "images"
    missing = [name for name in examples if not (gallery / f"{name}.png").is_file()]
    if missing:
        raise SystemExit("missing gallery PNG: " + ", ".join(missing))
    for name in examples:
        path = gallery / f"{name}.png"
        if path.stat().st_size > MAX_PNG:
            raise SystemExit(f"{path} exceeds 300 KiB")
        width, height = png_size(path)
        if (width, height) != (900, 520):
            raise SystemExit(f"{path} must be 900x520 ({width}x{height})")
        if args.generated and path.stat().st_mtime < (ROOT / "examples" / f"{name}.lms").stat().st_mtime:
            raise SystemExit(f"{path} is older than its example source")
    gif = gallery / "animation_wave.gif"
    if not gif.is_file():
        raise SystemExit("missing animation_wave.gif")
    if gif.stat().st_size > MAX_GIF:
        raise SystemExit("animation_wave.gif exceeds 3 MiB")
    width, height = gif_size(gif)
    if width <= 0 or height <= 0:
        raise SystemExit(f"{gif} has invalid dimensions ({width}x{height})")
    if args.generated and gif.stat().st_mtime < (ROOT / "examples" / "animation_wave.lms").stat().st_mtime:
        raise SystemExit(f"{gif} is older than its example source")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    for name in examples:
        url = f"https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/{name}.lms"
        if f"lmctl autorun {url}" not in readme:
            raise SystemExit(f"README missing autorun URL for {name}")
    if args.generated:
        print(f"checked {len(examples)} PNG gallery figures and animation_wave.gif")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
