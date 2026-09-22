"""Screen preview widget with pointer and key injection."""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt, Signal
from PySide6.QtGui import QColor, QImage, QPainter, QPen
from PySide6.QtWidgets import QWidget

from .geometry import device_point, fitted_rect


class ScreenView(QWidget):
    """Show the latest device frame and translate mouse input to taps/drags."""

    pointer = Signal(str, int, int)  # down / move / up
    tap = Signal(int, int)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._image: QImage | None = None
        self._overlay = "not connected"
        self._pressed = False
        self._dragged = False
        self._press = (0, 0)
        self.setMinimumSize(320, 240)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def set_image(self, image: QImage) -> None:
        self._image = image
        # A frame can arrive while the user is dragging. Keep the gesture state
        # so the matching UP still reaches the device; clearing it here would
        # leave the device pointer pressed and reject later taps.
        if not self._pressed:
            self._dragged = False
        self.update()

    def clear_image(self) -> None:
        self._image = None
        self.update()

    def set_overlay(self, text: str) -> None:
        self._overlay = text
        self.update()

    def image_size(self) -> tuple[int, int]:
        if self._image is None or self._image.isNull():
            return (0, 0)
        return (self._image.width(), self._image.height())

    def _map(self, x: float, y: float) -> tuple[int, int] | None:
        width, height = self.image_size()
        return device_point(x, y, width, height, self.width(), self.height())

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.fillRect(self.rect(), QColor(24, 24, 28))
        if self._image is not None and not self._image.isNull():
            width, height = self.image_size()
            left, top, fitted_width, fitted_height = fitted_rect(
                width, height, self.width(), self.height())
            painter.drawImage(QRectF(left, top, fitted_width, fitted_height),
                              self._image)
            painter.setPen(QPen(QColor(90, 90, 100)))
            painter.drawRect(QRectF(left, top, fitted_width, fitted_height))
        painter.setPen(QColor(200, 200, 210))
        metrics = painter.fontMetrics()
        text_width = metrics.horizontalAdvance(self._overlay)
        text_height = metrics.height()
        painter.fillRect(6, 6, text_width + 12, text_height + 4,
                         QColor(0, 0, 0, 160))
        painter.drawText(12, 6 + metrics.ascent() + 2, self._overlay)
        painter.end()

    def mousePressEvent(self, event) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        point = self._map(event.position().x(), event.position().y())
        if point is None:
            return
        self._pressed = True
        self._dragged = False
        self._press = point
        self.setFocus()

    def mouseMoveEvent(self, event) -> None:
        if not self._pressed:
            return
        point = self._map(event.position().x(), event.position().y())
        if point is None:
            return
        if not self._dragged:
            distance = abs(point[0] - self._press[0]) + abs(point[1] - self._press[1])
            if distance < 4:
                return
            self._dragged = True
            self.pointer.emit("down", self._press[0], self._press[1])
        self.pointer.emit("move", point[0], point[1])

    def mouseReleaseEvent(self, event) -> None:
        if event.button() != Qt.MouseButton.LeftButton or not self._pressed:
            return
        point = self._map(event.position().x(), event.position().y())
        self._pressed = False
        if point is None:
            point = self._press
        if self._dragged:
            self.pointer.emit("up", point[0], point[1])
        else:
            self.tap.emit(point[0], point[1])
        self._dragged = False
