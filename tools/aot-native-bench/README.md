# Native / AOT device comparison

This standalone diagnostic runs the same `kernels.c` as ESP-IDF native code
and as WAMR AOT. Use the SDK's pinned compiler and `config/aot-targets.json`
recipes; this diagnostic does not update either LLVM or WAMR.

Eight kernels cover 32/64-bit integers, single-precision arithmetic, branches,
indirect calls, a 4 KiB working set, a 256 KiB working set, and explicit
unaligned byte access. Both paths use the same guest PSRAM buffer. Each
sample resets its input outside the timed interval and compares the return
value and a checksum over the whole buffer. `host_check.c` additionally checks
every output byte against native code at several iteration counts.

Native and Wasm compile with `-O3 -ffp-contract=off -fno-builtin`. Native code
uses the installed ESP-IDF GCC compiler and mapped flash; AOT code uses the
SDK's existing LLVM backend and executable PSRAM. Those compiler and code
placement differences are part of the measured deployment configuration.
This is a kernel comparison, not an application FPS or UI benchmark.

The task runs on Core 1 at the board's fixed frequency, without starting
display, network or audio services. It calibrates each native kernel to at
least 10 ms, warms both paths, then alternates native/AOT order over seven
pairs per boot. The runner performs two boots by default and reports the
median paired AOT/native time ratio plus time per iteration.

Build `kernels.c` once with the resolved WASI SDK using `-nostdlib`,
`--no-entry`, a 32 KiB guest stack, and equal initial/maximum memory of 1 MiB.
Compile that identical Wasm file to each architecture using its current SDK
AOT recipe. Configure this IDF project with `IDF_TARGET`, an artifact-local
`SDKCONFIG`, and `BENCH_AOT` pointing to that architecture's generated AOT.
The target-specific defaults select 240 MHz/80 MHz PSRAM for S3 and
320 MHz/250 MHz PSRAM for S31.

Before running, read the actual device partition table and back up its entire
factory application partition. Do not substitute a repository firmware binary:
the installed firmware and partition layout may differ. The runner requires
exactly one factory application and checks the backup size and image magic.

```sh
python tools/aot-native-bench/run_device.py \
  --chip esp32s31 --port /dev/ttyUSB0 \
  --build-dir "$task_out/esp32s31/build" \
  --backup "$task_out/esp32s31-restore.bin" \
  --partition-table "$task_out/esp32s31-partitions.bin" \
  --output "$task_out/esp32s31/device"
```

Use the ESP-IDF Python environment containing esptool and pyserial. The runner
only writes the detected application offset, restores the actual backup in
`finally`, and retains esptool's flash hash verification log. It does not write
the bootloader, partition table, NVS or application data partitions. After a
power loss or forced process termination, restore the saved partition manually.

All build trees, binary backups, logs and results belong under
`local/artifacts/native-aot-bench/<YYYYMMDD-HHMMSS>/`.
