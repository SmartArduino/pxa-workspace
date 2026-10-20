#!/usr/bin/env python3
"""Guard regressions without connecting to any device."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("main_loop_probe", Path(__file__).with_name("run_main_loop_probe.py"))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class Client:
    def __init__(self, serial, active=False):
        self.serial, self.active, self.commands = serial, active, []

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass

    def hello(self):
        return f"serial={self.serial}"

    def request(self, command, **_):
        self.commands.append(command)
        if command == "PACKAGES" and self.active:
            return [SimpleNamespace(kind="PKG", payload="publisher:pxa-main-loop-probe\tprobe\t0.1.0\tactive=1")]
        return [SimpleNamespace(kind="OK", payload="")]


class ProbeGuardTest(unittest.TestCase):
    def run_guard(self, client):
        with tempfile.TemporaryDirectory() as tmp:
            argv = ["probe", "--port", "never-open-a-port", "--serial", "expected", "--output", tmp]
            with patch.object(sys, "argv", argv), patch.object(probe.pxadb, "PxaDbClient", return_value=client):
                with self.assertRaises(AssertionError):
                    probe.main()
            report = json.loads((Path(tmp) / "report.json").read_text())
            self.assertFalse(report["qualified"])
            self.assertEqual(report["failure"]["type"], "AssertionError")

    def test_wrong_serial_never_reaches_cleanup_commands(self):
        client = Client("different-device")
        self.run_guard(client)
        self.assertEqual(client.commands, [])

    def test_already_running_probe_is_not_stopped_by_failed_guard(self):
        client = Client("expected", active=True)
        self.run_guard(client)
        self.assertTrue(client.commands)
        self.assertTrue(all(cmd in {"PACKAGES", "MEMORY", "INFO"} for cmd in client.commands))

    def test_invalid_replacement_is_rejected_before_opening_a_port(self):
        argv = ["probe", "--port", "never-open-a-port", "--serial", "expected", "--output", "/unused", "--replace-with", "invalid\nINPUT KEY HOME"]
        with patch.object(sys, "argv", argv), patch.object(probe.pxadb, "PxaDbClient") as open_client, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                probe.main()
            self.assertEqual(error.exception.code, 2)
            open_client.assert_not_called()


if __name__ == "__main__":
    unittest.main()
