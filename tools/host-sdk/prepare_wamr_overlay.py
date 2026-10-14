#!/usr/bin/env python3
"""Materialize Core's WAMR patch series from an immutable Host SDK archive.

CMake verifies the SDK against the product's external lock before invoking
this tool. No Git metadata is required or added to the SDK source tree.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path, PurePosixPath
import shutil
import sys

sys.dont_write_bytecode = True  # Imports must not add files to the immutable SDK.
from pxa_host_sdk import verify


def prepare(root: Path, source: Path, output: Path,
            metadata: Path, series: Path) -> list[Path]:
    root, source, output, metadata, series = (
        p.resolve() for p in (root, source, output, metadata, series))
    system = root / 'deps/pxa-system'
    if source != system / 'wamr' or metadata != system / 'config/wamr.json' or \
            series != system / 'platforms/esp-idf/wamr/patches/series':
        raise ValueError('Overlay input must come from the verified Host SDK')
    if output.is_relative_to(root) or root.is_relative_to(output):
        raise ValueError('WAMR overlay must be outside the immutable SDK')
    manifest = verify(root)
    pin = json.loads(metadata.read_text())['commit']
    if manifest['sources']['wamr'] != pin:
        raise ValueError('Host SDK WAMR pin differs from Core metadata')
    spec = importlib.util.spec_from_file_location(
        'pxa_core_overlay', system / 'tools/wamr/prepare_overlay.py')
    core = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = core  # dataclasses resolves the defining module.
    spec.loader.exec_module(core)
    names = [line.strip() for line in series.read_text().splitlines()
             if line.strip() and not line.lstrip().startswith('#')]
    if not names:
        raise ValueError('WAMR patch series is empty')
    staged = {}
    for name in names:
        relative = PurePosixPath(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('Unsafe patch series entry')
        patch = series.parent / relative
        for file_patch in core.parse_patch(patch):
            path = file_patch.path
            if path not in staged:
                staged[path] = (source / path).read_text().splitlines(keepends=True)
            staged[path] = core.apply_hunks(staged[path], file_patch.hunks, f'{name}:{path}')
    # Validate again before replacing any prior build output. Source files
    # remain untouched, and failed validation preserves the prior overlay.
    verify(root)
    if output.exists():
        shutil.rmtree(output)
    written = []
    for path, lines in staged.items():
        destination = output / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(''.join(lines))
        written.append(destination)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--series', type=Path, required=True)
    args = parser.parse_args()
    try:
        written = prepare(Path(__file__).resolve().parents[2], args.source,
                          args.output, args.metadata, args.series)
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        print(f'PXA Host SDK WAMR overlay: {error}', file=sys.stderr)
        return 1
    print(f'Prepared verified archive WAMR overlay at {args.output} ({len(written)} files)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
