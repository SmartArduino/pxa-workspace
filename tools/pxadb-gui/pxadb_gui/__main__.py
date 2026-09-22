"""Entry point for the PXADB GUI."""

from __future__ import annotations

import argparse
import pathlib
import sys

from PySide6.QtWidgets import QApplication

from .library import pxadb
from .main_window import MainWindow
from .session import DeviceChoice, DeviceSession, SessionSignals


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="pxadb-gui", description="Desktop GUI for PXADB devices")
    parser.add_argument("--port", help="USB serial port to connect on startup")
    parser.add_argument("--simulator", metavar="PROFILE[@INSTANCE]",
                        help="local simulator profile to connect on startup")
    parser.add_argument("--connect", metavar="HOST:PORT",
                        help="PXADB2 TCP endpoint to connect on startup")
    parser.add_argument("--token", metavar="FILE",
                        help="PXADB2 TCP token file (requires --connect)")
    parser.add_argument("--interval", type=float, default=0.3,
                        help="preview capture interval in seconds (default 0.3)")
    parser.add_argument("--baud", type=int,
                        help="serial baud rate for UART-transport boards "
                             "(default 115200; USB Serial/JTAG ignores it)")
    return parser


def startup_choice(arguments: argparse.Namespace,
                   parser: argparse.ArgumentParser) -> DeviceChoice | None:
    if arguments.simulator:
        try:
            target = pxadb.simulator_socket(arguments.simulator)
        except argparse.ArgumentTypeError as error:
            parser.error(str(error))
        return DeviceChoice(f"simulator:{arguments.simulator}", target,
                            "", arguments.simulator)
    if arguments.connect:
        if not arguments.token:
            parser.error("--connect requires --token FILE")
        try:
            token = pathlib.Path(arguments.token).read_text(encoding="ascii").strip()
        except OSError as error:
            parser.error(f"unable to read PXADB2 token: {error}")
        if not token or any(character.isspace() for character in token):
            parser.error("PXADB2 token is invalid")
        return DeviceChoice(f"tcp:{arguments.connect}",
                            f"tcp:{arguments.connect}|{token}")
    if arguments.port:
        return DeviceChoice(arguments.port, arguments.port)
    return None


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    arguments = parser.parse_args(argv)
    if arguments.token and not arguments.connect:
        parser.error("--token is only valid with --connect")
    if arguments.interval <= 0:
        parser.error("--interval must be positive")
    if arguments.baud is not None:
        if arguments.baud <= 0:
            parser.error("--baud must be a positive integer")
        # PxaDbClient reads PXADB_BAUD when it opens a serial port.
        os.environ["PXADB_BAUD"] = str(arguments.baud)
    choice = startup_choice(arguments, parser)

    app = QApplication([sys.argv[0]])
    app.setApplicationName("PXADB GUI")
    signals = SessionSignals()
    session = DeviceSession(signals)
    session.start()
    window = MainWindow(session, signals, choice, arguments.interval)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
