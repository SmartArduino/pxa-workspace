#!/usr/bin/env python3
"""Compare the production font's raster output with default and bounded pools."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[4]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
out = a.output.resolve()
if not out.is_relative_to(root / 'local/artifacts'):
    p.error('--output must be under local/artifacts')
out.mkdir(parents=True, exist_ok=True)
source = root / 'firmware/managed_components/espressif__freetype/freetype'
font = root / 'factory/base/system/fonts/noto_sans_cjk_common.ttf'
wrapper = out / 'source'
wrapper.mkdir(exist_ok=True)
(wrapper / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.16)
project(pxa_freetype_raster_test C)
set(SKIP_INSTALL_ALL TRUE)
set(BUILD_SHARED_LIBS OFF)
foreach(dep HARFBUZZ BZIP2 BROTLI PNG ZLIB)
    set(FT_DISABLE_${{dep}} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("{source}" freetype)
target_compile_options(freetype PRIVATE -fstack-usage)
if(PXA_FT_BOUNDED)
    target_include_directories(freetype PRIVATE "{root}/firmware/components/pxa/src/ui")
    target_compile_definitions(freetype PRIVATE FT_CONFIG_OPTIONS_H="pxa_freetype_options.h")
endif()
add_executable(glyph-test "{Path(__file__).with_name('pxa_freetype_raster_test.c')}")
target_link_libraries(glyph-test PRIVATE freetype)
''')

def build(name):
    dest = out / name
    with (out / (name + '.log')).open('w') as log:
        subprocess.run(['cmake', '-S', str(wrapper), '-B', str(dest), '-G', 'Ninja',
                        '-DCMAKE_BUILD_TYPE=Release',
                        '-DPXA_FT_BOUNDED=' + ('ON' if name == 'bounded' else 'OFF')],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['cmake', '--build', str(dest), '--parallel', '4'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    result = subprocess.check_output([str(dest / 'glyph-test'), str(font)], text=True)
    (out / (name + '-pixels.txt')).write_text(result)
    stack = []
    for file in dest.rglob('smooth.c.su'):
        for line in file.read_text().splitlines():
            if 'gray_convert_glyph' in line or 'gray_raster_render' in line:
                stack.append(line)
    assert stack, 'Missing real FreeType stack-usage information'
    (out / (name + '-stack.txt')).write_text('\n'.join(stack) + '\n')
    maximum = max(int(line.split('\t')[1]) for line in stack)
    return result, maximum

with ThreadPoolExecutor(max_workers=2) as executor:
    baseline, bounded = list(executor.map(build, ['baseline', 'bounded']))
assert baseline[0] == bounded[0], (baseline, bounded)
assert baseline[1] >= 16384 and bounded[1] < 4096, (baseline, bounded)
print(bounded[0].strip())
print(f'gray raster stack: default={baseline[1]} bounded={bounded[1]} bytes')
