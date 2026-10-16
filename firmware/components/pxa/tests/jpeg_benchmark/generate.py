#!/usr/bin/env python3
"""Generate deterministic JPEG inputs; Pillow is needed only for this test."""
from io import BytesIO
from pathlib import Path
import sys
from PIL import Image, ImageDraw

target = Path(sys.argv[1]); target.mkdir(parents=True, exist_ok=True)
definitions = []
for name, width, height, gray in [('small', 96, 64, False), ('photo', 320, 180, False),
                                  ('aligned', 320, 192, False), ('large', 640, 384, False),
                                  ('gray', 320, 180, True)]:
    image = Image.new('RGB', (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), (30 + x * 180 // width, 45 + y * 170 // height,
                                   (x * 3 + y * 7) % 256))
    ImageDraw.Draw(image).ellipse((width // 2, height // 8, width * 7 // 8, height * 3 // 4),
                                 fill=(255, 220, 100))
    if gray: image = image.convert('L')
    encoded = BytesIO(); image.save(encoded, format='JPEG', quality=88, subsampling=2)
    data = encoded.getvalue(); (target / (name + '.jpg')).write_bytes(data)
    definitions.append('static const unsigned char '+name+'[] = {'+
                       ','.join(str(byte) for byte in data)+'};')
(target / 'fixtures.h').write_text('\n'.join(definitions)+'\n')
