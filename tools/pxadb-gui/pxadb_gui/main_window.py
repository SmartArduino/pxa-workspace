"""Main window for the PXADB GUI."""

from __future__ import annotations

import collections
import os
import threading
import time

from PySide6.QtCore import QObject, Qt, Signal
from PySide6.QtGui import QAction, QKeySequence, QShortcut
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QDockWidget,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QSplitter,
    QTabWidget,
    QToolBar,
    QVBoxLayout,
    QWidget,
)

from .panels import FilePanel, LogPanel, PackagePanel
from .preview import ScreenView
from .session import DeviceChoice, DeviceSession, ScreenFrame, SessionSignals, scan_devices


class _ScanWorker(QObject):
    finished = Signal(object)

    def __init__(self, timeout: float) -> None:
        super().__init__()
        self.timeout = timeout

    def run(self) -> None:
        self.finished.emit(scan_devices(self.timeout))


class MainWindow(QMainWindow):
    def __init__(self, session: DeviceSession, signals: SessionSignals,
                 initial_choice: DeviceChoice | None = None,
                 preview_interval: float = 0.3) -> None:
        super().__init__()
        self.session = session
        self.signals = signals
        self._last_frame: ScreenFrame | None = None
        self._frame_times: collections.deque[float] = collections.deque(maxlen=20)
        self._device_summary = ""
        self.setWindowTitle("PXADB GUI")
        self.resize(1180, 760)

        self.preview = ScreenView()
        self.preview.pointer.connect(self.session.pointer)
        self.preview.tap.connect(self.session.tap)
        self.preview.set_overlay("not connected")

        self.files_panel = FilePanel(session, signals)
        self.packages_panel = PackagePanel(session, signals)
        self.log_panel = LogPanel(session, signals)

        tabs = QTabWidget()
        tabs.addTab(self.files_panel, "Files")
        tabs.addTab(self.packages_panel, "Packages")
        self.tabs = tabs

        self._build_key_row()

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(self.preview_container)
        splitter.addWidget(tabs)
        splitter.setStretchFactor(0, 3)
        splitter.setStretchFactor(1, 2)
        self.setCentralWidget(splitter)

        self._build_device_toolbar()
        self._build_control_toolbar()
        self._build_log_dock()
        self._build_status_bar()
        self._connect_signals()

        self.log_panel.view.appendPlainText(
            "Select a device and press Connect. "
            "Preview captures pause while you drag on the screen.")

        self.refresh_devices()
        if initial_choice is not None:
            self.session.connect(initial_choice)

    # -- construction ------------------------------------------------------

    def _build_device_toolbar(self) -> None:
        bar = QToolBar("Device")
        bar.setMovable(False)
        self.addToolBar(bar)

        self.scan_button = QPushButton("Rescan")
        self.scan_button.clicked.connect(self.refresh_devices)
        bar.addWidget(self.scan_button)

        self.device_combo = QComboBox()
        self.device_combo.setMinimumWidth(340)
        bar.addWidget(self.device_combo)

        self.baud_combo = QComboBox()
        for baud in ("115200", "460800", "921600", "2000000"):
            self.baud_combo.addItem(baud)
        self.baud_combo.setEditable(True)
        self.baud_combo.setCurrentText(os.environ.get("PXADB_BAUD", "115200"))
        self.baud_combo.setToolTip(
            "Serial baud rate for UART-transport boards; USB Serial/JTAG "
            "ignores it")
        self.baud_combo.currentTextChanged.connect(self._baud_changed)
        bar.addWidget(QLabel(" baud"))
        bar.addWidget(self.baud_combo)

        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self._toggle_connection)
        bar.addWidget(self.connect_button)

        bar.addSeparator()
        self.info_label = QLabel("")
        bar.addWidget(self.info_label)

    def _build_control_toolbar(self) -> None:
        bar = QToolBar("Controls")
        bar.setMovable(False)
        self.addToolBar(bar)

        self.preview_check = QCheckBox("Live preview")
        self.preview_check.toggled.connect(self._toggle_preview)
        bar.addWidget(self.preview_check)

        self.interval_spin = QDoubleSpinBox()
        self.interval_spin.setRange(0.05, 10.0)
        self.interval_spin.setSingleStep(0.05)
        self.interval_spin.setSuffix(" s")
        self.interval_spin.setValue(0.3)
        self.interval_spin.valueChanged.connect(self._interval_changed)
        bar.addWidget(QLabel(" interval"))
        bar.addWidget(self.interval_spin)

        self.shot_button = QPushButton("Screenshot")
        self.shot_button.clicked.connect(self.session.screenshot)
        bar.addWidget(self.shot_button)

        self.save_button = QPushButton("Save frame")
        self.save_button.clicked.connect(self._save_frame)
        bar.addWidget(self.save_button)

        bar.addSeparator()
        self.sync_button = QPushButton("Sync input")
        self.sync_button.clicked.connect(self.session.sync_input)
        bar.addWidget(self.sync_button)

        reboot = QPushButton("Reboot")
        reboot.clicked.connect(self._reboot)
        bar.addWidget(reboot)
        poweroff = QPushButton("Power off")
        poweroff.clicked.connect(self._poweroff)
        bar.addWidget(poweroff)

    def _build_key_row(self) -> None:
        container = QWidget()
        row = QHBoxLayout(container)
        row.setContentsMargins(0, 4, 0, 0)
        for label, key in (("Back", "back"), ("Home", "home"),
                           ("Vol -", "volume-down"), ("Vol +", "volume-up")):
            button = QPushButton(label)
            button.clicked.connect(
                lambda _checked=False, name=key: self.session.key(name))
            row.addWidget(button)
        row.addStretch(1)
        self.preview_container = QWidget()
        layout = QVBoxLayout(self.preview_container)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.preview, 1)
        layout.addWidget(container)

    def _build_log_dock(self) -> None:
        dock = QDockWidget("Device log", self)
        dock.setWidget(self.log_panel)
        dock.setAllowedAreas(Qt.DockWidgetArea.BottomDockWidgetArea |
                             Qt.DockWidgetArea.TopDockWidgetArea)
        self.addDockWidget(Qt.DockWidgetArea.BottomDockWidgetArea, dock)

    def _build_status_bar(self) -> None:
        self.busy_progress = QProgressBar()
        self.busy_progress.setRange(0, 0)
        self.busy_progress.setMaximumWidth(140)
        self.busy_progress.setVisible(False)
        self.statusBar().addPermanentWidget(self.busy_progress)

        screenshot_shortcut = QShortcut(QKeySequence("F5"), self)
        screenshot_shortcut.activated.connect(self.session.screenshot)
        save_shortcut = QShortcut(QKeySequence("Ctrl+S"), self)
        save_shortcut.activated.connect(self._save_frame)
        refresh_shortcut = QShortcut(QKeySequence("Ctrl+R"), self)
        refresh_shortcut.activated.connect(self.files_panel.refresh)
        back = QAction(self)
        back.setShortcut(QKeySequence("Escape"))
        back.triggered.connect(lambda: self.session.key("back"))
        self.addAction(back)

    def _connect_signals(self) -> None:
        self.signals.connected.connect(self._on_connected)
        self.signals.disconnected.connect(self._on_disconnected)
        self.signals.screenshot.connect(self._on_screenshot)
        self.signals.error.connect(self._on_error)
        self.signals.status.connect(self._on_status)
        self.signals.busy.connect(self._on_busy)
        self.signals.job_done.connect(self._on_job_done)

    # -- device discovery --------------------------------------------------

    def _baud_changed(self, text: str) -> None:
        text = text.strip()
        if not text.isdigit() or int(text) <= 0:
            return
        os.environ["PXADB_BAUD"] = text
        if not self.session.connected:
            self.refresh_devices()

    def refresh_devices(self) -> None:
        self.scan_button.setEnabled(False)
        self.device_combo.setEnabled(False)
        worker = _ScanWorker(0.6)
        worker.finished.connect(self._on_devices)
        threading.Thread(target=worker.run, name="pxadb-scan",
                         daemon=True).start()

    def _on_devices(self, choices) -> None:
        current = self.device_combo.currentData()
        self.device_combo.clear()
        for choice in choices:
            self.device_combo.addItem(choice.label, choice)
            index = self.device_combo.count() - 1
            self.device_combo.setItemData(index, choice.info,
                                          Qt.ItemDataRole.ToolTipRole)
        if current is not None:
            for index in range(self.device_combo.count()):
                if self.device_combo.itemData(index).target == current.target:
                    self.device_combo.setCurrentIndex(index)
                    break
        self.scan_button.setEnabled(True)
        self.device_combo.setEnabled(not self.session.connected)
        if not choices:
            self.device_combo.addItem("no PXADB devices found", None)

    # -- connection --------------------------------------------------------

    def _toggle_connection(self) -> None:
        if self.session.connected:
            self.session.disconnect()
            return
        choice = self.device_combo.currentData()
        if not isinstance(choice, DeviceChoice):
            self._on_error("no device selected")
            return
        self.connect_button.setEnabled(False)
        self.statusBar().showMessage(f"connecting to {choice.label}...")
        self.session.connect(choice)

    def _on_connected(self, info: str) -> None:
        self._device_summary = info
        self.connect_button.setText("Disconnect")
        self.connect_button.setEnabled(True)
        self.device_combo.setEnabled(False)
        self.baud_combo.setEnabled(False)
        self.scan_button.setEnabled(False)
        self.files_panel.set_connected(True)
        self.packages_panel.set_connected(True)
        self.log_panel.set_connected(True)
        self.preview.set_overlay(self._choice_label() or "connected")
        self.statusBar().showMessage("connected")
        self.files_panel.navigate(".")
        self.packages_panel.refresh()
        if self.preview_check.isChecked():
            self.session.set_preview(True, self.interval_spin.value())

    def _on_disconnected(self) -> None:
        self.connect_button.setText("Connect")
        self.connect_button.setEnabled(True)
        self.device_combo.setEnabled(True)
        self.baud_combo.setEnabled(True)
        self.scan_button.setEnabled(True)
        self.files_panel.set_connected(False)
        self.packages_panel.set_connected(False)
        self.log_panel.set_connected(False)
        self.preview.clear_image()
        self.preview.set_overlay("not connected")
        self._last_frame = None
        self.statusBar().showMessage("disconnected")

    def _choice_label(self) -> str:
        choice = self.session.target
        return choice.label if choice is not None else ""

    # -- preview and actions ----------------------------------------------

    def _toggle_preview(self, enabled: bool) -> None:
        if self.session.connected or not enabled:
            self.session.set_preview(enabled, self.interval_spin.value())

    def _interval_changed(self, value: float) -> None:
        if self.session.connected:
            self.session.set_preview(self.preview_check.isChecked(), value)

    def _on_screenshot(self, frame: ScreenFrame) -> None:
        self._last_frame = frame
        self.preview.set_image(frame.image)
        now = time.monotonic()
        self._frame_times.append(now)
        if len(self._frame_times) > 1:
            elapsed = self._frame_times[-1] - self._frame_times[0]
            fps = (len(self._frame_times) - 1) / elapsed if elapsed > 0 else 0.0
        else:
            fps = 0.0
        width = frame.image.width()
        height = frame.image.height()
        self.preview.set_overlay(
            f"{self._choice_label()}  {width}x{height}  {fps:.1f} fps")

    def _save_frame(self) -> None:
        if self._last_frame is None:
            QMessageBox.information(self, "Save frame",
                                    "No captured frame yet.")
            return
        target, _filter = QFileDialog.getSaveFileName(
            self, "Save frame", "screenshot.png",
            "PNG image (*.png);;JPEG image (*.jpg *.jpeg)")
        if target:
            if self._last_frame.image.save(target):
                self.statusBar().showMessage(f"saved {target}", 5000)
            else:
                self._on_error(f"could not save {target}")

    def _reboot(self) -> None:
        answer = QMessageBox.question(
            self, "Reboot", "Restart the connected device?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if answer == QMessageBox.StandardButton.Yes:
            self.session.reboot()

    def _poweroff(self) -> None:
        answer = QMessageBox.question(
            self, "Power off", "Request a software power-off?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if answer == QMessageBox.StandardButton.Yes:
            self.session.poweroff()

    # -- status ------------------------------------------------------------

    def _on_error(self, message: str) -> None:
        self.statusBar().showMessage(message, 10000)
        self.log_panel.view.appendPlainText(f"[error] {message}")

    def _on_status(self, message: str) -> None:
        self.statusBar().showMessage(message)

    def _on_busy(self, busy: bool) -> None:
        self.busy_progress.setVisible(busy)
        if busy:
            self.statusBar().showMessage("working...")

    def _on_job_done(self, name: str, payload) -> None:
        if name.startswith("install"):
            self.packages_panel.refresh()
        if payload:
            self.statusBar().showMessage(str(payload), 8000)

    # -- lifecycle ---------------------------------------------------------

    def closeEvent(self, event) -> None:
        self.session.shutdown()
        super().closeEvent(event)
