#!/usr/bin/env python3
"""Wrap the complete system font in WOFF1 without changing glyphs or metrics."""
import argparse
import hashlib
from pathlib import Path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[2]

def compress(source, destination):
    font = TTFont(source, recalcTimestamp=False)
    destination.parent.mkdir(parents=True, exist_ok=True)
    font.flavor = 'woff'
    font.save(destination)
    packed = TTFont(destination, recalcTimestamp=False)
    if font.getBestCmap() != packed.getBestCmap():
        raise ValueError('Font character mappings changed')
    for tag in font.keys():
        if tag == 'GlyphOrder':
            continue
        if font.getTableData(tag) != packed.getTableData(tag):
            raise ValueError('Font table changed: ' + tag)
    print(destination, destination.stat().st_size, hashlib.sha256(destination.read_bytes()).hexdigest())

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'factory/base/system/fonts/noto_sans_cjk_common.ttf')
    parser.add_argument('--output', type=Path, default=ROOT/'factory/variants/compact-fonts/pxa_sans_sc_common.woff')
    args = parser.parse_args()
    compress(args.source, args.output)
