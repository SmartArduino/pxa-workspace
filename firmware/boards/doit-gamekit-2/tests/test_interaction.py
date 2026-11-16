#!/usr/bin/env python3
"""Validate the product descriptor through the real serializer and matcher."""
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
BOARD = ROOT / 'firmware/boards/doit-gamekit-2'
sys.path.insert(0, str(ROOT / 'tools/pxadb'))
from interaction_contract import evaluate, validate_device


def main():
    output = Path(os.environ.get('PXA_TEST_OUTPUT', str(
        ROOT / 'local/artifacts/gamekit2-interaction' / datetime.now().strftime('%Y%m%d-%H%M%S'))))
    output.mkdir(parents=True, exist_ok=True)
    flags = ['-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-I' + str(ROOT / 'firmware/components/pxa/include')]
    obj = output / 'interaction.o'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', *flags, '-c', str(
        ROOT / 'firmware/components/pxa/src/runtime/pxa_host_interaction.c'), '-o', str(obj)], check=True)
    binary = output / 'interaction'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', *flags,
                    '-I' + str(BOARD / 'include'), str(BOARD / 'tests/interaction_test.cc'),
                    str(obj), '-o', str(binary)], check=True)
    documents = {}
    for scenario, expected in [('two', 2), ('one', 1), ('failed', None), ('hole', None), ('disabled', 0)]:
        raw = subprocess.check_output([str(binary), scenario])
        value = validate_device(json.loads(raw))
        assert value['physical']['max_contacts'] == expected
        assert value['physical']['touch'] is (True if expected else False if expected == 0 else None)
        assert value['physical']['simultaneous'] is None
        assert value['display'] == {'width': 320, 'height': 240, 'shape': 'rectangle'}
        assert 'serial' not in raw.decode() and 'mac' not in raw.decode()
        controller = next(mode for mode in value['modes'] if mode['kind'] == 'controller')
        assert set(controller['actions']) == {'up', 'down', 'left', 'right', 'a', 'b',
                                              'start', 'select', 'x', 'y', 'l', 'r'}
        assert not {'menu', 'return'} & set(controller['actions'])
        home = next(rule for rule in value['reserved'] if rule['action'] == 'system.home')
        assert home['control'] == 'menu' and home['threshold_ms'] == 600
        documents[scenario] = value
        (output / (scenario + '.json')).write_bytes(raw + b'\n')

    def app(kind, actions, **fields):
        return {'schema': 'pxa-app-interaction-1', 'profiles': [
            {'id': 'test', 'kind': kind, 'complete': True, 'requires': actions, **fields}]}

    assert evaluate(app('touch', {'tap': 'tap'}, contacts=2), documents['two'])['status'] == 'compatible'
    for scenario in ['one', 'failed', 'hole', 'disabled']:
        assert evaluate(app('touch', {'tap': 'tap'}, contacts=2), documents[scenario])['status'] != 'compatible'
    assert evaluate(app('controller', {'a': 'press'}), documents['two'])['status'] == 'compatible'
    assert evaluate(app('controller', {'up': 'press', 'a': 'press'},
                        simultaneous=[['up', 'a']]), documents['two'])['status'] == 'unknown'
    assert evaluate(app('controller', {'a': 'hold'}, holds={'a': 1000}), documents['two'])['status'] == 'unknown'
    assert evaluate(app('controller', {'tap': 'tap'}), documents['two'])['status'] == 'incompatible'
    assert evaluate(app('controller', {'menu': 'press'}), documents['two'])['status'] == 'incompatible'
    assert evaluate(app('focus', {'confirm': 'tap'}), documents['two'])['status'] == 'compatible'
    assert evaluate(app('pointer', {'primary': 'tap'}), documents['two'])['status'] == 'compatible'
    print('PASS: real product serialization, physical route failures, controller/native modes, '
          'system reservations and conservative compatibility admission')


if __name__ == '__main__':
    main()
