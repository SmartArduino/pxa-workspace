#!/usr/bin/env python3
"""Measure signed direct-volume installs and candidate cleanup on a real device.

Only installs/removes pxa-transfer-probe* packages. Use local/artifacts for output.
"""
import argparse
import contextlib
import json
import os
from pathlib import Path
import sys
import time
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=2000000)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--package-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--volume", default="sd")
    parser.add_argument("--via", choices=("serial", "usb", "wifi", "auto"), default="serial")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--unlock-swipe", type=int, nargs=4)
    parser.add_argument("--faults", action="store_true")
    parser.add_argument("--hard-reset", action="store_true",
                        help="also toggle RTS/EN on a known ESP UART bridge during reception")
    parser.add_argument("--network-faults", action="store_true",
                        help="with --faults, test short HTTP bodies and serial fallback")
    args = parser.parse_args()
    identity = pxadb.package_manifest_identity(args.package)
    if not identity.startswith("pxa-transfer-probe") or args.rounds < 1:
        parser.error("requires a disposable probe and positive rounds")
    os.environ["PXADB_BAUD"] = str(args.baud)
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, port=args.port, via=args.via, runs=[], faults=[])
    log = (args.output / "serial.jsonl").open("w", buffering=1)
    launches = []

    def save():
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")

    def logged(frame):
        log.write(json.dumps(dict(kind=frame.kind, payload=frame.payload)) + "\n")
        if ("Application foregrounded:" in frame.payload and identity in frame.payload) or \
                "PxaSurface: First frame submitted:" in frame.payload:
            launches.append(time.monotonic())

    def connect():
        c = pxadb.PxaDbClient(args.port, timeout=20)
        c.raw_callback = lambda raw: log.write(json.dumps(dict(raw=raw.decode("utf8", "replace"))) + "\n")
        c.log_callback = logged
        c.hello(attempts=20); c.timeout=120; c.subscribe_logs()
        return c

    c = connect()

    def remove_probe():
        packages = c.request("PACKAGES")
        if any(f.kind == "PKG" and f.payload.split("\t")[0].rsplit(":", 1)[-1] == identity
               and "installed=1" in f.payload for f in packages):
            try:
                c.request(f"PACKAGE stop {identity}")
            except pxadb.PxaDbRejectedError as error:
                if str(error) != "package_not_running":
                    raise
            # Stop is queued; wait for guest/storage retirement before removal.
            deadline = time.monotonic()+20
            while time.monotonic() < deadline:
                states = c.request("PACKAGES")
                if not any(f.kind == "PKG" and f.payload.split("\t")[0].rsplit(":", 1)[-1] == identity
                           and "active=1" in f.payload for f in states):
                    break
                time.sleep(.2)
            time.sleep(.5)
            c.request(f"PACKAGE uninstall {identity}")

    def absent(remote):
        entries = pxadb.NormalFsClient(c).list(str(Path(remote).parent))
        assert Path(remote).name not in [name for _, _, name in entries], remote

    def private_inventory():
        volume = pxadb.select_install_volume(c, args.volume)
        base = ("" if volume["relative"] == "." else volume["relative"] + "/") + "PXA/apps"
        fs = pxadb.NormalFsClient(c)
        rows = []

        def walk(path, depth):
            assert depth < 5, path
            for kind, size, name in fs.list(path):
                child = path + "/" + name
                if kind == "D":
                    walk(child, depth + 1)
                else:
                    rows.append(dict(path=child, bytes=size))

        for kind, _, publisher in fs.list(base):
            if kind != "D":
                continue
            parent = base + "/" + publisher
            if identity not in [name for kind, _, name in fs.list(parent) if kind == "D"]:
                continue
            home = parent + "/" + identity
            for kind, _, name in fs.list(home):
                if kind == "D" and name in ("data", "private"):
                    walk(home + "/" + name, 0)
        return dict(files=rows, file_bytes=sum(row["bytes"] for row in rows),
                    allocation_unit=int(volume["unit"]))

    try:
        report["hello"] = c.device_info
        report["before"] = [f.payload for f in c.request("INFO")]
        report["volumes_before"] = pxadb.device_volumes(c)
        remove_probe()
        for turn in range(args.rounds):
            start = time.monotonic()
            epoch, remote, transport = pxadb.stage_package_on_volume(c, args.package, args.volume, args.via)
            uploaded = time.monotonic()
            # The bounded live log queue may fill while HTTP blocks the command
            # task. Fetch its recent history before later launch logs replace it.
            c.request("LOGSUB")
            staged_volumes = pxadb.device_volumes(c)
            print(json.dumps(dict(phase="received", seconds=uploaded-start,
                                  KB_s=args.package.stat().st_size/(uploaded-start)/1000)), flush=True)
            # Independently hash every expanded file outside the timed upload.
            for path in args.package_directory.rglob("*"):
                if not path.is_file():
                    continue
                device_path = remote + "/" + path.relative_to(args.package_directory).as_posix()
                actual = pxadb.info_properties(c.request("FSSHA " + pxadb.client_encode_path(device_path))[-1].payload)
                assert actual["sha256"] == pxadb.file_sha256(path), device_path
            commit = time.monotonic()
            c.request(f"PACKAGE install-volume {epoch} {pxadb.client_encode_path(remote)}")
            installed = time.monotonic()
            committed_volumes = pxadb.device_volumes(c)
            private_before_launch = private_inventory()
            print(json.dumps(dict(phase="committed", seconds=installed-commit)), flush=True)
            absent(remote)
            if args.unlock_swipe:
                c.request("INPUT SWIPE " + " ".join(map(str,args.unlock_swipe)) + " 300 12")
                c.request("INPUT KEY HOME")
                c.request("INPUT SYNC")
            count = len(launches); begin = time.monotonic()
            c.request(f"PACKAGE run {identity}")
            while len(launches) == count and time.monotonic() < begin + 30:
                c.pump_logs(); time.sleep(.01)
            launch_seconds=launches[-1]-begin if len(launches)>count else None
            state = next(f.payload for f in c.request("PACKAGES") if f.kind == "PKG"
                         and f.payload.split("\t")[0].rsplit(":", 1)[-1] == identity)
            assert "active=1" in state, state
            total = args.package.stat().st_size
            row = dict(round=turn+1, transport=transport, bytes=total, upload_seconds=uploaded-start,
                       commit_seconds=installed-commit, KB_s=total/(uploaded-start)/1000,
                       install_KB_s=total/(uploaded-start+installed-commit)/1000,
                       launch_seconds=launch_seconds, state=state,
                       private_storage=private_before_launch,
                       staged_volumes=staged_volumes, committed_volumes=committed_volumes)
            report["runs"].append(row); save(); print(json.dumps(row), flush=True)
            time.sleep(1)
            capture = pxadb.screenshot_capture(c.request("SCREENSHOT JPEG", timeout=60))
            assert capture.data.startswith(b"\xff\xd8"), "missing rendered frame"
            (args.output / f"launch-{turn+1}.jpg").write_bytes(capture.data)
            remove_probe()
        if args.faults:
            raw = args.package.read_bytes()
            volume = pxadb.select_install_volume(c, args.volume)
            epoch = int(volume["epoch"])
            prefix = "" if volume["relative"] == "." else volume["relative"] + "/"
            remote = prefix + "PXA/cache/store/.stream-" + os.urandom(4).hex()

            def begin_partial():
                pxadb.begin_file_upload(c, f"FSINSTALL {pxadb.client_encode_path(remote)} {len(raw)} {pxadb.file_sha256(args.package)} {epoch}", 60)
                stop = min(len(raw)-1, pxadb.stream_container_prefix(args.package)+40000)
                for offset in range(0, stop, 2048):
                    c.send_bulk_upload(offset, raw[offset:min(offset+2048, stop)], 60)

            faults = ["cancel-partial", "disconnect-partial", "cancel-finished"]
            if args.hard_reset:
                faults.append("hardware-reset-partial")
            for name in faults:
                if name == "cancel-finished":
                    pxadb.NormalFsClient(c).put(args.package, remote, progress=lambda *_: None,
                                                stream_install=True, stream_volume=epoch)
                else:
                    begin_partial()
                if name == "disconnect-partial":
                    c._hello_completed = False; c.log_subscribed = False
                    c.close(); c = connect()
                elif name == "hardware-reset-partial":
                    c._hello_completed = False; c.log_subscribed = False
                    uart = c.serial.serial_port
                    uart.baudrate = 115200; uart.reset_input_buffer()
                    uart.dtr = False; uart.rts = True; time.sleep(.15); uart.rts = False
                    boot = bytearray(); deadline = time.monotonic()+1
                    while time.monotonic() < deadline:
                        boot.extend(uart.read(2048))
                    uart.baudrate = args.baud
                    (args.output / "reset-boot.bin").write_bytes(boot)
                    assert b"ESP-ROM:" in boot or b"rst:" in boot, "ROM reset evidence missing"
                    c.close(); time.sleep(3)
                    c = connect()
                    deadline = time.monotonic()+30
                    while pxadb.info_properties(c.request("INFO")[-1].payload).get("pxa") != "ready":
                        assert time.monotonic() < deadline, "Host did not recover"
                        time.sleep(.2)
                    epoch = int(pxadb.select_install_volume(c, args.volume)["epoch"])
                else:
                    c.request("FSABORT")
                absent(remote)
                report["faults"].append(dict(name=name, passed=True)); save()
            changed = bytearray(raw); changed[-1] ^= 1
            tampered = args.output / "tampered.pxa"; tampered.write_bytes(changed)
            try:
                pxadb.NormalFsClient(c).put(tampered, remote, progress=lambda *_: None,
                                            stream_install=True, stream_volume=epoch)
                raise AssertionError("tampered candidate accepted")
            except pxadb.PxaDbRejectedError as error:
                assert "verification_failed" in str(error), str(error)
            absent(remote)
            report["faults"].append(dict(name="signed-payload-tamper", passed=True)); save()
            if args.network_faults:
                network = pxadb.info_properties(c.request("NETINFO")[-1].payload)
                short = args.output / "short-body.pxa"
                short.write_bytes(raw[:pxadb.stream_container_prefix(args.package)+40000])
                c.request_until(f"FSINSTALL {pxadb.client_encode_path(remote)} {len(raw)} {pxadb.file_sha256(args.package)} {epoch}", {"READY"})
                with pxadb.package_http_source(short, network["ipv4"]) as url:
                    try:
                        c.request("FSFETCH " + pxadb.client_encode_path(url))
                        raise AssertionError("short HTTP body accepted")
                    except pxadb.PxaDbRejectedError as error:
                        assert "stream_fetch_failed" in str(error), str(error)
                absent(remote)
                report["faults"].append(dict(name="short-http-body", passed=True)); save()
                real_source = pxadb.package_http_source

                @contextlib.contextmanager
                def missing_source(source, device):
                    with real_source(source, device) as url:
                        yield url + "-not-found"

                with mock.patch.object(pxadb, "package_http_source", missing_source):
                    _, candidate, method = pxadb.stage_package_on_volume(c, args.package, args.volume, "auto")
                assert method == "serial"
                c.request("FSABORT"); absent(candidate)
                report["faults"].append(dict(name="http-failure-serial-fallback", passed=True)); save()
        report["after"] = [f.payload for f in c.request("INFO")]
        report["volumes_after"] = pxadb.device_volumes(c)
        report["passed"] = True
    except BaseException as error:
        report["error"] = repr(error)
        raise
    finally:
        save()
        try:
            c.request("FSABORT"); remove_probe()
        finally:
            c.close(); log.close()


if __name__ == "__main__":
    main()
