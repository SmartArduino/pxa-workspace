#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a length-prefixed benchmark fixture from raw Annex B H.264."""

import argparse
import json
from pathlib import Path
import struct
import subprocess


def prepare(source: Path, output: Path, first_frame: int, frames: int) -> None:
    probe = subprocess.check_output([
        "ffprobe", "-v", "error", "-f", "h264", "-select_streams", "v:0",
        "-show_streams", "-show_packets",
        "-show_entries", "stream=width,height,profile,level:packet=pos,size,flags",
        "-of", "json", str(source),
    ])
    info = json.loads(probe)
    stream, = info["streams"]
    packets = info["packets"]
    if not 0 <= first_frame < len(packets) or frames <= 0:
        raise ValueError("Set --start-frame within the input and --frames above zero")
    keyframes = [i for i in range(first_frame + 1) if "K" in packets[i]["flags"]]
    if not keyframes:
        raise ValueError("No key packet precedes the requested start frame")
    first_keyframe = keyframes[-1]
    selected = packets[first_keyframe:first_keyframe + frames]
    with source.open("rb") as raw, output.open("wb") as fixture:
        fixture.write(struct.pack("<4sIII", b"H264", stream["width"], stream["height"], len(selected)))
        for packet in selected:
            size = int(packet["size"])
            raw.seek(int(packet["pos"]))
            data = raw.read(size)
            if len(data) != size or not data.startswith((b"\x00\x00\x01", b"\x00\x00\x00\x01")):
                raise ValueError(f"Invalid Annex B packet at byte {packet['pos']}")
            fixture.write(struct.pack("<I", size))
            fixture.write(data)
    print(json.dumps({
        **stream, "start_frame": first_keyframe, "packets": len(selected),
        "fixture_bytes": output.stat().st_size,
    }))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start-frame", type=int, default=0)
    parser.add_argument("--frames", type=int, default=400)
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error("The fixture must have a different path from the source")
    prepare(args.source, args.output, args.start_frame, args.frames)
