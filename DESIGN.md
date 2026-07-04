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
  `depends on ZMK_CUSTOM_SETTINGS` (not `PMW3610`, so it also builds/works
  with zero PMW3610 devices, e.g. native_sim -- mirrors `ZMK_PMW3610_STUDIO_RPC`).
- `ZMK_PMW3610_STUDIO_RPC` — custom Studio RPC subsystem.
  `depends on ZMK_STUDIO` (not `PMW3610`, same reasoning).
- `ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE` — frame pixel buffer (default 484 = 22x22).

CMake: driver sources compile under `CONFIG_PMW3610`; `src/studio/*` and nanopb
proto generation only under `CONFIG_ZMK_PMW3610_STUDIO_RPC`; settings glue
(`src/settings/pmw3610_settings.c`) under `CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS`.

## Driver changes (src/pmw3610.c)

- Fix latent bug: `DT_FOREACH_STATUS_OKAY(pixart_pmw3610, ...)` →
  `cormoran_pmw3610` (activity listener array was always empty). (Already
  fixed in Phase A.)
- Runtime config lives in `pixart_data.runtime` (a `struct
  pixart_runtime_config`, `src/pixart.h`), seeded from Kconfig/DT in
  `pmw3610_init()`: cpi, swap_xy, invert_x, invert_y, force_awake,
  smart_algorithm, run/rest1/rest2 downshift ms, rest1/2/3 sample ms, report
  interval min ms.
- A per-device `struct k_mutex lock` in `pixart_data` guards composite
  register sequences (multi-write CPI, the clock-on/off wrapped `pmw3610_write`
  helper used by downshift/sample/performance/smart-algorithm writes, and
  reads of the runtime struct) against concurrent access from the Studio RPC
  thread while the trigger work runs on the system workqueue. SPI bus
  arbitration alone does not serialize *sequences* of transactions.
- Public API (`include/cormoran/pmw3610/pmw3610_api.h`):
  - `pmw3610_device_count()` / `pmw3610_get_device(index)` (existing).
  - `pmw3610_is_ready(dev)`, `pmw3610_get_init_error(dev)` (existing).
  - `pmw3610_read_register` / `pmw3610_write_register(dev, addr, value)` (new
    write) -- both mutex-guarded.
  - `pmw3610_read_diagnostics(dev, struct pmw3610_diagnostics *out)` — SQUAL
    (0x06), shutter hi/lo (0x07/0x08), pix max/avg/min (0x09/0x0A/0x0B).
  - `pmw3610_get_runtime_config(dev, struct pmw3610_runtime_config *out)`.
  - Setters (validate → update runtime value → push to sensor immediately
    if `data->ready`; otherwise only the in-memory value is updated, and it
    is applied to hardware by `pmw3610_async_init_configure()` once the
    async init reaches its CONFIGURE step):
    `pmw3610_set_cpi_runtime`, `pmw3610_set_run_downshift_ms`,
    `pmw3610_set_rest1_downshift_ms`, `pmw3610_set_rest2_downshift_ms`,
    `pmw3610_set_rest1_sample_ms`, `pmw3610_set_rest2_sample_ms`,
    `pmw3610_set_rest3_sample_ms`, `pmw3610_set_axis_flags`,
    `pmw3610_set_force_awake`, `pmw3610_set_smart_algorithm`,
    `pmw3610_set_report_interval_min`.
  - `pmw3610_capture_frame(dev, buf, count, &out_count)` — Phase C, not yet
    implemented.
- `pmw3610_async_init_configure` applies the *runtime* config (overlaid with
  effective custom-setting values when `CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS`
  is enabled -- see "Settings" below for exactly how/when), so boot-time
  application is inherent to the normal init flow, not a separate step.
- The `#if CONFIG_PMW3610_REPORT_INTERVAL_MIN > 0` compile-time blocks in the
  report path became a runtime `if (report_interval_min_ms > 0)` check
  (semantics unchanged when the value is fixed at build time and equal).
- `dx`/`dy` accumulators (and `last_smp_time`/`last_rpt_time`, previously
  `static` inside `pmw3610_report_data`) moved into `pixart_data` (bugfix for
  multi-instance cross-talk).
- `pmw3610_set_performance` semantics changed slightly: it now takes the
  *runtime* force-awake flag explicitly as a parameter (instead of reading
  `config->force_awake`, the DT-only value) — callers
  (`pmw3610_async_init_configure`, `pmw3610_set_force_awake`,
  `on_activity_state`) pass `data->runtime.force_awake`. Behavior when the
  flag is false is unchanged (register untouched); the activity-listener
  interaction is preserved (active/inactive still maps to
  force-awake-enabled/disabled while the runtime flag is on).

## Settings (src/settings/pmw3610_settings.c)

Settings entries for subsystem `cormoran__pmw3610`, all RPC_PUBLIC + UNSECURE,
with range constraints:

| key | type | default | constraint |
| --- | --- | --- | --- |
| `cpi` | int32 | 600 (DT default) | 200..3200 |
| `swap_xy` / `invert_x` / `invert_y` | bool | Kconfig | — |
| `force_awake` | bool | `false` (see note below) | — |
| `smart_algorithm` | bool | Kconfig | — |
| `run_downshift_ms` | int32 | Kconfig (128) | 32..8160 |
| `rest1_downshift_ms` | int32 | Kconfig (5000) | `16*sample`..`255*16*sample` using the *default* rest1 sample time |
| `rest2_downshift_ms` | int32 | Kconfig (17000) | `128*sample`..`255*128*sample` using the *default* rest2 sample time |
| `rest1_sample_ms` / `rest2_sample_ms` / `rest3_sample_ms` | int32 | Kconfig (40/100/500) | 10..2550 |
| `report_interval_min_ms` | int32 | Kconfig | 0..1000 |

Deviation from the original plan: entries are defined with
`STRUCT_SECTION_ITERABLE(zmk_custom_setting, ...)` directly (matching the
pattern in zmk-feature-custom-settings' own
`src/test/zmk_config_sample_settings.c`), **not** via the
`ZMK_CUSTOM_SETTING_DEFINE()` convenience macro. Reason: that macro's
constraint argument ends up inside another static array initializer
(`_name##_constraints[] = {__VA_ARGS__}`), and `ZMK_CUSTOM_SETTING_RANGE_INT32()`
expands to a compound literal whose `.range.min`/`.range.max` members are
*themselves* compound literals (via `ZMK_CUSTOM_SETTING_VALUE_INT32()`).
Nested compound literals are not valid in a static/file-scope initializer
per C11 6.6p9, and `arm-zephyr-eabi-gcc` 12.2.0 enforces this strictly
("initializer element is not constant"), even though the pattern is present
verbatim in the dependency's own `src/test/custom_settings_test.c`. Plain
designated-initializer syntax (`{.type = ZMK_CUSTOM_SETTING_CONSTRAINT_RANGE,
.range = {.min = {...}, .max = {...}}}`, no compound-literal casts) avoids the
problem and is what this module uses. **If a future zmk-feature-custom-settings
version fixes/works around this, `ZMK_CUSTOM_SETTING_DEFINE()` could be used
again** — worth revisiting.

`force_awake`'s setting default is a compile-time `false`, independent of any
per-device DT `force-awake` property (a custom setting default must be a
single value shared by all devices of a subsystem, while `force-awake` is a
per-device DT property). A device with `force-awake;` in its DT node boots
with force-awake enabled (seeded in `pmw3610_init()`), but once
`CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS` is enabled, `pmw3610_async_init_configure()`
overlays the *setting's* effective value (`false` unless explicitly changed)
before the sensor is configured — i.e. the DT property is effectively
overridden to disabled the first time settings are applied. Documented in
README.md; users who want force-awake on by default with custom settings
enabled should set the `force_awake` setting via RPC/persisted storage,
not (only) the DT property.

### Boot-apply mechanism (deviates from the original plan)

The original plan was a `SYS_INIT(APPLICATION, 99)` hook in the settings
module that iterates devices and applies effective setting values, relying
on boot-order timing (the hook would run before the ~260ms-delayed async
init CONFIGURE step, but *after* `custom_settings_init()`'s own
`SYS_INIT(APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY)`, and hopefully
after `main()`'s `settings_load()` too, since persisted values only become
available then).

Investigation of the actual boot sequence
(`dependencies/zmk/app/src/main.c`, `dependencies/zmk-feature-custom-settings/src/custom_settings.c`):

1. `POST_KERNEL`: `pmw3610_init()` seeds `pixart_data.runtime` from Kconfig/DT
   and schedules the async init work (first step fires after
   `10 + CONFIG_PMW3610_INIT_POWER_UP_EXTRA_DELAY_MS` ms; the CONFIGURE step
   -- where runtime config is pushed to hardware -- only runs after the
   `POWER_UP` (≥10ms) → `CLEAR_OB1` (200ms) → `CHECK_OB1` (50ms) steps, i.e.
   **~260ms after driver init**, all via `k_work_schedule` on the system
   workqueue).
2. `APPLICATION`: `custom_settings_init()` resets every setting's
   `memory_value`/`persistent_value` to its compiled-in `default_value` and
   sets `has_persistent_value = false`. **No persisted values are loaded
   here.**
3. `main()` (runs after all `SYS_INIT` levels, i.e. after every module's
   `POST_KERNEL`/`APPLICATION` init has completed) calls
   `settings_subsys_init()` then `settings_load()`, which is what actually
   reads persisted values from flash via each subsystem's registered
   `SETTINGS_STATIC_HANDLER_DEFINE` `H_SET` callback
   (`custom_settings_handle_set()` for `zmk-feature-custom-settings`). This
   handler mutates `memory_value`/`persistent_value` directly -- it does
   **not** call `zmk_custom_setting_write()`, so **no
   `zmk_custom_setting_changed` event fires for values loaded this way.**

So there are two ordering hazards with a `SYS_INIT`-based apply hook: (a) it
must run after `custom_settings_init()` (same init level, priority-ordered --
fragile to get right across module boundaries) and (b) it must run after
`main()`'s `settings_load()`, which is not a `SYS_INIT` step at all and has
no Kconfig priority to hook into.

**Chosen mechanism**: instead of a separate hook, the driver itself calls
`pmw3610_settings_apply_to_device(dev)` (declared in
`include/cormoran/pmw3610/pmw3610_settings_apply.h`, implemented in
`src/settings/pmw3610_settings.c`, called only when
`IS_ENABLED(CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS)`) from inside
`pmw3610_async_init_configure()`, immediately before applying the runtime
config to hardware. Since that function only runs from the async init work
queued ~260ms after `POST_KERNEL`, and `main()` (which runs `settings_load()`)
executes synchronously right after all `SYS_INIT` levels complete -- i.e.
enormously earlier than 260ms of wall-clock boot time in any realistic
scenario -- `settings_load()` is guaranteed to have already returned by the
time `pmw3610_async_init_configure()` runs. This closes the race **by
construction** (correct call order in one code path) rather than by relying
on two independently-configured `SYS_INIT` priorities happening to line up.
The setters called by `pmw3610_settings_apply_to_device()` store the runtime
value unconditionally and only push to hardware if `data->ready` (false at
this point), so this call only updates the in-memory `pixart_data.runtime`
struct, which is then applied to hardware by the rest of
`pmw3610_async_init_configure()` right after.

**Change handling** (post-boot): a `zmk_custom_setting_changed` listener
(`pmw3610_settings_changed_listener`, subscribed via `ZMK_SUBSCRIPTION`) checks
`ev->setting->custom_subsystem_id` against `"cormoran__pmw3610"` and, on a
match, re-applies *all* keys' current effective values to *all* PMW3610
devices (simple and correct, if slightly wasteful; settings changes are rare
interactive events, not a hot path). This handles VALUE_UPDATED / SAVED /
DISCARDED / RESET uniformly since all four just mean "re-read the effective
value and push it."

Settings get/set/save/discard/reset/export over RPC is provided generically
by zmk-feature-custom-settings' own subsystem — this module does not
duplicate it.

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

- `GetInfo` → per-device: ready flag, product id, revision id, init error,
  and a `RuntimeConfig` sub-message snapshot (cpi, swap_xy, invert_x,
  invert_y, force_awake, smart_algorithm, run/rest1/rest2 downshift ms,
  rest1/2/3 sample ms, report_interval_min_ms) — implemented in Phase B.
  `has_runtime_config` is set only when `pmw3610_get_runtime_config()`
  succeeds (nanopb `has_<field>` requirement).
- `ReadDiagnostics{device_index}` → SQUAL, shutter, pix min/avg/max.
  Implemented in Phase B.
- `ReadRegister{device_index, address}` / `WriteRegister{device_index,
  address, value}` — debug/tuning path, bounds-checked (`address`/`value`
  must fit a byte) and device_index-checked; errors carry the errno value in
  the message text. Implemented in Phase B. **WriteRegister has no
  additional validation beyond fitting in a byte** -- it is a raw register
  write, documented as a debug facility in README.md.
- `CaptureFrame{device_index, pixel_count}` → captures into a static buffer,
  returns `{frame_id, pixel_count, chunk_size}`. **Phase C, not yet
  implemented.**
- `GetFrameChunk{frame_id, offset}` → `{offset, bytes data}` (chunk ≤ 128 B,
  `.options` bounded). Web polls chunks; "streaming" = repeated captures.
  **Phase C, not yet implemented.**
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
  (driver only), `pmw3610_rpc` (driver + custom RPC, no custom settings),
  `pmw3610_settings_rpc` (driver + custom RPC + custom settings + custom
  settings' own Studio RPC: `-DCONFIG_ZMK_STUDIO=y
  -DCONFIG_ZMK_PMW3610_STUDIO_RPC=y -DCONFIG_ZMK_CUSTOM_SETTINGS=y
  -DCONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y
  -DCONFIG_ZMK_PMW3610_CUSTOM_SETTINGS=y`).
- `tests/studio/native_sim.conf` additionally enables
  `CONFIG_SETTINGS`/`CONFIG_ZMK_CUSTOM_SETTINGS`/
  `CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC`/`CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS`
  with zero PMW3610 devices present, proving the settings module compiles
  and boots without crashing (both `cormoran__pmw3610` and
  `cormoran_custom_settings` subsystems print at boot;
  `tests/studio/keycode_events.snapshot` asserts both lines).

## Validation plan (hardware)

1. `python3 -m unittest` (build tests + native_sim unit tests).
2. Flash `pmw3610_settings_rpc` via J-Link (`debug-zmk-jlink` skill;
   nRF52840_xxAA).
3. `tools/zmk-studio-rpc … info / custom-list` → both subsystems visible.
4. Custom settings RPC: list, change `cpi`, verify sensor register (via
   ReadRegister 0x85 RES_STEP under SPI page) and persistence across reboot.
   Verify a setting change over RPC while the device is idle/active reaches
   hardware promptly (change listener path), and that a value changed while
   the sensor is mid-init (before `data->ready`) is still correctly applied
   once init completes.
5. Exercise `ReadDiagnostics`/`ReadRegister`/`WriteRegister` against a real
   device (index bounds, register 0x85 RES_STEP round trip, error path for
   an out-of-range device_index).
6. `CaptureFrame` + chunks (Phase C) → assemble image, verify plausible
   pixel data (covered sensor = dark, lit = bright); tune procedure via
   register RPC if PG_VALID never asserts; determine true array size.
7. Web UI smoke test (vite dev + Chrome WebSerial) if environment allows;
   otherwise CLI-driven equivalent via `custom-call`.
