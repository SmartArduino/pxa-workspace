# PXADB2 Binary Transport

PXADB2 is the binary transport used by the desktop product simulator. PXADB1
remains the firmware protocol over USB Serial/JTAG, UART or board-initialized
TinyUSB CDC (ESP-Mosaico), so existing devices and
tools remain compatible.

## Firmware binary upload batches

Firmware can advertise `fs_bulk_cobs=1;max_bulk=2048` during PXADB1 HELLO.
This is an optional binary upload extension; commands and replies retain their
existing text framing. After ordinary `FSPUT`, the client sends:

```text
FSBULK <confirmed-offset> <decoded-byte-count> <crc32-hex>
```

Only after `CONTINUE <decoded-byte-count>` may it send a COBS-encoded body and
one zero delimiter. The decoder accepts exactly the admitted size and checks
the IEEE CRC-32 before writing anything. `READY` reports the remaining file
bytes; final `OK` still follows the existing complete on-disk SHA-256 check
and publication. A batch acknowledgement is not a persistent resume checkpoint.

The whole encoded batch fits in the transport receive ring. The worker receives
it before entering filesystem writes, avoiding Flash write stalls during a
batch. Its temporary buffer is allocated in PSRAM and released on completion,
error, timeout or session cleanup. Invalid and oversized bodies are drained
through their zero delimiter without entering the command parser. After a
partial/disconnected transfer, the client sends zero and newline before HELLO;
old firmware ignores that line. HELLO retains the existing incomplete-upload
cleanup policy; resume within a session uses the confirmed FSPUT offset.

The client negotiates this path automatically. `PXADB_BULK=0` selects the old
Base64 path for comparison. A rejected or lost batch can fall back to checked
text chunks at the confirmed offset. Socket PXADB2 remains unchanged.

## Framing

### Negotiated firmware data mode (PXADB1 extension)

Firmware with `fs_fast=1` accepts `FSFAST <requested-chunk>` after `FSPUT` or
`FSINSTALL`. It replies `OK chunk=N;timeout_ms=10000;window=1`. N is 256–3840,
bounded by the request, current free heap (at most one quarter), largest free
allocation, and the receive ring. PSRAM is preferred; small internal buffers
are allowed. This allocation lasts only for that transfer. The old `FSBULK`
path remains available with `PXADB_FAST=0`.

Each data frame is COBS encoded and terminated by zero. Its decoded layout is
little-endian (independent of the socket PXADB2 framing below):

```text
"PXD1" | sequence:u32 | offset:u64 | data_size:u32 | data | crc32:u32
```

The IEEE CRC covers the header and data. `data_size` must be nonzero, at most
N, and within the remaining upload; offset must equal the confirmed position.
One `READY remaining` or final `OK` follows each complete verified frame, using
its sequence. There is no per-frame `CONTINUE`. Only one frame may be in flight.
The maximum encoded 3840-byte frame plus delimiter is 3881 bytes, below both
4096-byte receive rings. Final SHA-256 and streamed package signatures remain
mandatory. On fast-frame failure the candidate is aborted; fallback starts
again with an independently checked transfer.

Completion, failure, explicit cancellation and ten seconds without bytes
release the data buffer. A partial invalid/expired frame drains to its zero
delimiter before any command can execute. At a frame boundary an empty frame
cancels; the client sends two zeros and `FSABORT` to recover either state,
including Ctrl-C and an uncertain negotiation ACK. HELLO also resynchronizes.
Progress callbacks may update the UI but must not send text commands during
this mode; the Python client rejects such calls before writing any bytes.
No USB PHY handoff, re-enumeration or filesystem unmount is involved. A cable
disconnect without an orderly close is recovered by the timeout or next HELLO.

### Socket framing

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

On serial USB CDC, the client sends separately sequenced PING keepalives while
waiting for PACKAGE deploy. This wakes pending IN responses after long flash/
filesystem operations; PING replies never count as the installation result.
Deploy still waits for its original OK/ERR sequence. If that result is lost,
the timeout reports that installation may already have completed and recommends
checking PACKAGES/logs. It does not diagnose an already-established session as
PXADB being disabled.

Before staging a local package, the client reads INFO storage metrics when the
firmware exposes them. Its LittleFS estimate includes the uploaded source, the
decoded installation files, file/directory metadata and commit headroom. An
upgrade must keep the current installed files until commit, so their space is
not counted as available. Insufficient space fails before any upload or inbox
mutation and reports the estimated deficit. Older peers without storage metrics
retain the existing behavior. This check is an estimate, not a reservation;
device signature verification and installation checks remain authoritative.

### Firmware receive lifetime and storage

Firmware `INFO` can additionally advertise `package_receive=1` and
`package_directory_adopt=1`. These are optional host capabilities; a simulator
or older firmware without them keeps its existing staging behavior.

On a capable host the CLI/GUI sends files into
`pxa-state/inbox/.receiving-<app-id>` (or `.receiving-<app-id>.pxa`). This reserved
candidate is invisible to the app catalog. Installation keeps one connection
and finishes with `PACKAGE deploy <app-id>`: the host authenticates the complete
candidate, checks its actual App ID, publishes it, and installs it in the same
command. An explicit `pxadb package stage` instead uses `PACKAGE publish
<app-id>` to retain a verified offline source for later installation.

Directory installation adopts the authenticated inbox tree into the existing
slot transaction by rename. It avoids a second copy of the decoded payload;
the transaction still verifies every incoming file before replacing current.
The admission estimate therefore reserves commit headroom rather than another
full directory payload. Container installation still needs the compressed
source alongside decoded incoming files: Container 0.1 authenticates the entire
encoded stream before decompression. This is not HTTP streaming decompression.

`FSABORT`, graceful `BYE`, a new `HELLO`, the firmware's 30-second inactive
control-session timeout, and cold startup discard unpublished receive sources.
The single in-flight file uses `/.pxadb-upload`, which is removed at those same
boundaries and after file checksum/write/commit errors. An abruptly disconnected
device can retain a partial file until timeout, reconnection, or reboot; it is
never visible as an installable app. CLI/GUI error and cancellation cleanup is
best-effort; host cleanup handles a lost connection.

Failed deployment removes its staging copy and recovers its slot transaction.
Cold startup recovers interrupted transactions before scanning apps, then
removes obsolete previous/session copies and legacy incomplete inbox uploads.
An unrecoverable transaction is retained and reported to preserve the last
recoverable application. Installed apps and private data are not temporary
files. Successfully published explicit staging sources survive disconnect and
reboot until consumed or explicitly removed.

## Simulator Debug Commands

Simulator application management uses `PACKAGE enable <app-id>`,
`PACKAGE disable <app-id>`, `PACKAGE clear-data <app-id>` and
`PACKAGE uninstall <app-id>`. A live full-system UI owns the operation and stops
the app before changing files. `OK accepted` acknowledges the request;
completion updates the app list and posts a system toast. Without a live control
socket, the offline installer helper returns `OK completed` after the change.
Errors from a live owner never trigger offline deletion. Uninstall removes the
user package and its private data; clear-data keeps the package. Disable state
persists across restarts and replacement installs. Built-in system apps cannot
be uninstalled. `PACKAGES` reflects the persistent enabled state.

The local simulator also supports `SCREENSHOT`, `INPUT CAPABILITIES`,
`INPUT POINTER <DOWN|MOVE|UP|CANCEL> X Y ID`, `INPUT CANCEL`, `INPUT TAP X Y`,
`INPUT SWIPE X1 Y1 X2 Y2 DURATION_MS STEPS`, `INPUT KEY <KEY>`, and
`INPUT SYNC`. Screenshot replies begin with PNG `META`, followed by offset
`DATA` response frames and `OK`. The client exposes these as `pxadb screenshot`
and `pxadb input`; raw PNG bytes remain inside the local control bridge and
are Base64 encoded only in the response records required by the existing PXADB
command contract.
Read `max_pointers` from `INPUT CAPABILITIES` before choosing an ID. The current
product simulator supports five independent contacts (IDs 0–4); the bridge
queries the running owner instead of assuming the limit from its screen profile.

## Firmware performance diagnostics

Firmware HELLO advertises `perf-raster,heap-local` when test control is enabled.
`PERF START` / `PERF STOP` record bounded raster and distinct FrameId LCD-complete
intervals. Check both overflow counters before deriving displayed FPS.
`PERF INPUT` drains the existing sampled-pointer latency counters, returning
`guest`, `present` and `visible` total microseconds, count, p95 and maximum in
three `DATA` frames followed by `OK`. Start an explicit `PERF START` window,
read `PERF INPUT` to clear startup/history, apply input, then read it at the end
and release the probe with `PERF CLEAR`. During this window the periodic display
consumer leaves the counters untouched; without it, a read may only cover the
remainder since the display's last drain. This reuses the optional raster probe
(2112 bytes on pai-touch), with no new permanent counters or persistent setting.
All stages start at the sampled input timestamp: `present` ends at submission
to the render pipeline; `visible` ends at completed LCD transfer. Captures must
also prove that the game surface is visible and that input changes pixels below
the FPS overlay. Counts can differ because delivery and presentation coalesce.
`tools/measure-device-game-input.py` performs that check; it does not measure
physical touch scanning delay before the timestamped synthetic sample.
`MEMORY START` resets ESP-IDF allocation minima for the current measurement;
`MEMORY STOP` returns the snapshot before restoring the boot-wide minima.
This includes allocations freed between snapshots, without a polling task or
sampling buffer. Do not treat boot-wide minima or Surface historical peaks as
the peak of the current app. Separate launch and warmed steady-state windows,
and always stop the local monitor when a measurement fails.

### 流式安装 PXA 0.2

设备的 HELLO / INFO 中 `package_stream=2` 表示支持流式安装。Host 用
`FSINSTALL <base64-path> <container-bytes> <sha256>` 打开
`pxa-state/inbox/.receiving-<app-id>`，随后沿用 FSBULK / FSDATA2 的已确认偏移。
输入是完整 0.2 `.pxa` 字节流，输出是接收目录；设备不保存完整压缩包。
接收器先验证签名索引，再逐块验证编码数据、解压写入并验证文件摘要。
完整容器的传输 SHA-256 也必须匹配，最后才返回 OK。

DEFLATE 要求 `package_codecs` 包含 `2`，解压块大小不得超过
`package_max_block`；客户端在创建接收目录前检查这些能力，即使设备没有
存储余量遥测也执行检查。内存分配失败返回 `ERR insufficient_memory`，
清理本次接收状态并保留原安装；客户端提示关闭其他应用或重新打包为
4096 字节分块。`--via auto` 遇到此错误不再尝试串口重传。传输帧可以协商
缩小，包内已签名的解压块大小则需要重新打包才能改变。

`PACKAGE deploy <app-id>` 沿用目录认证和事务重命名。连接中断后的清理只删除
未提交的接收目录；设备重启后从头传输，不承诺跨重启断点续传。活动会话内
丢失批次确认可以查询同一个 FSINSTALL 的剩余字节并继续。

`pxadb install` 自动识别 0.2 包和设备能力，空间预检按展开文件、索引和事务
余量估算。旧设备使用 0.1 包。商城对 0.2 的“下载并安装”采用相同的接收器，
同卷缓存目录通过 rename 提交；离线下载仍保存完整安装包。运行时继续读取
普通文件，不引入资源访问时的解压。打包参数和索引格式见
`deps/pxa-system/spec/draft/container.md`。

支持 `package_stream_volume=1` 的固件允许直接在指定卷接收：

```sh
pxadb volumes --port /dev/ttyACM0
pxadb package install app.pxa --volume sd --via auto --yes --port /dev/ttyACM0
```

`--volume` 接受 `sd`、`internal` 或 `volumes` 列出的 mount epoch；同类卷存在
多个候选时必须指定 epoch。要求本地文件采用 PXA 0.2。目标卷同时保存展开
文件和安装事务，不经过内部 Flash 的 inbox。应用运行时读取普通文件。
`--via serial` 使用当前线缆；`--via usb` 要求已连接原生 USB；`--via wifi`
强制局域网传包。默认 `auto` 优先原生 USB 二进制模式，USB 转串口设备则
尝试网络，失败时清理候选后走线缆。未指定 `--volume` 时沿用 inbox 路径。
自动选择依据 HELLO 的 `transport=uart|usb-jtag|usb-cdc`，不会改变 USB
描述符、占用其他设备端口或将已挂载的文件系统暴露为 USB 磁盘。

网络传输需要设备已联网，且设备能够访问电脑的局域网地址和临时 TCP 端口。
电脑仅在通往设备的接口上临时提供该文件，用随机 URL 和设备 IPv4 限制访问，
退出或取消即关闭服务。控制命令仍通过串口发送；签名、每块及每文件摘要和
整个容器 SHA-256 的校验与串口路径相同。网络中断不提供跨会话续传。

协议扩展：

- `VOLUMES` 的 DATA 增加 `relative`、`state`、`unit`、`private_bytes`，用于
  目标选择和容量估算。`relative=.` 表示主挂载根；空值表示 PXADB 不可访问。
- `FSINSTALL <base64-path> <bytes> <sha256> <epoch>` 接收至该卷的
  `PXA/cache/store/.stream-<8位小写十六进制>`，持有此 mount epoch 至提交或取消。
- `NETINFO` 返回当前默认网络接口的 `ipv4`、`netmask`，未连接时返回错误。
- `FSFETCH <base64-http-url>` 仅用于尚未接收字节的 FSINSTALL，要求能力
  `stream_fetch=1`；禁止自动重定向，完成全部校验后返回 OK。
- `PACKAGE install-volume <epoch> <base64-path>` 提交已完成的候选目录。
  目标必须匹配当前接收器，提交后释放目录和卷租约。

`FSABORT`、会话结束和下次启动会清理尚未提交的私有接收目录，包含接收完成
但尚未安装的候选。提交前仍执行正常签名认证与安装事务。旧版本应用的回滚
副本、私有数据初始化和文件系统分配粒度仍占用空间，预检会计入这些开销。
