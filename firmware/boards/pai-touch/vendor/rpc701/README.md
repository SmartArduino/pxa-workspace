# RPC701 / JL701 vendor drop

Vendored copy of the `rpc_701` component — the protobuf-over-UART RPC stack
that talks to the JL701 (VB701) audio coprocessor. The pai-touch speaker *is*
that coprocessor, so pai-touch is the only board that needs the stack; it
therefore lives inside this board component instead of `firmware/components/`,
and the two former `components/rpc_701` + `components/rpc_adapter` submodules
are gone.

| Path | Origin |
| --- | --- |
| `rpc_*.c` / `*.h`, `port_esp_os.*` | `components/rpc_701` of `MZ79-AI-S3` |
| `protobuf-c/` | upstream protobuf-c, vendored by the same component |
| `protobuf/` | generated `rpc_messages` protobuf bindings |
| `pai_touch_rpc701.cmake` | source list consumed by the board `CMakeLists.txt` |
| `UPSTREAM_README.md` | the component README kept as delivered |

## How it is used

`src/jl701_audio_link.cc` is the only consumer. It drives the UART transport
and uses `rpc_701_init` / `rpc_701_feed_rx` / `rpc_701_reset_rx`, `rpc_vb_init`,
`rpc_heartbeat`, `rpc_music_volume_set` and `rpc_sen_audio_pcm`. The rest of
the published API (Bluetooth, music library, alarms, OTA, wake words) stays
compiled but unused — the audio output wrapper in `include/jl701_audio_link.h`
is deliberately the only surface the board exposes.

The RPC headers stay private to this component (`PRIV_INCLUDE_DIRS`); nothing
outside `firmware/boards/pai-touch` may include them.

## Refreshing

Copy the files above from the vendor `rpc_701` checkout (upstream
`git.doit:arzhe/rpc_701.git`; `MZ79-AI-S3/components/rpc_701` keeps a synced
working copy), keep `pai_touch_rpc701.cmake` in step with the component's
`SRCS`, and rebuild. Regenerating `protobuf/rpc_messages.pb-c.*` needs the
protobuf-c compiler; the generator lives in the vendor repository under
`protobuf/tools/` and is intentionally not copied here.
