#!/usr/bin/env python3
"""Package reviewed Windows binaries into a signed, relocatable DevKit.

This never builds LLVM or opens a device. Supply the pinned compiler, Windows
WASI SDK, embeddable Python, CMake/Ninja and reviewed DLL/license directories.
Validate the resulting archive on Windows (or identify Wine-only validation).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile
import xml.etree.ElementTree as ET

WORKSPACE = Path(__file__).resolve().parents[3]
SYSTEM = WORKSPACE / 'deps/pxa-system'
sys.path.insert(0, str(SYSTEM / 'tools/release'))
from compact import compact_python, compact_toolchain, strip_compiler
from assemble import archive, copy, sha

# OS libraries remain replaceable and are never copied into the DevKit.
OS_DLLS = set(('kernel32 user32 gdi32 advapi32 shell32 ole32 oleaut32 uuid '
              'ws2_32 winmm imm32 version setupapi hid dinput8 dxgi d3d9 d3d11 '
              'dwmapi uxtheme shlwapi rpcrt4 bcrypt crypt32 secur32 wldap32 '
              'normaliz iphlpapi ntdll msvcrt ucrtbase powrprof cfgmgr32 '
              'comdlg32 comctl32 winspool winhttp netapi32 userenv psapi '
              'dnsapi mswsock ncrypt cabinet wintrust authz dbghelp bcryptprimitives '
              'msvcp140 vcruntime140 vcruntime140_1 propsys msi').split())


def enable_utf8_paths(path):
    """Opt unsigned OpenSSL into Windows UTF-8 without changing executable code."""
    import lief  # Apache-2.0, build-only; see requirements.txt.
    binary = lief.PE.parse(str(path))
    if binary is None or not binary.has_resources:
        raise ValueError('OpenSSL must have a readable PE resource tree')
    original = binary.resources_manager.manifest
    document = ET.fromstring(original)
    code_page = '{http://schemas.microsoft.com/SMI/2019/WindowsSettings}activeCodePage'
    current = document.find('.//' + code_page)
    if current is not None and current.text == 'UTF-8':
        return
    if len(binary.signatures):
        raise ValueError('Do not rewrite Authenticode-signed OpenSSL; supply a UTF-8-enabled build')
    namespace = '{urn:schemas-microsoft-com:asm.v3}'
    application = document.find(namespace + 'application')
    if application is None:
        application = ET.SubElement(document, namespace + 'application')
    settings = application.find(namespace + 'windowsSettings')
    if settings is None:
        settings = ET.SubElement(application, namespace + 'windowsSettings')
    if current is None:
        current = ET.SubElement(settings, code_page)
    current.text = 'UTF-8'
    ET.register_namespace('', 'urn:schemas-microsoft-com:asm.v1')
    ET.register_namespace('asmv3', 'urn:schemas-microsoft-com:asm.v3')

    def sections(pe):
        return {s.name: (s.virtual_address, hashlib.sha256(bytes(s.content)).hexdigest())
                for s in pe.sections if s.name != '.rsrc'}

    def resources(pe):
        result = {}
        def visit(node, prefix):
            name = str(node.name) if node.has_name else str(node.id)
            name = prefix + '/' + name
            if node.is_data:
                result[name] = hashlib.sha256(bytes(node.content)).hexdigest()
            else:
                for child in node.childs:
                    visit(child, name)
        for child in pe.resources.childs:
            if child.has_name or child.id != 24:
                visit(child, '')
        return result

    original_sections, original_resources = sections(binary), resources(binary)
    entry = binary.optional_header.addressof_entrypoint
    binary.resources_manager.manifest = ET.tostring(document, encoding='unicode')
    temporary = path.with_suffix('.utf8.exe')
    binary.write(str(temporary))
    try:
        checked = lief.PE.parse(str(temporary))
        if (checked is None or checked.optional_header.addressof_entrypoint != entry
                or not all(sections(checked).get(name) == value for name, value in original_sections.items())
                or resources(checked) != original_resources):
            raise ValueError('OpenSSL executable code or non-manifest resources changed')
        if ET.fromstring(checked.resources_manager.manifest).find('.//' + code_page).text != 'UTF-8':
            raise ValueError('OpenSSL UTF-8 manifest was not preserved')
        temporary.chmod(path.stat().st_mode)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def imports(path, objdump):
    result = subprocess.run([objdump, '-p', str(path)], check=True, text=True, capture_output=True)
    return re.findall(r'DLL Name:\s*(\S+)', result.stdout)


def collect_dlls(root, directories, objdump):
    available = {}
    for directory in directories:
        for path in sorted(directory.rglob('*')):
            if path.is_file() and path.suffix.lower() == '.dll':
                available.setdefault(path.name.lower(), path)
    pending = [p for p in root.rglob('*') if p.suffix.lower() in ('.exe', '.dll', '.pyd')]
    checked, report = set(), {}
    runtime = root / 'runtime/bin'
    while pending:
        path = pending.pop()
        if path in checked:
            continue
        checked.add(path)
        names = imports(path, objdump)
        report[path.relative_to(root).as_posix()] = names
        for name in names:
            lower = name.lower()
            if lower in ('msys-2.0.dll', 'cygwin1.dll'):
                raise ValueError('A native DevKit cannot depend on ' + name)
            if lower.startswith(('api-ms-win-', 'ext-ms-win-')) or lower.removesuffix('.dll') in OS_DLLS:
                continue
            # Windows searches the executable directory before PATH. Embedded
            # Python and WASI SDK keep their own runtime libraries there.
            local = next((p for p in path.parent.iterdir() if p.name.lower() == lower), None)
            if local:
                pending.append(local)
                continue
            destination = runtime / name
            existing = next((p for p in runtime.iterdir() if p.name.lower() == lower), None)
            if existing:
                continue
            if lower not in available:
                raise ValueError(f'Unresolved Windows dependency: {path.name}: {name}')
            copy(available[lower], destination)
            pending.append(destination)
    (root / 'windows-runtime.json').write_text(json.dumps(report, sort_keys=True, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('simulator', 'wamrc', 'wasi', 'python', 'cmake', 'ninja', 'openssl', 'licenses', 'output', 'signing-key'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--dll-directory', type=Path, action='append', default=[])
    parser.add_argument('--objdump', default='objdump' if os.name == 'nt' else 'x86_64-w64-mingw32-objdump')
    parser.add_argument('--validation', choices=('windows', 'wine'), required=True)
    parser.add_argument('--toolchain-profile', choices=('pxa', 'full'), default='pxa')
    parser.add_argument('--strip', default='strip' if os.name == 'nt' else 'x86_64-w64-mingw32-strip')
    args = parser.parse_args()
    for checkout in (WORKSPACE, SYSTEM):
        if subprocess.check_output(['git', '-C', str(checkout), 'status', '--porcelain'], text=True).strip():
            parser.error('Official packaging requires committed source: ' + str(checkout))
    version = (SYSTEM / 'VERSION').read_text().strip()
    if (args.wasi / 'VERSION').read_text().strip().split('.')[0] != '34':
        parser.error('Use the pinned Windows WASI SDK 34')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = output / f'pxa-devkit-{version}-windows-x86_64'
    if root.exists():
        parser.error('Choose an empty output directory; existing packages are immutable')
    root.mkdir()
    ignore = shutil.ignore_patterns('__pycache__', '*.pyc', '.git', '.pxa', 'dist', 'build')
    for name in ('VERSION', 'LICENSE', 'sdk', 'config', 'docs', 'ai'):
        copy(SYSTEM / name, root / name, ignore=ignore)
    copy(WORKSPACE / 'extensions', root / 'extensions', ignore=ignore)
    copy(SYSTEM / 'spec/draft', root / 'spec/draft', ignore=ignore)
    for name in ('pxa_tools', 'package', 'i18n'):
        copy(SYSTEM / 'tools' / name, root / 'tools' / name, ignore=ignore)
    copy(WORKSPACE / 'tools/pxadb', root / 'tools/pxadb', ignore=ignore)
    copy(WORKSPACE / 'tools/simulator_pxadb.py', root / 'tools/simulator_pxadb.py')
    for name in ('pxa.cmd', 'pxadb.cmd'):
        copy(SYSTEM / 'bin' / name, root / 'bin' / name)
    copy(args.wamrc, root / 'bin/wamrc.exe')
    copy(args.wasi, root / 'toolchains/wasi-sdk', ignore=ignore)
    compact = {'toolchain': compact_toolchain(root / 'toolchains/wasi-sdk',
        profile=args.toolchain_profile, windows=True, objdump=args.objdump),
        'wamrc': strip_compiler(root / 'bin/wamrc.exe', args.strip)}
    copy(args.python, root / 'runtime/python', ignore=ignore)
    compact['python'] = compact_python(root / 'runtime/python')
    for path in (root / 'runtime/python').glob('*._pth'):
        # Embeddable Python deliberately ignores PYTHONPATH. Add only this kit.
        entries = [line for line in path.read_text().splitlines()
                   if line.strip() and not line.startswith(('#', 'import '))
                   and not re.match(r'^[A-Za-z]:', line) and not line.startswith(('/', '\\'))]
        path.write_text('\n'.join([*entries, '../../tools', '../../tools/package',
                                   '../../tools/i18n', 'import site']) + '\n')
    copy(args.cmake / 'bin/cmake.exe', root / 'runtime/bin/cmake.exe')
    for directory in ('share', 'doc'):
        copy(args.cmake / directory, root / 'runtime' / directory, ignore=ignore)
    copy(args.ninja, root / 'runtime/bin/ninja.exe')
    copy(args.openssl, root / 'runtime/bin/openssl.exe')
    enable_utf8_paths(root / 'runtime/bin/openssl.exe')
    for name in ('pxsys_desktop_simulator', 'pxsys_product_simulator', 'pxsys_package_installer'):
        copy(args.simulator / (name + '.exe'), root / 'simulator/bin' / (name + '.exe'))
    copy(WORKSPACE / 'simulator/profiles', root / 'share/pxa/profiles')
    for name in ('noto_sans_cjk_common.ttf', 'OFL.txt', 'OFL-Latin.txt', 'NOTICE'):
        copy(WORKSPACE / 'factory/base/system/fonts' / name, root / 'share/pxa/fonts' / name)
    copy(args.licenses, root / 'licenses')
    copy(SYSTEM / 'LICENSE', root / 'licenses/PXA-LICENSE')
    collect_dlls(root, [*args.dll_directory, args.python, args.wasi], args.objdump)
    (root / 'README.md').write_text(f'''# PXA DevKit {version} for Windows x64

Install Microsoft's Visual C++ 2015-2022 x64 runtime once if not already present:
https://aka.ms/vs/17/release/vc_redist.x64.exe
The upstream Windows WASI SDK needs MSVCP140/VCRUNTIME140. These OS prerequisites
are not replaced with Wine or MinGW implementations in this package.
Extract to a writable folder. Add bin to PATH, then run pxa init hello and
pxa run hello --offline. Compiler, SDK, simulator and Python are included.
USB: pxa device devices, then pxa device info --port COM7. Enable Settings /
PXADB debugging on the device where available. Use a USB data cable.
AI setup and guides: https://pxa.doit.am/docs/ai-development/
Validation: {args.validation}. Wine validation does not establish native Windows
window/audio/USB-driver acceptance. Windows 10 build 18362+; Windows 11 recommended.
''')
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH', '1791849600'))
    groups = {'sdk': ['VERSION', 'LICENSE', 'sdk', 'config', 'spec', 'docs', 'extensions'],
              'devtools': ['bin/pxa.cmd', 'bin/pxadb.cmd', 'tools', 'ai', 'README.md'],
              'runtime': ['runtime', 'licenses', 'windows-runtime.json'],
              'toolchain': ['bin/wamrc.exe', 'toolchains'], 'simulator': ['simulator', 'share']}
    components = []
    for name, paths in groups.items():
        path = output / f'pxa-{name}-{version}-windows-x86_64.tar.gz'
        archive(root, paths, path, epoch)
        components.append(dict(name=name, file=path.name, size=path.stat().st_size, sha256=sha(path)))
    aggregate = output / (root.name + '.zip')
    with zipfile.ZipFile(aggregate, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zipped:
        for path in sorted(root.rglob('*')):
            if path.is_file():
                info = zipfile.ZipInfo(path.relative_to(root.parent).as_posix(), (2026, 11, 8, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                zipped.writestr(info, path.read_bytes())
    sys.path.insert(0, str(SYSTEM / 'tools'))
    from pxa_tools.project import sdk_identity
    provenance = {name: {'commit': subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip(), 'dirty': False}
                  for name, checkout in [('workspace', WORKSPACE), ('system', SYSTEM)]}
    manifest = dict(schema='pxa-distribution-1', version=version, platform='windows-x86_64',
                    source=provenance, payload=compact, minimum_windows_build=18362, validation=args.validation,
                    prerequisites=['Microsoft Visual C++ 2015-2022 x64 runtime (MSVCP140/VCRUNTIME140)'],
                    sdk=sdk_identity(root), components=components,
                    devkit=dict(file=aggregate.name, size=aggregate.stat().st_size, sha256=sha(aggregate)),
                    toolchain=json.loads((SYSTEM / 'config/wamr.json').read_text()),
                    wasi_sdk_version=(args.wasi / 'VERSION').read_text().strip())
    path = output / 'distribution-windows-x86_64.json'
    path.write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
    subprocess.run(['openssl', 'dgst', '-sha256', '-sign', str(args.signing_key), '-out', str(path) + '.sig', str(path)], check=True)
    print(path)


if __name__ == '__main__':
    main()
