#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate the reviewed LittleFS space-stat adapter without editing its SDK."""
from pathlib import Path
import argparse
import hashlib

REVIEWED = {
    "c64f9c206cc7de1df3c8ad62f9c9dedf0da61edb033f916e4d26d268709d4926",
    # Same 1.22.3 driver with the existing 64-bit size saturation correction.
    "f8ca4502baacb8c57c27a261781e5b7328b60ef934309f5608a1bd81e3456750",
}
SIGNATURE = "void get_total_and_used_bytes(esp_littlefs_t *efs, size_t *total_bytes, size_t *used_bytes) {"
REPLACEMENT = '''#include "pxa_littlefs_usage.h"

void get_total_and_used_bytes(esp_littlefs_t *efs, size_t *total_bytes, size_t *used_bytes) {
    sem_take(efs);
    const uint64_t total = (uint64_t)efs->cfg.block_size * efs->fs->block_count;
    const size_t bounded_total = total > SIZE_MAX ? SIZE_MAX : (size_t)total;
    if (total_bytes) *total_bytes = bounded_total;
    int result = 0;
    if (used_bytes) {
        lfs_size_t blocks = 0;
        result = pxa_littlefs_usage_count(efs->fs, esp_littlefs_calloc, free, &blocks);
        const uint64_t used = (uint64_t)efs->cfg.block_size * blocks;
        /* Never advertise free space after allocation or traversal failure. */
        *used_bytes = result != 0 ? bounded_total :
            (used > SIZE_MAX ? SIZE_MAX : (size_t)used);
    }
    sem_give(efs);
    if (result != 0) ESP_LOGW(ESP_LITTLEFS_TAG, "Space usage traversal failed (%d)", result);
}'''


def prepare(source: Path, output: Path) -> Path:
    data = source.read_bytes()
    if hashlib.sha256(data).hexdigest() not in REVIEWED:
        raise ValueError("LittleFS driver differs from the reviewed 1.22.3 sources")
    if source.resolve() == output.resolve():
        raise ValueError("The installed LittleFS driver must remain unchanged")
    text = data.decode()
    if text.count(SIGNATURE) != 1:
        raise ValueError("LittleFS usage function changed")
    start = text.index(SIGNATURE)
    end = start + len(SIGNATURE)
    depth = 1
    while depth:
        if text[end] == "{": depth += 1
        if text[end] == "}": depth -= 1
        end += 1
    generated = (text[:start] + REPLACEMENT + text[end:]).encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != generated:
        output.write_bytes(generated)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        print(prepare(args.source, args.output))
    except ValueError as error:
        parser.exit(1, str(error) + "\n")
