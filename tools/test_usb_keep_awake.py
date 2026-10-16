import fcntl
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import usb_keep_awake as usb


class GuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.device = self.root/'sys/device'
        self.device.mkdir(parents=True)
        for name, value in {'idVendor': '303a', 'idProduct': '0020',
                            'busnum': '1', 'devnum': '24'}.items():
            (self.device/name).write_text(value)
        self.usbfs = self.root/'usbfs'
        (self.usbfs/'001').mkdir(parents=True)
        (self.usbfs/'001/024').write_text('old device')

    def test_read_only_descriptor_and_cleanup(self):
        guard = usb.UsbKeepAwake(self.device, self.usbfs)
        with guard:
            fd = guard.fd
            self.assertIsNotNone(fd)
            self.assertEqual(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_ACCMODE, os.O_RDONLY)
            self.assertEqual(os.read(fd, 30), b'old device')
        with self.assertRaises(OSError):
            os.fstat(fd)

    def test_reenumeration_uses_new_node_at_same_physical_port(self):
        guard = usb.UsbKeepAwake(self.device, self.usbfs)
        self.addCleanup(guard.close_fd)
        guard.update()
        (self.device/'devnum').write_text('25')
        (self.usbfs/'001/025').write_text('new device')
        guard.update()
        self.assertEqual(guard.token, (1, 25))
        self.assertEqual(os.read(guard.fd, 30), b'new device')

    def test_temporary_missing_node_can_recover(self):
        guard = usb.UsbKeepAwake(self.device, self.usbfs)
        self.addCleanup(guard.close_fd)
        guard.update()
        (self.device/'devnum').write_text('25')
        guard.update()
        self.assertIsNone(guard.fd)
        (self.usbfs/'001/025').touch()
        guard.update()
        self.assertEqual(guard.token, (1, 25))

    def test_does_not_open_other_usb_devices(self):
        (self.device/'idVendor').write_text('10c4')
        guard = usb.UsbKeepAwake(self.device, self.usbfs)
        with mock.patch.object(usb.os, 'open') as open_node:
            guard.update()
        open_node.assert_not_called()

    def test_resolves_symlink_to_usb_parent(self):
        interface = self.device/'interface'
        interface.mkdir()
        tty = self.root/'tty/ttyACM2'
        tty.mkdir(parents=True)
        (tty/'device').symlink_to(interface)
        alias = self.root/'by-id/test'
        alias.parent.mkdir()
        alias.symlink_to(tty)
        self.assertEqual(usb.usb_parent(str(alias), self.root/'tty'), self.device)

    def test_child_failure_releases_guard_and_preserves_exit_code(self):
        guard = usb.UsbKeepAwake(self.device, self.usbfs)
        with mock.patch.object(sys, 'argv', ['usb_keep_awake.py', '--port', '/dev/test', '--',
                                             sys.executable, '-c', 'raise SystemExit(7)']), \
                mock.patch.object(usb, 'usb_parent', return_value=self.device), \
                mock.patch.object(usb, 'UsbKeepAwake', return_value=guard):
            self.assertEqual(usb.main(), 7)
        self.assertIsNone(guard.fd)


class FirmwareRoutingTest(unittest.TestCase):
    def test_guard_precedes_build_lock_and_preserves_arguments(self):
        workspace = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix='usb guard ') as temp:
            root = Path(temp)
            source = workspace
            workspace = root / 'workspace'
            (workspace / 'tools').mkdir(parents=True)
            for name in ('firmware.sh', 'usb_keep_awake.py'):
                shutil.copy2(source / 'tools' / name, workspace / 'tools' / name)
            for name in ('esp-mosaico', 'pai-touch'):
                board_path = workspace / 'firmware/boards' / name
                board_path.mkdir(parents=True)
                (board_path / 'board.cmake').touch()
            record = root/'calls.jsonl'
            for name in ('flock', 'fake-idf'):
                script = root/name
                script.write_text('#!'+sys.executable+'\nimport json,os,sys\n'
                                  'with open(os.environ["PXA_TEST_RECORD"],"a") as f: '
                                  'f.write(json.dumps(dict(tool=os.path.basename(sys.argv[0]), '
                                  'args=sys.argv[1:], guard=os.environ.get("PXA_USB_GUARD_ACTIVE"), '
                                  'baud=os.environ.get("ESPBAUD")))+"\\n")\n')
                script.chmod(0o755)
            previous_board = None
            for board, action, expected_guard in [('esp-mosaico', 'app-flash', '1'),
                                                   ('esp-mosaico', 'build', None),
                                                   ('pai-touch', 'flash', None),
                                                   ('esp-mosaico', 'monitor', None)]:
                with self.subTest(board=board, action=action):
                    record.unlink(missing_ok=True)
                    env = {**os.environ, 'PATH': str(root)+os.pathsep+os.environ['PATH'],
                           'IDF_PY': str(root/'fake-idf'), 'PXA_TEST_RECORD': str(record)}
                    env.pop('PXA_USB_GUARD_ACTIVE', None)
                    env.pop('ESPBAUD', None)
                    literal = '-DNOTE=literal space $()'
                    subprocess.run([str(workspace/'tools/firmware.sh'), board, action,
                                    '--port', '/dev/pxa-test-no-device', '--baud', '921600', literal],
                                   env=env, check=True)
                    calls = [json.loads(v) for v in record.read_text().splitlines()]
                    expected_tools = ['fake-idf']
                    if action != 'monitor':
                        expected_tools = ['flock'] + ['fake-idf'] * (2 if previous_board != board else 1)
                        previous_board = board
                    self.assertEqual([v['tool'] for v in calls], expected_tools)
                    self.assertTrue(all(v['guard'] == expected_guard for v in calls))
                    args = calls[-1]['args']
                    self.assertEqual(args[-3:], ['921600', action, literal])
                    self.assertEqual(args[args.index('--port')+1], '/dev/pxa-test-no-device')
                    if board == 'esp-mosaico' and action == 'app-flash':
                        self.assertEqual(calls[-1]['baud'], '2000000')


if __name__ == '__main__':
    unittest.main()
