#!/usr/bin/env python3
"""Collect Guest timing and independent LCD-completion counters on one session."""
from pathlib import Path
import argparse
import json
import re
import sys
import time
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tools/pxadb"))
import pxadb

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port",default="/dev/ttyACM0")
    parser.add_argument("--output",type=Path,required=True)
    scope=parser.add_mutually_exclusive_group()
    scope.add_argument("--cpu-only",action="store_true",help="Stop after all five compute workloads")
    scope.add_argument("--fill-only",action="store_true",help="Stop after the first two fill workloads")
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    records=[];phase=None;phase_started=0.;recording=False;done=False;cpu_complete=False;fill_complete=False;accept=False
    with (args.output/"serial.log").open("w") as raw, pxadb.open_client(args.port,15) as client:
        def callback(frame):
            nonlocal phase,phase_started,done,cpu_complete,fill_complete
            raw.write(frame.kind+" "+frame.payload+"\n");raw.flush()
            if not accept:return
            if "BENCH " not in frame.payload:return
            clean=re.sub(r"\x1b\[[0-9;?]*[A-Za-z]","",frame.payload)
            record=dict(re.findall(r"(\w+)=([^\s;]+)",clean.split("BENCH ",1)[1]))
            record["host_time"]=time.monotonic();records.append(record)
            if record.get("test")=="frame_begin":
                phase=(int(record["round"]),int(record["mode"]));phase_started=time.monotonic()
                if phase==(0,2):fill_complete=True
                print("PXA phase",phase,flush=True)
            if record.get("test")=="cpu" and record.get("sample")=="8":print("PXA CPU",record["kind"],record["us"],"us",flush=True)
            if record.get("test")=="cpu" and record.get("kind")=="4" and record.get("sample")=="8":cpu_complete=True
            if record.get("test")=="done":done=True
        client.log_callback=callback
        client.raw_callback=lambda data:raw.write("RAW "+data.decode(errors="replace"))
        metadata={"info":client.device_info,"runtime":pxadb.runtime_profile(client),
                  "detail_info":[f.payload for f in client.request("INFO")],
                  "memory_before":[f.payload for f in client.request("MEMORY")]}
        # A foreground launch cannot dismiss an already-active lock screen.
        client.request("INPUT SWIPE 148 220 148 70 300 12")
        # On an unlocked device the same swipe opens Recents. Dismiss that
        # system overlay before launching, or Surface submissions can succeed
        # while no app frames reach the LCD.
        client.request("INPUT KEY HOME")
        client.request("INPUT SYNC")
        # Input SYNC drains routing, while the native UI animation/lifecycle
        # handoff can still be in flight. Settle before starting the guest.
        time.sleep(.5)
        metadata["home_settle_seconds"]=.5
        client.subscribe_logs()
        accept=True
        client.request("PACKAGE run pxa-system-comparison")
        deadline=time.monotonic()+200;active_phase=None;next_poll=0
        while time.monotonic()<deadline and not done and not(args.cpu_only and cpu_complete) and not(args.fill_only and fill_complete):
            client.pump_logs();now=time.monotonic()
            if recording and phase!=active_phase:
                result=client.request("PERF STOP")
                records.append({"test":"display","phase":active_phase,"host_time":now,"data":[f.payload for f in result]})
                recording=False
            if phase is not None and phase!=active_phase and now-phase_started>=2:
                client.request("PERF START display");recording=True;active_phase=phase
            if now>=next_poll:
                next_poll=now+1
                records.append({"test":"fps","phase":phase,"host_time":now,"data":[f.payload for f in client.request("PERF FPS")]})
            time.sleep(.02)
        if recording:
            records.append({"test":"display","phase":active_phase,"data":[f.payload for f in client.request("PERF STOP")]})
        metadata["done"]=done or (args.cpu_only and cpu_complete) or (args.fill_only and fill_complete)
        metadata["scope"]="cpu" if args.cpu_only else "fill" if args.fill_only else "full"
        metadata["guest_done"]=done
        metadata["memory_after"]=[f.payload for f in client.request("MEMORY")]
        client.request("PACKAGE stop pxa-system-comparison")
    (args.output/"records.json").write_text(json.dumps(records,indent=2)+"\n")
    (args.output/"metadata.json").write_text(json.dumps(metadata,indent=2)+"\n")
    if not metadata["done"]:raise RuntimeError("Benchmark did not complete the requested scope; inspect serial.log")

if __name__=="__main__":main()
