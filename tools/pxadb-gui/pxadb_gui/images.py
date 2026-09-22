"""Device frame decoding for the preview.

Firmware sends logical little-endian RGB565; Qt's Format_RGB16 is the same
16-bit layout on little-endian hosts, so no pixel conversion is needed. The
simulator sends PNG.
"""

from __future__ import annotations

from PySide6.QtGui import QImage

from .library import pxadb


def capture_to_image(capture: pxadb.Screenshot) -> QImage:
    pixel_format = capture.metadata.get("format", "jpeg")
    if pixel_format == "rgb565le":
        width = int(capture.metadata["width"])
        height = int(capture.metadata["height"])
        stride = int(capture.metadata.get("stride", width * 2))
        image = QImage(capture.data, width, height, stride,
                       QImage.Format.Format_RGB16)
        return image.copy()
    return QImage.fromData(capture.data)
