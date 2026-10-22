#!/usr/bin/env python3
"""Run production font streams with real FreeType and the real volume registry."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libpxa', type=Path)
    args = parser.parse_args()
    component = ROOT / 'firmware/components/pxa'
    sdk = ROOT / 'deps/pxa-system/libpxa'
    freetype = ROOT / 'firmware/managed_components/espressif__freetype/freetype'
    sanitize = os.environ.get('PXA_TEST_SANITIZERS') == '1'
    flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if sanitize else []
    with tempfile.TemporaryDirectory(prefix='pxa-font-source-') as temporary:
        work = Path(temporary)
        if args.libpxa:
            libpxa = args.libpxa.resolve()
        else:
            subprocess.run(['cmake', '-S', str(sdk), '-B', str(work / 'sdk'),
                            '-DPXA_BUILD_TESTS=OFF', '-DPXA_BUILD_ADAPTERS=OFF',
                            '-DCMAKE_C_FLAGS=' + ' '.join(flags)], check=True)
            subprocess.run(['cmake', '--build', str(work / 'sdk'), '-j2'], check=True)
            libpxa = work / 'sdk/libpxa.a'
        subprocess.run(['cmake', '-S', str(freetype), '-B', str(work / 'freetype'),
                        '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_SHARED_LIBS=OFF',
                        '-DCMAKE_C_FLAGS=' + ' '.join(flags),
                        *['-DFT_DISABLE_' + name + '=ON' for name in
                          ('ZLIB', 'BZIP2', 'PNG', 'HARFBUZZ', 'BROTLI')]], check=True)
        subprocess.run(['cmake', '--build', str(work / 'freetype'), '-j4'], check=True)
        (work / 'sdkconfig.h').write_text('#define CONFIG_LV_USE_FREETYPE 1\n')
        includes = [work, component / 'tests/esp_stubs', component / 'include',
                    component / 'src/services', component / 'src/package', sdk / 'include',
                    sdk / 'adapters/include', freetype / 'include', work / 'freetype/include']
        source = [component / 'tests/pxa_esp_font_source_test.c',
                  component / 'src/services/pxa_esp_font_source.c',
                  component / 'src/services/pxa_esp_volumes.c',
                  sdk / 'adapters/posix/pxa_posix_volume.c',
                  sdk / 'adapters/esp/pxa_esp_posix_shim.c']
        binary = work / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        *flags, '-DESP_PLATFORM=1', '-DCONFIG_LITTLEFS_FCNTL_GET_PATH=0',
                        *['-I' + str(path) for path in includes], *map(str, source), str(libpxa),
                        str(work / 'freetype/libfreetyped.a'), '-Wl,--wrap=FT_Stream_Open',
                        '-pthread', '-lmbedcrypto', '-lm', '-o', str(binary)], check=True)
        fixture = work / 'fixture'
        fixture.mkdir()
        subprocess.run([str(binary), str(fixture),
                        str(ROOT / 'factory/base/system/fonts/noto_sans_cjk_common.ttf')], check=True)


if __name__ == '__main__':
    main()
