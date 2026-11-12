#!/usr/bin/env python3
"""Run the actual ESP worker implementation against threaded RTOS doubles."""
import datetime
import os
from pathlib import Path
import subprocess

tests = Path(__file__).resolve().parent
project = tests.parents[3]
component = tests.parent
output = Path(os.environ.get("PXA_TEST_ARTIFACT_DIR", str(
    project / "local/artifacts/raster-workers" /
    datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))))
output.mkdir(parents=True, exist_ok=True)
source = (component / "src/services/pxa_esp_surface.c").read_text()
start = source.index('#include "pxa_raster_handoff.h"')
end = source.index("\n#endif\n\n#define PXA_ESP_RASTER_BACKGROUND_BANDS", start)
implementation = output / "raster_workers.inc"
implementation.write_text(source[start:end] + "\n")
binary = output / "raster-workers-test"
command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
           "-Wpedantic", "-Werror", "-g", "-pthread",
           "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           f'-DPXA_RASTER_WORKER_IMPLEMENTATION="{implementation.resolve()}"',
           "-I" + str(component / "src/services"),
           "-I" + str(project / "deps/pxa-system/libpxa/include"),
           str(tests / "pxa_raster_workers_test.c"), "-o", str(binary)]
subprocess.run(command, check=True)
subprocess.run([str(binary.resolve())], check=True, timeout=30)
