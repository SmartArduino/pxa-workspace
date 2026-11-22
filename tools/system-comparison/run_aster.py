#!/usr/bin/env python3
"""Run the seeded Aster package and collect completion counters over native USB."""
from pathlib import Path
import argparse
import json
import re
import time
import serial
import zlib
import os
from collections import deque
from serial.tools import list_ports

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port",default="/dev/ttyACM1")
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--package",type=Path)
    parser.add_argument("--fill-only",action="store_true",help="Stop after the first two fill phases and capture scoped native counter windows")
    parser.add_argument("--expected-serial",help="Expected native USB serial number")
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    device=next((p for p in list_ports.comports() if p.device==os.path.realpath(args.port)),None)
    usb_serial=device.serial_number if device else None
    if args.expected_serial and (usb_serial or "").lower()!=args.expected_serial.lower():
        raise RuntimeError("Unexpected Aster USB serial")
    port=serial.Serial(None,115200,timeout=.05,exclusive=True)
    port.dtr=True;port.rts=False;port.port=args.port;port.open()
    records=[];buffer=b"";phase=None;done=False;fill_complete=False;phase_started=0.
    counter_requests=deque();boundaries=deque();starts=set()
    with port,(args.output/"serial.log").open("wb") as raw:
        def command(text):port.write((text+"\n").encode());port.flush()
        def counter(target,reason):
            counter_requests.append({"phase":target,"reason":reason,"request_host_time":time.monotonic()})
            command("aster ui display-perf")
        def read():
            nonlocal buffer,phase,done,fill_complete,phase_started
            data=port.read(max(1,port.in_waiting));raw.write(data);raw.flush();buffer+=data;lines=[]
            while b"\n" in buffer:
                line,buffer=buffer.split(b"\n",1)
                text=re.sub(r"\x1b\[[0-9;?]*[A-Za-z]","",line.decode(errors="replace"));lines.append(text)
                for marker,kind in [("BENCH ",None),("DISPLAY ","display"),("MEMORY ","memory")]:
                    if marker not in text:continue
                    record=dict(re.findall(r"(\w+)=([^\s;]+)",text.split(marker,1)[1]))
                    record["host_time"]=time.monotonic()
                    if kind:record["test"]=kind;record["phase"]=phase
                    if kind=="display" and counter_requests:record.update(counter_requests.popleft())
                    records.append(record)
                    if record.get("test")=="frame_begin":
                        current=(int(record["round"]),int(record["mode"]))
                        if args.fill_only and phase is not None and current!=phase:boundaries.append(phase)
                        phase=current;phase_started=time.monotonic();print("Aster phase",phase,flush=True)
                        if phase==(0,2):fill_complete=True
                    if record.get("test")=="frames":
                        current=(int(record["round"]),int(record["mode"]))
                        if current!=phase:phase=current;print("Aster phase",phase,flush=True)
                    if record.get("test")=="cpu" and record.get("sample")=="8":print("Aster CPU",record["kind"],record["us"],"us",flush=True)
                    if record.get("test")=="done":done=True
            return lines
        command("");command("aster app stop")
        end=time.monotonic()+1
        while time.monotonic()<end:read()
        if args.package:
            blob=args.package.read_bytes()
            command(f"ASTERPUT /SD:/aster/install/benchmark.ast {len(blob)} {zlib.crc32(blob):08x} 0")
            position=0;deadline=time.monotonic()+120
            while time.monotonic()<deadline:
                lines=read()
                if any(line.strip()=="OK" for line in lines):break
                if any(line.strip().startswith(("ERR ","RESUME ")) for line in lines):raise RuntimeError("ASTERFS upload failed")
                for line in lines:
                    match=re.fullmatch(r"ACK (\d+)",line.strip())
                    if match:
                        offset=int(match[1])
                        if offset!=position:raise RuntimeError("Unexpected ASTERFS offset")
                        chunk=blob[position:position+512]
                        # IDF's REPL uses a small USB RX ring. Keep each hardware
                        # packet bounded while its consumer drains the ring.
                        for part in range(0,len(chunk),64):
                            port.write(chunk[part:part+64]);port.flush();time.sleep(.002)
                        position+=len(chunk)
            else:raise TimeoutError("ASTERFS upload did not finish")
        command("aster app install /SD:/aster/install/benchmark.ast")
        deadline=time.monotonic()+25;installed=False
        while time.monotonic()<deadline:
            lines=read()
            if any("installed OK" in line for line in lines):installed=True;break
            if any("install failed:" in line for line in lines):break
        if not installed:raise RuntimeError("Aster installation did not succeed; inspect serial.log")
        records.clear();phase=None
        command("aster ui memory");command("aster app run bench.comparison")
        deadline=time.monotonic()+210;next_poll=time.monotonic()+1
        while time.monotonic()<deadline and not done and not(args.fill_only and fill_complete):
            read()
            if args.fill_only:
                while boundaries:counter(boundaries.popleft(),"end")
                if phase in ((0,0),(0,1)) and phase not in starts and time.monotonic()-phase_started>=2:
                    starts.add(phase);counter(phase,"start")
            elif time.monotonic()>=next_poll:
                next_poll=time.monotonic()+1;counter(phase,"poll")
        if args.fill_only:command("aster app stop")
        command("aster ui memory");counter(phase,"final")
        end=time.monotonic()+1
        while time.monotonic()<end:read()
    (args.output/"records.json").write_text(json.dumps(records,indent=2)+"\n")
    complete=done or (args.fill_only and fill_complete)
    (args.output/"metadata.json").write_text(json.dumps({"done":complete,"guest_done":done,"scope":"fill" if args.fill_only else "full","port":args.port,"usb_serial":usb_serial},indent=2)+"\n")
    if not complete:raise RuntimeError("Aster benchmark did not finish the requested scope")

if __name__=="__main__":main()
