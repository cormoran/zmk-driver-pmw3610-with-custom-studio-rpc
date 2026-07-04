# zmk-driver-pmw3610-with-custom-studio-rpc — Design

ZMK module providing the PMW3610 optical sensor driver with:

1. Runtime-configurable settings stored via
   [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings),
   applied to the sensor at boot and whenever changed over RPC.
2. A custom Studio RPC subsystem (`cormoran__pmw3610`) for sensor info,
   diagnostics, raw register access, and frame (image) capture.
3. A web UI (WebSerial) to edit settings and view streamed sensor images.

Based on [cormoran/zmk-pmw3610-driver](https://github.com/cormoran/zmk-pmw3610-driver)
(driver core, DT compat `cormoran,pmw3610` is kept identical — this module is a
drop-in replacement and MUST NOT be used together with the original module).

## Naming

| Item | Value |
| --- | --- |
| Zephyr module name | `zmk-driver-pmw3610-with-custom-studio-rpc` |
| Custom subsystem id | `cormoran__pmw3610` |
| Proto package | `cormoran.pmw3610` |
| Proto path | `proto/cormoran/pmw3610/pmw3610.proto` |
| DT compatible | `cormoran,pmw3610` (unchanged) |
| Settings subsystem id | `cormoran__pmw3610` (keys below) |

## Kconfig

- `PMW3610` — driver core (same options as the original driver; Kconfig values
  remain the *defaults* for runtime settings).
- `ZMK_PMW3610_CUSTOM_SETTINGS` — runtime settings integration.
  `depends on PMW3610 && ZMK_CUSTOM_SETTINGS`.
- `ZMK_PMW3610_STUDIO_RPC` — custom Studio RPC subsystem.
  `depends on PMW3610 && ZMK_STUDIO`.
- `ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE` — frame pixel buffer (default 484 = 22x22).

CMake: driver sources compile under `CONFIG_PMW3610`; `src/studio/*` and nanopb
proto generation only under `CONFIG_ZMK_PMW3610_STUDIO_RPC`; settings glue under
`CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS`.

## Driver changes (src/pmw3610.c)

- Fix latent bug: `DT_FOREACH_STATUS_OKAY(pixart_pmw3610, ...)` →
  `cormoran_pmw3610` (activity listener array was always empty).
- Runtime config lives in `pixart_data` (initialized from Kconfig/DT):
  cpi, swap_xy, invert_x, invert_y, force_awake, smart_algorithm,
  run/rest1/rest2 downshift ms, rest1/2/3 sample ms, report interval min ms.
- Public API (new header `include/cormoran/pmw3610/pmw3610_api.h`):
  - `pmw3610_foreach_device()` / device list accessor
  - `pmw3610_is_ready(dev)`, product/revision id read
  - Setters that update runtime state and push to sensor when ready:
    `pmw3610_set_cpi_runtime`, `pmw3610_set_downshift_*`, `pmw3610_set_sample_*`,
    `pmw3610_set_axis_flags`, `pmw3610_set_force_awake`, `pmw3610_set_smart_algorithm`,
    `pmw3610_set_report_interval_min`
  - `pmw3610_read_register` / `pmw3610_write_register` (for RPC debug)
  - `pmw3610_read_diagnostics` (SQUAL, shutter, pix min/avg/max)
  - `pmw3610_capture_frame(dev, buf, count, &out_count)` — see below.
- `pmw3610_async_init_configure` applies the *runtime* config (which was
  seeded from settings when enabled), so boot-time application is inherent.
- Static `dx/dy` accumulators move into `pixart_data` (bugfix for multi-instance).

## Settings (src/settings/pmw3610_settings.c)

`ZMK_CUSTOM_SETTING_DEFINE` entries, subsystem `cormoran__pmw3610`, all
RPC_PUBLIC + UNSECURE, with range constraints:

| key | type | default | constraint |
| --- | --- | --- | --- |
| `cpi` | int32 | DT `cpi` (600) | 200..3200 |
| `swap_xy` / `invert_x` / `invert_y` | bool | Kconfig | — |
| `force_awake` | bool | DT `force-awake` | — |
| `smart_algorithm` | bool | Kconfig | — |
| `run_downshift_ms` | int32 | 128 | 32..8160 |
| `rest1_downshift_ms` | int32 | 5000 | range from formula |
| `rest2_downshift_ms` | int32 | 17000 | range from formula |
| `rest1_sample_ms` / `rest2_sample_ms` / `rest3_sample_ms` | int32 | 40/100/500 | 10..2550 |
| `report_interval_min_ms` | int32 | Kconfig | 0..255 |

- At driver configure step: if `CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS`, seed
  runtime config from `zmk_custom_setting_read_by_key` (falls back to defaults).
- ZMK listener on `zmk_custom_setting_changed`: if subsystem id matches, apply
  the changed key to all PMW3610 devices immediately (VALUE_UPDATED/ SAVED /
  DISCARDED / RESET all re-apply the effective value).
- Settings get/set/save/export over RPC is provided generically by
  zmk-feature-custom-settings' own subsystem — this module does not duplicate it.

## Frame capture

PMW3610 exposes `PIXEL_GRAB` (0x35) and `FRAME_GRAB` (0x36). The public 8-page
datasheet documents only the register table, not the procedure, so the
implementation follows the common PixArt pixel-grab convention and is
hardware-validated (we have the sensor wired to a XIAO nRF52840 + J-Link):

1. Disable motion IRQ; set an atomic `capture_active` flag (report path skips).
2. Write `PIXEL_GRAB` = 0x00 to reset the pixel pointer (and/or `FRAME_GRAB`
   latch — validated empirically via the register read/write RPC).
3. Loop `count` times: read `PIXEL_GRAB`; bit7 = PG_VALID, bits[6:0] = pixel.
   Valid → store & advance; invalid → retry (bounded).
4. Power-up reset + full reconfigure (navigation is disturbed by frame grab),
   re-enable IRQ.

Pixel count is a request parameter (default 484 = 22x22); the exact array size
is confirmed on hardware. Raw register RPC allows tuning the procedure without
reflashing.

## RPC proto (`cormoran.pmw3610`)

```proto
Request  = oneof { GetInfoRequest, ReadDiagnosticsRequest, CaptureFrameRequest,
                   GetFrameChunkRequest, ReadRegisterRequest, WriteRegisterRequest }
Response = oneof { ErrorResponse, GetInfoResponse, ReadDiagnosticsResponse,
                   CaptureFrameResponse, GetFrameChunkResponse,
                   ReadRegisterResponse, WriteRegisterResponse }
```

- `GetInfo` → per-device: ready flag, product id, revision id, init error.
- `ReadDiagnostics` → SQUAL, shutter, pix min/avg/max.
- `CaptureFrame{device_index, pixel_count}` → captures into a static buffer,
  returns `{frame_id, pixel_count, chunk_size}`.
- `GetFrameChunk{frame_id, offset}` → `{offset, bytes data}` (chunk ≤ 128 B,
  `.options` bounded). Web polls chunks; "streaming" = repeated captures.
- `ReadRegister`/`WriteRegister` — debug/tuning path.
- No 64-bit proto fields (nanopb + `CONFIG_ZMK_STUDIO` restriction).
- Sub-messages require `has_<field> = true` on the firmware side.

RPC buffer note: default `ZMK_STUDIO_RPC_TX_BUF_SIZE` is 64 — README and tester
config must raise it (e.g. 256) for chunked frame responses, and
`ZMK_STUDIO_RPC_RX_BUF_SIZE` to 128 for custom settings.

## Web UI (web/)

Template stack: React + vite + buf/ts-proto + `@cormoran/zmk-studio-react-hook`.

- Connection card (from template).
- Sensor info + diagnostics card (poll).
- Settings panel: generic custom-settings client (list/get/set/save/discard/
  reset for subsystem `cormoran__pmw3610`) — reuse patterns from
  zmk-feature-custom-settings web app.
- Frame viewer: capture loop with canvas rendering (NxN grayscale, N selectable
  19–22, default 22), FPS counter, start/stop, single-shot, SQUAL overlay.

## Test config (tests/zmk-config)

- board `xiao_ble//zmk`, shield `tester_xiao`, snippet `studio-rpc-usb-uart`.
- Overlay (from zmk-keyboard-dya2 right-trackball, user's wiring):
  - spi0 pinctrl: SCK = P0.05 (D5), MOSI = MISO = P1.13 (D8, 3-wire shared)
  - CS = `&xiao_d 10`, IRQ = `&xiao_d 9` (active low, pull-up)
  - `cormoran,pmw3610`, `spi-max-frequency = <2000000>`, `disable-burst-read`
  - **Important**: tester_xiao's kscan uses `xiao_d 0..10`; the overlay must
    `/delete-property/ input-gpios` and redefine with D0–D4 only to avoid
    conflicts with D5/D8/D9/D10.
  - `zmk,input-listener` node for the sensor. `CONFIG_ZMK_POINTING=y`.
- build.yaml artifacts: `pmw3610_disabled` (module off), `pmw3610_plain`
  (driver only), `pmw3610_rpc` (driver + RPC + custom settings + studio).

## Validation plan (hardware)

1. `python3 -m unittest` (build tests + native_sim unit tests).
2. Flash `pmw3610_rpc` via J-Link (`debug-zmk-jlink` skill; nRF52840_xxAA).
3. `tools/zmk-studio-rpc … info / custom-list` → subsystem visible.
4. Custom settings RPC: list, change `cpi`, verify sensor register (via
   ReadRegister 0x85 RES_STEP under SPI page) and persistence across reboot.
5. `CaptureFrame` + chunks → assemble image, verify plausible pixel data
   (covered sensor = dark, lit = bright); tune procedure via register RPC
   if PG_VALID never asserts; determine true array size.
6. Web UI smoke test (vite dev + Chrome WebSerial) if environment allows;
   otherwise CLI-driven equivalent via `custom-call`.
