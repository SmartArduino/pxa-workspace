#!/usr/bin/env python3
"""Verify a companion manifest with a separately trusted PXA release key."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def verify(manifest: Path, public_key: Path, openssl: str):
    subprocess.run([openssl, 'dgst', '-sha256', '-verify', str(public_key),
                    '-signature', str(manifest) + '.sig', str(manifest)], check=True)
    data = json.loads(manifest.read_text())
    if data.get('schema') != 'pxa-host-distribution-1':
        raise ValueError('Unsupported manifest schema')
    for artifact in data['artifacts']:
        name = artifact['file']
        if not isinstance(name, str) or Path(name).name != name or '\\' in name:
            raise ValueError('Invalid artifact filename')
        path = manifest.parent / name
        digest = hashlib.sha256()
        with path.open('rb') as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
        if path.stat().st_size != artifact['size'] or digest.hexdigest() != artifact['sha256']:
            raise ValueError('Artifact size or hash mismatch: ' + name)
        print('Verified: ' + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--public-key', required=True, type=Path,
                        help='key obtained from an already trusted DevKit or verified source')
    parser.add_argument('--openssl', default='openssl', help='native OpenSSL executable')
    args = parser.parse_args()
    try:
        verify(args.manifest, args.public_key, args.openssl)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Verification failed: {error}\n')


if __name__ == '__main__':
    main()
