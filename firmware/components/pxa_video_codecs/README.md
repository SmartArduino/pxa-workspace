# Host video codecs

ESPP4-A registers this optional backend before activating applications. Other
boards retain the standard Video service with an empty capability list. A board
may supply a different backend, including hardware implementations, without
changing application imports. See the [Video contract](../../../deps/pxa-system/spec/draft/video.md).

The H.264 decoder is the portable decoder/common subset of Cisco OpenH264
2.7.0, commit `1bca62311e447fd9c39fc93de2c4b96cd5ce971c`, BSD-2-Clause.
A pinned BSD-2-Clause patch set from the upstream thread-correctness issue
repairs deblocking, reference lifetime, row readiness, destination ownership
and EOF synchronization. The patch and its full commit provenance are in
`vendor/openh264-patches` and `vendor/openh264/SOURCE.json`. Vendor files retain
their original copyright notices and full LICENSE; the SHA-256 inventory
describes the resulting source files. No vendor encoder, assembly source or prebuilt OpenH264 binary
is included. There are no third-party link dependencies in this subset.
This is a source build; Cisco's separate binary distribution license is not
claimed for it.

CMake creates an ESP-IDF overlay in the build directory: pthread condition
variables replace unavailable unnamed semaphores; two definitions match newlib's int32_t type; allocation
uses the owning application's existing resource budget. Scheduling stays under
Host control. A valid AVC VUI buffer bound limits picture allocation, including
reordering, output-queue and worker headroom; absent or invalid bounds retain the codec's
conservative allocation. Picture-row readiness uses release/acquire flags with
a bounded task sleep, avoiding hundreds of native RTOS mutex/condition
allocations; worker lifecycle events retain normal synchronization. The overlay
leaves the vendor snapshot intact.
H.264 defaults to single-thread decoding on ESPP4-A. Other board integrations
can evaluate `-DPXA_VIDEO_H264_WORKERS=2`; two workers are then pinned to
the HP cores. Extra concurrency increases picture storage and is not always
faster on a shared PSRAM bus.

P4 revision 3 builds add local Apache-2.0 PIE assembly kernels for 16-pixel
averaging, 8/16-pixel horizontal and vertical AVC half-pel interpolation, and
8-pixel chroma interpolation. The generated `mc.cpp` overlay selects these
kernels without modifying the vendor snapshot. Their rounding and clipping
match the portable implementation; smaller blocks and other chips retain C.
The kernels restore the incoming PIE control and shift registers. The PPA
input packer also uses aligned PIE unzip/interleave operations where possible.
H.264 capabilities still report software decoding: these instructions and PPA
accelerate portions of the pipeline, rather than supplying an H.264 decoder.

OpenH264's decoder/trace/thread control objects use checked placement creation;
reference pictures, bitstream storage, control objects and output images use the
app's memory quota. Small controls may use internal memory while preserving
Host headroom; picture storage remains in PSRAM. Allocation failure returns
an error without throwing. Codec workers inherit the owning allocator and
are joined before its lifetime ends. Their bounded infrastructure uses at most
two 32 KiB PSRAM stacks; a session falls back to single-thread decoding if
workers cannot be created or the first picture cannot start. Joining precedes
all context/picture teardown, including reset and early close. Public codec calls remain serialized on the runtime
thread.

MJPEG uses the existing esp_new_jpeg software decoder. Optional AAC-LC ADTS
audio uses the existing esp_audio_codec decoder with its scoped allocation
hooks. Their Espressif Modified MIT grants are limited to Espressif products;
the workspace's binary license and allocation audits still apply. Decoding
audio only returns PCM. Playback goes through Audio and its permission check.

Before allocating H.264 reference pictures, the adapter checks every supplied
SPS for profile, bit depth, chroma, progressive coding, dimensions and level.
Baseline/Main/High declarations through Level 5.1 are accepted within the
advertised size and memory limits. A high level declaration on a small video
does not grant larger dimensions, more than 16 reference frames, higher bit
depth or interlaced coding.
The session's declared maximum dimensions remain an upper bound after stream
changes. Resolution limits describe accepted streams within memory quotas;
frame rate recommendations do not guarantee real-time playback.

P4 PPA converts packed YUV420 into RGB565 with BT.601 limited range. Input
packing and DMA storage are budgeted and aligned to 128 bytes. Odd images,
unavailable PPA resources or rejected operations use the CPU converter.
The CPU RGB565 path uses BT.601 limited-range coefficients and lookup tables shared
across a 2×2 chroma block. Native regression checks exhaust the complete YUV
byte domain against scalar rounding and clipping, including padded/odd images.
Single- and two-worker output are compared over every visible Y/U/V byte,
including separate parameter packets, repeated runs and EOF. IDR jumps are
checked against a fresh decoder with reordered B pictures; a
player need not reinitialize the decoder just to skip to the next IDR.

Native regression checks (from the workspace root):

```sh
video_results="local/artifacts/video/$(date +%Y%m%d-%H%M%S)"
cmake -S firmware/components/pxa_video_codecs/tests -B "$video_results/decoder"
cmake --build "$video_results/decoder" -j
ctest --test-dir "$video_results/decoder" --output-on-failure
```

`tests/device-probe` is a PXA acceptance app for the actual board. It decodes
the same 24 High Profile frames, a JPEG and two AAC frames, then verifies an
oversized SPS is rejected. Each stage emits a `VIDEO_PROBE` log result. Build
and package it from a temporary copy under `local/artifacts`, so SDK caches,
signing/build metadata and logs do not accumulate beside the test sources.

`tests/pie_device_test.cpp` is a native on-device differential check for the P4
kernels. It covers every averaging byte pair, unaligned and aliased buffers,
AVC filter clipping, all 64 chroma fractional offsets, output canaries, and
control-register preservation with a nondefault rounding mode and shift.
