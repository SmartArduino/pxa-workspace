#!/usr/bin/env python3
"""Read only the clock checkpoint into the report, using ESP-IDF's NVS parser.

The raw partition is kept in a private temporary directory and removed on exit.
Entering/leaving ROM resets the chip without running application shutdown hooks.
Run after run_device_test.py has closed its serial connection.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", required=True)
parser.add_argument("--idf-root", required=True, type=Path)
parser.add_argument("--idf-python", required=True)
parser.add_argument("--output", required=True, type=Path)
args = parser.parse_args()
sys.path.insert(0, str(args.idf_root / "components/nvs_flash/nvs_partition_tool"))
from nvs_parser import NVS_Partition

with tempfile.TemporaryDirectory(prefix="pxa-clock-checkpoint-") as temporary:
    raw = Path(temporary) / "nvs.bin"
    subprocess.run([
        args.idf_python, "-m", "esptool", "--chip", "esp32s31", "--port", args.port,
        "--baud", "2000000", "--before", "default-reset", "--after", "hard-reset",
        "--no-stub", "read-flash", "0x9000", "0x6000", str(raw),
    ], check=True)
    partition = NVS_Partition("nvs", bytearray(raw.read_bytes()))

pages = [page for page in partition.pages if page.header["status"] in ("Active", "Full")]
namespace_ids = {
    entry.data["value"] for page in pages for entry in page.entries
    if entry.state == "Written" and entry.metadata["namespace"] == 0
    and entry.key == "pxa_clock"
}
matches = [
    (page, entry) for page in pages for entry in page.entries
    if entry.state == "Written" and entry.metadata["namespace"] in namespace_ids
    and entry.key == "utc" and entry.metadata["type"] == "int64_t"
]
assert matches, "No committed pxa_clock/utc checkpoint"
page, entry = max(matches, key=lambda item: (item[0].header["page_index"], item[1].index))
assert entry.metadata["crc"]["original"] == entry.metadata["crc"]["computed"]
assert page.header["crc"]["original"] == page.header["crc"]["computed"]
seconds = entry.data["value"]
result = {
    "namespace": "pxa_clock", "key": "utc", "utc_seconds": seconds,
    "utc_iso": datetime.fromtimestamp(seconds, timezone.utc).isoformat(),
    "entry_crc_valid": True, "page_header_crc_valid": True,
    "reset": "ROM read then hard reset; application shutdown handler is not run",
}
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
