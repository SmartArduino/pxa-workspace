#!/usr/bin/env python3
"""Run the website's actual Hello snippets with a released DevKit, offline.

Usage: python3 tools/test-website-tutorials.py --website /path/to/pxa-website
       --devkit /path/to/pxa-devkit-0.7.0-linux-x86_64 --output local/artifacts/tutorial-test/20261031-120000
Only a new output directory is accepted. No physical device or server is modified.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--website', type=Path, required=True)
    parser.add_argument('--devkit', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    site, devkit, output = (p.resolve() for p in (args.website, args.devkit, args.output))
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, XDG_DATA_HOME=str(output / 'user-data'), XDG_CACHE_HOME=str(output / 'cache'),
               SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', PYTHONDONTWRITEBYTECODE='1', PYTHONUNBUFFERED='1')
    cli = str(devkit / 'bin/pxa')
    passed = []
    timings = {}

    def run(name, arguments, *, expected=0):
        started = time.monotonic()
        result = subprocess.run([cli, *arguments], cwd=output, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        (output / (name + '.log')).write_text(result.stdout)
        if result.returncode != expected:
            raise RuntimeError(f'{name}: exit {result.returncode}\n{result.stdout[-4000:]}')
        passed.append(name)
        timings[name] = round(time.monotonic() - started, 3)
        print('PASS:', name, flush=True)
        return result.stdout

    def until(predicate, description, timeout=180):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.25)
        raise RuntimeError('Timed out: ' + description)

    # Every shell block is syntax-checked; illustrative hardware commands are not executed.
    shell_count = 0
    for page in (site / 'content').glob('*.md'):
        for language, block in re.findall(r'```([^\n]+)\n(.*?)```', page.read_text(), re.S):
            if language in ('sh', 'bash'):
                subprocess.run(['bash', '-n'], input=block, text=True, check=True)
                shell_count += 1
            elif language == 'toml':
                import tomllib
                tomllib.loads(block)
            elif language == 'json':
                json.loads(block)
    run('version', ['--version'])
    run('init', ['init', 'hello', '--id', 'pxa-hello'])
    # Ensure a repeated init cannot destroy the user's source.
    run('init-existing-refused', ['init', 'hello'], expected=1)
    tutorial = (site / 'content/first-app.md').read_text()
    for language, filename in [('cpp', 'main.cpp'), ('cmake', 'CMakeLists.txt')]:
        snippet = re.search(r'```' + language + r'\n(.*?)```', tutorial, re.S).group(1)
        (output / 'hello' / filename).write_text(snippet)
    run('build-universal', ['build', 'hello', '--target', 'esp32s3,esp32s31,simulator', '--offline'])
    before = (output / 'hello/dist/pxa-hello.pxa').read_bytes()
    run('build-unchanged', ['build', 'hello', '--target', 'esp32s3,esp32s31,simulator', '--offline'])
    if (output / 'hello/dist/pxa-hello.pxa').read_bytes() != before:
        raise RuntimeError('An unchanged build must reuse the signed package')
    package = 'hello/dist/pxa-hello.pxa'
    run('publish-check', ['publish', package, '--check-only'])
    for target in ['esp32s3', 'esp32s31']:
        run('variant-' + target, ['build', 'hello', '--target', target, '--aot-only', '--offline',
                                 '--output', 'hello/dist/pxa-hello-' + target + '.pxa'])
    for profile in ['pai-touch', 'sensecap-watcher', 'esp-mosaico', 'esp32s31-korvo-1', 'generic']:
        run('profile-' + profile, ['run', 'hello', '--offline', '--profile', profile,
                                  '--duration-ms', '1800', '--screenshot', str(output / (profile + '.png'))])

    source = output / 'hello/main.cpp'
    original = source.read_text()
    log_path = output / 'watch.log'
    with log_path.open('w') as log:
        process = subprocess.Popen([cli, 'run', 'hello', '--debug', '--watch', '--offline'], cwd=output,
                                   env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            match = until(lambda: re.search(r'PXADB: pxa device info --simulator (\S+)', log_path.read_text()),
                          'debug selector')
            selector = match.group(1)
            def capture(name):
                try:
                    result = subprocess.run([cli, 'device', 'screenshot', str(output / (name + '.png')),
                                             '--simulator', selector], cwd=output, env=env,
                                            capture_output=True, timeout=30)
                except subprocess.TimeoutExpired:
                    # The bridge can be announced before its first frame is
                    # ready, especially while another target is compiling.
                    return False
                return result.returncode == 0
            until(lambda: capture('debug-before'), 'first displayed frame')
            run('debug-info', ['device', 'info', '--simulator', selector])
            run('debug-click', ['device', 'input', 'tap', '148', '140', '--simulator', selector])
            until(lambda: capture('debug-after-click'), 'frame after tap')
            source.write_text(original + '\nthis is an intentional syntax error\n')
            until(lambda: 'Update failed; previous application retained' in log_path.read_text(), 'failed build')
            until(lambda: capture('debug-after-error'), 'previous Guest kept after build failure')
            source.write_text(original.replace('Hello PXA', 'Hello from my app'))
            until(lambda: len(re.findall('Built .*pxa-hello.pxa', log_path.read_text())) >= 2, 'successful rebuild')
            until(lambda: capture('debug-after-save'), 'new Guest after save')
            passed.append('watch-debug-click-error-recovery')
            print('PASS: watch/debug; inspect debug-*.png for count and title changes', flush=True)
        finally:
            source.write_text(original)
            os.killpg(process.pid, signal.SIGINT)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
    (output / 'validation.json').write_text(json.dumps({'passed': passed, 'shell_blocks': shell_count,
        'website': str(site), 'devkit': str(devkit), 'device_tests': False,
        'screenshots_require_visual_review': True, 'seconds': timings}, indent=2) + '\n')


if __name__ == '__main__':
    main()
