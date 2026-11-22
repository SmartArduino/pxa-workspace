#!/usr/bin/env python3
"""Compare PXA build profiles using checksum-verified device measurements."""
from pathlib import Path
import argparse
import json
import statistics
import sys

sys.dont_write_bytecode = True
from summarize import cpu, pxa_frames


def read_run(path, expected, pixel):
    metadata = json.loads((path / "metadata.json").read_text())
    if not metadata["done"]:
        raise ValueError(f"Incomplete run: {path}")
    records = json.loads((path / "records.json").read_text())
    if not any(r.get("test") == "pixels" and int(r["sum"]) == pixel for r in records):
        raise ValueError(f"Missing or incorrect pixel checksum: {path}")
    info = dict(field.split("=", 1) for field in metadata["info"].split(";") if "=" in field)
    identity = {key: info[key] for key in ("serial", "target", "version")}
    return {"path": str(path), "identity": identity, "cpu": cpu(records, expected)}, records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, action="append", required=True)
    parser.add_argument("--candidate", type=Path, action="append", required=True)
    parser.add_argument("--display-reference", type=Path,
                        help="Optional separate historical full run; not a fresh paired FPS baseline")
    parser.add_argument("--checksums", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    expected = {}
    pixel = None
    for line in args.checksums.read_text().splitlines():
        fields = line.split()
        if fields[0] == "cpu":
            expected[int(fields[1])] = int(fields[2])
        else:
            pixel = int(fields[1])
    result = {"method": "Median of run medians; each run discards two of nine CPU samples",
              "checksums_verified": True, "runs": {}, "cpu": []}
    identities = []
    display_runs = {"baseline": [], "candidate": []}
    for label, paths in (("baseline", args.baseline), ("candidate", args.candidate)):
        result["runs"][label] = []
        for path in paths:
            run, records = read_run(path, expected, pixel)
            result["runs"][label].append(run)
            identities.append(run["identity"])
            if any(r.get("test") == "done" for r in records):
                display_runs[label].append({"path": str(path), "fps": pxa_frames(records)})
    if any(identity != identities[0] for identity in identities):
        raise ValueError("Device identity, target or firmware version changed between runs")
    result["identity"] = identities[0]
    for kind in range(5):
        baseline = statistics.median(run["cpu"][kind]["median_ms"] for run in result["runs"]["baseline"])
        candidate = statistics.median(run["cpu"][kind]["median_ms"] for run in result["runs"]["candidate"])
        item = {"name": result["runs"]["baseline"][0]["cpu"][kind]["name"],
                "baseline_ms": baseline, "candidate_ms": candidate,
                "time_reduction_percent": (baseline - candidate) / baseline * 100}
        result["cpu"].append(item)
        print(f'{item["name"]}: {baseline:.3f} -> {candidate:.3f} ms; time reduction {item["time_reduction_percent"]:+.2f}%')
    result["display_runs"] = display_runs
    if display_runs["baseline"] and display_runs["candidate"]:
        result["display_comparison"] = []
        for mode in range(4):
            baseline = statistics.median(run["fps"][mode]["median_fps"] for run in display_runs["baseline"])
            candidate = statistics.median(run["fps"][mode]["median_fps"] for run in display_runs["candidate"])
            result["display_comparison"].append({
                "name": display_runs["baseline"][0]["fps"][mode]["name"],
                "baseline_fps": baseline, "candidate_fps": candidate,
                "fps_change_percent": (candidate - baseline) / baseline * 100 if baseline else None})
    if args.display_reference:
        reference, records = read_run(args.display_reference, expected, pixel)
        if reference["identity"] != identities[0]:
            raise ValueError("Historical FPS reference identity differs")
        result["historical_display_reference"] = {"path": str(args.display_reference),
                                                   "fps": pxa_frames(records),
                                                   "fresh_paired_measurement": False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
