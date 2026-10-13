import importlib.util
import os
from pathlib import Path
import sys
import unittest

spec = importlib.util.spec_from_file_location('pxa_container', Path(__file__).with_name('pxa_container.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ContainerArguments(unittest.TestCase):
    def test_literal_paths_and_command(self):
        args = module.invocation(Path('/a workspace'), Path('/state'), 'docker', module.DEFAULT_IMAGE,
                                 ['build', 'app', '--output', 'an output'])
        self.assertIn('type=bind,source=/a workspace,target=/workspace', args)
        self.assertEqual(args[-5:], [module.DEFAULT_IMAGE, 'build', 'app', '--output', 'an output'])
        self.assertIn('linux/amd64', args)

    def test_usb_stays_on_host(self):
        for command in ([], ['run', 'app', '--device', 'COM3'], ['run', 'app', '--device=/dev/ttyACM0']):
            with self.assertRaises(ValueError):
                module.invocation(Path('/work'), Path('/state'), 'docker', module.DEFAULT_IMAGE, command)

    def test_mount_parser_ambiguity_rejected(self):
        with self.assertRaises(ValueError):
            module.invocation(Path('/a,b'), Path('/state'), 'docker', module.DEFAULT_IMAGE, ['--version'])


if __name__ == '__main__':
    unittest.main()
