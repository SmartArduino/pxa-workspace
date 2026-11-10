#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the actual resource-placement function against volume fixtures."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "firmware/components/pxa_integration/tests"))
from test_startup import function

component = ROOT / "firmware/components/pxa"
source = (component / "src/package/pxa_esp_package_store.c").read_text()
body = function(source, "static __attribute__((noinline)) pxa_status_t bind_prepared_media(")
with tempfile.TemporaryDirectory(prefix="pxa-resource-placement-") as temporary:
    directory = Path(temporary)
    fixture = directory / "test.c"
    fixture.write_text((component / "tests/pxa_esp_resource_placement_test.c").read_text().replace("// PRODUCTION", body))
    flags = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-g"]
    if os.environ.get("PXA_TEST_SANITIZERS") == "1":
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    binary = directory / "test"
    subprocess.run(["cc", *flags, "-I" + str(component / "include"),
                    "-I" + str(ROOT / "deps/pxa-system/libpxa/include"),
                    str(fixture), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
