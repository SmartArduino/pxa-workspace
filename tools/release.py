#!/usr/bin/env python3
"""Build the independent DevKit in a pinned Linux container.

This is a maintainer entry point, not an App build dependency. It keeps mutable
WAMR/LLVM/build caches outside the checkout and never embeds a private key.
Publication is separate: validate these assets before creating a release.
"""
from __future__ import annotations

import argparse
import json
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
    runtime_recipe = WORKSPACE / 'tools/portable/native-builder'
    image = json.loads((runtime_recipe / 'sources.json').read_text())['builder_image']
    inspected = subprocess.run([engine, 'image', 'inspect', '--format', '{{.Id}}', image],
                               capture_output=True, text=True)
    if inspected.returncode or inspected.stdout.strip().removeprefix('sha256:') != image.removeprefix('sha256:'):
        parser.error('Build the reviewed native runtime image in tools/portable/native-builder/README.md; '
                     'sources.json pins its inspected image ID. A plain distro image is not a release runtime.')
    base = [engine, 'run', '--rm', '-v', f'{WORKSPACE}:/src:ro', '-v', f'{build}:/build:rw',
            '-v', f'{runtime_recipe}:/release-runtime:ro']
    # Git worktrees, shared object stores and submodules can name absolute host
    # paths. Preserve their read-only metadata mappings inside the builder.
    git_paths = set()
    for checkout in (WORKSPACE, SYSTEM, SYSTEM / 'wamr', SYSTEM / 'ui/backends/lvgl/page_manager'):
        for name in ('--absolute-git-dir', '--git-common-dir'):
            value = Path(subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', name], text=True).strip())
            git_paths.add(value.resolve() if value.is_absolute() else (checkout / value).resolve())
    for path in sorted(git_paths):
        base += ['-v', f'{path}:{path}:ro']
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
    native_build = f'/build/simulator-{version}'
    container(['cmake', '-S', '/src/deps/pxa-system/simulator/desktop', '-B', native_build, '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=Release', '-DPXSYS_BUILD_TESTS=ON', '-DBUILD_TESTING=ON',
               '-DPXSYS_LVGL_SOURCE_DIR=/src/firmware/managed_components/lvgl__lvgl',
               '-DPXSYS_DESKTOP_APP_SOURCE_ROOT=/build/empty-catalog', '-DPXA_WAMR_SOURCE_DIR=/build/wamr-source',
               '-DCMAKE_PREFIX_PATH=/native', '-DSDL2_DIR=/native/lib/cmake/SDL2',
               '-DCURL_NO_CURL_CMAKE=ON', '-DCURL_LIBRARY_RELEASE=/native/lib/libcurl.so',
               '-DCURL_LIBRARY_DEBUG=/native/lib/libcurl.so', '-DCURL_INCLUDE_DIR=/native/include'])
    container(['cmake', '--build', native_build, '--parallel', str(args.jobs)])
    if not (build / 'test-publisher.pem').is_file():
        container(['openssl', 'genpkey', '-algorithm', 'EC', '-pkeyopt', 'ec_paramgen_curve:P-256',
                   '-out', '/build/test-publisher.pem'])
    (build / 'test-artifacts').mkdir(exist_ok=True)
    container(['ctest', '--test-dir', native_build, '--output-on-failure', '--parallel', '2'], environment=[
        ('WAMRC', compiler), ('WASI_SDK_DIR', wasi), ('PXA_WAMR_SOURCE_DIR', '/build/wamr-source'),
        ('PXA_SIGNING_KEY', '/build/test-publisher.pem'), ('PXA_SIMULATOR_FONT', '/src/factory/base/system/fonts/noto_sans_cjk_common.ttf'),
        ('PXA_TEST_ARTIFACT_ROOT', '/build/test-artifacts'),
        ('PXA_BUILD_CACHE_DIR', '/build/wasm-test-cache'), ('SDL_VIDEODRIVER', 'dummy'), ('SDL_AUDIODRIVER', 'dummy')])
    container(['python3', '-c',
               'from pathlib import Path; import shutil,sys; '
               'p=Path(sys.argv[1])/"native-runtime"; p.mkdir(exist_ok=True); '
               '[shutil.copy2(Path("/native/lib")/n,p/n,follow_symlinks=True) '
               'for n in ("libcurl.so.4","libSDL2-2.0.so.0","libarchive.so.13")]', native_build])
    container(['python3', '/release-runtime/assemble_parallel.py', '--workspace', '/src',
               '--simulator', native_build, '--wamrc', compiler, '--wasi', wasi,
               '--output', '/output', '--signing-key', '/run/secrets/distribution.pem',
               *(['--allow-dirty'] if args.allow_dirty else [])], signing=True)


if __name__ == '__main__':
    main()
