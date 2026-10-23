# Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
# Adapted from the xdg-open-win project.
#!/usr/bin/env python3
"""Generates a multi-resolution Windows .ico from a PNG.

Invoked by the top-level Makefile; not meant to be run standalone other
than for local testing. Usage: generate-icon.py <src.png> <dest.ico>
"""
import sys

from PIL import Image

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <src.png> <dest.ico>", file=sys.stderr)
        return 1

    src, dest = sys.argv[1], sys.argv[2]
    image = Image.open(src).convert("RGBA")
    image.save(dest, format="ICO", sizes=[(s, s) for s in SIZES])
    return 0


if __name__ == "__main__":
    sys.exit(main())
