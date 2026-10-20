# Firmware input acknowledgements and log timing

Firmware input ACKs include these semicolon-separated fields:

```text
event=529;device_us=676885788;log_ms=678430;status=processed
```

`device_us` is the ESP timer in microseconds. `log_ms` is the current 32-bit
millisecond clock used by framed `LOG` records. ESP-IDF's log clock includes
its own startup base and FreeRTOS tick counter; it is not necessarily
`device_us / 1000`. The clocks can have different offsets. Screenshot
completion timestamps remain in the `device_us` domain.

`log_ms` is an optional, backwards-compatible ACK extension. Tools requiring
precise log boundaries must require this field or use a separate observed
application marker. They must not silently substitute `device_us / 1000` when
testing older firmware. For a short test, a log is strictly after a marker when
`0 < ((timestamp_ms - log_ms) & 0xffffffff) < 0x80000000`. A record in the same
millisecond is ambiguous and must not prove subsequent progress. Deduplicate
replayed records by device timestamp and payload, and restrict results to the
intended app and activation.

`status=processed` means that the command executor accepted the command.
For controller input, the board adapter has accepted the state. It does not
prove that the Host, Guest or game logic has consumed it. `INPUT SYNC` orders
commands through that executor; it does not drain the board, Host or Guest
queues. Test controller press/release events in the Guest and actual game
movement/menu response separately. For press duration, distinguish the
requested interval, ACK interval and observed Guest interval.

GameKit retains up to eight distinct pending remote controller states. Repeated
identical states are keepalives and use no additional slot. The board posts
one state per input poll, acknowledging its local head only after the Host
accepts it. A full board queue rejects a new distinct state; PXADB's existing
failure handling then cancels synthetic input. Explicit cancellation, timeout
and physical takeover also clear pending remote states. A Host post already
in flight may still complete, but cannot consume the replacement queue.
These semantics do not preserve the original timing of an injection delayed
by a stalled UI task.

The main-loop probe refuses to start when any package is active. It checks
device identity and log-clock support before sending interactive input, and
does not stop an existing app during failed preflight cleanup:

```sh
python3 -m unittest discover -s tools/game-perf -p test_main_loop_probe.py -v
python3 firmware/boards/doit-gamekit/tests/run_controller.py
```

The latter runs the queue model, the actual board reporting method and the
actual firmware ACK encoder under ASan/UBSan. It does not connect to hardware.
