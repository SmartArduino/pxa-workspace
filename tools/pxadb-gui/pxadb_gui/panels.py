"""Files, logs and package panels for the main window."""

from __future__ import annotations

import pathlib

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QFileDialog,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .library import pxadb
from .session import DeviceSession, SessionSignals


def human_size(value: int) -> str:
    if value < 1024:
        return f"{value} B"
    if value < 1024 * 1024:
        return f"{value / 1024:.1f} KiB"
    return f"{value / (1024 * 1024):.1f} MiB"


class FilePanel(QWidget):
    """Browse and transfer files over PXADB filesystem commands."""

    def __init__(self, session: DeviceSession, signals: SessionSignals) -> None:
        super().__init__()
        self.session = session
        self.signals = signals
        self._path = "."

        self.path_edit = QLineEdit(".")
        self.path_edit.returnPressed.connect(self._go)
        go_button = QPushButton("Go")
        go_button.clicked.connect(self._go)
        up_button = QPushButton("Up")
        up_button.clicked.connect(self._up)
        refresh_button = QPushButton("Refresh")
        refresh_button.clicked.connect(self.refresh)

        path_row = QHBoxLayout()
        path_row.addWidget(QLabel("Path"))
        path_row.addWidget(self.path_edit, 1)
        path_row.addWidget(go_button)
        path_row.addWidget(up_button)
        path_row.addWidget(refresh_button)

        self.tree = QTreeWidget()
        self.tree.setColumnCount(2)
        self.tree.setHeaderLabels(["Name", "Size"])
        self.tree.header().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self.tree.header().setSectionResizeMode(1, QHeaderView.ResizeMode.ResizeToContents)
        self.tree.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.tree.itemDoubleClicked.connect(self._on_double_click)

        self.push_button = QPushButton("Push file")
        self.push_button.clicked.connect(self._push)
        self.pull_button = QPushButton("Pull")
        self.pull_button.clicked.connect(self._pull)
        self.mkdir_button = QPushButton("New folder")
        self.mkdir_button.clicked.connect(self._mkdir)
        self.delete_button = QPushButton("Delete")
        self.delete_button.clicked.connect(self._delete)
        self.progress = QProgressBar()
        self.progress.setVisible(False)

        buttons = QHBoxLayout()
        buttons.addWidget(self.push_button)
        buttons.addWidget(self.pull_button)
        buttons.addWidget(self.mkdir_button)
        buttons.addWidget(self.delete_button)
        buttons.addStretch(1)

        layout = QVBoxLayout(self)
        layout.addLayout(path_row)
        layout.addWidget(self.tree, 1)
        layout.addLayout(buttons)
        layout.addWidget(self.progress)

        self.signals.files_listed.connect(self._on_files_listed)
        self.signals.files_changed.connect(self._on_files_changed)
        self.signals.progress.connect(self._on_progress)
        self.set_connected(False)

    def set_connected(self, connected: bool) -> None:
        for widget in (self.path_edit, self.tree, self.push_button,
                       self.pull_button, self.mkdir_button,
                       self.delete_button):
            widget.setEnabled(connected)

    def refresh(self) -> None:
        if self.session.connected:
            self.session.list_files(self._path)

    def navigate(self, path: str) -> None:
        self._path = path or "."
        self.path_edit.setText(self._path)
        self.refresh()

    # -- slots -------------------------------------------------------------

    def _go(self) -> None:
        self.navigate(self.path_edit.text().strip() or ".")

    def _up(self) -> None:
        normalized = self._path.strip("/")
        if not normalized or normalized == "." or "/" not in normalized:
            self.navigate(".")
            return
        self.navigate(normalized.rsplit("/", 1)[0])

    def _on_files_listed(self, path: str, entries) -> None:
        self._path = path
        self.path_edit.setText(path)
        self.tree.clear()
        for kind, size, name in entries:
            label = f"{name}/" if kind == "D" else name
            item = QTreeWidgetItem([label, "" if kind == "D" else human_size(size)])
            item.setData(0, Qt.ItemDataRole.UserRole, (kind, name))
            self.tree.addTopLevelItem(item)

    def _on_files_changed(self, _path: str) -> None:
        self.refresh()

    def _on_progress(self, label: str, done: int, total: int) -> None:
        self.progress.setVisible(True)
        self.progress.setFormat(f"{label} %p%")
        self.progress.setRange(0, total if total > 0 else 0)
        self.progress.setValue(done)
        if total > 0 and done >= total:
            self.progress.setValue(total)

    def _selected(self) -> tuple[str, str] | None:
        items = self.tree.selectedItems()
        if not items:
            return None
        return items[0].data(0, Qt.ItemDataRole.UserRole)

    def _join(self, name: str) -> str:
        base = self._path.strip("/")
        if not base or base == ".":
            return name
        return f"{base}/{name}"

    def _on_double_click(self, item: QTreeWidgetItem, _column: int) -> None:
        kind, name = item.data(0, Qt.ItemDataRole.UserRole)
        if kind == "D":
            self.navigate(self._join(name))

    def _push(self) -> None:
        selected, _filter = QFileDialog.getOpenFileNames(self, "Push files")
        for local in selected:
            remote = self._join(pathlib.Path(local).name)
            self.session.upload(local, remote)

    def _pull(self) -> None:
        selection = self._selected()
        if selection is None or selection[0] != "F":
            QMessageBox.information(self, "Pull", "Select a file to download.")
            return
        _kind, name = selection
        target, _filter = QFileDialog.getSaveFileName(self, "Save file as", name)
        if target:
            self.session.download(self._join(name), target)

    def _mkdir(self) -> None:
        name, accepted = QInputDialog.getText(self, "New folder", "Folder name")
        if accepted and name.strip():
            self.session.make_directory(self._join(name.strip()))

    def _delete(self) -> None:
        selection = self._selected()
        if selection is None:
            return
        kind, name = selection
        remote = self._join(name)
        question = (f"Delete {remote} and everything in it?"
                    if kind == "D" else f"Delete {remote}?")
        answer = QMessageBox.question(
            self, "Delete", question,
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if answer == QMessageBox.StandardButton.Yes:
            self.session.remove_path(remote, recursive=kind == "D")


class LogPanel(QWidget):
    """Live device log and raw console output."""

    def __init__(self, session: DeviceSession, signals: SessionSignals) -> None:
        super().__init__()
        self.session = session
        self.signals = signals

        self.live = QCheckBox("Subscribe to device logs")
        self.live.toggled.connect(self._toggle)
        clear_button = QPushButton("Clear")
        clear_button.clicked.connect(lambda: self.view.clear())

        controls = QHBoxLayout()
        controls.addWidget(self.live)
        controls.addStretch(1)
        controls.addWidget(clear_button)

        self.view = QPlainTextEdit()
        self.view.setReadOnly(True)
        self.view.setMaximumBlockCount(5000)
        self.view.setPlaceholderText(
            "Device logs appear here while subscribed;\n"
            "raw console output is collected continuously.")

        layout = QVBoxLayout(self)
        layout.addLayout(controls)
        layout.addWidget(self.view, 1)
        self.signals.log_line.connect(self._append)
        self.set_connected(False)

    def set_connected(self, connected: bool) -> None:
        self.live.setEnabled(connected)
        if not connected and self.live.isChecked():
            self.live.setChecked(False)

    def _toggle(self, enabled: bool) -> None:
        if self.session.connected:
            self.session.set_logs(enabled)

    def _append(self, text: str) -> None:
        self.view.appendPlainText(text)


class PackagePanel(QWidget):
    """List, install and control PXA packages on the connected device."""

    ACTIONS = (
        ("Run", "run"),
        ("Stop", "stop"),
        ("Uninstall", "uninstall"),
        ("Enable", "enable"),
        ("Disable", "disable"),
        ("Clear data", "clear-data"),
    )

    def __init__(self, session: DeviceSession, signals: SessionSignals) -> None:
        super().__init__()
        self.session = session
        self.signals = signals
        self._packages: list[list[str]] = []

        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["ID", "Name", "Version", "State"])
        self.table.horizontalHeader().setSectionResizeMode(
            0, QHeaderView.ResizeMode.Stretch)
        self.table.setSelectionBehavior(
            QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(
            QAbstractItemView.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)

        refresh_button = QPushButton("Refresh")
        refresh_button.clicked.connect(self.refresh)
        install_button = QPushButton("Install .pxa")
        install_button.clicked.connect(self._install)
        self.progress = QProgressBar()
        self.progress.setVisible(False)

        buttons = QHBoxLayout()
        buttons.addWidget(refresh_button)
        buttons.addWidget(install_button)
        self._action_buttons: list[QPushButton] = []
        for label, action in self.ACTIONS:
            button = QPushButton(label)
            button.clicked.connect(
                lambda _checked=False, name=action: self._action(name))
            buttons.addWidget(button)
            self._action_buttons.append(button)

        layout = QVBoxLayout(self)
        layout.addWidget(self.table, 1)
        layout.addLayout(buttons)
        layout.addWidget(self.progress)

        self.signals.packages_listed.connect(self._on_packages)
        self.signals.progress.connect(self._on_progress)
        self.set_connected(False)

    def set_connected(self, connected: bool) -> None:
        self.table.setEnabled(connected)
        for button in self._action_buttons:
            button.setEnabled(connected)

    def refresh(self) -> None:
        if self.session.connected:
            self.session.list_packages()

    def _on_packages(self, packages) -> None:
        self._packages = [list(package) for package in packages]
        self.table.setRowCount(len(self._packages))
        for row, package in enumerate(self._packages):
            for column in range(4):
                value = package[column] if column < len(package) else ""
                self.table.setItem(row, column, QTableWidgetItem(value))

    def _on_progress(self, label: str, done: int, total: int) -> None:
        self.progress.setVisible(True)
        self.progress.setFormat(f"{label} %p%")
        self.progress.setRange(0, total if total > 0 else 0)
        self.progress.setValue(done)

    def _selected_identity(self) -> str | None:
        rows = self.table.selectionModel().selectedRows() if self.table.selectionModel() else []
        if not rows:
            return None
        item = self.table.item(rows[0].row(), 0)
        return item.text() if item is not None else None

    def _installed(self, identity: str) -> bool:
        for package in self._packages:
            if package and package[0] == identity and len(package) > 3:
                return "installed=1" in package[3].split(";")
        return False

    def _install(self) -> None:
        source, _filter = QFileDialog.getOpenFileName(
            self, "Install package", "", "PXA packages (*.pxa);;All files (*)")
        if not source:
            return
        try:
            _path, identity = pxadb.package_source(source, None)
        except Exception as error:
            QMessageBox.warning(self, "Install", str(error))
            return
        if self._installed(identity):
            answer = QMessageBox.question(
                self, "Replace package",
                f"{identity} is already installed and will be replaced "
                "(possibly with a downgrade). Continue?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes:
                return
        self.session.install_package(source)

    def _action(self, action: str) -> None:
        identity = self._selected_identity()
        if identity is None:
            QMessageBox.information(self, action.title(),
                                    "Select a package first.")
            return
        if action == "run":
            self.session.run_package(identity)
            return
        self.session.package_action(action, identity)
