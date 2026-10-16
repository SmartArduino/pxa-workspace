import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import dev


class BoardTargetTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="pxa board target ")
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        self.board = "new-board-not-in-any-mapping"
        self.metadata = self.root / "firmware/boards" / self.board / "board.cmake"
        self.metadata.parent.mkdir(parents=True)
        for name in ("app.sh", "board-target.cmake", "pxa/package_app.sh"):
            destination = self.root / "tools" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(dev.ROOT / "tools" / name, destination)
        backend = self.root / "deps/pxa-system/tools/package/package_app.sh"
        backend.parent.mkdir(parents=True)
        backend.write_text("""#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$2" >> "$PXA_TEST_TARGET_LOG"
mkdir -p "$3/artifacts"
touch "$3/artifacts/main.$2.aot"
""")
        backend.chmod(0o755)
        self.target_log = self.root / "targets.log"
        self.source = self.root / "source"
        (self.source / "demo").mkdir(parents=True)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("PXA_", "PXSYS_"))}
        self.env["PXA_TEST_TARGET_LOG"] = str(self.target_log)

    def declare(self, target: str) -> None:
        # Exercise actual CMake evaluation, including includes, variables and
        # status messages, rather than relying on one particular set() syntax.
        self.metadata.write_text('include("${CMAKE_CURRENT_LIST_DIR}/chip.cmake")\n'
                                 'message(STATUS "Reading new board metadata")\n'
                                 'set(PXA_BOARD_TARGET\n  ${chip}\n)\n')
        (self.metadata.parent / "chip.cmake").write_text(f'set(chip "{target}")\n')

    def build(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        self.target_log.unlink(missing_ok=True)
        return subprocess.run(
            ["bash", str(self.root / "tools/app.sh"), "build", "demo",
             "--board", self.board, "--source-root", str(self.source), *arguments],
            env=self.env, capture_output=True, text=True, timeout=15,
        )

    def test_new_board_device_target_comes_from_metadata(self) -> None:
        args = SimpleNamespace(mode="device", app_id="demo", board=self.board)
        developer = dev.Developer(args, mock.Mock(), self.source)
        for target in ("esp32s31", "esp32s3"):
            with self.subTest(target=target):
                self.declare(target)
                with mock.patch.object(dev, "ROOT", self.root), \
                        mock.patch.object(dev, "stream_command") as stream:
                    developer.build()
                command = stream.call_args.args[0]
                self.assertEqual(command[command.index("--target") + 1], target)

    def test_new_board_app_and_package_defaults_follow_metadata(self) -> None:
        for target in ("esp32s31", "esp32s3"):
            with self.subTest(target=target):
                self.declare(target)
                result = self.build()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.target_log.read_text().splitlines(), [target])

    def test_explicit_target_overrides_board_default(self) -> None:
        self.declare("esp32s31")
        result = self.build("--target", "esp32s3")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.target_log.read_text().splitlines(), ["esp32s3"])

    def test_direct_packager_reads_new_board_metadata(self) -> None:
        self.declare("esp32s31")
        result = subprocess.run(
            ["bash", str(self.root / "tools/pxa/package_app.sh"), "demo",
             str(self.root / "output"), self.board],
            env=self.env, capture_output=True, text=True, timeout=15,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.target_log.read_text().splitlines(), ["esp32s31"])

    def test_all_keeps_new_board_as_primary_target(self) -> None:
        for target in ("esp32s31", "esp32s3"):
            with self.subTest(target=target):
                self.declare(target)
                result = self.build("--target", "all")
                self.assertEqual(result.returncode, 0, result.stderr)
                targets = self.target_log.read_text().splitlines()
                self.assertEqual(set(targets), {"esp32s3", "esp32s31", "simulator"})
                self.assertEqual(targets[-1], target)  # Extras build before the primary package.

    def test_missing_board_or_target_fails_without_s3_fallback(self) -> None:
        for contents in (None, 'set(PXA_BOARD_COMPONENT "new-board")\n'):
            with self.subTest(contents=contents):
                if contents is not None:
                    self.metadata.write_text(contents)
                with mock.patch.object(dev, "ROOT", self.root), \
                        self.assertRaises(dev.DevError):
                    dev.board_target(self.board)
                self.assertNotEqual(self.build().returncode, 0)
                self.assertFalse(self.target_log.exists())

    def test_unsupported_native_target_fails_but_simulator_override_works(self) -> None:
        self.declare("esp32p4")
        result = self.build()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported", result.stderr)
        self.assertFalse(self.target_log.exists())
        result = self.build("--target", "simulator")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.target_log.read_text().splitlines(), ["simulator"])


if __name__ == "__main__":
    unittest.main()
