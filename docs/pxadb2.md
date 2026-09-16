# PXADB2 Binary Transport

PXADB2 is the binary transport used by the desktop product simulator. PXADB1
remains the USB Serial/JTAG protocol for firmware, so existing devices and
tools remain compatible.

## Framing

Every value is network byte order (big-endian). A frame has a fixed 10-byte
header followed by `payload_length` raw bytes:

```text
u32 payload_length
u8  frame_type
u8  flags             # reserved; must be zero
u32 sequence
bytes payload
```

The maximum payload is 1 MiB. Frame types are:

| Type | Name | Payload |
| --- | --- | --- |
| 1 | command | UTF-8 command line, without a trailing newline |
| 2 | response | UTF-8 `kind`, NUL byte, UTF-8 detail |
| 3 | upload-data | `u64 offset` followed by raw file bytes |

`upload-data` is limited to 64 KiB of file bytes. This avoids Base64 expansion
and allows a package upload to use much larger chunks than PXADB1 serial
frames.

## Session

Unix sockets rely on filesystem permissions. A TCP listener is disabled unless
explicitly requested and requires the first command to be:

```text
AUTH <token>
```

The service responds `OK` only after a constant-time token comparison. Then
the client sends `HELLO`; its response reports `protocol=2`, `binary=1`, and
the negotiated `max_chunk` value.

The current TCP mode authenticates but does not encrypt traffic. It is for a
trusted development LAN only; a future public-network transport must layer TLS
or another encrypted channel below these frames.

## Package Upload

The package workflow preserves the PXADB command contract:

```text
FSPUT <base64-path> <size> <sha256>
upload-data frames
PACKAGE deploy <app-id>
```

`FSPUT` returns `READY <remaining>`. Each binary data frame is acknowledged by
`READY` or `OK`; its offset supports retry and resume. `PACKAGE deploy` invokes
the same signed POSIX installation transaction used by the simulator's local
installer.

## Simulator Debug Commands

The local simulator also supports `SCREENSHOT`, `INPUT CAPABILITIES`,
`INPUT POINTER <DOWN|MOVE|UP> X Y 0`, `INPUT TAP X Y`,
`INPUT SWIPE X1 Y1 X2 Y2 DURATION_MS STEPS`, `INPUT KEY <KEY>`, and
`INPUT SYNC`. Screenshot replies begin with PNG `META`, followed by offset
`DATA` response frames and `OK`. The client exposes these as `pxadb screenshot`
and `pxadb input`; raw PNG bytes remain inside the local control bridge and
are Base64 encoded only in the response records required by the existing PXADB
command contract.
