# Host video codecs

ESPP4-A registers a video backend before activating applications. Boards
without a registered backend expose the Video service with an empty capability
list. The same application imports work with other decoders, including
hardware implementations. See the [Video contract](../../../deps/pxa-system/spec/draft/video.md).

### Espressif prebuilt decoder

ESPP4-A, DOIT GameKit 2 and ESP-Mosaico support Espressif's local `esp_openh264` 1.0.0
package. Pass its directory to the firmware build as
`-DPXA_ESP_OPENH264_DIR=/path/to/esp_openh264`. Fresh configurations
pick the memory profile for the board. Update saved configurations with the
settings below.

- ESPP4-A uses `CONFIG_ESP_OPENH264_PROFILE_BALANCED=y`,
  `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` and
  `CONFIG_PXA_RUNTIME_TASK_AFFINITY=1`. Queued packets run on core 0.
  The prebuilt library's second stage runs on core 1.
- DOIT GameKit 2 uses the balanced memory profile and flash XIP. Its TLS
  allocator uses PSRAM to leave internal memory for Wi-Fi and LCD DMA. In
  `MAX_SPEED` mode the profiled S31 archive keeps selected CABAC and motion
  compensation code in instruction RAM and places less frequently used decoder
  code in flash, freeing internal SRAM for per-macroblock data. The linker
  selects this layout by archive hash. An unrecognized archive uses its own
  layout, as does a build configured with `-DPXA_GAMEKIT_COLD_VIDEO_IN_FLASH=OFF`.
- ESP-Mosaico uses `CONFIG_ESP_OPENH264_PROFILE_MINIMAL=y` and
  `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` with PSRAM XIP disabled to leave
  memory for decoding and playback. The board places the P/B CABAC
  macroblock loops and residual parser in internal RAM. Large RGB565 reads
  into a padded Surface can use DMA2D while preserving the caller's stride.
  For installed boards, see the
  [partition migration guidance](../../boards/esp-mosaico/README.md).
- GameKit 2 uses hardware AES for HTTPS media refills. Set
  `CONFIG_MBEDTLS_HARDWARE_AES=y` when updating a saved GameKit 2 video
  configuration. ESP-Mosaico and ESPP4-A use software AES to preserve DMA
  memory.

Pass an empty `PXA_ESP_OPENH264_DIR` to select the standard board build.

For native GameKit 2 scanout at 640×360 → 320×180, the packer averages
each 2×2 block in the luma and chroma planes before the PPA converts and
rotates the picture. Each Surface slot holds 86,400 bytes of packed YUV.
Set `PXA_GAMEKIT_BOX420_360=OFF` to use full-resolution PPA input at that size.

The adapter validates Annex-B input against the session limits and passes
the decoder's reordered timestamps through the Video service. On S31, a
Host-produced Surface can use strided YUV420 planes for hardware downscales
of inputs of at least 640×360 pixels. A software downscale also uses planes
when its destination has at most one third of the source area. The S31 PPA consumes
packed YUV420 directly and scales during BT.601 conversion. This also
applies at half size, where the YUV444 input path can produce incorrect colors.
Unaligned source dimensions also use the plane path. When the decoder returns a
pinned reference picture, a bound Surface can consume it while the worker
decodes the next access unit. S31 completes the Surface write first for
pictures above 256 Ki pixels to leave room for audio, DASH and the DPB.
It packs and converts 32 source rows at a time into the same RGB565 Surface.
An 852×480 input uses 41,472 bytes of packed scratch shared between slots.
Sequential pixel reads materialize RGB565 on demand. Other aligned
S31 output uses the codec's packed RGB565 conversion. ESPP4-A uses
packed RGB565 for those dimensions up to 172,800 pixels, including 360×480.
Larger ESPP4-A frames and other dimensions use strided YUV with PPA color
conversion and a CPU fallback. On P4 v3, aligned packing rows let PIE process full 32-pixel blocks even
when a cropped frame ends in a short scalar tail. For planar sessions up to
854×480 pixels in area,
ESPP4-A can overlap PPA output with the next H.264 decode. The pipeline
charges one extra RGB565 buffer to the video allocator and retains the
frame until DMA completes. Reset discards queued output and drain emits
it in timestamp order. With insufficient memory, conversion stays synchronous.
On ESP32-S31 and supported ESP32-P4 revisions the Host creates a pinned
core-0 worker before enabling the vendor's PIE kernels. The prebuilt decoder
runs its second stage on core 1. Synchronous headers, input and drain lend
their packet until the worker completes. Asynchronous submission owns a
padded copy. If the worker cannot be created, the decoder starts in scalar
mode. For PPA-eligible sessions the optional preferred-size query reports
dimensions supported by the PPA scale registers at 1/16 scale steps.
On ESPP4-A and GameKit 2 a bound Surface can retain packed YUV420 in its producer slot.
The display lease keeps those bytes alive after the decoder releases its picture.
For unobstructed playback the board supplies the physical framebuffer and its
rotation, and one PPA operation converts, scales and rotates the video into it.
An RGB consumer such as the compositor can materialize the logical RGB565
rectangle once and reuse it for the lifetime of that frame. If staging is
unavailable the decoder uses the existing RGB Surface output.

Fresh ESPP4-A, DOIT GameKit 2 and ESP-Mosaico builds default to `MAX_SPEED`.
`MAX_SPEED` skips deblocking, uses simpler interpolation, and can
introduce visible inter-frame drift until the next IDR. Override either
default with `-DPXA_ESP_OPENH264_DECODE_MODE=FAST` or
`-DPXA_ESP_OPENH264_DECODE_MODE=MAX_SPEED`. Existing build directories retain
their configured value. This setting is independent of the build-time memory
profile.
The vendor library manages its own decoder heap. PXA charges output,
conversion, and audio buffers to the application resource budget.

The binary audit pins the supplied archives and license notice. Distribute
the package notice and the full Apache-2.0 and BSD-2-Clause licenses with
firmware using the prebuilt decoder. [Cisco OpenH264 2.6.0 license](https://github.com/cisco/openh264/blob/v2.6.0/LICENSE).

### Source decoder

The H.264 decoder is the portable decoder/common subset of Cisco OpenH264
2.7.0, commit `1bca62311e447fd9c39fc93de2c4b96cd5ce971c`, BSD-2-Clause.
A pinned BSD-2-Clause patch set from the upstream thread-correctness issue
repairs deblocking, reference lifetime, row readiness, destination ownership
and EOF synchronization. The patch and its full commit provenance are in
`vendor/openh264-patches` and `vendor/openh264/SOURCE.json`. Vendor files retain
their original copyright notices and full LICENSE; the SHA-256 inventory
describes the resulting source files. This subset contains portable decoder
sources and has no third-party link dependencies.
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
