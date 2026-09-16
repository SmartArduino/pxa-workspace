# PXA Unverified Functionality

This file tracks PXA Draft 0.1 behavior that has source integration and/or
host-side coverage but has not been validated in a running simulator or on an
ESP32-S3 device. It is intentionally kept until each item has reproducible
runtime evidence; passing a syntax check does not close an item.

## Checks already run

- Core runtime, private FS, Storage, IPC Broker, execution lease, Sensor,
  permission
  policy, package installer and transaction tests: passed on the host.
- Guest SDK encoders/parsers and existing Canvas App smoke tests: passed.
- Package-manifest generator and Draft specification/golden-vector checks:
  passed.
- ESP translation-unit syntax checks and simulator translation-unit syntax
  checks: passed using existing generated compiler flags.
- `pxa-lab` (including Private FS, KV Counter, Storage, Permission and
  Execution Lease) compiles to portable `wasm32-unknown-unknown` modules with
  `-nostdlib`.

None of the host checks builds a firmware image, writes device flash, or proves
persistence across a real restart. Execution Lease and Background Work are
both visible modules in PXA ABI Lab.

## Future Product Simulator Validation

The current workspace simulator is the UI-only `pxa-system` desktop target; it
does not install or execute PXA packages. The checks below remain blocked until
the product runtime simulator described in `../../../simulator/README.md`
exists and shares the firmware package and runtime adapters.

- Run a PXA App with `PXA_SIMULATOR_TRACE=/tmp/pxa-trace.jsonl`, interact with
  it, then close the simulator normally. Expected: the bounded JSONL file has
  both `guest-control` and `host-event` entries in sequence order and contains
  only metadata plus `payload_fingerprint`, never raw payload bytes. The
  `--pxa-trace-self-test` validates both directions with a Tetris Clock event.
- Reconfigure/build and start the simulator after automatic discovery of the
  five consolidated Packages. Expected: the Apps page shows the validation Apps
  with their Package icons and no package-verification rejection.
- Launch `pxa-lab`, open `Private FS`, tap repeatedly, leave and relaunch it.
  Expected:
  the persisted entry count increases and survives process restart.
- Use App Manager to clear `pxa-lab` data, then reopen `Private FS`. Expected:
  its count resets without uninstalling the Package.
- Launch `pxa-lab`, open `KV Counter`, tap repeatedly, leave and relaunch it. Expected:
  its `counter.total` value survives process restart. Clear its App data in App
  Manager and relaunch. Expected: the counter resets and no `.pxa-kv-*` file
  is visible through FS v1 or charged to the private-FS quota.
- Launch `pxa-lab`, open `Storage`. Expected: `WRITE` increments `demo.value`, `READ`
  restores it after relaunch, `LIST` reports one key after a write, and
  `DELETE` resets the value and reports zero keys. Clear its App data from App
  Manager, relaunch, and confirm the same zero-key state.
- Launch `pxa-lab`, open `IPC` and tap `PING` repeatedly. Expected: every tap
  changes its counter after a request reaches the signed `demo.echo` endpoint
  in the `responder` service Component and returns one result event; it does
  not freeze, re-enter Guest callbacks or terminate the runtime.
- Launch `pxa-lab`, open `Background Work`, tap `ENQUEUE 2S WORK`, return to the
  desktop, wait at least two seconds, then relaunch it. Expected: its declared
  worker starts with Work ID, attempt and deadline context, writes `work.runs`,
  reports success, and the reopened UI displays `Background work completed`.
  Starting another Package before eligibility ends the current resident scope.
- In App Manager allow `audio.playback@media` for `pxa-lab`, then launch it,
  open `Audio` and tap `PLAY TEST TONE`. Expected: the simulator reports
  live `submitted / accepted / queued` sample counters after one permission
  acquisition, one session result and one atomic graph result. Tap
  `STOP TEST TONE`; expected: Audio 0.2 `FLUSH` completes and `queued` becomes
  zero. On ESP, submit at most
  one negotiated signed-16-bit PCM frame per `PXA_IO_WRITE` call after graph
  commit. Expected: up to three PXA sessions mix through Game inputs without
  blocking a Guest callback; retry `PXA_STATUS_WOULD_BLOCK` on a later event.
  Hardware audibility, gain, ducking and session-close flush behavior still
  require device validation.
- Launch `pxa-lab` and open `RGB565 帧缓冲`. Expected: a centered 256 x 240
  test frame displays color bars, a gradient and a checkerboard without
  corruption. Tapping the frame changes the pattern. This submits one logical
  frame as two length-bounded RGB565 Canvas records.
- Launch `pxa-lab` and open `Surface 合成`. Expected: the 240 x 176 RGB565
  gradient remains inside the LVGL title and metrics bands, its white scanline
  moves continuously, and the displayed FPS updates. `queued` should increase;
  `blocked` may increase when the producer reaches the two-buffer limit but the
  UI must stay responsive. The simulator presents the Surface through a stable
  LVGL image descriptor while exercising the same Surface ABI.
- Launch `pxa-lab` and open `手柄输入`. In the simulator, hold arrow keys
  with Z/X and press Enter or Tab. Expected: the page reports the complete
  D-pad, A/B, Start and Select combination. Losing window focus reports a
  disconnected controller with zero buttons. On hardware, inject the same
  states through `pxa_host_post_controller_state`.
- Allow `net.client@https://postman-echo.com` for `pxa-lab` in App Manager,
  launch it, open `Network`, then tap `RUN ALL`. Expected: the simulator reports `PASS 12
  FAIL 0` after covering all six HTTP methods, v1.0 FETCH compatibility,
  request and response headers, inline bodies, response Streams, redirects,
  HTTP errors, timeout and response-limit results. On an
  ESP target the same controls exercise Postman's public Echo API, so internet
  availability and service changes can affect results. The Host uses
  the ESP default route where available and otherwise the board's AT HTTP
  transport; neither path runs blocking network work during a Guest import.
- Allow `device.identity@mac.wifi.station.hardware` and
  `net.client@http://8.166.128.230:3100` for `pxa-weather` in App Manager and
  launch it. Expected: the app reads the STA hardware MAC through Device v1 and sends it through
  the 吉米兔天气 API and displays the returned city, current condition, Celsius
  temperature and report time; tapping `刷新` starts another bounded
  asynchronous GET. The simulator returns deterministic Shenzhen data and the
  `--pxa-weather-self-test` command verifies the complete request, JSON decode,
  Stream close and visible UI result. ESP hardware still requires validation
  against the live endpoint and the device's actual MAC-based location.
- In App Manager allow `sensor.read@ambient.temperature` for `pxa-lab`, then
  launch it and open `Temperature`. Expected: the simulator lists the synthetic temperature
  descriptor, creates one subscription and updates its milli-Celsius value.
  On the current target board it must instead report no compatible sensor
  without a runtime failure until a reviewed board Provider is added.
- In App Manager allow `sensor.read@ambient.light` for `pxa-lab`, then launch
  it and open `Ambient Light`. Expected: the simulator discovers the separate synthetic
  milli-lux descriptor and updates its value through the unchanged Sensor v1
  subscription ABI. The current ESP Provider must report no compatible sensor;
  its absence is intentional until a reviewed board integration exists.
- Open `pxa-lab` / `Permission` before allowing `net.client@demo.local`. Expected:
  it displays denied; after changing the App Manager grant while stopped and
  relaunching, it displays allowed and can acquire/close its permission Handle
  without terminating the runtime.
- Confirm permission updates persist after simulator restart and that disabling
  an App, uninstalling a user App and clearing private data retain their
  documented independent effects.

## ESP32-S3 build and device validation

- Build firmware with `CONFIG_PXA_ENABLED=y` and the intended trusted publisher
  configuration. Expected: all PXA sources, including FS, Storage, IPC,
  Execution Lease and Permission v1, link successfully and generated assets
  contain all five signed built-in Packages.
- Open `pxa-lab` / `Surface 合成` on the target. Expected: the panel shows one
  moving frame per Surface submission without a 120 KiB Canvas control
  message. `DisplayPerf frames`, `FastRotate frames`, and `te_sync_panel
  updates` should remain close; `FastRotate areas` should stay one per frame.
  Compare FPS and `rotate_us` with `RGB565 帧缓冲` FULL and DIRTY modes. Closing
  the page must immediately reveal the LVGL background and release both PSRAM
  Surface buffers without a reset or stale frame.
- Flash a clean device and inspect boot logs. Expected: the assets partition
  mounts, trusted publishers load, built-in sync installs once, and subsequent
  boots report packages unchanged rather than reinstalling them.
- Verify the Apps page and App Manager on the target display. Expected: PNG
  icons render correctly, text fits, and the permission panel is scrollable and
  uses enabled/disabled switches correctly.
- Run the same `Private FS` lifecycle as the simulator test across normal
  reboot and power loss. Expected: private FS data survives, data clear resets
  it, and no App can observe another App identity's root.
- Repeat the `KV Counter` lifecycle across normal reboot and an interrupted
  write. Expected: Storage selects the newest CRC-valid slot, retains the
  previous value after a torn alternate-slot write, and clear-data removes both
  private FS and Storage state for that signed App identity.
- Run the same permission lifecycle as the simulator test across reboot.
  Expected: NVS policy decisions survive, required permissions gate activation,
  optional acquire returns a Component-owned Handle, and policy revocation
  closes authority-bound Handles and posts the reliable revocation event;
  acquiring an optional permission after launch does not reset or panic the
  device.
- Run `pxa-lab` / `IPC` on the target and tap `PING` repeatedly. Expected:
  the `responder` service Component starts once, every request receives one
  result event, and exiting the App while a request is pending leaves no reset,
  panic or stale callback.
- Run the Work Lab lifecycle on the target: enqueue the two-second Work, leave
  its UI, wait until it is eligible and reopen it. Expected: the worker receives
  a nonzero Work ID, attempt 1 and deadline, writes its Storage marker, reports
  success, and is stopped at the granted bound if it fails to complete. After a
  reboot, Work v1 must not reinterpret an old monotonic deadline.
- Run `pxa-sensor-lab` after allowing its scoped permission. Expected: the
  present target's empty Provider yields its no-compatible-sensor state; adding
  a board Provider later must deliver only descriptors and units defined by
  Sensor v1, without ABI changes.
- Network v1 device integration remains pending product decisions for the
  transport, TLS/CA policy, connection limits and background-transfer policy.
- Exercise install, enable, disable, uninstall and clear-data actions for a
  user Inbox Package and a built-in Package. Expected: built-in source cannot
  be uninstalled, active Apps cannot be mutated, and private data remains only
  until explicit clear-data.
- Test factory-content provisioning, interrupted install/update recovery, and
  limited free space. Expected: `CONFIG_PXA_BUILTIN_PACKAGE_ROOT` provisioning
  does not erase mutable `CONFIG_PXA_STATE_ROOT`, and failed transitions
  recover to a verified package.

## ABI and extension validation still required

- Run the multi-Component IPC validation App through WAMR on ESP, then extend
  the simulator runtime to the same model. Add cancellation and mailbox-
  pressure coverage without nested Guest callbacks.
- Keep Secrets reserved until the device provides a reviewed protected-key
  storage authority. Do not model an ordinary file, LittleFS slot or unencrypted
  NVS value as a PXA Secret.
- Validate service requirement rejection for an unavailable service on both
  Hosts, including the new `package.json` `services` source metadata path.
- Validate portable Wasm fallback and target-specific AOT selection with actual
  WAMR loading, not only package parsing.
- Run malformed-message, path-escape, quota-exhaustion and mailbox-saturation
  cases against the running ESP and simulator bridges.
- Before Draft 1.0 stabilization, add runtime coverage for multi-Component
  activation and cross-App IPC with signed endpoint policy.
