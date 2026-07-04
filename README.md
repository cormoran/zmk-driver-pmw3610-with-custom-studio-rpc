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

## Summary

This module currently includes (Phase A + Phase B + Phase C):

- **Firmware**: PMW3610 sensor driver (`src/pmw3610.c`) with a
  runtime-configurable parameter set (cpi, axis flags, force-awake, smart
  algorithm, downshift/sample times, minimum report interval — see
  `include/cormoran/pmw3610/pmw3610_api.h`), a custom Studio RPC handler
  (`src/studio/pmw3610_handler.c`) exposing sensor info/diagnostics/raw
  register access/frame capture, and an optional settings integration
  (`src/settings/pmw3610_settings.c`) via
  [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings).
- **Protocol**: Protobuf definition (`proto/cormoran/pmw3610/pmw3610.proto`) —
  `GetInfo` (per-device info + runtime config snapshot), `ReadDiagnostics`,
  `ReadRegister`/`WriteRegister`, `CaptureFrame`/`GetFrameChunk`.
- **Web UI**: React + TypeScript app (`web/`) using
  [@cormoran/zmk-studio-react-hook](https://github.com/cormoran/react-zmk-studio) —
  sensor info/diagnostics cards, a generic settings editor (talking to
  zmk-feature-custom-settings' own Studio RPC subsystem), and a frame
  viewer (capture-once / streaming, canvas rendering, FPS counter).
- **Tests**: Firmware unit tests (`tests/studio/`), build tests
  (`tests/zmk-config/`, including a `pmw3610_settings_rpc` artifact that
  builds with custom settings enabled), and web unit tests (`web/test/`,
  including pure-function tests for frame chunk reassembly).

Next (Phase D): hardware validation of the frame-capture procedure (see
"Frame capture" below) — the exact PIXEL_GRAB/FRAME_GRAB sequence is not
documented in the public datasheet and is tunable via `CaptureFrame`
request parameters pending that validation.

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
registered (all public/unsecured, readable and writable over the generic
custom-settings Studio RPC subsystem provided by
[zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings)):

| key | type | default | valid range |
| --- | --- | --- | --- |
| `cpi` | int32 | 600 | 200 – 3200 |
| `swap_xy` | bool | `CONFIG_PMW3610_SWAP_XY` | — |
| `invert_x` | bool | `CONFIG_PMW3610_INVERT_X` | — |
| `invert_y` | bool | `CONFIG_PMW3610_INVERT_Y` | — |
| `force_awake` | bool | `false` | — |
| `smart_algorithm` | bool | `CONFIG_PMW3610_SMART_ALGORITHM` | — |
| `run_downshift_ms` | int32 | `CONFIG_PMW3610_RUN_DOWNSHIFT_TIME_MS` | 32 – 8160 |
| `rest1_downshift_ms` | int32 | `CONFIG_PMW3610_REST1_DOWNSHIFT_TIME_MS` | `16*sample` – `255*16*sample`, using the *default* `rest1_sample_ms` |
| `rest2_downshift_ms` | int32 | `CONFIG_PMW3610_REST2_DOWNSHIFT_TIME_MS` | `128*sample` – `255*128*sample`, using the *default* `rest2_sample_ms` |
| `rest1_sample_ms` | int32 | `CONFIG_PMW3610_REST1_SAMPLE_TIME_MS` | 10 – 2550 |
| `rest2_sample_ms` | int32 | `CONFIG_PMW3610_REST2_SAMPLE_TIME_MS` | 10 – 2550 |
| `rest3_sample_ms` | int32 | `CONFIG_PMW3610_REST3_SAMPLE_TIME_MS` | 10 – 2550 |
| `report_interval_min_ms` | int32 | `CONFIG_PMW3610_REPORT_INTERVAL_MIN` | 0 – 1000 |

Notes:

- **`force_awake` default is always `false`**, regardless of any per-device
  DT `force-awake;` property. A device with `force-awake;` set in DT boots
  with it enabled, but as soon as `CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS` is
  enabled, the setting's effective value (`false` unless you change it) is
  applied during sensor init and overrides the DT default. If you want
  force-awake on by default with custom settings enabled, either change a
  persisted value once (it'll survive reboots), or set the `force_awake`
  setting via Studio RPC / the settings export/import mechanism.
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
  reset).
- Settings persistence/export/import (`get`/`set`/`save`/`discard`/`reset`)
  is handled generically by zmk-feature-custom-settings' own Studio RPC
  subsystem, not duplicated here — see that module's README/web app for the
  matching client-side patterns.

### Frame capture (`CaptureFrame` / `GetFrameChunk`)

The PMW3610 exposes `PIXEL_GRAB` (0x35) and `FRAME_GRAB` (0x36) registers,
but the public 8-page datasheet does not document the procedure to read a
still image. This module implements the common PixArt pixel-grab
convention, with the variable points exposed as request parameters so the
procedure can be tuned without reflashing — **hardware validation of this
procedure is a follow-up phase (Phase D)**; treat captured frames with
skepticism until then.

- `CaptureFrame{device_index, pixel_count, max_invalid_retries,
  write_frame_grab, frame_grab_value, skip_pixel_grab_reset}` → captures into
  a firmware-side static buffer (size
  `CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE`, default 484 = 22x22) and
  returns `{frame_id, pixel_count, chunk_size}`. `pixel_count` is clamped to
  the buffer size; 0 uses the driver default (484). All non-pixel_count
  numeric fields being 0/false select the driver's defaults
  (`max_invalid_retries` 300, no `FRAME_GRAB` write,
  `skip_pixel_grab_reset=false` i.e. reset **is** written).
- `GetFrameChunk{frame_id, offset}` → `{frame_id, offset, bytes data}`, up to
  128 bytes per call. Call it repeatedly with increasing offsets (0, 128,
  256, ...) until `offset + data.size >= pixel_count` to assemble the full
  frame; `frame_id` must match the most recent `CaptureFrame` response or the
  call fails.
- Each raw byte's bit7 is `PG_VALID` (bits[6:0] are the pixel value); the
  firmware stores the byte as-is (does not mask it) so the host can inspect
  capture quality. The web UI masks bit7 for display and separately counts
  invalid bytes as a warning.
- During capture: the motion IRQ is disabled and an internal flag makes the
  normal motion-report path a no-op, so capturing a frame does not interleave
  with mouse movement reporting. Capture disturbs the sensor's navigation
  state, so on return (success **or** failure) the driver always re-runs the
  power-up-reset + reconfigure flow (same as `pmw3610_resume()`'s not-ready
  path) before returning, and the whole read loop is bounded to ~2 seconds of
  wall-clock time regardless of `pixel_count`/`max_invalid_retries`, so a
  misbehaving sensor cannot hang the Studio RPC thread.
- A 128-byte `GetFrameChunk` data chunk plus proto framing overhead must fit
  in one `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE` (256 in this module's test
  config) — a `BUILD_ASSERT` in `src/studio/pmw3610_handler.c` catches a
  too-small value at compile time.
- The web UI's "streaming" mode is a sequential capture loop (`CaptureFrame`
  → drain `GetFrameChunk` → render → repeat), not a fixed-interval timer —
  see [web/README.md](./web/README.md#frame-viewer-notes).

Example raw request/response JSON shapes for CLI tools (e.g.
`tools/zmk-studio-rpc custom-call`) — see `proto/cormoran/pmw3610/pmw3610.proto`
for the exact field numbers:

```jsonc
// CaptureFrame request (defaults: 22x22, standard procedure)
{ "captureFrame": { "deviceIndex": 0, "pixelCount": 484 } }
// -> { "captureFrame": { "frameId": 1, "pixelCount": 484, "chunkSize": 128 } }

// GetFrameChunk request (repeat with offset 0, 128, 256, 384)
{ "getFrameChunk": { "frameId": 1, "offset": 0 } }
// -> { "getFrameChunk": { "frameId": 1, "offset": 0, "data": "<128 bytes base64>" } }
```

### RPC debug facilities

The `WriteRegister` RPC call writes an arbitrary sensor register with no
validation beyond fitting in a byte. It is a debug/tuning facility (e.g. for
exploring the frame-capture procedure above interactively), but it can just
as easily put the sensor in a bad state until the next power-up reset /
reconfigure. The subsystem is unsecured (matching this module's template
default) — treat access to it like access to a debug/JTAG interface: fine
for your own trusted host, not something to expose over an untrusted
transport.

### Web UI

See [web/README.md](./web/README.md) for web UI development instructions,
including the settings panel, diagnostics polling, and frame viewer.

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
