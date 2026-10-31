#!/usr/bin/env python3
"""Exercise low-space streaming and an optional UART hardware reset mid-transfer.

Only a pxa-transfer-probe package and a unique receiving filler are removed.
--hard-reset toggles RTS/EN on a known ESP UART bridge; do not use on arbitrary
serial devices. Reports and filler data belong under local/artifacts.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--baud", type=int, default=2000000)
    p.add_argument("--legacy", type=Path, required=True)
    p.add_argument("--stream", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--hard-reset", action="store_true")
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=True)
    os.environ["PXADB_BAUD"] = str(a.baud)
    identity = pxadb.package_manifest_identity(a.stream)
    if not identity.startswith("pxa-transfer-probe"): p.error("requires a disposable probe")
    report = dict(passed=False, hard_reset=a.hard_reset)
    filler = f"pxa-state/inbox/.receiving-pxa-stream-filler-{uuid.uuid4().hex[:8]}.pxa"
    report["filler"] = filler
    root = f"pxa-state/inbox/.receiving-{identity}"
    log = (a.output / "serial.jsonl").open("w", buffering=1)

    def connect():
        c = pxadb.PxaDbClient(a.port, timeout=90)
        c.raw_callback = lambda raw: log.write(json.dumps(dict(raw=raw.decode("utf8", "replace"))) + "\n")
        c.log_callback = lambda f: log.write(json.dumps(dict(kind=f.kind, payload=f.payload)) + "\n")
        c.hello(); c.subscribe_logs(); return c

    c = connect(); fs = pxadb.NormalFsClient(c)
    def info(): return pxadb.info_properties(c.request("INFO")[-1].payload)
    def uninstall():
        rows = c.request("PACKAGES")
        if any(f.kind == "PKG" and f.payload.split("\t")[0].rsplit(":", 1)[-1] == identity and "installed=1" in f.payload for f in rows):
            try: c.request(f"PACKAGE stop {identity}")
            except pxadb.PxaDbRejectedError as error:
                if str(error) != "package_not_running": raise
            c.request(f"PACKAGE uninstall {identity}", timeout=120)
        pxadb.clear_staged_source(c, identity)
    try:
        uninstall(); report["before"] = info()
        old_upload, install = pxadb.package_storage_required(a.legacy)
        prefix = pxadb.stream_container_prefix(a.stream)
        stream_required = install + ((prefix+4031)//4032+3)*4096
        legacy_required = old_upload + install
        target = (stream_required+legacy_required)//2
        delta = int(info()["storage_free"])-target
        filler_bytes = max(0, (delta//4096-3)*4032)
        source = a.output / "filler.bin"
        with source.open("wb") as output: output.truncate(filler_bytes)
        assert filler_bytes > 0, "not enough space to establish controlled comparison"
        fs.put(source, filler, progress=lambda *_: None)
        report["constrained"] = info()
        report["estimated_legacy_bytes"] = legacy_required
        report["estimated_stream_bytes"] = stream_required
        assert stream_required <= int(report["constrained"]["storage_free"]) < legacy_required
        try:
            pxadb.check_package_storage(fs, a.legacy)
            raise AssertionError("legacy admission should reject this free space")
        except pxadb.PxaDbError as error:
            assert "insufficient device storage" in str(error)
        pxadb.stage_package_files(fs, a.stream, identity, 120, publish=False)
        c.request(f"PACKAGE deploy {identity}", timeout=120)
        pxadb.clear_staged_source(c, identity)
        report["installed_under_pressure"] = info()
        fs.remove(filler)
        if a.hard_reset:
            raw = a.stream.read_bytes()
            c.request_until(f"FSINSTALL {pxadb.client_encode_path(root)} {len(raw)} {pxadb.file_sha256(a.stream)}", {"READY"})
            stop = min(len(raw)-1, prefix+40000)
            for offset in range(0, stop, 2048):
                c.send_bulk_upload(offset, raw[offset:min(offset+2048, stop)], 30)
            assert any(name == Path(root).name for _, _, name in fs.list("pxa-state/inbox"))
            report["before_reset"] = info()
            # Bypass BYE and the software reboot command: EN reset occurs with
            # an open, partially expanded file still owned by the receiver.
            c._hello_completed = False; c.log_subscribed = False
            uart = c.serial.serial_port
            uart.baudrate = 115200
            uart.reset_input_buffer()
            uart.dtr = False; uart.rts = True; time.sleep(.15); uart.rts = False
            boot = bytearray()
            deadline = time.monotonic()+1
            while time.monotonic()<deadline: boot.extend(uart.read(2048))
            uart.baudrate = a.baud
            (a.output / "reset-boot.bin").write_bytes(boot)
            # A subsequent HELLO also discards receiving directories. Require
            # independent ROM evidence so that reconnect cannot impersonate a reset.
            assert b"ESP-ROM:" in boot or b"rst:" in boot, "hardware reset not confirmed by ROM output"
            report["rom_reset_confirmed"] = True
            c.close(); time.sleep(2)
            deadline = time.monotonic()+40
            while True:
                try:
                    c = connect(); fs = pxadb.NormalFsClient(c)
                    if info().get("pxa") == "ready": break
                    c._hello_completed = False; c.log_subscribed = False; c.close()
                except Exception:
                    try: c.close()
                    except Exception: pass
                if time.monotonic()>deadline: raise AssertionError("device failed to recover")
                time.sleep(1)
            assert not any(name == Path(root).name for _, _, name in fs.list("pxa-state/inbox")), "cold boot left unpublished stream"
            report["after_reset"] = info()
            report["hardware_reset_recovery"] = True
        c.request(f"PACKAGE run {identity}")
        deadline = time.monotonic()+3
        while time.monotonic()<deadline: c.pump_logs(); time.sleep(.02)
        rows = c.request("PACKAGES")
        assert any(f.kind == "PKG" and identity in f.payload and "active=1" in f.payload for f in rows)
        report["installed_app_survived"] = True
        uninstall(); report["after"] = info(); report["passed"] = True
    except BaseException as error:
        report["error"] = repr(error); raise
    finally:
        (a.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        try: fs.remove(filler)
        except Exception: pass
        try: c.request("FSABORT"); uninstall()
        except Exception: pass
        c.close(); log.close()


if __name__ == "__main__": main()
