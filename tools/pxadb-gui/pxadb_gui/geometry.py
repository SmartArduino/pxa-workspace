"""Pure geometry helpers for mapping view coordinates onto device pixels.

Kept free of Qt imports so the mapping rules can be unit tested directly.
"""

from __future__ import annotations


def fitted_rect(content_width: int, content_height: int,
                view_width: int, view_height: int
                ) -> tuple[float, float, float, float]:
    """Return the centered, aspect-preserving rectangle for content in a view."""
    if content_width <= 0 or content_height <= 0 or view_width <= 0 or view_height <= 0:
        return (0.0, 0.0, 0.0, 0.0)
    scale = min(view_width / content_width, view_height / content_height)
    width = content_width * scale
    height = content_height * scale
    return ((view_width - width) / 2.0, (view_height - height) / 2.0,
            width, height)


def device_point(x: float, y: float, content_width: int, content_height: int,
                 view_width: int, view_height: int) -> tuple[int, int] | None:
    """Map a widget position to device pixels, or None when outside the image."""
    left, top, width, height = fitted_rect(content_width, content_height,
                                           view_width, view_height)
    if width <= 0 or height <= 0:
        return None
    if not (left <= x < left + width and top <= y < top + height):
        return None
    device_x = int((x - left) * content_width / width)
    device_y = int((y - top) * content_height / height)
    return (min(content_width - 1, max(0, device_x)),
            min(content_height - 1, max(0, device_y)))
