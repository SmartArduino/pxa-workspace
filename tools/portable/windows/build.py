#!/usr/bin/env python3
"""Build the native Windows x64 PXA development tools from a MinGW environment."""
from __future__ import annotations

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

WORKSPACE = Path(__file__).resolve().parents[3]
SYSTEM = WORKSPACE / 'deps/pxa-system'
MANIFEST = Path(__file__).resolve().parent
TARGETS = ('pxsys_desktop_simulator', 'pxsys_product_simulator', 'pxsys_package_installer', 'pxa_windows_fs_test')


def run(args, *, env=None, capture=False):
    result = subprocess.run([str(a) for a in args], env=env, check=True, text=True, encoding='utf-8',
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wasi-sdk', type=Path, help='Windows WASI SDK 34 root')
    parser.add_argument('--vcpkg-root', type=Path, default=os.environ.get('VCPKG_ROOT'))
    parser.add_argument('--prefix', type=Path, help='Use an already reviewed native dependency prefix instead of vcpkg')
    parser.add_argument('--output', type=Path, default=WORKSPACE / 'local/artifacts/windows-native' / datetime.now().strftime('%Y%m%d-%H%M%S'))
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument('--simulator-only', action='store_true', help='Skip the lengthy LLVM/wamrc build')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Run this entry point in native Windows with MinGW-w64, Python, Git, CMake and Ninja on PATH')
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be in 1..64')
    if args.wasi_sdk:
        args.wasi_sdk = args.wasi_sdk.resolve()
        if not (args.wasi_sdk / 'bin/clang++.exe').is_file() or not (args.wasi_sdk / 'VERSION').is_file():
            parser.error('--wasi-sdk must contain bin/clang++.exe and VERSION')
        if (args.wasi_sdk / 'VERSION').read_text(encoding='utf-8').strip().split('.')[0] != '34':
            parser.error('This recipe requires WASI SDK 34')
    for name in ('gcc', 'g++', 'git', 'cmake', 'ninja'):
        if not shutil.which(name):
            parser.error(f'Missing {name}; use a native MinGW-w64 compiler environment')
    if not (shutil.which('pkg-config') or shutil.which('pkgconf')):
        parser.error('Missing native pkg-config/pkgconf')
    triple = run(['gcc', '-dumpmachine'], capture=True).lower()
    if 'mingw' not in triple or not triple.startswith('x86_64-'):
        parser.error('gcc must target Windows x64, rather than x86 or an MSYS/Cygwin POSIX runtime')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    options = []
    if args.prefix:
        prefix = args.prefix.resolve()
        options += ['-DCMAKE_PREFIX_PATH=' + prefix.as_posix()]
    else:
        baseline = json.loads((MANIFEST / 'vcpkg.json').read_text())['builtin-baseline']
        vcpkg = args.vcpkg_root.resolve() if args.vcpkg_root else output / 'vcpkg'
        if not vcpkg.exists():
            run(['git', 'init', vcpkg])
            run(['git', '-C', vcpkg, 'config', 'core.longpaths', 'true'])
            run(['git', '-C', vcpkg, 'remote', 'add', 'origin', 'https://github.com/microsoft/vcpkg.git'])
            # Versioned ports need the baseline's Git tree, not just the HEAD files.
            run(['git', '-C', vcpkg, 'fetch', 'origin', baseline])
            run(['git', '-C', vcpkg, 'checkout', '--detach', 'FETCH_HEAD'])
        actual = run(['git', '-C', vcpkg, 'rev-parse', 'HEAD'], capture=True)
        if actual != baseline:
            parser.error(f'vcpkg checkout must be {baseline}; existing checkouts are never reset automatically')
        if not (vcpkg / 'vcpkg.exe').is_file():
            run([vcpkg / 'bootstrap-vcpkg.bat', '-disableMetrics'])
        installed = output / 'vcpkg_installed'
        prefix = installed / 'x64-mingw-dynamic'
        options += ['-DCMAKE_TOOLCHAIN_FILE=' + (vcpkg / 'scripts/buildsystems/vcpkg.cmake').as_posix(),
                    '-DVCPKG_TARGET_TRIPLET=x64-mingw-dynamic',
                    '-DVCPKG_MANIFEST_DIR=' + MANIFEST.as_posix(),
                    '-DVCPKG_INSTALLED_DIR=' + installed.as_posix()]
    env = os.environ.copy()
    env['PYTHONUTF8'] = '1'
    env['PXA_WAMR_CC'] = shutil.which('gcc')
    env['PXA_WAMR_CXX'] = shutil.which('g++')
    env['PATH'] = str(prefix / 'bin') + os.pathsep + str(prefix / 'tools/openssl') + os.pathsep + env['PATH']
    env['PKG_CONFIG_PATH'] = str(prefix / 'lib/pkgconfig')
    build = output / 'simulator'
    run(['cmake', '-S', SYSTEM, '-B', build, '-G', 'Ninja', *options,
         '-DCMAKE_C_COMPILER=' + shutil.which('gcc'),
         '-DCMAKE_CXX_COMPILER=' + shutil.which('g++'),
         '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
         '-DPXSYS_BUILD_TESTS=OFF', '-DBUILD_TESTING=OFF', '-DPXSYS_ENABLE_INSTALL=OFF',
         '-DPXSYS_WARNINGS_AS_ERRORS=OFF'], env=env)
    run(['cmake', '--build', build, '--target', *TARGETS, '--parallel', args.jobs], env=env)
    run([build / 'libpxa/pxa_windows_fs_test.exe'], env=env)
    smoke = dict(env, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    run([build / 'simulator/desktop/pxsys_desktop_simulator.exe', '--smoke-test'], env=smoke)
    run([build / 'simulator/desktop/pxsys_desktop_simulator.exe', '--self-test'], env=smoke)
    wamrc = None
    if not args.simulator_only:
        wamrc = run([sys.executable, SYSTEM / 'tools/package/build_wamrc.py',
                     '--cache', output / 'compiler', '--jobs', args.jobs], env=env, capture=True)
        run([wamrc, '--version'], env=env)
    # This environment file uses only native executable paths. No symlinks,
    # administrator privileges, container engine or Bash launcher are needed.
    values = {'PYTHON': sys.executable,
              'PXA_SIMULATOR_DIR': str(build / 'simulator/desktop'),
              'PXA_SIMULATOR_FONT': str(WORKSPACE / 'factory/base/system/fonts/noto_sans_cjk_common.ttf')}
    if wamrc:
        values['WAMRC'] = wamrc
    if args.wasi_sdk:
        values['WASI_SDK_DIR'] = str(args.wasi_sdk)
    escaped_prefix = str(prefix).replace('%', '%%')
    lines = ['@echo off', 'chcp 65001 >nul', *[f'set "{k}={v.replace("%", "%%")}"' for k, v in values.items()],
             f'set "PATH={escaped_prefix}\\bin;{escaped_prefix}\\tools\\openssl;%PATH%"']
    (output / 'env.cmd').write_text('\r\n'.join(lines) + '\r\n', encoding='utf-8')
    if wamrc and args.wasi_sdk:
        sample = output / 'smoke/hello'
        check = dict(smoke, **values)
        check['PYTHONPATH'] = str(SYSTEM / 'tools') + os.pathsep + env.get('PYTHONPATH', '')
        # This build fixture has a separate signing identity and never touches
        # a developer's normal identity or a physical serial device.
        check['XDG_DATA_HOME'] = str(output / 'smoke/data')
        check.pop('PXA_SIGNING_KEY', None)
        if not (sample / 'package.json').is_file():
            run([sys.executable, '-m', 'pxa_tools', 'init', sample, '--id', 'pxa-windows-smoke'], env=check)
        run([sys.executable, '-m', 'pxa_tools', 'build', sample, '--offline', '--update-lock',
             '--aot-only', '--target', 'windows-simulator,esp32s3,esp32s31,esp32p4',
             '--output', output / 'smoke/universal', '--jobs', args.jobs], env=check)
        run([sys.executable, '-m', 'pxa_tools', 'run', sample, '--offline', '--duration-ms', '1500',
             '--store-screenshots', '--screenshot', output / 'smoke/windows-hello.png',
             '--output', output / 'smoke/native'], env=check)
    print(f'Windows build: {output}\nIn cmd.exe: call "{output / "env.cmd"}"')
    print(f'CLI: "{SYSTEM / "bin/pxa.cmd"}" --help')


if __name__ == '__main__':
    main()
