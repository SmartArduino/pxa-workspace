#!/usr/bin/env python3
"""Exercise negotiated binary transfers and recovery on one real PXADB device.

Only writes a unique disposable receiving file. Reports belong under local/artifacts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time
import uuid
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=2000000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    os.environ["PXADB_BAUD"] = str(args.baud)
    args.output.mkdir(parents=True, exist_ok=True)
    remote = f"pxa-state/inbox/.receiving-pxa-fast-{uuid.uuid4().hex[:10]}.pxa"
    path = pxadb.client_encode_path(remote)
    data = bytes(range(256)) * 20
    digest = hashlib.sha256(data).hexdigest()
    report = dict(passed=False, remote=remote, tests=[])
    log = (args.output / "serial.jsonl").open("w", buffering=1)

    def connect():
        client = pxadb.PxaDbClient(args.port, timeout=20)
        client.raw_callback = lambda raw: log.write(json.dumps(dict(raw=raw.decode("utf8", "replace"))) + "\n")
        client.hello(attempts=20)
        return client

    c = connect()

    def done(name, **values):
        assert c.request("PING")[-1].payload == "pong"
        report["tests"].append(dict(name=name, passed=True, **values))
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(name, "passed", flush=True)

    def absent():
        entries = pxadb.NormalFsClient(c).list(str(Path(remote).parent))
        assert not any(name.startswith(Path(remote).name) for _, _, name in entries)

    def begin(chunk=2048, sha=digest):
        assert pxadb.begin_file_upload(c, f"FSPUT {path} {len(data)} {sha}", 30)[-1].payload == str(len(data))
        return c.start_fast_upload(chunk)

    try:
        report["hello"] = c.device_info
        assert pxadb.info_properties(c.device_info).get("fs_fast") == "1"
        report["memory_before"] = [f.payload for f in c.request("MEMORY")]
        for requested in (256, 512, 1024, 2048, 3840):
            chunk = begin(requested)
            assert 256 <= chunk <= requested
            for at in range(0, len(data), chunk):
                c.send_fast_upload(at, data[at:at + chunk], 30)
            actual = pxadb.info_properties(c.request(f"FSSHA {path}")[-1].payload)
            assert actual["sha256"] == digest
            pxadb.NormalFsClient(c).remove(remote)
            done(f"negotiated-{requested}", chunk=chunk)

        for name in ("crc", "offset", "oversize", "invalid-cobs", "command-in-body"):
            begin(); seq = c._next_sequence()
            raw = struct.pack("<4sIQI", b"PXD1", seq, 1 if name == "offset" else 0, 256) + data[:256]
            crc = zlib.crc32(raw) ^ (1 if name == "crc" else 0)
            wire = pxadb.encode_bulk(raw + struct.pack("<I", crc))
            if name == "oversize": wire = b"\x01" * 4000 + b"\0"
            if name == "invalid-cobs": wire = b"\x05ab\0"
            if name == "command-in-body": wire = pxadb.encode_bulk(b"PXADB1 999 REBOOT\n")
            c._write_bulk_bytes(wire)
            # Malformed headers cannot carry a trusted reply sequence; collect
            # the admission sequence for those, then assert cleanup and liveness.
            expected_seq = seq if name in {"crc", "offset"} else seq - 1
            try:
                c._collect_serial_frames(expected_seq, {"READY", "OK"}, name, time.monotonic() + 10)
                raise AssertionError("invalid frame accepted")
            except pxadb.PxaDbRejectedError:
                pass
            c.fast_upload_active = False
            absent(); done(name)

        for name in ("cancel-boundary", "cancel-partial", "timeout-boundary", "timeout-partial"):
            begin()
            c.send_fast_upload(0, data[:256], 10)
            if name.endswith("partial"):
                c._write_bulk_bytes(b"\x08abc\n")
            if name.startswith("timeout"):
                deadline = time.monotonic() + 11
                while time.monotonic() < deadline:
                    time.sleep(.1)
                # A fresh HELLO begins with a delimiter and must be usable even
                # if a late body fragment was outstanding when the lease expired.
                c.fast_upload_active = False
                c.hello()
            else:
                c.cancel_fast_upload()
            absent(); done(name)

        begin(); c.send_fast_upload(0, data[:256], 10)
        # Simulate process death without the client's orderly close/abort.
        c.fast_upload_active = False; c._hello_completed = False; c.log_subscribed = False
        c.close(); c = connect()
        absent(); done("disconnect-reconnect")

        begin(sha="0" * 64)
        try:
            for at in range(0, len(data), 2048):
                c.send_fast_upload(at, data[at:at + 2048], 30)
            raise AssertionError("invalid whole-file hash accepted")
        except pxadb.PxaDbRejectedError as error:
            assert str(error) == "checksum_mismatch"
        c.cancel_fast_upload(); absent(); done("file-sha256")
        report["memory_after"] = [f.payload for f in c.request("MEMORY")]
        report["passed"] = True
    finally:
        try:
            c.cancel_fast_upload(); c.request("FSABORT")
            pxadb.NormalFsClient(c).remove(remote)
        except Exception:
            pass
        c.close(); log.close()
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
