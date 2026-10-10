"""Check board-profile forwarding through the real simulator shell entry point."""

import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parent.parent


class SimulatorArgumentsTest(unittest.TestCase):
    def test_mosaico_display_profile_reaches_both_runners(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "tools").mkdir()
            (root / "simulator/profiles").mkdir(parents=True)
            shutil.copy(ROOT / "tools/simulator.sh", root / "tools/simulator.sh")
            shutil.copy(ROOT / "simulator/profiles/esp-mosaico.toml",
                        root / "simulator/profiles/esp-mosaico.toml")
            pxsys = root / "pxsys"
            (pxsys / "simulator/desktop").mkdir(parents=True)
            (pxsys / "simulator/desktop/CMakeLists.txt").touch()
            package = root / "package"
            package.mkdir()
            key = root / "publisher.der"
            key.touch()
            output = root / "argv.json"
            build = root / "build/simulator/esp-mosaico"
            build.mkdir(parents=True)
            for runner in ("pxsys_desktop_simulator", "pxsys_product_simulator"):
                stub = build / runner
                stub.write_text(
                    "#!/usr/bin/env python3\n"
                    "import json, os, pathlib, sys\n"
                    "pathlib.Path(os.environ['PXA_TEST_ARGV']).write_text(json.dumps(sys.argv))\n"
                )
                stub.chmod(0o755)
            environment = dict(os.environ, PXSYS_ROOT=str(pxsys), CMAKE=shutil.which("true"),
                               PXA_SIMULATOR_AUTOSTART_PXADB="0", PXA_TEST_ARGV=str(output))
            for mode, runner in (("ui", "pxsys_desktop_simulator"),
                                 ("product", "pxsys_product_simulator")):
                with self.subTest(mode=mode):
                    subprocess.run([
                        "bash", str(root / "tools/simulator.sh"), mode, "run",
                        "--profile", "esp-mosaico", "--package", str(package),
                        "--publisher-key", str(key),
                    ], env=environment, check=True, capture_output=True, text=True)
                    args = json.loads(output.read_text())
                    self.assertEqual(pathlib.Path(args[0]).name, runner)
                    for flag, value in (("--width", "480"), ("--height", "480"),
                                        ("--density-dpi", "305"), ("--corner-radius", "58"),
                                        ("--safe-insets", "12,12,12,12"), ("--locale", "zh-CN")):
                        self.assertEqual(args[args.index(flag) + 1], value)


if __name__ == "__main__":
    unittest.main()
