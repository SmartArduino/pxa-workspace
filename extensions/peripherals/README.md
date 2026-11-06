# PXA peripherals extension 0.7

`org.doit.peripherals` provides logical GPIO, I²C, UART and module-slot discovery.
It is optional and separate from standard Sensor, Audio, Storage and Haptics.
Applications use `<pxa-ext/peripherals.hpp>` and link `pxa::peripherals` after
including `sdk/Peripherals.cmake`. Host builds use `pxa_peripherals`.

Service/resource ID 33026 is allocated in this workspace's extension registry;
identity is UUID `26531fa8-55a1-446a-b917-316c58b06582`. Always start with
`co_await pxa::ext::peripherals::open(ctx)`: HELLO verifies this identity and
negotiates 0.1–0.7. RGB ports require 0.2 and vision streams require 0.3. The Host still accepts 0.1 clients,
hiding RGB ports from LIST and rejecting their OPEN attempts, while GPIO/I²C/
UART/slot behavior stays compatible. Clients limited to 0.2 cannot list or open
vision ports. Version ranges compare (major, minor) lexicographically;
a wider range is accepted when it contains the selected version. An unavailable service or mismatched identity means the extension
cannot be used. Keep it optional rather than declaring it as a required standard
service. The Host requires successful HELLO for each component activation.

All integers are little endian, carried in Core v1 envelopes. Completions start
with i32 status; failures contain only status. Sizes below exclude that status.

| Opcode | Request | Successful body |
|---|---|---|
| 1 HELLO | UUID[16], min-major/minor u16 each, max-major/minor u16 each | UUID[16], selected-major/minor u16 each |
| 2 LIST | empty | count u16, followed by port records |
| 3 MODULE_INFO | slot-port u16 | generation u32, flags u32, type/id/hardware-version/vendor u16 each, serial u32, name[32] |
| 4 OPEN | port u16, mode u16, parameter u32, address u16, reserved u16=0, permission u64 | handle u64 |
| 5 TRANSFER | handle u64, tx-size/rx-size u16 each, tx bytes | rx bytes |
| 6 FRAME_STREAM (0.4) | camera-port handle u64 | finite Core STREAM handle u64 |
| 7 CAMERA_PROFILES (0.5) | camera-port ID u16 | count u16, profile records |
| 8 JPEG_FILE_FRAME (0.6) | FILE u64, offset u64, length u32, first-cookie u64 | finite STREAM u64 |
| 9 CAMERA_PREVIEW (0.7) | camera-port handle u64 | IMAGE handle u64, Assets descriptor (20 bytes) |

A port record is id u16, kind u16, flags u32, name-length u16, then 1–31 ASCII
name bytes. Names are canonical permission scopes. At most 32 ports are returned.
Kinds: GPIO=1, I²C=2, UART=3, slot=4, RGB indicator=5, vision stream=6. Port flags: readable=1, writable=2,
reserved=4. Module flags: present=1, valid-descriptor=2, claimed=4,
unknown=8, I/O-or-descriptor-error=16. Discovery is cached and read only;
absence, unknown state, invalid CRC and a valid module are distinct outcomes.
Generation changes after rediscovery or a descriptor change. Claimed means an
application owns that slot's pins and automatic probing is suspended.

OPEN requires a foreground component and `org.doit.peripherals` permission
scoped to the exact enumerated name. Modes are input=1, output=2, I²C=3,
UART=4, slot-claim=5, RGB=6, read-only vision stream=7. Parameter is initial logical output level (0/1), I²C bus Hz,
UART baud, or packed RGB `0xRRGGBB`; otherwise zero. Address is a 7-bit address for I²C and zero for
other modes. Resources close on permission revocation, component stop or loss of
foreground ownership. Explicitly reset handles when entering the background.
A failed native open can retain an internal reservation until rollback finishes;
a retry may return Busy while its pins/address are being released. OPEN completes only after driver
preparation succeeds; preparation errors contain only a failure status. A Core
handle and its authority are reserved before starting native setup. Revocation,
component stop or foreground loss also cancel opens that are still preparing.

GPIO/UART use Core resource READ=1 and WRITE=2, returning a byte count or
WouldBlock without waiting. GPIO values are exactly one byte, 0 or 1; LED values
are logical on/off. RGB resources accept exactly three bytes (red, green, blue)
through WRITE, or `Port::rgb(r,g,b)`. Values are 0–255, without implicit gamma.
The board worker applies the latest accepted color; intermediate writes may be
coalesced. Closing, revocation or backgrounding queues black and retains the
physical reservation until cleanup completes. READ and TRANSFER are unsupported.
UART can return a partial write; retry the remaining bytes.
I²C uses asynchronous TRANSFER; tx and rx are each bounded to 256 bytes and at
least one must be nonempty. Success returns exactly the requested rx-size;
a malformed backend success becomes ProtocolError with no data. Combined write/read uses a repeated start. Input is
copied before the call returns; completion is delivered after worker I/O, with
at most four pending operations (opens and transfers together) and eight
handles per activation, including handles reserved for pending opens. Closing a
handle cancels delivery; native resources remain owned until in-flight I/O ends.
Accepted transfers are committed side effects; cancelling a request cannot
undo a transaction already accepted by the worker. Its token remains owned until
the reliable completion is delivered. Closing the port still cancels delivery.
I/O failure can mean a partial physical transaction; the Host does not retry
writes automatically. No raw flash, EEPROM provisioning, power rails or board
peripheral registers are exposed by this extension.

```cpp
namespace ports = pxa::ext::peripherals;
auto client = co_await ports::open(ctx);
if (!client) co_return std::unexpected(client.error());
std::array<ports::PortInfo, 32> available;
auto count = co_await client->list(available);
// Select a port by name/kind; request the exact scope from available[i].scope().
```

Keep large port arrays outside a coroutine frame on memory-constrained Guests;
the SDK writes descriptors into the caller's span. Hardware pin mappings and
reserved resources are documented by the board, not hard-coded in applications.
Module-specific LED matrix and audio accessory drivers remain separate work.
Watcher exposes `indicator.rgb`, `grove.i2c`, `camera.jpeg` and `vision.inference`;
Korvo exposes `indicator.rgb` and `camera.jpeg`. GameKit1 exposes the single-color
`indicator.status` GPIO-kind port.
See the board READMEs for reserved addresses and electrical constraints. The
`examples/indicator` app demonstrates discovery, exact permission scope, RGB
writes and foreground cleanup without requiring a vendor service at activation.

```sh
tools/app.sh build indicator --source-root extensions/peripherals/examples --target all
```

## Vision streams (0.3)

Include `<pxa-ext/vision.hpp>`. Select a readable kind-6 port by its exact scope,
request that scope, and open with mode 7 and zero parameter/address. Reading
`camera.jpeg` activates the camera; `vision.inference` invokes its board-specific
on-chip model. A board can omit either port. Hardware absence, invalid models,
busy devices and driver deadlines produce errors; a port record alone does not
promise that a model is installed. WRITE and TRANSFER are unsupported.

Every frame is a 24-byte header and exactly one payload. Header offsets:

| Offset | Field |
|---|---|
| 0 | ASCII `PXVF` (4 bytes) |
| 4 | Header version u16 = 1 |
| 6 | Format u16: JPEG=1, JSON=2 |
| 8 | Payload bytes u32, nonzero; JPEG ≤262144, JSON ≤8192 |
| 12 | Sequence u32 (wraps modulo 2³²) |
| 16 | Capture-completion monotonic microseconds u64, not UTC |

All integers are little endian. READ returns at most 256 bytes and may split
any header or payload. `FrameReader::header()` retains partial progress and
validates magic/version/format/size; then drain `read_payload()` into a
caller-owned buffer. WouldBlock preserves progress. Call `next()` only after
`complete()`. Close a malformed stream; scanning arbitrary payload bytes for a
new header is unsafe. JSON is UTF-8 board-specific inference output; Watcher
returns the SSCMA `INVOKE` event envelope (`type`, `name`, `code`, `data`), with
no image or unrelated identity/configuration replies. Consult its model schema
before interpreting boxes/classes. This is not a standard PXA Sensor service.

Native capture runs on a worker with one physical session. A complete frame
remains immutable until drained; backpressure prevents unbounded frame queues.
Backgrounding, permission revocation and close cancel delivery, retaining the
reservation until bounded in-flight I/O and cleanup finish. Frame and transport
buffers use the shared device memory budget. Applications must also release
their own buffers and ports when backgrounded.

## Camera profiles (0.5)

`Client::camera_profiles(port, output)` queries cached Host capabilities without
starting the sensor. A profile record is exactly 24 little-endian bytes, ordered
as ID u32, encoding/width/height/quality u16 each, max-frame-bytes/period-us/flags
u32 each. At most 16 unique nonzero IDs are returned, with exactly one default.
Encoding is currently JPEG=1. Quality zero means native or unspecified. Period
zero means unspecified, otherwise it is a suggested sampling interval in
microseconds, without a guaranteed frame rate. Flags are default=1 and
native-size=2. Native-size profiles may report width=height=0, meaning that the
sensor coprocessor chooses its current dimensions. Read the actual JPEG size.

Pass the selected ID as OPEN's parameter. Zero keeps the existing default
behavior. Nonzero camera parameters require negotiation of 0.5. Unknown IDs
fail before native setup. Change a profile by closing all frame resources and
the port, then reopening. Busy can persist briefly while the old worker releases
its buffers. Resource exhaustion is reported by OPEN, never silently replaced
with a lower resolution. No sensor registers or board GPIO appear in the App.

Profiles describe the format delivered to applications. A sensor's RGB565 or YUV input
can be encoded by its Host to JPEG. The API reports that delivered JPEG format.
PAI Touch currently offers three JPEG dimensions/quality combinations. Watcher
exposes its native Himax JPEG configuration without claiming arbitrary sensor
reconfiguration. Other encodings need an explicitly negotiated extension version
and an implementation on the selected board.

## JPEG frame resources (0.4)

`Client::frame(port)` borrows one complete JPEG from an open `camera.jpeg`
port. The returned move-only `FrameStream` is a standard readable Core STREAM
resource. Pass it to `ctx.assets().decode_image(std::move(frame), options)` to
keep encoded data and decoded pixels on the Host. Its READ strips the PXVF
header, returns the finite payload, and then EOF. WouldBlock preserves partial
header and payload progress. A port allows one frame resource at a time.
Raw port reads return Busy until that resource closes.

The frame inherits the camera permission and component ownership. Revocation,
backgrounding and component teardown close both resources. Closing a fully
consumed frame leaves the camera open for the next frame. Closing an incomplete
or malformed frame also closes its parent port because its byte position cannot
be reused safely. Reset and reopen the port before retrying. This operation is
unsupported when negotiation selected a minor version below 4.

The `examples/camera` app requests access after a Preview or Photograph button
press. Preview fits the source aspect ratio inside the display safe area and
keeps a single retained Host image. Photograph writes `capture.tmp`, closes it,
and atomically renames it to the next unused `photo-000001.jpg` style filename
in app-private storage. Existing photographs survive restarts and failed saves.
Preview does not write files. Stop or backgrounding releases the camera session.
Retrieve photographs with normal PXADB app-data tools.

```sh
tools/app.sh build camera --source-root extensions/peripherals/examples --target all
```

Native backends can append the optional `ready` callback. In that mode `open`
only reserves a device and queues worker setup; `ready` is a nonblocking cached
status query that returns WouldBlock, Ok or an error. No driver I/O belongs in
`ready`. The same four-operation bound covers deferred opens and transfers.
Older backend structures without this callback retain synchronous behavior.

Native backend authors transfer ownership of any non-null `open` output to the
Host, even when initialization reports an error. The Host calls `close` to roll
back partially initialized devices; a successful `open` must return a device.

The implementation is Apache-2.0 and adds no third-party dependency. Module
EEPROM format and board pin assignments follow Espressif's
[ESP-MOSAICO BSP](https://github.com/esp-mosaico/esp-mosaico-bsp/tree/7cbfb390638dcb93522fc3cde094b2d43cf907c2).

## File-backed JPEG frames (0.6)

`JPEG_FILE_FRAME` (opcode 8) takes an owned Core FILE handle (u64), byte offset
(u64), length (u32, 1–262144) and first ReadAt cookie (u64). The reply contains one finite Core STREAM
handle (u64). It reads only that range with cursor-preserving ReadAt, using a
1024-byte Host cache. This allows Host image decode of MJPEG AVI frames without
a Guest JPEG decoder or a whole-video allocation. The caller must validate its
container and frame bounds.

The foreground component must own the parent FILE. Every read, including cached
bytes, revalidates parent ownership and generation. Closing the frame cancels a
pending ReadAt but keeps the parent file open. Parent closure, permission
revocation or foreground loss terminates access. The stream has no independent
file authority. The C++ entry point is `Client::file_frame(file, offset, bytes, next_cookie)`.

The caller owns one monotonically increasing ReadAt cookie counter per file.
The SDK reserves `length` cookies before opening a slice, enough even for
one-byte short reads, so subsequent caller reads remain newer than Host reads.
Read operations on the same file must be serialized. WouldBlock retries reuse
the pending cookie and preserve the request. Cookie ranges must not wrap.

## Native preview images (0.7)

`Client::preview(port)` returns one immutable Core IMAGE. Native acquisition
runs on its worker and the Host completes the pending Core request when ready.
Hosts may also return WouldBlock, which older clients handle by yielding before
retrying. Each camera allows one pending preview. JPEG streams and raw port
reads return Busy until that preview completes. Unsupported boards may fall
back to JPEG FrameStream decoding. This operation requires the same live,
foreground camera permission as JPEG reads and returns Busy while a JPEG stream
is open. The SDK owns and validates the image handle before exposing its pixels
through Canvas. No native pointer or raw pixels enter Guest memory.

The current preview contract uses RGB565 or BGRA8888 (straight or premultiplied alpha), at most320×320 pixels. VGA capture may
produce a smaller preview while photos and video keep the selected profile.
The Host wires the Assets service with `pxa_peripherals_service_set_assets`.
Native producers allocate through a bounded Host resource allocator and publish
via the Host-only `pxa_assets_publish_image` bridge. Asset ownership and resource
limits follow ordinary decoded images. A prepared image remains valid after its
camera session closes, until the last image or submitted-frame reference drops.
Pending native results are released on close or backgrounding.

PAI Touch uses one sensor framebuffer. After copying an immutable preview, it
returns that buffer so acquisition can overlap app/UI work. WHEN_EMPTY parks
DMA once the next frame is queued, bounding prefetch to one frame. The app pauses the camera before writing Flash. A JPEG capture and its following preview
share that raw frame, so recording needs one acquisition and one JPEG encode per
video frame. GC2145 supplies YUYV422 directly to the JPEG encoder. Native preview
converts that frame to an immutable RGB565 image using full-range BT.601, sharing
chroma calculations between adjacent pixels. There is no JPEG round trip in this
preview path.

Watcher decodes its cached Himax JPEG on the native worker. Recording preview
reuses those bytes directly, without rereading the file being recorded.
Korvo uses the OV3660 hardware JPEG encoder with 320×240 and optional640×480
profiles. Both JPEG cameras use `PxaBoardVisionPreviewJpeg` for the same bounded
native decoder, allocation limits and immutable RGB565 ownership.

After a session first requests preview, the native worker prepares the image
as soon as each JPEG is published. Guest JPEG reads overlap preview conversion
or decoding. At most one image is retained, and the next JPEG request releases
an unused image. JPEG-only sessions keep their on-demand allocation behavior.
Worker completions wake the Host owner through `pxa_host_io_notify`, so result
delivery can proceed immediately. The worker never calls Guest code.

Finite JPEG STREAM reads coalesce up to4096 bytes per Core call, with at most16
native transfers. Individual peripheral transfers remain bounded to256 bytes.
Short reads and WouldBlock retain their usual stream semantics.
