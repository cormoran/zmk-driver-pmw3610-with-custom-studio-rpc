# zmk-driver-pmw3610-with-custom-studio-rpc

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-module-template/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-module-template/actions/workflows/zmk-module.yml) [![Devcontainer](https://github.com/cormoran/zmk-module-template/actions/workflows/devcontainer.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-module-template/actions/workflows/devcontainer.yml)

ZMK module providing the [PMW3610](https://github.com/cormoran/zmk-pmw3610-driver)
optical mouse sensor driver, with a Web UI and an **unofficial** custom ZMK
Studio RPC protocol for sensor info/diagnostics, runtime settings, raw
register access, and frame (image) capture.

This is a drop-in replacement for `cormoran/zmk-pmw3610-driver` (same
devicetree compatible `cormoran,pmw3610`) — do not use both modules together.

See [DESIGN.md](./DESIGN.md) for the full module design and roadmap.

## Security — read this before adopting the module

**The entire `cormoran__pmw3610` Studio RPC subsystem is SECURED.** ZMK
Studio must be *unlocked* before any of this subsystem's RPCs succeed —
including read-only ones like `GetInfo`/`ReadDiagnostics`. This is not
configurable per-method: ZMK's custom-subsystem RPC dispatch checks lock
state once per subsystem, not per request, and `WriteRegister` (an
unvalidated raw sensor register write) is reason enough to secure the whole
thing rather than leave it open by default.

Two important consequences for anyone adding this module to a keyboard:

1. **Unlocking is physical-key-only in this ZMK fork.** There is no RPC or
   PIN-based unlock (`zmk.core.Request` only exposes `getDeviceInfo` /
   `getLockState` / `lock` / `resetSettings` — no `unlock`). A
   `&studio_unlock` behavior binding calls `zmk_studio_core_unlock()`
   directly when pressed
   (`dependencies/zmk/app/src/behaviors/behavior_studio_unlock.c`). **Your
   keyboard's keymap MUST bind `&studio_unlock` somewhere**, or once Studio
   locks there is no way to use this module's RPCs (including the web UI)
   again without reflashing a keymap that has the binding. See
   `tests/zmk-config/config/tester_xiao.keymap` for an example override.
2. **Studio auto-locks.** By default (`CONFIG_ZMK_STUDIO_LOCKING`, on for
   any non-`native_sim` board) it locks after
   `CONFIG_ZMK_STUDIO_LOCK_IDLE_TIMEOUT_SEC` (default 600s) of RPC idleness
   and whenever BLE disconnects
   (`CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT`). If the web UI suddenly shows a
   "locked" banner and stops working, this is why — press the keyboard's
   `&studio_unlock` key and it resumes automatically.

Frame-streaming *notifications* (see "Frame capture" below) are not
gated by lock state at the transport level, only the RPC call that
starts/stops a stream is secured — a stream already running keeps emitting
notifications through a later auto-lock unless something stops it, so the
firmware itself force-stops any active stream when Studio locks.

## Summary

This module currently includes (Phase A through Phase E):

- **Firmware**: PMW3610 sensor driver (`src/pmw3610.c`) with a
  runtime-configurable parameter set (cpi, axis flags, force-awake, smart
  algorithm, downshift/sample times, minimum report interval — see
  `include/cormoran/pmw3610/pmw3610_api.h`), a **secured** custom Studio RPC
  handler (`src/studio/pmw3610_handler.c`) exposing sensor info/diagnostics/
  raw register access/frame capture/frame streaming, and an optional
  settings integration (`src/settings/pmw3610_settings.c`) via
  [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings).
- **Protocol**: Protobuf definition (`proto/cormoran/pmw3610/pmw3610.proto`) —
  `GetInfo` (per-device info + runtime config snapshot), `ReadDiagnostics`,
  `ReadRegister`/`WriteRegister`, `CaptureFrame`/`GetFrameChunk`,
  `SetFrameStream` (notification-based streaming) plus a `Notification`
  message carrying `FrameStreamChunk`.
- **Web UI**: React + TypeScript app (`web/`) using
  [@cormoran/zmk-studio-react-hook](https://github.com/cormoran/react-zmk-studio) —
  sensor info/diagnostics cards, a generic settings editor (talking to
  zmk-feature-custom-settings' own Studio RPC subsystem), a frame viewer
  (capture-once / notification-based streaming, canvas rendering, FPS
  counter), and a Studio lock-state banner that disables controls while
  locked.
- **Tests**: Firmware unit tests (`tests/studio/`), build tests
  (`tests/zmk-config/`, including a `pmw3610_settings_rpc` artifact that
  builds with custom settings enabled), and web unit tests (`web/test/`,
  including pure-function tests for frame chunk reassembly/streaming
  assembly).

Phase D (hardware validation) is complete: the frame-capture procedure
follows the official PMW3610 datasheet (R2.4) `Pixel_Grab` sequence and has
been validated against a real sensor (see "Frame capture" below). Phase E
secures the whole subsystem and replaces the web UI's client-driven
streaming loop with firmware-pushed notifications (see "Security" above and
"Frame capture" below).

## More Info

For more info on modules, you can read through through the [Zephyr modules page](https://docs.zephyrproject.org/3.5.0/develop/modules.html) and [ZMK's page on using modules](https://zmk.dev/docs/features/modules). [Zephyr's west manifest page](https://docs.zephyrproject.org/3.5.0/develop/west/manifest.html#west-manifests) may also be of use.

## Module User Guide

1. Add dependency to your `config/west.yml`. Note: this module requires a patched ZMK with custom Studio RPC support.

   ```yml
   manifest:
       remotes:
           ...
           - name: cormoran
           url-base: https://github.com/cormoran
       projects:
           ...
           - name: zmk-driver-pmw3610-with-custom-studio-rpc
           remote: cormoran
           revision: main+custom-studio-protocol # or latest commit hash
           import: true
           ...
           # Required: patched ZMK with custom Studio RPC support
           - name: zmk
           remote: cormoran
           revision: main+custom-studio-protocol
           import:
               file: app/west.yml
   ```

2. Add the sensor to your board/shield overlay (see
   `tests/zmk-config/snippets/pmw3610-trackball/pmw3610-trackball.overlay` for
   a full example) and enable the driver in your `config/<shield>.conf`:

   Leave `disable-burst-read` unset (the default) unless your SPI bus has no
   NCS/`cs-gpios` under the controller's control -- burst read needs chip
   select held low across the whole multi-byte motion-burst transfer, so it
   only needs disabling on wiring that can't do that. This module's own test
   config builds and validates both: `pmw3610-trackball` (burst read ON,
   recommended) and `pmw3610-trackball-no-burst` (OFF) under
   `tests/zmk-config/snippets/`.

   ```conf
   CONFIG_PMW3610=y
   CONFIG_INPUT=y
   CONFIG_ZMK_POINTING=y

   # Optionally enable custom Studio RPC (sensor info/diagnostics/raw
   # register access/frame capture)
   CONFIG_ZMK_STUDIO=y
   CONFIG_ZMK_PMW3610_STUDIO_RPC=y

   # Optionally enable runtime settings (cpi, axis flags, downshift/sample
   # times, etc; requires the zmk-feature-custom-settings module dependency,
   # see its README for adding it to west.yml)
   CONFIG_ZMK_CUSTOM_SETTINGS=y
   CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
   CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS=y

   # Raise Studio RPC buffers: 256 for chunked frame-capture responses
   # (128-byte data chunk + proto framing overhead), 128 for custom
   # settings requests/exports.
   CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256
   CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128

   # Optional: frame capture buffer size (bytes/pixels), default 484 = 22x22.
   # CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE=484
   ```

3. Explore the protocol / firmware handler / web UI:
   - `proto/cormoran/pmw3610/pmw3610.proto` — message types
   - `src/studio/pmw3610_handler.c` — firmware RPC handler
   - `src/settings/pmw3610_settings.c` — runtime settings integration
   - `web/src/App.tsx` — web UI

### Runtime settings (`cormoran__pmw3610` custom settings)

With `CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS=y`, the following keys are
registered **once per PMW3610 devicetree instance**, as `"<param>@<id>"`
(all public/unsecured, readable and writable over the generic
custom-settings Studio RPC subsystem provided by
[zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings)):

| param | type | default | valid range |
| --- | --- | --- | --- |
| `cpi` | int32 | this device's DT `cpi` property (600 if unset) | 200 – 3200 |
| `swap_xy` | bool | `CONFIG_PMW3610_SWAP_XY` | — |
| `invert_x` | bool | `CONFIG_PMW3610_INVERT_X` | — |
| `invert_y` | bool | `CONFIG_PMW3610_INVERT_Y` | — |
| `force_awake` | bool | this device's DT `force-awake` property (`false` if unset) | — |
| `smart_algorithm` | bool | `CONFIG_PMW3610_SMART_ALGORITHM` | — |
| `run_downshift_ms` | int32 | `CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS` | 32 – 8160 |
| `rest1_downshift_ms` | int32 | `CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS` | `16*sample` – `255*16*sample`, using the *default* `rest1_sample_ms` |
| `rest2_downshift_ms` | int32 | `CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS` | `128*sample` – `255*128*sample`, using the *default* `rest2_sample_ms` |
| `rest1_sample_ms` | int32 | `CONFIG_PMW3610_REST1_SAMPLE_TIME_MS` | 10 – 2550 |
| `rest2_sample_ms` | int32 | `CONFIG_PMW3610_REST2_SAMPLE_TIME_MS` | 10 – 2550 |
| `rest3_sample_ms` | int32 | `CONFIG_PMW3610_REST3_SAMPLE_TIME_MS` | 10 – 2550 |
| `report_interval_min_ms` | int32 | `CONFIG_PMW3610_REPORT_INTERVAL_MIN` | 0 – 1000 |

**`<id>`** is this device's `settings_id`, reported per-device in `GetInfo`
(`DeviceInfo.settings_id`): the devicetree `settings-id` property if set
(recommended, up to 8 ASCII characters, e.g. `settings-id = "trackball";`),
otherwise a 4-hex-digit hash of the devicetree node's path (stable across
reordering of sibling devicetree nodes, but not human-readable — set
`settings-id` explicitly if you want a readable key, e.g. with more than one
PMW3610 device in one firmware image). A single-sensor board without
`settings-id` set ends up with keys like `cpi@a3f2`; with `settings-id =
"trackball"` it would be `cpi@trackball`.

Notes:

- **Each device's settings are independent.** With two PMW3610 devices in
  one firmware image (e.g. a split keyboard's central *and* peripheral each
  having a sensor, or two sensors on one half), changing `cpi@<id>` for one
  device does not affect the other's `cpi@<other-id>`.
- `force_awake` and `cpi` default to **this device's own devicetree
  properties** (`force-awake;` / `cpi = <...>;`), not a single value shared
  by every device.
- The `rest1_downshift_ms`/`rest2_downshift_ms` **range constraints are
  computed from the default sample time**, but the sensor's actual valid
  range at any given moment depends on the *current* `rest1_sample_ms` /
  `rest2_sample_ms` value. If you change the sample time first, a downshift
  value that passed the setting's static range check may still be rejected
  by the driver (and vice versa) — change sample time and downshift time
  together if you push both away from their defaults.
- Changes take effect immediately (pushed to the sensor over SPI as soon as
  it has finished async init) and are re-applied to all PMW3610 devices
  whenever any `cormoran__pmw3610` setting changes (save, discard, or
  reset) — each device only ever reads its own `"<param>@<id>"` keys, so
  this is harmless for devices unrelated to the change.
- Settings persistence/export/import (`get`/`set`/`save`/`discard`/`reset`)
  is handled generically by zmk-feature-custom-settings' own Studio RPC
  subsystem, not duplicated here — see that module's README/web app for the
  matching client-side patterns.

### Frame capture (`CaptureFrame` / `GetFrameChunk`)

Frame capture follows the official PMW3610 datasheet (R2.4) `Pixel_Grab`
procedure: an arm sequence (SPI clock request on, page-1 magic enable,
SPI clock request off, test clock on, `PIXEL_GRAB` (0x35) = 0x01), then per
pixel: wait for `OBSERVATION1` (0x2D) bit2, read `PIXEL_GRAB`, write
`OBSERVATION1` = 0x01 to advance. `PRBS_TEST_CTL` (0x47) bit0 afterwards
reports whether the sensor considers all 484 pixels read. The full pixel
array is 22x22 = 484 pixels (datasheet Figure 17).

- `CaptureFrame{device_index, pixel_count, max_invalid_retries}` → captures
  into a firmware-side static buffer (size
  `CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE`, default 484 = 22x22) and
  returns `{frame_id, pixel_count, chunk_size, complete, duration_ms}`.
  `pixel_count` is clamped to the buffer size; 0 uses the driver default
  (484). `max_invalid_retries` is the per-pixel budget of 10ms
  ready-bit-wait retries (0 = driver default 3, clamped to 1..100).
  `complete` mirrors the sensor's own all-484-pixels-read status bit;
  `duration_ms` is the wall-clock capture duration.
- `GetFrameChunk{frame_id, offset}` → `{frame_id, offset, bytes data}`, up to
  128 bytes per call. Call it repeatedly with increasing offsets (0, 128,
  256, ...) until `offset + data.size >= pixel_count` to assemble the full
  frame; `frame_id` must match the most recent `CaptureFrame` response or the
  call fails.
- Each raw byte's bit7 is `PG_Valid` (bits[6:0] are the pixel value); the
  firmware stores the byte as-is (does not mask it) so the host can inspect
  capture quality. The web UI masks bit7 for display and separately counts
  invalid bytes as a warning.
- During capture: the motion IRQ is disabled and an internal flag makes the
  normal motion-report path a no-op, so capturing a frame does not interleave
  with mouse movement reporting. The datasheet requires a reset to resume
  navigation after a pixel grab, so on return (success **or** failure) the
  driver always re-runs the power-up-reset + reconfigure flow (same as
  `pmw3610_resume()`'s not-ready path) before returning, and the whole
  procedure is bounded to ~5 seconds of wall-clock time regardless of
  `pixel_count`/`max_invalid_retries`, so a misbehaving sensor cannot hang
  the Studio RPC thread. (A typical complete capture is far faster.)
- A 128-byte `GetFrameChunk` data chunk plus proto framing overhead must fit
  in one `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE` (256 in this module's test
  config) — a `BUILD_ASSERT` in `src/studio/pmw3610_handler.c` catches a
  too-small value at compile time.
- `CaptureFrame` rejects with an `ErrorResponse` ("frame streaming is
  active...") if a `SetFrameStream` stream is currently running — stop the
  stream first (see below).

#### Frame streaming (`SetFrameStream` + `FrameStreamChunk` notification)

Instead of a client-driven poll loop, the firmware can push captured frames
on its own:

- `SetFrameStream{device_index, enable, pixel_count, max_invalid_retries}` →
  `{streaming}`. `enable = true` starts a background loop (system
  workqueue) that repeatedly calls the same capture procedure as
  `CaptureFrame` and, on each successful capture, raises one
  `FrameStreamChunk` **notification** (not a response) per 128-byte chunk —
  `{frame_id, offset, bytes data, total_size, complete}`, where `total_size`
  and `complete` are repeated on every chunk of a frame so the client can
  detect the last chunk without a separate "done" message. `enable = false`
  stops the loop (the in-flight capture, if any, still finishes first).
  `pixel_count`/`max_invalid_retries` follow the same 0-means-default,
  clamped semantics as `CaptureFrame`.
- `SetFrameStream` and `CaptureFrame`/`GetFrameChunk` share the same
  firmware-side static buffer/`frame_id` counter, so only one can be active
  — enabling streaming while a one-shot capture is somehow still in
  progress is not possible (`CaptureFrame` is synchronous), and
  `CaptureFrame` rejects while streaming is on (see above).
- Because a 484-pixel `Pixel_Grab` capture takes roughly 2 seconds on real
  hardware (measured in Phase D), the loop's "as fast as possible" pacing
  naturally lands around 0.4-0.5 fps — this is a hardware limitation of the
  sensor's Pixel_Grab procedure, not a bug or artificial throttle.
- If Studio locks while a stream is running, the firmware stops it after
  the current in-flight capture (a listener on the core lock-state event
  sets the internal "streaming active" flag to false) — see "Security"
  above. Notifications themselves are not lock-gated at the transport
  level, only the `SetFrameStream` call that starts/stops a stream is.
- The web UI's frame viewer uses this instead of a client poll loop — see
  [web/README.md](./web/README.md#frame-viewer-notes).

Example raw request/response/notification JSON shapes for CLI tools (e.g.
`tools/zmk-studio-rpc custom-call`) — see `proto/cormoran/pmw3610/pmw3610.proto`
for the exact field numbers:

```jsonc
// CaptureFrame request (defaults: full 22x22 frame)
{ "captureFrame": { "deviceIndex": 0, "pixelCount": 484 } }
// -> { "captureFrame": { "frameId": 1, "pixelCount": 484, "chunkSize": 128,
//      "complete": true, "durationMs": 480 } }

// GetFrameChunk request (repeat with offset 0, 128, 256, 384)
{ "getFrameChunk": { "frameId": 1, "offset": 0 } }
// -> { "getFrameChunk": { "frameId": 1, "offset": 0, "data": "<128 bytes base64>" } }

// SetFrameStream request (start)
{ "setFrameStream": { "deviceIndex": 0, "enable": true, "pixelCount": 484 } }
// -> { "setFrameStream": { "streaming": true } }
// ... followed by repeated notifications, e.g.:
// { "frameStreamChunk": { "frameId": 1, "offset": 0, "data": "<128 bytes base64>",
//     "totalSize": 484, "complete": true } }

// SetFrameStream request (stop)
{ "setFrameStream": { "deviceIndex": 0, "enable": false } }
// -> { "setFrameStream": { "streaming": false } }
```

### RPC debug facilities

The `WriteRegister` RPC call writes an arbitrary sensor register with no
validation beyond fitting in a byte. It is a debug/tuning facility (e.g. for
exploring the frame-capture procedure above interactively), but it can just
as easily put the sensor in a bad state until the next power-up reset /
reconfigure. As of Phase E the whole subsystem (including `WriteRegister`)
requires ZMK Studio to be unlocked — see "Security" above — but treat access
to it like access to a debug/JTAG interface regardless: fine for your own
trusted host, not something to expose over an untrusted transport.

### Split keyboards / multiple sensors (`source`, `CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY`)

**Multiple PMW3610 devices in one firmware image** (e.g. two sensors on the
same half) already work with no extra configuration: each devicetree
instance gets its own settings (see "Runtime settings" above) and its own
entry in `GetInfo`'s `devices` list (`device_index`/`settings_id`).

**Reaching a sensor on a split keyboard's *peripheral* half** additionally
requires `CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY=y` on **both** halves (depends
on ZMK's `CONFIG_ZMK_SPLIT_RELAY_EVENT`) plus, since the largest relayed
message needs more room than the framework's 128-byte default,
`CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN=240`. Every request
(`GetInfo`/`ReadDiagnostics`/`ReadRegister`/`WriteRegister`/`CaptureFrame`/
`GetFrameChunk`/`SetFrameStream`) has a `source` field: `0` (the default)
targets local devices, `1` the first peripheral. Since relaying is
inherently asynchronous, a relayed request's RPC call returns immediately
with a `DeferredResponse{request_id}`; the real answer arrives later as a
`PeripheralResponse{source, request_id, response}` Studio notification.

```jsonc
// GetInfo targeting the first split peripheral's own devices
{ "getInfo": { "source": 1 } }
// -> { "deferred": { "requestId": 7 } }   // immediate
// ... later, as a notification:
// { "peripheralResponse": { "source": 1, "requestId": 7,
//     "response": { "getInfo": { "devices": [ ... ] } } } }
```

**Frame streaming from a peripheral's sensor** works the same way:
`SetFrameStream{source: 1, enable: true, deviceIndex: 0}` starts the
existing capture loop on the peripheral, and each chunk relays back as a
`FrameStreamChunk` notification with `source` filled in (0 for a locally
streamed frame). `CaptureFrame`/`GetFrameChunk` against a peripheral work
the same as their local form, just via the deferred/notification pattern
above instead of a synchronous response.

The underlying relay transport broadcasts a request to *every* connected
peripheral rather than addressing one specifically (each one's answer still
arrives correctly tagged with its own `source`) — fine for the common
single-peripheral split, but means a multi-peripheral build gets one
`PeripheralResponse`/`FrameStreamChunk` per peripheral for the same
`request_id`/stream.

This has been build-tested (both roles compile, see
`tests/zmk-config/build.yaml`'s `pmw3610_split_*` artifacts) and the
peripheral-side request execution (including a CaptureFrame call and the
genuinely-unsupported-request-kind fallback) has a native_sim self-test
(`tests/split_peripheral`), but **not yet validated against real split
hardware** — no split-capable board pair was available while developing
this feature.

### Web UI

See [web/README.md](./web/README.md) for web UI development instructions,
including the settings panel, diagnostics polling, and frame viewer. The
Sensor and Frame Viewer cards each have a "Split source" input (0 = local,
matching the proto default); a relayed request's `DeferredResponse` is
awaited transparently behind the scenes (`web/src/relay.ts`) so the rest of
the UI code doesn't need to special-case it. The generic settings panel
(`cormoran_custom_settings`, a separate subsystem provided by
zmk-feature-custom-settings) does not have a source selector yet -- its
calls always target `source: 0` (local devices).

### Publishing Web UI

**GitHub Pages**: Merge a pull request into `main+custom-studio-protocol` to deploy to `https://<account>.github.io/<repo>/`.

**Cloudflare Workers (PR previews)**: Configure `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID` secrets.

## Module Development Guide

### Setup for running test

#### Option0: Dev container (recommended)

Open this repository in VS Code with the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers). The container automatically initializes the west workspace using the isolated layout.

#### Option1: west workspace directory layout

Set west topdir as parent of repository root and download dependencies under `../`.
This layout is useful to reduce disk usage by sharing dependencies with other zephyr modules.
The build result is located in `../build`.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
# rm -r .west # if exists to reset workspace
west init -l . --mf west/west-test-workspace.yml
west update --narrow
west zephyr-export
```

#### Option2: isolated directory layout

Set west topdir as repository root and download dependencies under `./dependencies`.
This layout is useful if you don't want to share dependencies to other zephyr modules.
Dev container and github actions uses this layout.
The build result is located in `./build`.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

### Pre-commit

Every commit need to pass pre-commit verification. The verification contains formatting code and running tests.

```
pip install pre-commit
pre-commit install

# Run pre-commit manually
pre-commit run --all-files
# Run for git staged files
pre-commit run
```

### Running Test

```bash
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```

### Sync changes from template

Run `Actions > Sync Changes in Template > Run workflow` to get the latest template changes as a pull request.

If the template contains changes in `.github/workflows/*`, register a GitHub personal access token as `GH_TOKEN` repository secret (`repo` + `workflow` scopes).

### Coding agent on actions

Actions for github copilot and claude are available.

- Mention `@copilot`
- Setup `ANTHROPIC_API_KEY` secret and mention `@claude`
  - Or fix [claude.yml](./github/workflows/claude.yml) to use `CLAUDE_CODE_OAUTH_TOKEN`
