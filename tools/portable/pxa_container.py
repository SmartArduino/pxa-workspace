#!/usr/bin/env python3
"""Use the Linux DevKit from Docker/Podman and keep device USB on the host."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

DEFAULT_IMAGE = 'pxa-devkit:0.5.0-linux-amd64'


def invocation(workspace: Path, state: Path, engine: str, image: str, arguments: list[str]):
    if not arguments:
        raise ValueError('Pass a PXA command after --, for example -- build hello --target esp32s3')
    if any(arg == '--device' or arg.startswith('--device=') for arg in arguments):
        raise ValueError('Build in the container, then use host-native pxadb package install/run with COM or /dev/cu.*; USB forwarding is unnecessary')
    for path in (workspace, state):
        if ',' in str(path):
            raise ValueError('Container mount paths cannot contain a comma')
    command = [engine, 'run', '--rm', '--platform', 'linux/amd64']
    if os.name == 'posix' and sys.platform.startswith('linux'):
        command += ['--user', f'{os.getuid()}:{os.getgid()}', '--userns=keep-id'] if Path(engine).name == 'podman' else ['--user', f'{os.getuid()}:{os.getgid()}']
    command += ['--mount', f'type=bind,source={workspace},target=/workspace',
                '--mount', f'type=bind,source={state / "data"},target=/pxa-data',
                '--mount', f'type=bind,source={state / "cache"},target=/pxa-cache',
                '--workdir', '/workspace', image, *arguments]
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path.cwd(), help='parent containing app and shared sources')
    parser.add_argument('--state', type=Path, help='persistent identities and cache, default workspace/.pxa-container')
    parser.add_argument('--engine', default='docker', choices=('docker', 'podman'))
    parser.add_argument('--image', default=DEFAULT_IMAGE)
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        workspace = args.workspace.expanduser().resolve(strict=True)
        if not workspace.is_dir():
            raise ValueError('Workspace must be an existing directory')
        state = (args.state or workspace / '.pxa-container').expanduser().resolve()
        arguments = args.command[1:] if args.command[:1] == ['--'] else args.command
        engine = shutil.which(args.engine)
        if engine is None:
            raise ValueError('Install Docker Desktop or Podman and load the DevKit image first')
        cmd = invocation(workspace, state, engine, args.image, arguments)
        if args.dry_run:
            import json
            print(json.dumps(cmd, ensure_ascii=False))
            return 0
        for name in ('data', 'cache'):
            (state / name).mkdir(parents=True, exist_ok=True)
        return subprocess.run(cmd).returncode
    except (OSError, ValueError) as error:
        print('pxa-container: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
