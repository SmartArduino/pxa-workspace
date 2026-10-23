import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BoardDependencySwitch(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        (self.root / 'tools').mkdir()
        shutil.copyfile(Path(__file__).with_name('firmware.sh'), self.root / 'tools/firmware.sh')
        for board in ('a', 'b'):
            p = self.root / 'firmware/boards' / board
            p.mkdir(parents=True)
            (p / 'board.cmake').touch()
        fake = self.root / 'idf.py'
        fake.write_text('#!/usr/bin/env python3\nimport os,json,sys\n'
                        'with open(os.environ["IDF_TEST_LOG"], "a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n'
                        'if "reconfigure" in sys.argv and os.environ.get("FAIL_CONFIGURE"): sys.exit(7)\n')
        fake.chmod(0o755)
        self.log = self.root / 'calls.jsonl'
        self.env = dict(os.environ, IDF_PY=str(fake), IDF_TEST_LOG=str(self.log))

    def run_board(self, board, action='build', fail=False):
        env = dict(self.env)
        if fail:
            env['FAIL_CONFIGURE'] = '1'
        return subprocess.run(['bash', str(self.root / 'tools/firmware.sh'), board, action], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode

    def calls(self):
        return [json.loads(line)[-1] for line in self.log.read_text().splitlines()]

    def test_switch_back_refreshes_graph_but_same_board_stays_fast(self):
        for board in ('a', 'a', 'b', 'a'):
            self.assertEqual(self.run_board(board), 0)
        self.assertEqual(self.calls(), ['reconfigure', 'build', 'build', 'reconfigure', 'build', 'reconfigure', 'build'])

    def test_failed_configuration_does_not_mark_graph_ready(self):
        self.assertEqual(self.run_board('a'), 0)
        self.assertEqual(self.run_board('b', fail=True), 7)
        self.assertEqual((self.root / '.pxa/firmware-components-board').read_text().strip(), 'a')
        self.assertEqual(self.run_board('b'), 0)
        self.assertEqual(self.calls()[-3:], ['reconfigure', 'reconfigure', 'build'])

    def test_monitor_does_not_configure_or_change_graph(self):
        self.assertEqual(self.run_board('a', 'monitor'), 0)
        self.assertEqual(self.calls(), ['monitor'])
        self.assertFalse((self.root / '.pxa/firmware-components-board').exists())

    def test_cmake_definitions_are_global_for_configure_and_build(self):
        result = subprocess.run(
            ['bash', str(self.root / 'tools/firmware.sh'), 'a', 'build',
             '-DPXA_KORVO_LVGL_PIE=ON', '-D', 'PXA_KORVO_LVGL_DIAGNOSTICS=ON'],
            env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([command[-1] for command in commands], ['reconfigure', 'build'])
        for command in commands:
            self.assertIn('-DPXA_KORVO_LVGL_PIE=ON', command[:-1])
            index = command.index('-D')
            self.assertEqual(command[index + 1], 'PXA_KORVO_LVGL_DIAGNOSTICS=ON')


if __name__ == '__main__':
    unittest.main()
