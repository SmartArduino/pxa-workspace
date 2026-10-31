#!/usr/bin/env python3
"""Measure checked PXADB uploads and inject rejected/interrupted bulk frames.

Uses only a uniquely named receiving file; never removes installed apps.
Pass an output directory under local/artifacts for raw logs and measurements.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
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
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--sizes", type=int, nargs="+", default=[65536, 262144])
    parser.add_argument("--faults", action="store_true")
    args = parser.parse_args()
    if args.rounds < 1 or any(size < 1 for size in args.sizes):
        parser.error("rounds and sizes must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["PXADB_BAUD"] = str(args.baud)
    report = dict(port=args.port, baud=args.baud, runs=[], faults=[], passed=False)
    remote = f"pxa-state/inbox/.receiving-pxa-transfer-{uuid.uuid4().hex[:10]}.pxa"
    report["remote"] = remote
    path = pxadb.client_encode_path(remote)

    def save():
        (args.output / "report.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")

    with (args.output / "serial.jsonl").open("w", buffering=1) as log:
        def connect():
            c = pxadb.PxaDbClient(args.port, timeout=20)
            c.raw_callback = lambda raw: log.write(json.dumps(dict(raw=raw.decode("utf8", "replace"))) + "\n")
            c.log_callback = lambda f: log.write(json.dumps(dict(kind=f.kind, payload=f.payload)) + "\n")
            c.hello()
            c.subscribe_logs()
            return c

        c = connect()
        try:
            report["hello"] = c.device_info
            report["before"] = [f.payload for f in c.request("INFO")]
            fs = pxadb.NormalFsClient(c)
            bulk = pxadb.info_properties(c.device_info).get("fs_bulk_cobs") == "1"
            for size in args.sizes:
                source = args.output / f"sample-{size}.bin"
                source.write_bytes(random.Random(size).randbytes(size))
                digest = pxadb.file_sha256(source)
                modes=["legacy", "bulk"] if bulk else ["legacy"]
                if pxadb.info_properties(c.device_info).get("fs_fast")=="1":modes.append("fast")
                for mode in modes:
                    os.environ["PXADB_BULK"] = "0" if mode == "legacy" else "1"
                    os.environ["PXADB_FAST"] = "1" if mode == "fast" else "0"
                    for turn in range(args.rounds):
                        begin = time.monotonic()
                        fs.put(source, remote, progress=lambda done, total: None)
                        elapsed = time.monotonic() - begin
                        actual = pxadb.info_properties(c.request(f"FSSHA {path}", timeout=60)[-1].payload)
                        assert actual.get("sha256") == digest and int(actual["bytes"]) == size
                        row = dict(mode=mode, bytes=size, round=turn + 1, seconds=elapsed, KiB_s=size / 1024 / elapsed)
                        report["runs"].append(row)
                        fs.remove(remote)
                        save()
                        print(json.dumps(row), flush=True)

            if args.faults:
                if not bulk:
                    raise RuntimeError("fault tests require fs_bulk_cobs=1")
                os.environ["PXADB_BULK"] = "1"
                data = bytes(range(256)) * 8
                digest = hashlib.sha256(data).hexdigest()

                def begin(expected=digest):
                    return c.request_until(f"FSPUT {path} {len(data)} {expected}", {"READY", "OK"})[-1]

                def reject(name, body, crc, size=len(data), reason="invalid_file_chunk"):
                    assert begin().payload == str(len(data))
                    seq = c._next_sequence()
                    command = f"FSBULK 0 {size} {crc:08x}"
                    c._write_command(seq, command)
                    c._collect_serial_frames(seq, {"CONTINUE"}, command, time.monotonic() + 10)
                    c._write_bulk_bytes(body)
                    try:
                        c._collect_serial_frames(seq, {"OK", "READY"}, command, time.monotonic() + 15)
                        raise AssertionError("invalid frame accepted")
                    except pxadb.PxaDbRejectedError as error:
                        assert str(error) == reason, str(error)
                    assert begin().payload == str(len(data)), "rejected data advanced offset"
                    assert c.request("PING")[-1].payload == "pong"
                    report["faults"].append(dict(name=name, passed=True)); save()

                reject("crc", pxadb.encode_bulk(data), zlib.crc32(data) ^ 1, reason="chunk_checksum_mismatch")
                reject("truncated-cobs", b"\x04ab\0", 0)
                reject("oversize-body", b"\x01" * 2100 + b"\0", 0)
                reject("command-in-body", pxadb.encode_bulk(b"PXADB1 999 REBOOT\n"), 0)
                # Confirmed prefix survives a rejected subsequent batch.
                assert begin().kind == "READY"
                c.send_bulk_upload(0, data[:1024], 10)
                try:
                    c.send_bulk_upload(0, data[:1024], 10)
                    raise AssertionError("duplicate offset accepted")
                except pxadb.PxaDbRejectedError as error:
                    assert str(error) == "offset_mismatch"
                assert begin().payload == str(len(data) - 1024)
                c.send_bulk_upload(1024, data[1024:], 30)
                assert pxadb.info_properties(c.request(f"FSSHA {path}")[-1].payload)["sha256"] == digest
                fs.remove(remote)
                report["faults"].append(dict(name="resume-confirmed-prefix", passed=True)); save()

                assert begin("0" * 64).kind == "READY"
                try:
                    c.send_bulk_upload(0, data, 30)
                    raise AssertionError("invalid file digest accepted")
                except pxadb.PxaDbRejectedError as error:
                    assert str(error) == "checksum_mismatch"
                try:
                    c.request(f"FSSHA {path}")
                    raise AssertionError("rejected file published")
                except pxadb.PxaDbRejectedError:
                    pass
                report["faults"].append(dict(name="final-sha-rejection", passed=True)); save()

                assert begin().kind == "READY"
                seq = c._next_sequence(); command = f"FSBULK 0 {len(data)} 00000000"
                c._write_command(seq, command)
                c._collect_serial_frames(seq, {"CONTINUE"}, command, time.monotonic() + 10)
                c._write_bulk_bytes(pxadb.encode_bulk(data)[:128])
                # Close the transport without BYE, just like a crashed client.
                c._hello_completed = False; c.log_subscribed = False; c.close()
                c = connect(); fs = pxadb.NormalFsClient(c)
                assert begin().payload == str(len(data))
                c.send_bulk_upload(0, data, 30)
                assert pxadb.info_properties(c.request(f"FSSHA {path}")[-1].payload)["sha256"] == digest
                fs.remove(remote)
                report["faults"].append(dict(name="disconnect-partial-reconnect", passed=True)); save()
            report["after"] = [f.payload for f in c.request("INFO")]
            report["passed"] = True
        except BaseException as error:
            report["error"] = repr(error)
            raise
        finally:
            save()
            try: c.request("FSABORT")
            except Exception: pass
            c.close()


if __name__ == "__main__":
    main()
