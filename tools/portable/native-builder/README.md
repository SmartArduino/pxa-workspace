# Linux DevKit native runtime builder

Maintainer-only layer over `deps/pxa-system/tools/release/Dockerfile`. Build that
base first and pass its inspected immutable image ID as `BASE_IMAGE`. Supply
`sdl2.tar.gz` and `curl.tar.gz` in the build context, with top-level `sdl2/` and
`curl/` directories. Compare input digests with `sources.json`; retain upstream
licenses. Release source archives are kept with the signed release assets, not
in this directory.

Build this Dockerfile, then configure the desktop simulator with
`CMAKE_PREFIX_PATH=/native`, `SDL2_DIR=/native/lib/cmake/SDL2`,
`CURL_LIBRARY_RELEASE=/native/lib/libcurl.so`,
`CURL_LIBRARY_DEBUG=/native/lib/libcurl.so` and `CURL_INCLUDE_DIR=/native/include`.
Run the normal source license gate and CTest suite. Seed copies of
`/native/lib/libcurl.so.4` and `/native/lib/libSDL2-2.0.so.0` in a `native-runtime/`
subdirectory of the simulator build before assembly so distro CMake cannot pull
a different curl into the private dependency closure.

Mount this directory at `/release-runtime` and use `assemble_parallel.py` in
place of `assemble.py`, with the same arguments. It preserves the upstream tar
layout and signing scheme, verifies the actual ELF closure before archiving,
and includes the runtime inventory, recipe and upstream notices. The base PXA
source tree must remain clean and committed. Optional Python GPL bindings and
Pillow Raqm/TIFF/ImageQuant are excluded; normal PXA PNG/JPEG/WebP and FreeType
font tooling remain available. Native Linux audio requires PipeWire. X11 or
Wayland and the normal operating-system loader/glibc remain host requirements.
This does not permit arbitrary LGPL libraries in the shipped kit.

Verify the final archive in an offline, read-only container without the source
tree: build all four targets, run the simulator and install from its signed
split manifest. Keep test output and build logs under ignored `local/artifacts`.
