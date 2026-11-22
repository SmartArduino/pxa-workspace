#!/usr/bin/env python3
"""Verify checksums and summarize independent completion counts, not submissions."""
from pathlib import Path
import argparse
import json
import statistics

CPU_NAMES=["Integer mix","Float recurrence","128 KiB sequential update","128 KiB pointer chase","64x64 matrix"]
FRAME_NAMES=["Repeated constant fill","Changing-color fill","Integer pixel effect","128 guest rectangles"]

def load(path):
    if not json.loads((path/"metadata.json").read_text())["done"]:raise ValueError(f"Incomplete run: {path}")
    return json.loads((path/"records.json").read_text())

def cpu(records,expected):
    output=[]
    for kind in range(5):
        group=[r for r in records if r.get("test")=="cpu" and int(r["kind"])==kind]
        if len(group)!=9:raise ValueError(f"Expected 9 CPU samples, kind={kind}: {len(group)}")
        if any(int(r["sum"])!=expected[kind] for r in group):raise ValueError(f"Incorrect CPU output: kind={kind}")
        values=[int(r["us"])/1000 for r in group if int(r["sample"])>=2]
        output.append({"name":CPU_NAMES[kind],"samples_ms":values,"median_ms":statistics.median(values),"min_ms":min(values),"max_ms":max(values)})
    return output

def pxa_frames(records):
    output=[]
    for mode in range(4):
        group=[r for r in records if r.get("test")=="display" and r["phase"][1]==mode]
        if len(group)!=3:raise ValueError(f"Expected 3 frame rounds: mode={mode}")
        values=[]
        for r in group:
            fields=dict(x.split("=",1) for x in r["data"][-1].split(";"))
            if not 5000000<=int(fields["window_us"])<=7000000:raise ValueError(f"Invalid measurement window: {fields}")
            values.append(int(fields["completed"])*1e6/int(fields["window_us"]))
        output.append({"name":FRAME_NAMES[mode],"rounds_fps":values,"median_fps":statistics.median(values)})
    return output

def aster_frames(records):
    output=[]
    for mode in range(4):
        values=[];guest=[]
        for round_no in range(3):
            begin=next(r["host_time"] for r in records if r.get("test")=="frame_begin" and int(r["mode"])==mode and int(r["round"])==round_no)
            group=[r for r in records if r.get("test")=="display" and r.get("phase")==[round_no,mode] and begin+2<=r["host_time"]<=begin+7.7]
            if len(group)<3:raise ValueError("Insufficient independent Aster display samples")
            values.append((int(group[-1]["completed"])-int(group[0]["completed"]))/ (group[-1]["host_time"]-group[0]["host_time"]))
            ticks=[r for r in records if r.get("test")=="frames" and int(r["mode"])==mode and int(r["round"])==round_no and 2000000<=int(r["elapsed_us"])<=7500000]
            rate=(int(ticks[-1]["frames"])-int(ticks[0]["frames"]))*1e6/(int(ticks[-1]["elapsed_us"])-int(ticks[0]["elapsed_us"]))
            guest.append(rate)
            if abs(values[-1]-rate)>.5:raise ValueError("Aster guest and hardware completion counters disagree")
        output.append({"name":FRAME_NAMES[mode],"rounds_fps":values,"guest_rounds_fps":guest,"median_fps":statistics.median(values)})
    return output

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pxa",type=Path,required=True);parser.add_argument("--aster",type=Path,required=True)
    parser.add_argument("--pxa-release",type=Path);parser.add_argument("--checksums",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    expected={};pixel=None
    for line in args.checksums.read_text().splitlines():
        fields=line.split()
        if fields[0]=="cpu":expected[int(fields[1])]=int(fields[2])
        else:pixel=int(fields[1])
    pxa=load(args.pxa);aster=load(args.aster)
    for records in [pxa,aster]:
        if not any(r.get("test")=="pixels" and int(r["sum"])==pixel for r in records):raise ValueError("Missing/mismatched pixel checksum")
    result={"checksums_verified":True,"cpu":{"pxa":cpu(pxa,expected),"aster":cpu(aster,expected)},"display":{"pxa_timer":pxa_frames(pxa),"aster":aster_frames(aster)}}
    if args.pxa_release:
        release=load(args.pxa_release);cpu(release,expected)
        result["display"]["pxa_release"]=pxa_frames(release)
    (args.output/"summary.json").write_text(json.dumps(result,indent=2)+"\n")
    lines=["# Pai Touch PXA / Aster measurement", "", "Same shared C/Wasm object; 240 MHz CPU, 80 MHz octal PSRAM, 296x240 RGB565, 80 MHz SPI.","",
      "CPU: median of 7 samples after 2 warmups; lower is better. Aster clock resolution: 1 ms.","",
      "| Workload | PXA ms | Aster ms | PXA/Aster time |","|---|---:|---:|---:|"]
    for p,a in zip(result["cpu"]["pxa"],result["cpu"]["aster"]):lines.append(f'| {p["name"]} | {p["median_ms"]:.3f} | {a["median_ms"]:.3f} | {p["median_ms"]/a["median_ms"]:.3f}x |')
    lines += ["", "LCD-transfer completion FPS: median of 3 rounds, first 2 seconds excluded. Higher is better.","", "| Workload | PXA timer | PXA release-paced | Aster |", "|---|---:|---:|---:|"]
    for i in range(4):
        p=result["display"]["pxa_timer"][i];a=result["display"]["aster"][i]
        fast=result["display"].get("pxa_release")
        f=f'{fast[i]["median_fps"]:.2f}' if fast else "-"
        lines.append(f'| {p["name"]} | {p["median_fps"]:.2f} | {f} | {a["median_fps"]:.2f} |')
    lines += ["", "All 5 CPU checksums and the complete pixel-effect frame match the native reference.","",
      "Limits: two physical boards; no crossover firmware run. The Aster port uses its IDF backend, subset fonts, blocking presenter, and has no JL701 audio/camera/ADC-key integration. PXA retains its full existing product firmware. The wrappers use PXA C++ SDK and Aster C ABI; full binaries and background services differ. Compute times include OS preemption and contain no system calls inside the shared kernels. This is a comparison of these configurations, not an ABI-dispatch attribution, ABI ceiling, or memory-parity claim."]
    (args.output/"REPORT.md").write_text("\n".join(lines)+"\n")
    print("\n".join(lines))

if __name__=="__main__":main()
