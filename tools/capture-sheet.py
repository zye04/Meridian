#!/usr/bin/env python3
"""One labelled contact sheet JPG from a --capture-views output folder.

capture-sheet.py DIR [OUT.jpg] [--columns N] [--width PX]

Tiles are the folder's PNGs in the order they were written (the view
list's order), each scaled to --width pixels and labelled with its name.
OUT defaults to DIR/sheet.jpg.
"""
import argparse
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dir", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path, nargs="?")
    parser.add_argument("--columns", type=int, default=4)
    parser.add_argument("--width", type=int, default=640)
    args = parser.parse_args()

    paths = sorted(args.dir.glob("*.png"), key=lambda p: (p.stat().st_mtime_ns, p.name))
    if not paths:
        sys.exit(f"no PNGs in {args.dir}")
    font = ImageFont.load_default(size=max(12, args.width // 32))
    label = font.size + 8
    tiles = []
    for path in paths:
        image = Image.open(path).convert("RGB")
        height = round(image.height * args.width / image.width)
        tiles.append((path.stem, image.resize((args.width, height), Image.LANCZOS)))
    tile_height = max(t.height for _, t in tiles) + label
    columns = min(args.columns, len(tiles))
    rows = math.ceil(len(tiles) / columns)
    sheet = Image.new("RGB", (columns * args.width, rows * tile_height), "black")
    draw = ImageDraw.Draw(sheet)
    for i, (name, tile) in enumerate(tiles):
        x, y = (i % columns) * args.width, (i // columns) * tile_height
        sheet.paste(tile, (x, y + label))
        draw.text((x + 4, y + 4), name, fill="white", font=font)
    out = args.out or args.dir / "sheet.jpg"
    sheet.save(out, quality=88)
    print(f"{out}: {len(tiles)} views, {columns}x{rows}")


if __name__ == "__main__":
    main()
