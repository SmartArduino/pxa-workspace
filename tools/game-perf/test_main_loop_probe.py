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
    def __init__(self, serial, active=False, app="pxa-main-loop-probe"):
        self.serial, self.active, self.commands = serial, active, []
        self.app = app

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass

    def hello(self):
        return f"serial={self.serial}"

    def request(self, command, **_):
        self.commands.append(command)
        if command == "PACKAGES" and self.active:
            return [SimpleNamespace(kind="PKG", payload=f"publisher:{self.app}\tprobe\t0.1.0\tactive=1")]
        return [SimpleNamespace(kind="OK", payload="")]


class ProbeGuardTest(unittest.TestCase):
    def run_guard(self, client):
        with tempfile.TemporaryDirectory() as tmp:
            argv = ["probe", "--port", "never-open-a-port", "--serial", "expected", "--output", tmp]
            with patch.object(sys, "argv", argv), patch.object(probe.pxadb, "PxaDbClient", return_value=client):
                with self.assertRaises(RuntimeError):
                    probe.main()
            report = json.loads((Path(tmp) / "report.json").read_text())
            self.assertFalse(report["qualified"])
            self.assertEqual(report["failure"]["type"], "RuntimeError")

    def test_wrong_serial_never_reaches_cleanup_commands(self):
        client = Client("different-device")
        self.run_guard(client)
        self.assertEqual(client.commands, [])

    def test_already_running_probe_is_not_stopped_by_failed_guard(self):
        client = Client("expected", active=True)
        self.run_guard(client)
        self.assertTrue(client.commands)
        self.assertTrue(all(cmd in {"PACKAGES", "MEMORY", "INFO"} for cmd in client.commands))

    def test_another_running_app_is_not_interrupted(self):
        client = Client("expected", active=True, app="pxa-nes")
        self.run_guard(client)
        self.assertTrue(client.commands)
        self.assertTrue(all(cmd in {"PACKAGES", "MEMORY", "INFO"} for cmd in client.commands))

    def test_old_firmware_without_log_clock_is_rejected_before_mutations(self):
        client = Client("expected")
        self.run_guard(client)
        self.assertIn("INPUT SYNC", client.commands)
        self.assertTrue(all(cmd in {"PACKAGES", "MEMORY", "INFO", "INPUT SYNC"} for cmd in client.commands))

    def test_log_barrier_uses_log_clock_and_requires_success(self):
        def ack(text):
            return [SimpleNamespace(kind="OK", payload=text)]
        self.assertEqual(probe.log_barrier(ack("event=1;device_us=2000000;log_ms=3600;status=processed")), 3600)
        for text in ("device_us=2000000;status=processed",
                     "log_ms=3600;status=rejected",
                     "log_ms=-1;status=processed",
                     "log_ms=4294967296;status=processed"):
            with self.assertRaises(RuntimeError):
                probe.log_barrier(ack(text))

    def test_invalid_replacement_is_rejected_before_opening_a_port(self):
        argv = ["probe", "--port", "never-open-a-port", "--serial", "expected", "--output", "/unused", "--replace-with", "invalid\nINPUT KEY HOME"]
        with patch.object(sys, "argv", argv), patch.object(probe.pxadb, "PxaDbClient") as open_client, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                probe.main()
            self.assertEqual(error.exception.code, 2)
            open_client.assert_not_called()

    def test_old_history_duplicates_and_other_apps_cannot_prove_progress(self):
        def row(stamp, count, app="pxa-main-loop-probe", kind="LOG"):
            return {"kind": kind, "payload":
                f"{stamp}\t[PXA app={app} component=65537 level=INFO] MAINLOOP alive value=0 heartbeat={count}"}
        records = [row(100, 10), row(1100, 1), row(1100, 1), row(1200, 2, "another-app"), row(1300, 3, kind="RAW")]
        self.assertEqual(probe.heartbeat_progress(probe.fresh_probe_records(records, 1000)), [1])
        records += [row(2100, 2), row(3100, 3)]
        self.assertEqual(probe.heartbeat_progress(probe.fresh_probe_records(records, 1000)), [1, 2, 3])

    def test_different_clock_origins_cannot_admit_old_history(self):
        records = [{"kind": "LOG", "payload":
                    f"{stamp}\t[PXA app=pxa-main-loop-probe component=65537 level=INFO] MAINLOOP alive value=0 heartbeat={count}"}
                   for stamp, count in ((3000, 90), (3500, 91), (3600, 92), (4600, 1))]
        barrier = probe.log_barrier([SimpleNamespace(kind="OK", payload=
            "event=1;device_us=2000000;log_ms=3600;status=processed")])
        self.assertEqual(probe.heartbeat_progress(probe.fresh_probe_records(records, barrier)), [1])

    def test_log_clock_wrap_rejects_history_and_accepts_new_records(self):
        records = [{"kind": "LOG", "payload":
                    f"{stamp}\t[PXA app=pxa-main-loop-probe component=65537 level=INFO] MAINLOOP alive value=0 heartbeat={count}"}
                   for stamp, count in ((4294967285, 90), (4294967290, 91), (2, 1), (1002, 2))]
        self.assertEqual(probe.heartbeat_progress(probe.fresh_probe_records(records, 4294967290)), [1, 2])
        self.assertEqual(probe.heartbeat_progress(probe.fresh_probe_records(records, 3)), [2])

    def test_launch_card_is_not_an_application_canvas(self):
        metadata = {"format": "rgb565le", "width": "320", "height": "240"}
        with self.assertRaisesRegex(RuntimeError, "covered or missing"):
            probe.require_probe_canvas(SimpleNamespace(metadata=metadata, data=b"\0" * (320 * 240 * 2)))
        probe.require_probe_canvas(SimpleNamespace(metadata=metadata, data=b"\x2b\x12" * (320 * 240)))


if __name__ == "__main__":
    unittest.main()
