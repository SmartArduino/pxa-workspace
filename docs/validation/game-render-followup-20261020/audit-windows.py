"""Recheck stored device evidence, without opening a serial port or running games."""
from pathlib import Path
import json
import re
import statistics

ROOT = Path(__file__).resolve().parent


def properties(text):
    return dict(re.findall(r"(\w+)=([0-9,-]+)", text))


def audit(directory, game, expected_windows):
    base = ROOT / directory
    report = json.loads((base / "report.json").read_text())
    records = [json.loads(line) for line in (base / "serial.jsonl").read_text().splitlines()]
    samples = []
    pending = False
    for record in records:
        if record.get("kind") == "COMMAND" and record.get("payload") == "PERF FPS":
            assert not pending, (directory, "unanswered FPS command")
            pending = True
        elif pending and record.get("kind") == "OK" and "presentation_fps_x10=" in record.get("payload", ""):
            samples.append((record["time"], int(properties(record["payload"])["presentation_fps_x10"]) / 10))
            pending = False
    windows = report["windows"]
    assert len(windows) == expected_windows, (directory, len(windows))
    assert len(samples) == expected_windows * 60 and not pending, (directory, len(samples))
    stamped = [(row, int(row["payload"].split("\t", 1)[0])) for row in records
               if re.match(r"^\d+\t", row.get("payload", ""))]
    offset = min(row["time"] - stamp / 1000 for row, stamp in stamped)
    result = []
    for index, window in enumerate(windows):
        raw = samples[index * 60:(index + 1) * 60]
        values = [value for _, value in raw]
        assert values == [sample["fps"] for sample in window["fps_samples"]]
        assert len(values) == 60
        span = raw[-1][0] - raw[0][0]
        assert 58 <= span <= 61, (directory, window["label"], span)
        for key, actual in {"median": statistics.median(values), "mean": statistics.mean(values),
                            "min": min(values), "max": max(values)}.items():
            assert abs(window["fps"][key] - actual) < 1e-8
        row = {"label": window["label"], "samples": 60, "first_to_last_seconds": span,
               "fps": window["fps"], "audio_fidelity_qualified": False,
               "visual_review_required": True}
        if game in ("quake", "duke"):
            unique = {}
            excluded = 0
            tag = "PXA QUAKE " if game == "quake" else "PXA DUKE INPUT "
            for record, stamp in stamped:
                if not raw[0][0] - 1.5 <= record["time"] <= raw[-1][0] + .1:
                    continue
                if tag not in record["payload"]:
                    continue
                if record["time"] - stamp / 1000 - offset > 2:
                    excluded += 1
                    continue
                unique.setdefault(stamp, properties(record["payload"]))
            minimum_records = 10 if game == "quake" else 8
            assert len(unique) >= minimum_records, (directory, window["label"], len(unique))
            stamps = sorted(unique)
            assert stamps[-1] - stamps[0] >= 45000
            states = list(unique.values())
            if game == "quake":
                mode = str(window["state_before"]["mode"])
                assert all(s["ready"] == "1" and s["hp"] == "100" and
                           s["xyz"] == "544,288,50" and s["mode"] == mode for s in states)
                assert all(s.get("hr", "0") == "0" for s in states)
                if mode != "0":
                    assert all(int(s["hb"]) > 0 for s in states)
                timing_keys = ("view", "sv", "prep", "grad", "rows", "load", "pack", "io", "hv", "hx")
                row["mode"] = int(mode)
                row["median_profile_us"] = {
                    key: statistics.median(int(s[key]) for s in states if int(s["pn"]) > 0)
                    for key in timing_keys}
                row["timing_scope"] = "sv/prep/grad/rows/load/pack are 1/16 sampled frames with probe overhead; stages are nested. view/io/hv/hx use all completed frames."
            else:
                assert all(s["mode"] == "4" and s["x"] == "-31243" and s["y"] == "7160" and
                           s["z"] == "-158228" and s["angle"] == "422" for s in states)
                if directory.endswith("native"):
                    assert all(s["hr"] == "0" for s in states)
                    before, after = window["native_before"], window["native_after"]
                    assert after["hb"] > before["hb"] and after["hr"] == before["hr"] == 0
                    row["native_successful_blits"] = after["hb"] - before["hb"]
                    row["native_execute_delta_us"] = after["hx"] - before["hx"]
            row.update(unique_fresh_game_records=len(unique), device_span_ms=stamps[-1] - stamps[0],
                       minimum_state_records=minimum_records,
                       maximum_state_record_gap_ms=max(b - a for a, b in zip(stamps, stamps[1:])),
                       replayed_or_delayed_records_excluded=excluded, states=unique,
                       actual_state_gate_passed=True)
        else:
            row["actual_state_gate"] = "C&C has no machine-readable level flag; both screenshots were manually reviewed."
        row["passed"] = True
        result.append(row)
    return result


result = {
    name: audit(name, game, count) for name, game, count in (
        ("cnc-v9-device", "cnc", 1),
        ("duke-v9-baseline", "duke", 1),
        ("duke-v9-native", "duke", 1),
        ("quake-probe-baseline", "quake", 1),
        ("quake-probe-device", "quake", 4),
    )
}
(ROOT / "window-audit.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({name: [{"label": row["label"], "passed": row["passed"],
                          "fresh_records": row.get("unique_fresh_game_records")}
                         for row in rows] for name, rows in result.items()}, indent=2))
