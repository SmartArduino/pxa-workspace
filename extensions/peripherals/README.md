# PXA peripherals extension 0.3

`org.doit.peripherals` provides logical GPIO, I²C, UART and module-slot discovery.
It is optional and separate from standard Sensor, Audio, Storage and Haptics.
Applications use `<pxa-ext/peripherals.hpp>` and link `pxa::peripherals` after
including `sdk/Peripherals.cmake`. Host builds use `pxa_peripherals`.

Service/resource ID 33026 is allocated in this workspace's extension registry;
identity is UUID `26531fa8-55a1-446a-b917-316c58b06582`. Always start with
`co_await pxa::ext::peripherals::open(ctx)`: HELLO verifies this identity and
negotiates 0.1–0.3. RGB ports require 0.2 and vision streams require 0.3. The Host still accepts 0.1 clients,
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

The `examples/camera` app requests access only after a button press. It saves a
complete frame atomically as `snapshot.jpg` or `inference.json` in app-private
files, and releases capture before writing. It never uploads images or installs
models. Retrieve the file with the normal PXADB app-data tools. The last complete
capture survives a failed subsequent capture; leftover `capture.tmp` is replaced
on the next attempt.

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
