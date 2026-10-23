#!/usr/bin/env python3
"""Check the committed lmimg gallery without third-party image packages."""
from __future__ import annotations

import argparse
import struct
import re
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
    parser.add_argument("--generated-dir", type=Path, default=ROOT / "images")
    args = parser.parse_args()
    examples = sorted(p.stem for p in (ROOT / "examples").glob("*.lms"))
    if len(examples) < 12:
        raise SystemExit("gallery requires at least 12 image-producing examples")
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
        if args.generated:
            fresh = args.generated_dir / f"{name}.png"
            if not fresh.is_file() or png_size(fresh) != (900, 520):
                raise SystemExit(f"missing or invalid fresh output: {fresh}")
            if fresh.stat().st_size > MAX_PNG:
                raise SystemExit(f"fresh output exceeds PNG byte budget: {fresh}")
    gifs = sorted(gallery.glob("*.gif"))
    if not any(g.name == "animation_wave.gif" for g in gifs):
        raise SystemExit("missing animation_wave.gif")
    for gif in gifs:
        if gif.stem not in examples:
            raise SystemExit(f"{gif} has no example producing it")
        if gif.stat().st_size > MAX_GIF:
            raise SystemExit(f"{gif} exceeds 3 MiB")
        width, height = gif_size(gif)
        if width <= 0 or height <= 0:
            raise SystemExit(f"{gif} has invalid dimensions ({width}x{height})")
        if args.generated:
            fresh = args.generated_dir / gif.name
            if not fresh.is_file() or gif_size(fresh) != gif_size(gif) or fresh.stat().st_size > MAX_GIF:
                raise SystemExit(f"missing or invalid fresh GIF: {fresh}")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    for name in examples:
        url = f"https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/{name}.lms"
        caption = re.compile(
            r"!\[[^\]]*\]\(docs/images/" + re.escape(name) +
            r"\.png\)(?:<br>|\s)*`" + re.escape(f"lmctl autorun {url}") + r"`"
        )
        if not caption.search(readme):
            raise SystemExit(f"README missing image with autorun caption for {name}")
    for gif in gifs:
        gif_url = f"https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/{gif.stem}.lms"
        gif_caption = re.compile(
            r"!\[[^\]]*\]\(docs/images/" + re.escape(gif.name) + r"\)(?:<br>|\s)*`" +
            re.escape(f"lmctl autorun {gif_url}") + r"`"
        )
        if not gif_caption.search(readme):
            raise SystemExit(f"README missing GIF autorun caption for {gif.name}")
    if args.generated:
        for name, dimensions in [("batch_small", (180, 240)),
                                 ("batch_medium", (210, 280)),
                                 ("batch_large", (240, 320))]:
            fresh = args.generated_dir / f"{name}.png"
            if not fresh.is_file() or png_size(fresh) != dimensions:
                raise SystemExit(f"missing or distorted batch deliverable: {fresh}")
        print(f"checked {len(examples)} PNG gallery figures, three batch files and {len(gifs)} GIFs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
