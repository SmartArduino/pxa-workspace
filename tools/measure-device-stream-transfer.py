#!/usr/bin/env python3
"""Compare old bulk and negotiated binary streams, including verification/decode.

Receives only disposable pxa-transfer-probe packages and aborts their candidates;
does not install/uninstall apps. Use measure-device-volume-install.py for commits.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent / "pxadb"))
import pxadb


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=2000000)
    parser.add_argument("--packages", type=Path, nargs="+", required=True)
    parser.add_argument("--volume", default="sd")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--modes", nargs="+", choices=("bulk", "fast"), default=["bulk", "fast"])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    for source in args.packages:
        if not pxadb.package_manifest_identity(source).startswith("pxa-transfer-probe"):
            parser.error("requires disposable probe packages")
    os.environ["PXADB_BAUD"] = str(args.baud)
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(passed=False, runs=[])
    with pxadb.PxaDbClient(args.port, timeout=20) as c:
        c.hello(attempts=20); c.timeout = 120
        report["hello"] = c.device_info
        report["before"] = pxadb.device_volumes(c)
        try:
            for source in args.packages:
                header = source.read_bytes()[:64]
                unpacked = int.from_bytes(header[40:48], "little")
                for mode in args.modes:
                    os.environ["PXADB_FAST"] = "1" if mode == "fast" else "0"
                    for turn in range(args.rounds):
                        started = time.monotonic()
                        epoch, remote, transport = pxadb.stage_package_on_volume(c, source, args.volume, "serial")
                        elapsed = time.monotonic() - started
                        staged = pxadb.device_volumes(c)
                        c.request("FSABORT")
                        assert Path(remote).name not in [name for _, _, name in pxadb.NormalFsClient(c).list(str(Path(remote).parent))]
                        assert c.request("PING")[-1].payload == "pong"
                        row = dict(package=str(source), mode=mode, round=turn + 1, transport=transport,
                                   seconds=elapsed, bytes=source.stat().st_size, expanded_bytes=unpacked,
                                   KB_s=source.stat().st_size / elapsed / 1000,
                                   expanded_KB_s=unpacked / elapsed / 1000, staged_volumes=staged)
                        report["runs"].append(row)
                        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
                        print(json.dumps({k: v for k, v in row.items() if k != "staged_volumes"}), flush=True)
            report["after"] = pxadb.device_volumes(c)
            report["passed"] = True
        finally:
            c.request("FSABORT")
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
