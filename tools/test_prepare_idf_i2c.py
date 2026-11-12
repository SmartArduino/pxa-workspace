#!/usr/bin/env python3
import datetime
import os
from pathlib import Path
import subprocess
import unittest
import prepare_idf_i2c as fix

ROOT = Path(__file__).resolve().parents[1]


class ProbeFixTests(unittest.TestCase):
    def test_receive_after_failed_read(self):
        output = Path(os.environ.get("PXA_TEST_ARTIFACT_DIR", str(
            ROOT / "local/artifacts/idf-i2c" /
            datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))))
        output.mkdir(parents=True, exist_ok=True)
        source = (ROOT / "tools/tests/fixtures/esp_idf_i2c_probe.c").read_text()
        for name, text, success in [("original", source, False),
                                    ("fixed", fix.patch_source(source), True)]:
            fixture = output / (name + ".inc")
            fixture.write_text(text)
            binary = output / name
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall",
                "-Wextra", "-Werror", "-Wno-unused-function",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
                f'-DPXA_I2C_PROBE_FIXTURE="{fixture.resolve()}"',
                str(ROOT / "tools/tests/idf_i2c_probe_test.c"),
                "-o", str(binary)], check=True)
            result = subprocess.run([str(binary.resolve())], capture_output=True,
                                    text=True, timeout=10)
            (output / (name + ".log")).write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode == 0, success, result.stderr)
            if not success:
                self.assertIn("contains_read", result.stderr)

    def test_source_review_and_no_external_mutation(self):
        source = ROOT / "tools/tests/fixtures/esp_idf_i2c_probe.c"
        original = source.read_bytes()
        with self.assertRaisesRegex(ValueError, "Unreviewed"):
            fix.prepare(source, Path("unused-overlay.c"))
        self.assertEqual(source.read_bytes(), original)

    def test_changed_source_is_rejected(self):
        source = (ROOT / "tools/tests/fixtures/esp_idf_i2c_probe.c").read_text()
        with self.assertRaises(ValueError):
            fix.patch_source(source.replace(".ops = i2c_ops,", ".ops = changed,"))


if __name__ == "__main__":
    unittest.main()
