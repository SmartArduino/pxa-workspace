#!/usr/bin/env python3
"""Compare legacy and streamed installation of one disposable signed probe.

Requires an app ID beginning with pxa-transfer-probe. Installs/runs/uninstalls
that probe only. Reports go in the caller's ignored local/artifacts directory.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=2000000)
    parser.add_argument("--legacy", type=Path, required=True)
    parser.add_argument("--stream", type=Path, required=True)
    parser.add_argument("--package-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--faults", action="store_true")
    parser.add_argument("--unlock-swipe", type=int, nargs=4, metavar=("X1", "Y1", "X2", "Y2"),
                        help="wake/unlock a device with its known swipe gesture before each launch")
    args = parser.parse_args()
    identity = pxadb.package_manifest_identity(args.stream)
    if not identity.startswith("pxa-transfer-probe") or pxadb.package_manifest_identity(args.legacy) != identity:
        parser.error("both inputs must be the same disposable probe")
    args.output.mkdir(parents=True, exist_ok=True)
    os.environ["PXADB_BAUD"] = str(args.baud)
    report = dict(passed=False, runs=[], faults=[], port=args.port)
    root = f"pxa-state/inbox/.receiving-{identity}"

    def save():
        (args.output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")

    with (args.output / "serial.jsonl").open("w", buffering=1) as log:
        c = pxadb.PxaDbClient(args.port, timeout=60)
        c.raw_callback = lambda raw: log.write(json.dumps(dict(raw=raw.decode("utf8", "replace"))) + "\n")
        launch_events = []
        def record_log(frame):
            log.write(json.dumps(dict(kind=frame.kind, payload=frame.payload)) + "\n")
            # The four-entry live queue can drop the foreground notification
            # during startup. A submitted first frame also proves launch; no
            # other application runs during this probe.
            if ("Application foregrounded:" in frame.payload and identity in frame.payload) or \
                    "PxaSurface: First frame submitted:" in frame.payload:
                launch_events.append(time.monotonic())
        c.log_callback = record_log
        c.hello(); c.subscribe_logs()
        fs = pxadb.NormalFsClient(c)

        def request(command, timeout=120):
            frames = c.request(command, timeout=timeout)
            log.write(json.dumps(dict(command=command, response=[f.payload for f in frames])) + "\n")
            return frames

        def info():
            result = {}
            for frame in request("INFO"):
                if frame.kind == "OK": result.update(pxadb.info_properties(frame.payload))
            return result

        def pump(seconds):
            until = time.monotonic() + seconds
            while time.monotonic() < until:
                c.pump_logs(); time.sleep(.01)

        def remove_probe():
            packages = request("PACKAGES")
            if any(f.kind == "PKG" and f.payload.split("\t")[0].rsplit(":", 1)[-1] == identity and "installed=1" in f.payload for f in packages):
                try: request(f"PACKAGE stop {identity}")
                except pxadb.PxaDbRejectedError as error:
                    if str(error) != "package_not_running": raise
                request(f"PACKAGE uninstall {identity}")
            pxadb.clear_staged_source(c, identity)

        try:
            report["hello"] = c.device_info
            assert pxadb.info_properties(c.device_info).get("package_stream") == "2"
            report["before"] = info(); remove_probe()
            for directory in ("pxa-state", "pxa-state/inbox"):
                fs.mkdir(directory)
            if args.faults:
                raw = args.stream.read_bytes()
                index = 64 + struct.unpack_from("<I", raw, 16)[0] + 216
                for name, at in (("index-tamper", index + 16), ("payload-tamper", len(raw)-1)):
                    path = args.output / f"{name}.pxa"
                    changed = bytearray(raw); changed[at] ^= 1; path.write_bytes(changed)
                    try:
                        fs.put(path, root, progress=lambda *_: None, stream_install=True)
                        raise AssertionError("tampered stream accepted")
                    except pxadb.PxaDbRejectedError as error:
                        assert str(error) == "stream_verification_failed", str(error)
                    request("FSABORT")
                    assert not any(name == Path(root).name for _, _, name in fs.list("pxa-state/inbox"))
                    report["faults"].append(dict(name=name, passed=True)); save()
                # Cancel after authenticated metadata and some payload have
                # arrived. No full container file or executable is published.
                digest = hashlib.sha256(raw).hexdigest()
                c.request_until(f"FSINSTALL {pxadb.client_encode_path(root)} {len(raw)} {digest}", {"READY"})
                prefix = pxadb.stream_container_prefix(args.stream)
                stop = min(len(raw)-1, prefix+40000)
                offset = 0
                while offset < stop:
                    chunk = raw[offset:min(offset+2048, stop)]
                    c.send_bulk_upload(offset, chunk, 30); offset += len(chunk)
                request("FSABORT")
                assert not any(name == Path(root).name for _, _, name in fs.list("pxa-state/inbox"))
                report["faults"].append(dict(name="cancel-partial-expanded-tree", passed=True)); save()
            for mode, source in (("legacy", args.legacy), ("stream", args.stream)):
                for turn in range(args.rounds):
                    remove_probe()
                    before = info(); samples = []
                    start = time.monotonic(); last_sample = -32768
                    def progress(done, total):
                        nonlocal last_sample
                        if done == total or done >= last_sample + 32768:
                            # Fast mode owns the wire until the final ACK. A
                            # progress callback must not inject an INFO command
                            # into its binary frame; sample space at completion.
                            free=None if getattr(c,"fast_upload_active",False) else int(info()["storage_free"])
                            samples.append(dict(received=done, free=free))
                            last_sample = done
                    fs.put(source, root if mode == "stream" else root+".pxa", progress=progress, stream_install=mode == "stream")
                    uploaded = time.monotonic(); staged = info()
                    if mode == "stream":
                        for file in args.package_directory.rglob("*"):
                            if not file.is_file(): continue
                            remote = root + "/" + file.relative_to(args.package_directory).as_posix()
                            digest = pxadb.info_properties(request("FSSHA " + pxadb.client_encode_path(remote))[-1].payload)
                            assert digest["sha256"] == pxadb.file_sha256(file), str(file)
                    commit = time.monotonic(); request(f"PACKAGE deploy {identity}"); installed = time.monotonic()
                    committed = info()
                    pxadb.clear_staged_source(c, identity)
                    after = info()
                    launches = []
                    for launch in range(2):
                        if args.unlock_swipe:
                            request("INPUT SWIPE " + " ".join(map(str,args.unlock_swipe)) + " 300 12")
                            # On an already unlocked screen the same gesture
                            # opens recents. Return home before launching so an
                            # active background app cannot pass as visible UI.
                            request("INPUT KEY HOME")
                            request("INPUT SYNC"); pump(.5)
                        count = len(launch_events)
                        begin = time.monotonic(); request(f"PACKAGE run {identity}")
                        while len(launch_events) == count and time.monotonic() < begin + 15:
                            pump(.05)
                        # Logs are explicitly lossy. Do not turn a dropped
                        # notification into a product failure or invent a
                        # latency sample; verify runtime state and pixels too.
                        elapsed = launch_events[-1] - begin if len(launch_events) > count else None
                        pump(1)
                        packages = request("PACKAGES")
                        state = next(f.payload for f in packages if f.kind == "PKG" and f.payload.split("\t")[0].rsplit(":", 1)[-1] == identity)
                        assert "active=1" in state, state
                        capture = pxadb.screenshot_capture(c.request("SCREENSHOT JPEG", timeout=60))
                        assert capture.data.startswith(b"\xff\xd8"), "missing rendered JPEG"
                        screenshot = f"{mode}-{turn+1}-launch-{launch+1}.jpg"
                        (args.output / screenshot).write_bytes(capture.data)
                        launches.append(dict(seconds=elapsed, state=state, screenshot=screenshot))
                        request(f"PACKAGE stop {identity}"); pump(.2)
                    row = dict(mode=mode, round=turn+1, bytes=source.stat().st_size,
                               upload_seconds=uploaded-start, commit_seconds=installed-commit,
                               before=before, staged=staged, committed=committed, after=after, samples=samples, launches=launches)
                    report["runs"].append(row); save()
                    print(json.dumps({key: row[key] for key in ("mode", "round", "bytes", "upload_seconds", "commit_seconds")}), flush=True)
            remove_probe(); report["after"] = info(); report["passed"] = True
        except BaseException as error:
            report["error"] = repr(error); raise
        finally:
            save()
            try: request("FSABORT"); remove_probe()
            except Exception: pass
            c.close()


if __name__ == "__main__":
    main()
