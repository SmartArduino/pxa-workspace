#!/usr/bin/env python3
"""Run production legacy-update control flow against real signed transactions."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
SDK = ROOT / "deps/pxa-system"
sys.path.insert(0, str(ROOT / "firmware/components/pxa_integration/tests"))
from test_startup import function

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--libpxa", required=True)
    args = parser.parse_args()
    component = ROOT / "firmware/components/pxa"
    source = (component / "src/package/pxa_esp_package_store.c").read_text()
    bodies = "\n".join(function(source, signature) for signature in (
        "static __attribute__((noinline)) pxa_status_t snapshot_legacy_update(",
        "static pxa_status_t legacy_update_space(",
        "static pxa_status_t legacy_update_incoming(",
        "static __attribute__((noinline)) pxa_status_t commit_legacy_update(",
    ))
    with tempfile.TemporaryDirectory(prefix="pxa-legacy-update-") as temporary:
        temp = Path(temporary)
        fixture = temp / "fixture.c"
        fixture.write_text((component / "tests/pxa_esp_legacy_update_test.c").read_text().replace("// PRODUCTION", bodies))
        flags = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-g"]
        if os.environ.get("PXA_TEST_SANITIZERS") == "1":
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        includes = [component / "src/package", component / "src/services", component / "include", SDK / "libpxa/include", SDK / "libpxa/adapters/include"]
        subprocess.run(["cc", *flags, "-Wno-error", "-c", str(SDK / "libpxa/adapters/image/vendor/miniz.c"), "-o", str(temp / "miniz.o")], check=True)
        sources = ["posix/pxa_posix_installer.c", "posix/pxa_posix_fs.c", "openssl/pxa_openssl.c"]
        binary = temp / "test"
        subprocess.run(["cc", *flags, *("-I" + str(p) for p in includes), str(fixture),
                        *(str(SDK / "libpxa/adapters" / p) for p in sources), str(temp / "miniz.o"),
                        args.libpxa, "-lcrypto", "-llz4", "-lm", "-o", str(binary)], check=True)
        subprocess.run([sys.executable, str(component / "tests/test_installations.py"), str(binary), str(SDK)], check=True)

if __name__ == "__main__":
    main()
