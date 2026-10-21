#!/usr/bin/env python3
"""Build full board images using the exact signed Store shipped in a release.

Never opens a device. Merged images are for initial provisioning; application
images are also included for updates that preserve the existing data partition.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BOARDS = ('pai-touch', 'esp-mosaico', 'esp32s31-korvo-1', 'sensecap-watcher', 'doit-gamekit', 'doit-gamekit-1')


def sha(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def byte_size(value: str) -> int:
    match = re.fullmatch(r'(0x[0-9a-fA-F]+|[0-9]+)([KMG]B?)?', value.strip())
    if not match:
        raise ValueError(f'Invalid size: {value}')
    return int(match[1], 0 if match[1].startswith('0x') else 10) * (
        1024 ** ('KMG'.index(match[2][0]) + 1) if match[2] else 1)


def validate_regions(regions: list[dict], flash_bytes: int) -> None:
    end = 0
    for region in sorted(regions, key=lambda row: row['offset']):
        if region['offset'] < end or region['size'] <= 0:
            raise ValueError(f'Overlapping or empty flash region: {region["file"]}')
        end = region['offset'] + region['size']
        if end > flash_bytes:
            raise ValueError(f'Flash region exceeds capacity: {region["file"]}')


def config(board: Path, name: str, default: str) -> str:
    for path in (board / 'sdkconfig.defaults', ROOT / 'firmware/sdkconfig.defaults'):
        match = re.search(rf'^{re.escape(name)}=(?:"([^"\n]*)"|([^\r\n]*))$', path.read_text(), re.M)
        if match:
            return match[1] if match[1] is not None else match[2]
    return default


def build_board(args, name: str, version: str) -> dict:
    if not args.skip_build:
        subprocess.run(['bash', str(ROOT / 'tools/firmware.sh'), name, 'build'], check=True,
                       env={**os.environ, 'PXA_FIRMWARE_BUILD_ROOT': str(args.build_root)})
    board = ROOT / 'firmware/boards' / name
    build = args.build_root / name
    flash = json.loads((build / 'flasher_args.json').read_text())
    chip = flash['extra_esptool_args']['chip']
    target = 'esp32s3' if chip == 'esp32s3' else 'esp32s31'
    package = args.packages / target / 'pxa-store'
    container = package.with_suffix('.pxa')
    if not container.is_file() or not all((package / p).is_file() for p in ('manifest.pxm', 'signature.pxs')):
        raise ValueError(f'Missing canonical signed Store package: {package}')
    destination = args.output / name
    destination.mkdir(parents=True, exist_ok=True)
    settings = flash['flash_settings']
    capacity = byte_size(settings['flash_size'])
    label = config(board, 'CONFIG_PXA_STORAGE_PARTITION_LABEL', 'assets')
    package_root = Path(config(board, 'CONFIG_PXA_BUILTIN_PACKAGE_ROOT', 'system/pxa/builtin'))
    if package_root.is_absolute() or '..' in package_root.parts:
        raise ValueError('Invalid factory package root')
    partitions = []
    for row in csv.reader(line for line in (board / 'partitions.csv').read_text().splitlines()
                          if line.strip() and not line.lstrip().startswith('#')):
        partitions.append(dict(name=row[0].strip(), offset=byte_size(row[3]), size=byte_size(row[4])))
    storage = next(row for row in partitions if row['name'] == label)
    name_max = int(config(board, 'CONFIG_LITTLEFS_OBJ_NAME_LEN', '192'))
    with tempfile.TemporaryDirectory(prefix='pxa-release-factory-') as temporary:
        files = Path(temporary)
        # Expanded package comes from the same builder output as the published
        # container. Never rebuild/sign an unknown digest for the factory image.
        if any(p.is_symlink() for p in package.rglob('*')):
            raise ValueError('Factory package contains symlinks')
        shutil.copytree(package, files / package_root / package.name)
        fonts = files / 'system/fonts'
        fonts.mkdir(parents=True)
        for filename in ('noto_sans_cjk_common.ttf', 'NOTICE', 'OFL.txt'):
            shutil.copyfile(ROOT / 'factory/base/system/fonts' / filename, fonts / filename)
        subprocess.run([str(args.littlefs_python), 'create', str(files), str(destination / 'pxa-data.bin'),
                        f'--fs-size={storage["size"]}', f'--name-max={name_max}', '--block-size=4096'], check=True)
    regions = []
    for address, filename in flash['flash_files'].items():
        source = build / filename
        leaf = source.name
        shutil.copyfile(source, destination / leaf)
        regions.append(dict(file=leaf, offset=int(address, 0), size=source.stat().st_size, sha256=sha(source)))
    regions.append(dict(file='pxa-data.bin', offset=storage['offset'], size=storage['size'],
                        sha256=sha(destination / 'pxa-data.bin')))
    validate_regions(regions, capacity)
    for part in partitions:
        for region in regions:
            if region['offset'] == part['offset'] and region['size'] > part['size']:
                raise ValueError(f'Image exceeds {part["name"]} partition')
    merged = args.output / f'pxa-firmware-{version}-{name}-merged.bin'
    command = [str(args.idf_python), '-m', 'esptool', '--chip', chip, 'merge-bin',
               '-o', str(merged), '--flash-mode', settings['flash_mode'],
               '--flash-freq', settings['flash_freq'], '--flash-size', settings['flash_size'],
               '--fill-flash-size', settings['flash_size']]
    for row in sorted(regions, key=lambda row: row['offset']):
        command += [hex(row['offset']), str(destination / row['file'])]
    subprocess.run(command, check=True)
    if merged.stat().st_size != capacity:
        raise ValueError('Merged image does not cover the full declared flash')
    # Esptool may patch bootloader flash-header bytes. Every other region must
    # remain byte-identical, especially the canonical package and filesystem.
    with merged.open('rb') as stream:
        for row in regions:
            if row['file'] == flash['bootloader']['file'].split('/')[-1]:
                continue
            stream.seek(row['offset'])
            if hashlib.sha256(stream.read(row['size'])).hexdigest() != row['sha256']:
                raise ValueError(f'Merged image changed {row["file"]}')
    app_region = next(row for row in regions if row['offset'] == int(flash['app']['offset'], 0))
    app_name = f'pxa-firmware-{version}-{name}-app.bin'
    shutil.copyfile(destination / app_region['file'], args.output / app_name)
    record = dict(board=name, version=version, chip=chip, flash_bytes=capacity, partitions=partitions,
                  workspace_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  system_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT / 'deps/pxa-system', text=True).strip(),
                  store_container_sha256=sha(container), regions=regions,
                  merged=dict(file=merged.name, bytes=merged.stat().st_size, sha256=sha(merged)),
                  application=dict(file=app_name, bytes=app_region['size'], sha256=app_region['sha256']))
    (destination / 'flash-map.json').write_text(json.dumps(record, indent=2) + '\n')
    port = '/dev/ttyACM2' if name == 'esp-mosaico' else '/dev/ttyACM0'
    before = 'no-reset' if chip == 'esp32s31' else 'default-reset'
    stub_option = '--no-stub ' if not flash['extra_esptool_args'].get('stub', True) else ''
    for language in ('zh-CN', 'en'):
        text = (f'# PXA {version} / {name}\n\n'
                + ('合并固件用于首次安装，会重写存储分区并清除已有应用、存档和设置。包含系统字体与商城，勿用于保留数据的升级。\n\n'
                   if language == 'zh-CN' else
                   'Merged firmware is for initial provisioning. It replaces application/data partitions and removes existing apps, saves and settings. Includes the system font and Store.\n\n')
                + f'```sh\npython -m esptool --chip {chip} --port {port} --baud 2000000 '
                  f'--before {before} {stub_option}write-flash 0x0 {merged.name}\n```\n\n'
                + ('保留数据的升级：仅烧录 app 文件到以下地址，先确认当前分区表与 flash-map.json 相同。ESP32-S31 使用 BOOT/RESET 手动进入下载模式；串口号码仅为示例。\n\n'
                   if language == 'zh-CN' else
                   'To preserve data, flash only the application below after checking that the existing partition table matches flash-map.json. Enter ROM download mode manually on ESP32-S31. Port names are examples.\n\n')
                + f'```sh\npython -m esptool --chip {chip} --port {port} --baud 2000000 '
                  f'--before {before} {stub_option}write-flash {hex(app_region["offset"])} {app_name}\n```\n\n'
                + 'SHA-256: SHA256SUMS / flash-map.json.\n')
        (destination / f'README.{language}.md').write_text(text)
    # A ZIP is convenient for tools which expect a local map and separate bins.
    archive = args.output / f'pxa-firmware-{version}-{name}.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as out:
        for path in sorted(destination.iterdir()):
            out.write(path, path.name)
        out.write(merged, merged.name)
        out.write(args.output / app_name, app_name)
    record['archive'] = dict(file=archive.name, bytes=archive.stat().st_size, sha256=sha(archive))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--packages', type=Path, required=True)
    parser.add_argument('--boards', nargs='+', choices=BOARDS, default=BOARDS)
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--build-root', type=Path, default=ROOT / 'build/firmware')
    parser.add_argument('--idf-python', type=Path, default=Path(sys.executable))
    parser.add_argument('--littlefs-python', type=Path, default=ROOT / '.pxa-tools/littlefs-venv/bin/littlefs-python')
    args = parser.parse_args()
    args.output = args.output.resolve(); args.packages = args.packages.resolve()
    args.build_root = args.build_root.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    version = (ROOT / 'deps/pxa-system/VERSION').read_text().strip()
    reports = []
    for board in args.boards:
        reports.append(build_board(args, board, version))
        print('VERIFIED FIRMWARE', board, flush=True)
    (args.output / 'firmware-manifest.json').write_text(json.dumps(reports, indent=2) + '\n')
    (args.output / 'SHA256SUMS').write_text(''.join(
        f'{sha(p)}  {p.name}\n' for p in sorted(args.output.iterdir())
        if p.is_file() and p.name != 'SHA256SUMS'))


if __name__ == '__main__':
    main()
