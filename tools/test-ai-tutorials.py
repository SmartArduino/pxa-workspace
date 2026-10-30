#!/usr/bin/env python3
"""Exercise the public AI/C/C++ tutorial in an already configured fresh workspace.

Run setup.py first with the desired XDG/PXA_AGENT_HOME, then use the same
environment here. Never flashes devices, authorizes accounts or uploads apps.
Logs/results must use a new directory under local/artifacts/.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--website', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    ws, site, output = (p.resolve() for p in (args.workspace, args.website, args.output))
    projects = [ws / 'apps/tutorial-cpp', ws / 'apps/tutorial-c']
    if any(p.exists() for p in projects):
        parser.error('Use a fresh configured workspace; existing tutorial projects are retained')
    output.mkdir(parents=True, exist_ok=False)
    result = {'passed': False, 'steps': [], 'physical_device_tested': False,
              'published': False, 'visual_review_required': True, 'screenshots': []}
    env = {**os.environ, 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'}
    cli = str(ws / '.pxa/pxa')

    def run(name, arguments, expected=0):
        started = time.monotonic()
        p = subprocess.run([cli, *map(str, arguments)], cwd=ws, env=env,
                           capture_output=True, text=True, timeout=900)
        (output / (name + '.log')).write_text(p.stdout + p.stderr)
        record = {'step': name, 'seconds': round(time.monotonic() - started, 3),
                  'returncode': p.returncode}
        result['steps'].append(record)
        if p.returncode != expected:
            raise RuntimeError(f'{name} failed; inspect {output / (name + ".log")}')
        print('PASS:', name, record['seconds'], flush=True)
        return p.stdout

    def verify(name, arguments):
        value = json.loads(run(name, ['verify', *arguments]))
        assert value['runtime_passed'], value
        result['screenshots'].extend(value['screenshots'])

    try:
        run('doctor', ['doctor'])
        run('init-cpp', ['init', 'apps/tutorial-cpp', '--id', 'pxa-tutorial-cpp'])
        run('init-existing-refused', ['init', 'apps/tutorial-cpp'], expected=1)
        run('context-cpp', ['context', 'apps/tutorial-cpp', '--query', 'Button'])
        text = (site / 'content/ai-tools-reference.md').read_text()
        scenario = json.loads(re.search(r'```json\n(.*?)```', text, re.S).group(1))
        (projects[0] / 'tests').mkdir()
        (projects[0] / 'tests/hello.json').write_text(json.dumps(scenario))
        verify('cpp-interaction', ['apps/tutorial-cpp', '--profiles', 'pai-touch', 'sensecap-watcher',
                                  '--scenario', 'apps/tutorial-cpp/tests/hello.json', '--store-screenshots'])
        run('scope-after-cpp', ['scope', 'check'])
        run('init-c', ['init', 'apps/tutorial-c', '--id', 'pxa-tutorial-c', '--language', 'c'])
        assert (projects[1] / 'main.c').is_file() and not (projects[1] / 'main.cpp').exists()
        run('context-c', ['context', 'apps/tutorial-c', '--query', 'pxa_ui_button', '--language', 'c'])
        run('c-four-targets', ['build', 'apps/tutorial-c', '--target', 'esp32s3,esp32s31,simulator,wasm'])
        scenario = {'pai-touch': [
            {'name': 'confirm', 'action': 'controller', 'buttons': 16, 'ms': 100},
            {'name': 'settle', 'action': 'wait', 'ms': 180},
            {'name': 'count-one', 'action': 'screenshot'}]}
        # Reusable test inputs belong to the selected app, within its edit scope.
        (projects[1] / 'tests').mkdir()
        (projects[1] / 'tests/hello.json').write_text(json.dumps(scenario))
        verify('c-controller', ['apps/tutorial-c', '--profiles', 'pai-touch',
                                '--scenario', 'apps/tutorial-c/tests/hello.json', '--store-screenshots'])
        (projects[1] / 'store.json').write_text(json.dumps({
            'kind': 'app', 'category': 'tools', 'tags': ['计数', '工具'],
            'icon': None, 'cover': None}, ensure_ascii=False))
        value = json.loads(run('store-prepare', ['store', 'prepare', 'apps/tutorial-c']))
        assert set(value['omitted']) == {'icon', 'cover'} and not value['needs_choice']
        assert value['listing']['tags'] == ['计数', '工具']
        run('scope-final', ['scope', 'check'])
        run('scope-end', ['scope', 'end'])
        result['passed'] = True
    finally:
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
