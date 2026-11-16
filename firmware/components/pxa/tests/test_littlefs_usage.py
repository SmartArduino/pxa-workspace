#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise generated storage statistics against the real LittleFS library."""
from pathlib import Path
import argparse
import importlib.util
import subprocess
import sys
import tempfile

COMPONENT = Path(__file__).resolve().parents[1]
ROOT = COMPONENT.parents[2]
spec = importlib.util.spec_from_file_location("usage_adapter", COMPONENT / "tools/prepare_littlefs_usage.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver-dir", type=Path,
                        default=ROOT / "firmware/managed_components/joltwallet__littlefs")
    args = parser.parse_args()
    args.driver_dir = args.driver_dir.resolve()
    library = args.driver_dir / "src/littlefs"
    with tempfile.TemporaryDirectory(prefix="pxa-littlefs-usage-") as temporary:
        output = Path(temporary)
        source = args.driver_dir / "src/esp_littlefs.c"
        generated = output / "esp_littlefs.c"
        before = source.read_bytes()
        adapter.prepare(source, generated)
        assert source.read_bytes() == before
        first = generated.read_bytes()
        stamp = generated.stat().st_mtime_ns
        adapter.prepare(source, generated)
        assert generated.read_bytes() == first and generated.stat().st_mtime_ns == stamp
        modified = output / "unreviewed.c"
        modified.write_bytes(before + b"\n/* changed dependency */\n")
        try:
            adapter.prepare(modified, generated)
        except ValueError:
            pass
        else:
            raise AssertionError("Changed dependency was not rejected")
        assert generated.read_bytes() == first
        try:
            adapter.prepare(source, source)
        except ValueError:
            pass
        else:
            raise AssertionError("SDK source modification was not rejected")
        # Compile the very function emitted into the driver, with native lock
        # and allocator adapters; the filesystem implementation remains real.
        (output / "generated_usage.c").write_text(adapter.REPLACEMENT)
        binary = output / "usage-test"
        command = ["cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-DLFS_NO_ERROR",
                   "-I" + str(library), "-I" + str(args.driver_dir / "src"),
                   "-I" + str(COMPONENT / "src/services"),
                   "-I" + str(output), str(COMPONENT / "tests/littlefs_usage_test.c"),
                   str(library / "lfs.c"), str(library / "lfs_util.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=60)
        # Check that deferred component configuration replaces the driver in
        # its actual target instead of compiling an unused generated copy.
        cmake = output / "cmake"
        cmake.mkdir()
        (cmake / "public").mkdir()
        (cmake / "public/lfs.h").write_text('#error "Public raw LittleFS API must remain disabled"\n')
        (cmake / "header_smoke.c").write_text('''#include <stdlib.h>
#include "pxa_littlefs_usage.h"
int smoke(lfs_t *fs) { lfs_size_t used; return pxa_littlefs_usage_count(fs, calloc, free, &used); }
''')
        (cmake / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.20)
project(usage_adapter C)
add_library(driver OBJECT "{source}" "{library / 'lfs.c'}")
target_include_directories(driver PUBLIC "${{CMAKE_CURRENT_SOURCE_DIR}}/public")
add_library(header_smoke OBJECT header_smoke.c)
target_include_directories(header_smoke PRIVATE $<TARGET_PROPERTY:driver,INCLUDE_DIRECTORIES>)
set_source_files_properties("{source}" PROPERTIES
 COMPILE_FLAGS "-DLFS_CONFIG=lfs_config.h"
 COMPILE_DEFINITIONS "REVIEWED_SOURCE_CONFIG=1")
function(idf_component_get_property output component property)
 if(property STREQUAL "COMPONENT_LIB")
  set(${{output}} driver PARENT_SCOPE)
 else()
  set(${{output}} "{args.driver_dir.resolve()}" PARENT_SCOPE)
 endif()
endfunction()
function(idf_build_get_property output property)
 set(${{output}} "{sys.executable}" PARENT_SCOPE)
endfunction()
include("{COMPONENT / 'littlefs_usage.cmake'}")
cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY \\\"${{CMAKE_SOURCE_DIR}}\\\" CALL pxa_configure_littlefs_usage \\\"{COMPONENT}\\\")")
function(check_sources)
 get_target_property(sources driver SOURCES)
 file(WRITE "${{CMAKE_BINARY_DIR}}/sources.txt" "${{sources}}")
 get_source_file_property(flags "${{CMAKE_BINARY_DIR}}/pxa-littlefs-usage/esp_littlefs.c"
  TARGET_DIRECTORY driver COMPILE_FLAGS)
 get_source_file_property(defs "${{CMAKE_BINARY_DIR}}/pxa-littlefs-usage/esp_littlefs.c"
  TARGET_DIRECTORY driver COMPILE_DEFINITIONS)
 if(NOT flags STREQUAL "-DLFS_CONFIG=lfs_config.h" OR
    NOT defs STREQUAL "REVIEWED_SOURCE_CONFIG=1")
  message(FATAL_ERROR "Generated driver lost its source compilation configuration")
 endif()
endfunction()
cmake_language(DEFER CALL check_sources)
''')
        subprocess.run(["cmake", "-S", str(cmake), "-B", str(cmake / "build")], check=True)
        subprocess.run(["cmake", "--build", str(cmake / "build"), "--target", "header_smoke"], check=True)
        sources = (cmake / "build/sources.txt").read_text().split(";")
        assert str(source) not in sources
        assert str(library / "lfs.c") in sources
        assert len(sources) == 2 and sources[-1].endswith("/pxa-littlefs-usage/esp_littlefs.c")


if __name__ == "__main__":
    main()
