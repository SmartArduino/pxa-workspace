# LCD transfer benchmark

Build from the project root, using the installed SDK and development signer:

```sh
tools/app.sh build display-benchmark \
  --source-root firmware/components/pxa_board_common/tests/apps \
  --board doit-gamekit-1 --aot-only --output local/app-output/display-test
```

Install the resulting package with PXADB, then run `tests/measure_display.py`
with the explicit board, stable serial port, expected MAC and an ignored artifact
output directory. The helper restarts only this benchmark, measures completed
LCD transfers and captures a frame after the measurement window. It always
cancels diagnostic input before disconnecting. Use `--existing` to measure an
already running game without restarting it.

Both mapped buffers retain a precomputed pattern; only a narrow moving bar is
redrawn on each tick. Red/green/blue/yellow corners expose orientation and wire
byte order. Fullscreen hides system bars so the direct Surface path can run.
The source frame period is 16 ms (up to 62.5 submissions/s); a benchmark reaching
that limit does not establish a higher raw transport ceiling. LCD completions
are neither emulator simulation Hz nor optical panel scan Hz. Observe the
physical screen to assess tearing; a framebuffer screenshot cannot prove its
absence. Keep package identity and source identical for before/after tests.
