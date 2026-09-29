#!/usr/bin/env python3
"""Regenerate Weather's fixed-size, LVGL 9.6 premultiplied image sources.

Build tools/compile-lvgl-premul-icon.c against the same LVGL 9.6 as firmware,
then pass its executable here. --check only compares the committed outputs.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

from PIL import Image

ICONS = (("info", 18), ("location", 19), ("clock", 17), ("refresh", 17))
PXR_HEADER = struct.Struct("<4sHHHHHHIIQ")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generator", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    app = Path(__file__).resolve().parent.parent / "local/pxa-apps/weather"
    generator = args.generator.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="pxa-weather-premul-") as temporary:
        source = Path(temporary) / "source.pxr"
        output = Path(temporary) / "scaled.bgra"
        for name, side in ICONS:
            with Image.open(app / "resources" / f"{name}.png") as image:
                if image.size != (64, 64):
                    raise ValueError(f"{name} is not 64x64")
                pixels = image.convert("RGBA").tobytes("raw", "BGRA")
            source.write_bytes(PXR_HEADER.pack(b"PXR1", 1, 0, 4, 7, 64, 64,
                                               len(pixels), PXR_HEADER.size, 0) + pixels)
            subprocess.run([str(generator), str(source), str(output), str(side)],
                           check=True)
            target = app / "resources" / f"{name}-{side}-premultiplied.bgra"
            generated = output.read_bytes()
            if args.check:
                if not target.is_file() or target.read_bytes() != generated:
                    raise ValueError(f"{target} differs from LVGL 9.6 output")
            else:
                target.write_bytes(generated)
            print(f"{name}: {len(generated)} bytes OK")


if __name__ == "__main__":
    main()
