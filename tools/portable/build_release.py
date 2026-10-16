#!/usr/bin/env python3
"""Package host tools and a tested Docker archive as a signed companion release."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

VERSION = (Path(__file__).resolve().parents[2] / 'deps/pxa-system/VERSION').read_text().strip()


def git(root: Path, *args: str):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


def sha(path: Path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def build(root: Path, output: Path, image: Path, image_id: str, devkit_sha: str, key: Path):
    if git(root, 'status', '--porcelain'):
        raise ValueError('Commit all source changes before building an official release')
    if output.exists():
        raise ValueError('Use a new output directory')
    output.mkdir(parents=True)
    commit = git(root, 'rev-parse', 'HEAD')
    epoch = int(git(root, 'show', '-s', '--format=%ct', 'HEAD'))
    host_name = f'pxa-host-tools-{VERSION}'
    with tempfile.TemporaryDirectory(prefix='pxa-host-release-') as temporary:
        stage = Path(temporary) / host_name
        stage.mkdir()
        for name in ('pxa_container.py', 'verify_manifest.py', 'Dockerfile',
                     'README.md', 'README.en.md', 'requirements.txt'):
            shutil.copy2(root / 'tools/portable' / name, stage / name)
        shutil.copy2(root / 'tools/pxadb/pxadb.py', stage / 'pxadb.py')
        shutil.copy2(root / 'deps/pxa-system/LICENSE', stage / 'LICENSE.pxa-system')
        # This copy is convenient, not a trust anchor. Verification requires a
        # separately trusted key; the README explains where to obtain it.
        shutil.copy2(root / 'deps/pxa-system/config/release-public.pem', stage / 'release-public.pem')
        (stage / 'manifest.json').write_text(json.dumps({'schema': 'pxa-host-files-1',
            'version': VERSION, 'files': {p.name: sha(p) for p in sorted(stage.iterdir())}},
            sort_keys=True, indent=2) + '\n')
        with (output / (host_name + '.tar.gz')).open('wb') as raw:
            with gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=epoch) as compressed:
                with tarfile.open(fileobj=compressed, mode='w') as archive:
                    for path in [stage, *sorted(stage.iterdir())]:
                        info = archive.gettarinfo(str(path), str(path.relative_to(stage.parent)))
                        info.uid = info.gid = 0
                        info.uname = info.gname = ''
                        info.mtime = epoch
                        info.mode = 0o755 if path.is_dir() else 0o644
                        if path.is_file():
                            with path.open('rb') as source:
                                archive.addfile(info, source)
                        else:
                            archive.addfile(info)
    image_name = f'pxa-devkit-container-{VERSION}-linux-amd64.tar.gz'
    shutil.copyfile(image, output / image_name)
    names = [host_name + '.tar.gz', image_name]
    manifest = {'schema': 'pxa-host-distribution-1', 'version': VERSION,
        'devkit': {'release': VERSION, 'archive_sha256': devkit_sha, 'core_major': 2},
        'source': {'commit': commit}, 'container': {'image': f'pxa-devkit:{VERSION}-linux-amd64',
            'image_id': image_id, 'platform': 'linux/amd64'},
        'artifacts': [{'file': name, 'size': (output / name).stat().st_size,
                       'sha256': sha(output / name)} for name in names]}
    name = f'host-tools-{VERSION}.json'
    (output / name).write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(key), '-out',
                    str(output / (name + '.sig')), str(output / name)], check=True)
    (output / 'SHA256SUMS').write_text(''.join(f'{sha(output / n)}  {n}\n'
        for n in [*names, name, name + '.sig']))
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--image-id', required=True)
    parser.add_argument('--devkit-sha256', required=True)
    parser.add_argument('--signing-key', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.source.resolve(), args.output.resolve(), args.image.resolve(),
        args.image_id, args.devkit_sha256, args.signing_key.resolve(strict=True)), indent=2))


if __name__ == '__main__':
    main()
