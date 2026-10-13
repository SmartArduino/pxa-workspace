#!/usr/bin/env python3
"""Build the independent DevKit in a pinned Linux container.

This is a maintainer entry point, not an App build dependency. It keeps mutable
WAMR/LLVM/build caches outside the checkout and never embeds a private key.
Publication is separate: validate these assets before creating a release.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess

WORKSPACE = Path(__file__).resolve().parents[1]
SYSTEM = WORKSPACE / 'deps/pxa-system'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=WORKSPACE / 'local/release-build')
    parser.add_argument('--output', type=Path, default=WORKSPACE / 'local/releases')
    parser.add_argument('--signing-key', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument('--engine', default='podman')
    parser.add_argument('--backend', type=Path)
    parser.add_argument('--allow-dirty', action='store_true', help='validation only; not a publishable release')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 64:
        parser.error('--jobs must be in 1..64')
    engine = shutil.which(args.engine)
    if not engine:
        parser.error('Install Podman, or select another compatible container engine')
    key = args.signing_key.expanduser().resolve(strict=True)
    if not args.allow_dirty:
        for checkout in (WORKSPACE, SYSTEM):
            if subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain'], text=True).strip():
                parser.error(f'Official releases require committed sources: {checkout}')
    version = (SYSTEM / 'VERSION').read_text().strip()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    build.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    metadata = ['python3', str(SYSTEM / 'tools/release/metadata.py'), '--workspace', str(WORKSPACE), '--check']
    if args.backend:
        metadata += ['--backend', str(args.backend.resolve())]
    subprocess.run(metadata, check=True)
    image = f'localhost/pxa-devkit-build:{version}'
    subprocess.run([engine, 'build', '-t', image, '-f', str(SYSTEM / 'tools/release/Dockerfile'),
                    str(SYSTEM / 'tools/release')], check=True)
    base = [engine, 'run', '--rm', '-v', f'{WORKSPACE}:/src:ro', '-v', f'{build}:/build:rw']
    def container(arguments, *, environment=(), capture=False, signing=False):
        command = [*base]
        for name, value in environment:
            command += ['-e', f'{name}={value}']
        if signing:
            command += ['-v', f'{key}:/run/secrets/distribution.pem:ro', '-v', f'{output}:/output:rw']
        return subprocess.run([*command, image, *arguments], check=True, text=True,
                              stdout=subprocess.PIPE if capture else None)

    if not (build / 'wamr-source/.git').is_dir():
        container(['git', 'clone', '--shared', '/src/deps/pxa-system/wamr', '/build/wamr-source'])
    compiler = container(['bash', '/src/deps/pxa-system/tools/package/build_wamrc.sh'], environment=[
        ('PXA_WAMR_SOURCE_DIR', '/build/wamr-source'), ('PXA_WAMR_CACHE_DIR', '/build/cache'),
        ('PXA_WAMR_JOBS', str(args.jobs)), ('PXA_WAMR_CC', 'clang'), ('PXA_WAMR_CXX', 'clang++')], capture=True).stdout.strip()
    wasi = container(['bash', '/src/deps/pxa-system/tools/package/resolve_wasi_sdk.sh'],
                     environment=[('PXA_WASI_SDK_CACHE_DIR', '/build/cache')], capture=True).stdout.strip()
    (build / 'empty-catalog').mkdir(exist_ok=True)
    container(['cmake', '-S', '/src/deps/pxa-system/simulator/desktop', '-B', '/build/simulator', '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=Release', '-DPXSYS_BUILD_TESTS=ON', '-DBUILD_TESTING=ON',
               '-DPXSYS_LVGL_SOURCE_DIR=/src/firmware/managed_components/lvgl__lvgl',
               '-DPXSYS_DESKTOP_APP_SOURCE_ROOT=/build/empty-catalog', '-DPXA_WAMR_SOURCE_DIR=/build/wamr-source'])
    container(['cmake', '--build', '/build/simulator', '--parallel', str(args.jobs)])
    if not (build / 'test-publisher.pem').is_file():
        container(['openssl', 'genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256',
                   '-out', '/build/test-publisher.pem'])
    container(['ctest', '--test-dir', '/build/simulator', '--output-on-failure', '--parallel', '2'], environment=[
        ('WAMRC', compiler), ('WASI_SDK_DIR', wasi), ('PXA_WAMR_SOURCE_DIR', '/build/wamr-source'),
        ('PXA_SIGNING_KEY', '/build/test-publisher.pem'), ('PXA_SIMULATOR_FONT', '/src/factory/base/system/fonts/noto_sans_cjk_common.ttf'),
        ('PXA_BUILD_CACHE_DIR', '/build/wasm-test-cache'), ('SDL_VIDEODRIVER', 'dummy'), ('SDL_AUDIODRIVER', 'dummy')])
    container(['python3', '/src/deps/pxa-system/tools/release/assemble.py', '--workspace', '/src',
               '--simulator', '/build/simulator', '--wamrc', compiler, '--wasi', wasi,
               '--output', '/output', '--signing-key', '/run/secrets/distribution.pem',
               *(['--allow-dirty'] if args.allow_dirty else [])], signing=True)


if __name__ == '__main__':
    main()
