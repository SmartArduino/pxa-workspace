import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'korvo_measure', Path(__file__).with_name('measure_lvgl.py'))
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


class FakeClient:
    def __init__(self, serial):
        self.serial = serial
        self.commands = []

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def hello(self):
        return f'target=esp32s31;serial={self.serial}'

    def request(self, command, timeout=None):
        self.commands.append(command)
        return [measure.pxadb.Frame(0, 'PKG', 'installed-app-v1')]


class MeasurementGuards(unittest.TestCase):
    def run_case(self, client, packages=None):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            firmware = root / 'firmware.bin'
            firmware.write_bytes(b'host test')
            argv = ['measure_lvgl.py', '--expected-serial', 'expected-device',
                    '--page', 'settings', '--firmware', str(firmware),
                    '--output', str(root / 'results')]
            if packages is not None:
                reference = root / 'packages.json'
                reference.write_text(json.dumps(packages))
                argv += ['--packages-reference', str(reference)]
            with mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(measure.pxadb, 'PxaDbClient', return_value=client):
                with self.assertRaises(RuntimeError):
                    measure.main()
            report = json.loads((root / 'results/report.json').read_text())
            self.assertFalse(report['passed'])

    def test_wrong_serial_never_injects_or_runs_cleanup_input(self):
        client = FakeClient('different-device')
        self.run_case(client)
        self.assertEqual(client.commands, [])

    def test_changed_app_list_is_rejected_before_any_input(self):
        client = FakeClient('expected-device')
        self.run_case(client, ['installed-app-v2'])
        self.assertEqual(client.commands, ['PACKAGES'])


if __name__ == '__main__':
    unittest.main()
