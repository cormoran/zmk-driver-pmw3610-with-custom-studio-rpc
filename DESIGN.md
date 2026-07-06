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

The Phase C implementation guessed a "common PixArt pixel-grab convention"
(write `PIXEL_GRAB` = 0x00, poll bit7) because only the 8-page public
datasheet was available; Phase D hardware validation showed PG_Valid never
asserts with that sequence. The full PMW3610 datasheet (R2.4, PIXEL_GRAB
register usage section) documents the official `Pixel_Grab` procedure,
which Phase D implemented and validated on hardware:

1. Write `0x41` = `0xBA` (SPI clock on request)
2. Write `0x7F` = `0xFF` (select page 1)
3. Write `0xB4` = `0xD7` (page-1 magic enable)
4. Write `0x7F` = `0x00` (back to page 0)
5. Write `0x41` = `0xB5` (SPI clock off)
6. Write `0x32` = `0x90` (test clock on)
7. Write `0x35` = `0x01` (arm pixel grab)
8. Read `0x47` until bit1 set (else wait 10ms, re-check; bounded)
9. Read `0x2D` until bit2 set (else wait 10ms, re-check; bounded)
10. Read `0x35` → one raw byte (bit7 = PG_Valid, bits[6:0] = pixel)
11. Write `0x2D` = `0x01` (advance to the next pixel)
    — repeat 9-11 for the full frame (484 = 22x22, datasheet Figure 17)
12. Read `0x47`; bit0 set = all 484 pixels read successfully (reported as
    `complete` in the RPC response, along with the measured `duration_ms`)

Driver-side wrapper behavior (`pmw3610_capture_frame()` in
`src/pmw3610.c`), unchanged from Phase C where still applicable:

- Require `data->ready` (else `-EBUSY`); take `data->lock`, set
  `data->capture_active = true` (checked and short-circuited by
  `pmw3610_report_data()`, belt-and-braces alongside disabling the IRQ),
  release the lock, then `pmw3610_set_interrupt(dev, false)`.
- The arm sequence (steps 1-7) uses **raw** `pmw3610_write_reg()` calls,
  NOT the `pmw3610_write()` clk-on/off wrapper -- the datasheet sequence
  manages the 0x41 SPI clock request itself (steps 1 and 5).
- Per-pixel ready waits (steps 8/9) sleep 10ms per retry, bounded by
  `max_invalid_retries` (reinterpreted in Phase D: number of 10ms wait
  retries per pixel, default 3, clamped 1..100) and an unconditional
  wall-clock deadline (`PMW3610_FRAME_CAPTURE_TIMEOUT_MS` = 5000ms), so
  the Studio RPC thread can never be blocked indefinitely.
- Raw bytes (bit7 kept) are stored; the host masks/interprets.
- Regardless of success/failure/timeout: re-take the lock, reset the async
  init state machine to step 0 (the datasheet requires a reset to resume
  navigation after a pixel grab -- exactly the not-ready path of
  `pmw3610_resume()`), reset `data->sw_smart_flag` (the capture wrote
  `0x32`, which the smart-algorithm logic tracks via that flag; the
  re-init's power-up reset clears the sensor-side register so the
  host-side flag must follow), clear `capture_active`, release the lock.
  The motion IRQ is re-enabled once async init reaches its last step.

Pixel count is a request parameter (default 484 = 22x22, clamped to the
firmware's static frame buffer, `CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE`).
The Phase C procedure-tuning knobs (`write_frame_grab`/`frame_grab_value`/
`skip_pixel_grab_reset`) were removed in Phase D (proto field numbers 4-6
reserved) -- the official sequence is fixed by the datasheet and needs no
tuning; only `pixel_count` and `max_invalid_retries` remain.

## RPC proto (`cormoran.pmw3610`)

```proto
Request  = oneof { GetInfoRequest, ReadDiagnosticsRequest, CaptureFrameRequest,
                   GetFrameChunkRequest, ReadRegisterRequest, WriteRegisterRequest,
                   SetFrameStreamRequest }
Response = oneof { ErrorResponse, GetInfoResponse, ReadDiagnosticsResponse,
                   CaptureFrameResponse, GetFrameChunkResponse,
                   ReadRegisterResponse, WriteRegisterResponse,
                   SetFrameStreamResponse }
Notification = oneof { FrameStreamChunk } // top-level message, Phase E
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
- `CaptureFrame{device_index, pixel_count, max_invalid_retries}` → captures
  into a static buffer, returns `{frame_id, pixel_count, chunk_size,
  complete, duration_ms}`. Implemented in Phase C, procedure replaced with
  the official datasheet sequence in Phase D (request fields 4-6, the old
  procedure knobs, reserved). `pixel_count` is clamped to
  `CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE` (0 → driver default 484).
  `frame_id` is a monotonically increasing counter (starts at 1); errors
  (bad `device_index`, driver failure) produce an `ErrorResponse`, not a
  crash, including with zero PMW3610 devices present (native_sim).
- `GetFrameChunk{frame_id, offset}` → `{frame_id, offset, bytes data}` (chunk
  ≤ 128 B, `.options` bounded, fixed `chunk_size` = 128). Implemented in
  Phase C. Validates `frame_id` matches the current captured frame and
  `offset < length`; the web app's one-shot "Capture Once" path drives the
  chunk loop sequentially (not parallel). Rejects while a `SetFrameStream`
  stream is active (Phase E).
- `SetFrameStream{device_index, enable, pixel_count, max_invalid_retries}` →
  `{streaming}`. Implemented in Phase E, replacing the original plan of
  "streaming" being a client-side `CaptureFrame`+`GetFrameChunk` poll loop.
  `enable=true` starts a `k_work_delayable` loop on the system workqueue
  that repeatedly calls `pmw3610_capture_frame()` and raises one
  `FrameStreamChunk` Notification per 128-byte chunk of every captured
  frame; `enable=false` stops it (current in-flight capture, if any, still
  finishes). Shares the static frame buffer/`frame_id` counter with
  `CaptureFrame`/`GetFrameChunk`, so only one of streaming or a one-shot
  capture can be active — `CaptureFrame` rejects while streaming is on.
  A `zmk_studio_core_lock_state_changed` listener force-stops the loop when
  Studio locks (notifications are not lock-gated at the transport level,
  only the RPC call that starts/stops the stream is).
- No 64-bit proto fields (nanopb + `CONFIG_ZMK_STUDIO` restriction).
- Sub-messages require `has_<field> = true` on the firmware side.

RPC buffer note: default `ZMK_STUDIO_RPC_TX_BUF_SIZE` is 64 — README, tester
config, and `tests/studio/native_sim.conf` all raise it to 256 for chunked
frame responses (a `BUILD_ASSERT` in `pmw3610_handler.c` enforces this at
compile time), and `ZMK_STUDIO_RPC_RX_BUF_SIZE` to 128 for custom settings.

## Web UI (web/)

Template stack: React + vite + buf/ts-proto + `@cormoran/zmk-studio-react-hook`.
Implemented in Phase C as four cards plus the connection card:

- Connection card (from template, kept as-is).
- `SensorInfo.tsx`: "Sensor" card (`GetInfo` on connect + refresh button,
  device selector when more than one device) and "Diagnostics" card
  (`ReadDiagnostics`, manual + 1s auto-refresh toggle).
- `SettingsPanel.tsx`: generic custom-settings editor for subsystem
  `cormoran__pmw3610`, talking to zmk-feature-custom-settings' *separate*
  Studio RPC subsystem (`cormoran_custom_settings`) — list with
  values/constraints, inline int32/bool/string edit, write with
  memory/persist mode, save/discard/reset buttons, per-row unsaved
  indicator. Deviation from the original plan: trimmed relative to
  zmk-feature-custom-settings' own reference app (no free-form
  subsystem/key filter UI, no JSON import/export panel, no array
  push/pop) since this module's settings are all fixed-key scalars.
- `FrameViewer.tsx`: size selector (19/20/21/22, default 22), device index
  input, capture-once button (`CaptureFrame`+`GetFrameChunk`, unchanged),
  start/stop streaming toggle, advanced collapsible (`max_invalid_retries`
  -- Phase D removed the obsolete procedure knobs), canvas render
  (grayscale, 12px/sensor-pixel, nearest-neighbor), complete/duration
  display, invalid-byte-count warning, FPS counter, and a `locked` prop
  (Phase E) that disables all controls. **Phase E deviation from the
  original Phase C plan**: streaming was originally a client-side sequential
  `CaptureFrame`+drain-`GetFrameChunk` loop; Phase E replaces it with
  `SetFrameStream{enable:true}` + subscribing to `FrameStreamChunk` custom
  notifications via `onNotification({type:"custom", subsystemIndex, ...})`,
  matching the proto/firmware redesign above -- no more client poll loop.
- `frame.ts`: pure functions (`assembleFrame`, `chunkOffsets`,
  `pixelByteToGray`, `frameToRgba`, `isValidPixelByte`) extracted for unit
  testing without a canvas/DOM dependency, plus (Phase E)
  `createFrameAssembler(totalSize)` — an incremental assembler for a stream
  of `(offset, data)` chunks arriving one notification at a time (as opposed
  to `assembleFrame`'s one-shot "all chunks already collected" input),
  returning `{addChunk, getBytes, bytesWritten}`; `addChunk` returns `true`
  once every byte in range has been written at least once.
- `useStudioLockState.ts` (Phase E): a small hook subscribing to
  `onNotification({type:"core", ...})` for `zmk.core.LockState` changes,
  used by `App.tsx` to show a "locked" banner and pass `locked` down to
  `SettingsPanel`/`FrameViewer`; also exports `isUnlockRequiredError()`,
  checking whether a thrown error is the ts-client's `MetaError` with
  `condition === ErrorConditions.UNLOCK_REQUIRED` (`call_rpc()` throws this
  directly, per `@zmkfirmware/zmk-studio-ts-client`'s `index.js`), used by
  `FrameViewer`/callers to show a clearer message if a locked-out RPC call
  fails before any lock notification was observed (e.g. immediately after
  reconnecting to an already-locked device).
- Proto vendoring: `web/buf.gen.yaml` lists a *second* `buf generate` input
  directory, `web/proto/`, containing a vendored copy of
  zmk-feature-custom-settings' `custom_settings.proto` (see the
  provenance/re-sync comment at the top of that file for the pinned commit
  and how to refresh it). This is **not** the repo-root `proto/` tree used by
  the firmware-side nanopb glob in `CMakeLists.txt` — vendoring under
  `web/proto/` instead avoids a conflicting second copy of the
  `cormoran.zmk.custom_settings` package in firmware builds that include
  both modules (e.g. `pmw3610_settings_rpc`).
  Originally this pointed straight at
  `../dependencies/zmk-feature-custom-settings/proto` (the west dependency
  checkout) instead of vendoring, which worked locally but broke CI:
  `web-ui.yml` builds `web/` standalone with plain `npm ci` and never runs
  `west update`, so that path doesn't exist there
  (`stat ../dependencies/zmk-feature-custom-settings/proto: no such file or
  directory`). Vendoring fixes this at the cost of manual re-sync when the
  dependency's proto changes.

## Test config (tests/zmk-config)

- board `xiao_ble//zmk`, shield `tester_xiao`, snippet `studio-rpc-usb-uart`.
- Overlay (from zmk-keyboard-dya2 right-trackball, user's wiring):
  - spi0 pinctrl: SCK = P0.05 (D5), MOSI = MISO = P1.13 (D8, 3-wire shared)
  - CS = `&xiao_d 10`, IRQ = `&xiao_d 9` (active low, pull-up)
  - `cormoran,pmw3610`, `spi-max-frequency = <2000000>`
  - **Important**: tester_xiao's kscan uses `xiao_d 0..10`; the overlay must
    `/delete-property/ input-gpios` and redefine with D0–D4 only to avoid
    conflicts with D5/D8/D9/D10.
  - `zmk,input-listener` node for the sensor. `CONFIG_ZMK_POINTING=y`.
- **Burst read ON vs OFF**: two snippets, `tests/zmk-config/snippets/
  pmw3610-trackball` (burst read ON -- no `disable-burst-read` property,
  the normal/recommended mode) and `.../pmw3610-trackball-no-burst`
  (`disable-burst-read` set). Burst read needs NCS (`cs-gpios`) held low
  across the whole multi-byte transfer; since this overlay's `&spi0` always
  configures `cs-gpios`, ON is fully usable and is what real deployments
  should use. OFF only matters for wiring/boards where the SPI bus has no
  controller-driven NCS at all -- provided here purely for build/motion
  coverage of that code path (`pmw3610_report_data()`'s
  `config->disable_burst_read` branch), via a dedicated
  `pmw3610_plain_no_burst` build.yaml artifact (motion reporting is the only
  thing burst mode affects; RPC/settings artifacts stay on the ON snippet).
- build.yaml artifacts: `pmw3610_disabled` (module off), `pmw3610_plain`
  (driver only, burst read ON), `pmw3610_plain_no_burst` (driver only, burst
  read OFF), `pmw3610_rpc` (driver + custom RPC, no custom settings),
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

## Phase E: secured subsystem + notification-based frame streaming

### Security

The whole `cormoran__pmw3610` subsystem meta moves from
`ZMK_STUDIO_RPC_HANDLER_UNSECURED` to `ZMK_STUDIO_RPC_HANDLER_SECURED`
(`src/studio/pmw3610_handler.c`). Security is per-subsystem, not per-method
(`zmk_rpc_custom_subsystem_meta` has one `security` field checked in
`custom_subsystem.c`'s `call()`), so this secures every method including
`GetInfo`/`ReadDiagnostics`, not just `WriteRegister` — an accepted trade-off
given `WriteRegister` is an arbitrary sensor register write.

Unlocking in this ZMK fork is **physical-key-only**: there is no RPC unlock
request (`zmk.core.Request` only has `getDeviceInfo`/`getLockState`/`lock`/
`resetSettings`). A `&studio_unlock` behavior binding calls
`zmk_studio_core_unlock()` directly on key press
(`behavior_studio_unlock.c`). `CONFIG_ZMK_STUDIO_LOCKING` defaults on for any
non-native_sim board (`imply ZMK_STUDIO_LOCKING if !ARCH_POSIX`), auto-locks
after `CONFIG_ZMK_STUDIO_LOCK_IDLE_TIMEOUT_SEC` (default 600s) and on BLE
disconnect. **Consequence for adopters**: any keyboard using this module must
bind `&studio_unlock` somewhere in its keymap, or its web UI becomes
permanently unusable once locked. `tests/zmk-config` gets a keymap override
binding it (see below) since the shield's own keymap doesn't.

Notifications (frame streaming) are **not** gated by lock state at the
transport level (`custom_event_mapper` in `custom_subsystem.c` has no lock
check, unlike `call()`) — only the RPC *call* that starts/stops streaming is
secured. A locked client therefore cannot start a stream, but a stream
started while unlocked would keep emitting notifications after a later
auto-lock unless explicitly stopped. Mitigate by subscribing to
`zmk_studio_core_lock_state_changed` and force-stopping any active stream
when the state becomes `LOCKED`.

### Frame streaming via notifications

New proto messages (package `cormoran.pmw3610`):

```proto
message SetFrameStreamRequest {
    uint32 device_index = 1;
    bool enable = 2;
    uint32 pixel_count = 3;         // 0 = driver default, same as CaptureFrameRequest
    uint32 max_invalid_retries = 4; // 0 = driver default, same as CaptureFrameRequest
}
message SetFrameStreamResponse { bool streaming = 1; }

message FrameStreamChunk {
    uint32 frame_id = 1;
    uint32 offset = 2;
    bytes data = 3;      // <=128 bytes, same bound as GetFrameChunkResponse.data
    uint32 total_size = 4; // repeated on every chunk so the client can detect the last one
    bool complete = 5;     // sensor-reported completion (0x47 bit0), same value each chunk of a frame
}

message Notification {
    oneof notification_type { FrameStreamChunk frame_stream_chunk = 1; }
}
```

`SetFrameStreamRequest`/`Response` join the existing `Request`/`Response`
oneofs (next free field numbers 7/8). `Notification` is a new top-level
message, independent of `Request`/`Response`, exactly mirroring
`zmk-feature-custom-settings`'s own `Notification` message — it is nanopb-
encoded and carried as the opaque `payload` bytes inside `zmk.custom
.CustomNotification`, itself found by resolving this subsystem's runtime
index (`STRUCT_SECTION_GET`/`STRUCT_SECTION_COUNT` over
`zmk_rpc_custom_subsystem`, exactly as
`custom_subsystem_index_for_identifier()` does in
`custom_settings_handler.c` — copy that pattern).

Firmware: a `k_work_delayable` loop (system workqueue) that, while streaming
is enabled, repeatedly calls the existing `pmw3610_capture_frame()` and, on
success, raises one `FrameStreamChunk` notification per 128-byte chunk
(`raise_zmk_studio_custom_notification`, synchronous per ZMK's event
manager — matches the proven settings-notification pattern, no extra
pacing needed a priori; add a small inter-chunk delay only if hardware
testing shows transport overrun). `SetFrameStream` and `CaptureFrame`/
`GetFrameChunk` share the same static frame buffer/frame_id counter, so
concurrent use is disallowed: `CaptureFrame` returns an error while a stream
is active; `SetFrameStream(enable=true)` validates `device_index` up front
(same bounds check as other handlers) so an invalid target fails immediately
instead of spinning.

Given the sensor takes ~2s per 484-pixel frame (measured in Phase D) and
frame capture inherently blocks normal cursor movement while it runs, "as
fast as possible" back-to-back capture already self-paces streaming to
~0.4-0.5 fps; document this in README as a hardware limitation, not a bug.

### Validating a physical-unlock-only security model without a human present

Phase D-style autonomous hardware validation cannot press a physical key, so
it can only prove the **rejection** path (secured call while locked →
`UNLOCK_REQUIRED`) against the real default build. To functionally validate
the streaming mechanism end-to-end, use a validation-only build with
`-DCONFIG_ZMK_STUDIO_LOCKING=n` (ad hoc `west build` cmake-arg, not committed
to `build.yaml`) — mirrors the earlier `boot0` scratch-overlay approach for
the same reason (environment can't do physical/human steps).

## Phase F: multiple sensors + split peripheral support

Goal: (1) first-class support for **multiple PMW3610 devices per firmware
image** (per-device settings instead of today's single global set), and
(2) **split keyboard support** — sensors living on a split *peripheral*
become visible/controllable through the central's Studio RPC (info,
diagnostics, registers, frame capture/streaming) and through
zmk-feature-custom-settings.

Everything below was design; implementation status per stage (F.6):

- **F-a (multi-device settings, local only) — implemented.** Per-device
  `"<param>@<id>"` custom-settings keys
  (`include/cormoran/pmw3610/pmw3610_settings_id.h`,
  `src/pmw3610_settings_id.c`, `src/settings/pmw3610_settings.c`), a new DT
  `settings-id` property (`dts/bindings/cormoran,pmw3610.yml`) with a
  4-hex-digit devicetree-path-hash fallback, `pmw3610_get_device_id()`
  (`include/cormoran/pmw3610/pmw3610_api.h`), and `GetInfo`
  `device_index`/`settings_id` fields (`proto/cormoran/pmw3610/pmw3610.proto`,
  `src/studio/pmw3610_handler.c`). `force_awake`/`cpi` defaults are now
  per-instance DT properties instead of one compile-time-shared value,
  resolving the Phase B `force_awake` caveat. Verified: `west zmk-build`
  (`pmw3610_settings_rpc_dual` build artifact, `tests/zmk-config/snippets/
  pmw3610-trackball-dual/`, two real devicetree instances on one SPI bus,
  one with an explicit `settings-id` and one relying on the hash fallback)
  plus an ELF-symbol assertion in `test.py` proving two independent
  `zmk_custom_setting` entry sets get generated with no symbol collisions;
  `west zmk-test` (native_sim, zero devices) and web unit
  tests/lint/build all still pass. Not yet hardware-validated (this
  environment has no PMW3610 sensor attached — see "Hardware validation
  status" below); README updated.
- **F-b + F-c (relay proto/Kconfig scaffolding, handler-to-executor
  refactor, peripheral executor, central bridge for non-frame RPCs) —
  implemented.** `GetInfoRequest`/`ReadDiagnosticsRequest`/
  `ReadRegisterRequest`/`WriteRegisterRequest` gained a `source` field
  (0 = local, N = peripheral N); a nonzero source on the central is relayed
  to split peripherals and answered asynchronously via a new
  `DeferredResponse` (returned immediately) followed by a `PeripheralResponse`
  Studio notification (`proto/cormoran/pmw3610/pmw3610.proto`). The
  four supported request handlers were extracted from
  `src/studio/pmw3610_handler.c` into a transport-independent executor
  (`src/studio/pmw3610_request_exec.c`/`.h`), shared by the Studio RPC
  handler (central, local requests) and the new split relay bridge
  (`src/split/pmw3610_relay.c`/`include/cormoran/pmw3610/pmw3610_relay.h`,
  `CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY`, both roles) — the relay events
  (`zmk_pmw3610_relay_request`/`_response`, identifiers `pmq`/`pmp`) and
  the Kconfig/nanopb decoupling (`CONFIG_ZMK_PMW3610_PROTOBUF`, needed
  because a peripheral has no `ZMK_STUDIO_RPC` to `select NANOPB` for it)
  follow exactly the F.0 design below.
  **Real constraint found while implementing** (not anticipated in the
  original F.2 design): ZMK's split relay event wire header
  (`relay_event_header.event_data_size`) is a single `uint8_t`, hard-capping
  any relayed message at 255 bytes regardless of
  `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` — a relayed `GetInfoResponse` for
  4 devices (this module's earlier max_count) encodes to ~440 bytes, far
  over that ceiling. Fixed by lowering `GetInfoResponse.devices` max_count
  from 4 to 2 (`pmw3610.options`; a 2-device `RelayResponse` is ~218 bytes,
  and both relay structs' sizes are checked at compile time against both
  `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` *and* the hard 255-byte ceiling —
  see the `BUILD_ASSERT`s in `pmw3610_relay.c`) and shrinking
  `DeviceInfo.settings_id`'s `max_size` from 16 to 9 (matching
  `PMW3610_SETTINGS_ID_BUF_SIZE` exactly instead of over-allocating).
  Verified: `tests/split_peripheral` (new native_sim test, peripheral role,
  zero local devices — synthesizes an encoded `RelayRequest` locally via a
  `CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY_TEST` self-test, since native_sim
  cannot exercise the real BLE transport, and asserts the decoded
  `RelayResponse` for both a supported kind (`GetInfo`) and an unsupported
  one (`CaptureFrame`, correctly rejected) — this is the same self-test
  pattern zmk-feature-custom-settings' own `tests/split_peripheral` uses
  for the identical reason); two new `west zmk-build` artifacts
  (`pmw3610_split_peripheral` with a real sensor, `pmw3610_split_central`)
  proving both roles compile as real ARM firmware; `python3 -m unittest`
  (build + native_sim + this new test) and web unit tests/lint/build all
  still pass.
- **F-d (frame capture/streaming over relay) — implemented.** `CaptureFrame`/
  `GetFrameChunk`/`SetFrameStream` gained a `source` field and now route
  through the same local-vs-relay dispatch as the other four request kinds.
  The frame capture/streaming handlers (static frame buffer, `frame_id`
  counter, the streaming work-queue loop) moved from
  `src/studio/pmw3610_handler.c` into `pmw3610_request_exec.c` too, so a
  split peripheral (no Studio of its own) can execute them locally when
  relayed. `FrameStreamChunk` gained a `source` field (0 = streamed
  locally); a peripheral's stream chunks relay up as a **new**
  one-directional (peripheral→central) relay event,
  `zmk_pmw3610_relay_notification` (identifier `pmn`, carrying an
  already-encoded `Notification`) via a new `pmw3610_relay_notify()` entry
  point, and the central re-raises it as the real Studio notification with
  `source` filled in — the same pattern as `PeripheralResponse`, just for
  an unsolicited (not request/response-shaped) notification instead. Two
  new stop-streaming safety nets, split by role (both only meaningful on
  one side): the central's existing Studio-lock-triggered stop stays
  central-only (`#if !PMW3610_IS_SPLIT_PERIPHERAL`); a new
  peripheral-only listener on `zmk_split_peripheral_status_changed` stops
  an active stream when the split link disconnects (`#if
  PMW3610_IS_SPLIT_PERIPHERAL`), matching the design's "peripheral
  force-stops on disconnect" plan. `pmw3610_handler.c` shrank to just
  decode → route (source==0 → `pmw3610_request_exec_handle()`, else →
  `pmw3610_relay_dispatch_request()`) → return — no request-kind-specific
  logic left in it at all.
  Verified: `tests/split_peripheral`'s self-test now also exercises a
  relayed `CaptureFrame` against zero local devices (still an
  ErrorResponse, but for "no such device" rather than "unsupported kind" —
  a genuinely-unsupported/malformed relayed request, tested separately via
  an unset `request_type`, is what now exercises that fallback path); all 8
  `west zmk-build` artifacts (including the split roles, both of which now
  compile the frame logic) and the full `python3 -m unittest` +
  web unit tests/lint/build all still pass. The largest relayed message
  (`Notification` wrapping a `PeripheralResponse` for a two-device
  `GetInfoResponse`, ~231 bytes) needed
  `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` raised from 224 to 240 (still
  comfortably under the 255-byte hard ceiling).
- **F-e (web UI plumbing for `source`, pmw3610 subsystem) — implemented.**
  `web/src/relay.ts` (new, pure/unit-tested):
  `PeripheralResponseCorrelator` tracks relayed requests by `request_id`
  (assigned by the firmware, globally unique, so one correlator instance is
  safe to share across concurrent callers), resolving/rejecting (6s
  timeout, comfortably above the firmware's ~5s frame-capture deadline)
  when fed a matching `PeripheralResponse` notification payload; and
  `callPmw3610Request()` wraps a raw RPC call, transparently awaiting the
  correlation when the immediate reply is a `DeferredResponse`, so callers
  don't need to special-case relayed vs. local requests. `SensorInfo.tsx`
  and `FrameViewer.tsx` each gained a "Split source" number input (0 =
  local, matching the proto default) plumbed into every request, and an
  always-on notification subscription (previously `FrameViewer`'s
  subscription was tied to the streaming start/stop lifecycle -- now
  permanent, since a non-streaming relayed `CaptureFrame`/`GetFrameChunk`
  call also needs to receive its `PeripheralResponse`) feeding both the
  correlator and (`FrameViewer` only) the existing `FrameStreamChunk`
  handling, itself now gated on an `isStreamingRef` so a stray/late chunk
  after "Stop Streaming" doesn't render. `FrameStreamChunk.source` is
  displayed as "Stream source" when nonzero.
  **Not done**: `SettingsPanel.tsx` (the *generic* `cormoran_custom_settings`
  subsystem provided by zmk-feature-custom-settings, not this module's own
  proto) still hardcodes `source: SOURCE_LOCAL` everywhere -- adding a
  source selector there is a separate, smaller chunk of remaining work
  against a different RPC subsystem's request/response shapes, left for
  later. At the time this was written there was also no device/peripheral
  auto-discovery UI (see F-f below, added afterwards) -- the source input
  was a plain number the user had to set manually with no way to know
  which sources existed.
  Verified: `web/test/relay.spec.ts` (new) covers
  resolve-on-matching-notification, ignore-unmatched-id,
  ignore-non-PeripheralResponse (e.g. a `FrameStreamChunk`),
  ignore-garbage-payload, timeout-rejects (fake timers), `clear()`-rejects,
  a resolved id not double-resolving, and `callPmw3610Request()`'s
  immediate-vs-deferred paths including an empty-response error; all prior
  `SensorInfo`/`FrameViewer` component tests, `npm run lint`, and
  `npm run build` (including `tsc` typecheck) still pass.
- **F-f (list every PMW3610 across the whole keyboard) — implemented.**
  F.2's original plan was a cached device inventory kept up to date by a
  peripheral "announce on connect" + a `DeviceInventoryChanged`
  notification; that was never built. Once F-a through F-e were done, a
  gap became concrete: there was still no way to discover *which* `source`
  values (peripherals) exist at all -- the user had to already know. Rather
  than build the F.2 caching design (adds boot-time announce plumbing +
  cache-invalidation state), this reuses the relay mechanism directly:
  `GetInfoRequest.source` gained a sentinel, `PMW3610_SOURCE_ALL =
  0xFFFFFFFF` (`#define` in `pmw3610_request_exec.h`) -- `source: 0`
  local devices are returned synchronously as usual, and (if
  `CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY`) the *same* request is additionally
  broadcast to every connected peripheral via a new fire-and-forget
  `pmw3610_relay_broadcast_request()` (factored out of
  `pmw3610_relay_dispatch_request()`'s existing encode/raise step into a
  shared `send_relay_request()` helper); each peripheral answers
  independently as its own `PeripheralResponse`, all sharing one new
  `GetInfoResponse.relay_request_id` field the caller correlates by.
  `pmw3610_handler.c`'s dispatch special-cases this exactly once (GetInfo +
  source == ALL: execute locally *and* broadcast, instead of the normal
  "local xor relay" branch) -- no other request kind supports the
  sentinel. web/src/relay.ts's `PeripheralResponseCorrelator` gained
  `collectBroadcast(requestId, windowMs)`, collecting every
  `PeripheralResponse` for a request_id over a window (as opposed to
  `waitFor()`'s single-match-then-done) instead of timing out on the
  (correct, expected) case of zero-or-more answers; `SensorInfo.tsx` got a
  "Scan All Sources" button/card listing one row per discovered source.
  **Deliberately not implemented**: a synchronous "list connected
  peripherals" query (which would be simpler and not need the async
  collect-over-a-window dance) -- ZMK's split transport tracks connection
  state in `active_transport` (`app/src/split/central.c`), but does not
  expose it via any public header (only a peripheral-battery-level getter
  exists for a *known* source, not enumeration), so reaching into it would
  mean depending on a ZMK-internal symbol not meant for module use. The
  relay broadcast achieves the same practical result (discover which
  peripherals exist, by asking them) without that.
  Verified: firmware -- all 8 `west zmk-build` artifacts still succeed,
  including `pmw3610_split_central` with a **new** central-role self-test
  (`pmw3610_split_relay_central_test_init`, asserts
  `pmw3610_relay_broadcast_request()` assigns distinct nonzero
  `request_id`s) enabled via `CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY_TEST=y`
  and checked by `test.py`'s ELF-symbol assertions; `tests/split_peripheral`
  and `tests/studio` still pass unchanged (the peripheral executor ignores
  `source` regardless of its value, so ALL needed no peripheral-side
  changes to test). **A dedicated native_sim `tests/split_central` test
  was attempted and abandoned**: `pmw3610_relay_broadcast_request()`
  synchronously raises the same `ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL`
  path as the already-central-only-testable
  `pmw3610_relay_dispatch_request()`, which calls
  `zmk_split_central_send_relay_event()` -- implemented only by ZMK's BLE
  split transport (`app/src/split/bluetooth/central.c`), which needs
  `CONFIG_ZMK_BLE=y` (a working Bluetooth stack) to link; getting that
  running under native_sim was judged out of scope for this check, so
  `pmw3610_split_central`'s central self-test is compile/link-verified
  only, the same limitation the (pre-existing, never native_sim-tested)
  single-target relay dispatch already had. Also fixed in passing: added a
  missing `#include <zmk/split/central.h>` in `pmw3610_relay.c` (needed for
  `zmk_split_central_send_relay_event`'s declaration -- `event_manager.h`'s
  `ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL()` doesn't include it for you, unlike
  the peripheral-side macro) -- harmless on a real board build (something
  else was pulling it in transitively there) but a hard compile error
  without it once a config without that transitive include was tried.
  web: `web/test/relay.spec.ts` gained `collectBroadcast()` coverage
  (collects multiple responses within the window, resolves empty when
  nobody answers, doesn't double-consume with a concurrent `waitFor()`);
  `npm test`/`lint`/`build` all still pass.

### Hardware validation status (2026-07)

Confirmed end-to-end on real hardware (2 physical XIAO nRF52840 boards, one
with a real PMW3610 sensor wired as the split peripheral, one as split
central, both over a real BLE split link): a Studio RPC `GetInfo{source: 1}`
sent to the central's `cormoran__pmw3610` subsystem returns an immediate
`DeferredResponse`, the peripheral executes the relayed request against its
real local PMW3610 (`product_id: 62` / 0x3E, `ready: true`, full
`runtime_config`), and the central re-raises the relayed `RelayResponse` as
a `PeripheralResponse` Studio notification carrying that real device data
back to the client — the full round trip this design's relay bridge exists
for.

Getting there surfaced a real firmware bug, since fixed (see below): the
peripheral's relay-request executor originally ran synchronously on
whichever thread ZMK core's split relay dispatch happened to use (the
system workqueue on both roles), and with this subsystem's ~230-byte
relayed messages that call chain (decode → `pmw3610_request_exec_handle()`
→ nanopb-encode → ZMK core's own relay-out path) overflowed the default
2048-byte system workqueue stack — an MPU fault / stack overflow, confirmed
via RTT, that silently killed the response before it ever reached central
(the request-side `DeferredResponse` still came back fine, since that part
completes before the crash). Fixed by moving every relay listener in
`src/split/pmw3610_relay.c`, plus frame streaming's capture loop in
`src/studio/pmw3610_request_exec.c` (same system-workqueue exposure, worse
in degree since it blocks for ~2s per captured frame), off the system
workqueue entirely and onto ZMK core's own dedicated low-priority workqueue
(`zmk_workqueue_lowprio_work_q()`, `app/src/workqueue.c` — the same queue
ZMK core itself uses for GATT notify work in `gatt_rpc_transport.c`, for the
identical reason). Each listener now copies its event payload into a static
buffer and submits a `k_work` item to that queue instead of processing
inline; `CONFIG_ZMK_PMW3610_PROTOBUF` now `select`s
`ZMK_LOW_PRIORITY_WORK_QUEUE`, and the split-rpc-relay snippet sizes
`CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE=4096` for it. This is strictly
better than the system-workqueue-stack-size band-aid it replaces: the low
priority queue's stack is dedicated to latency-tolerant, deliberately
deferred work, so sizing it for this subsystem's worst case doesn't bloat
every other system workqueue consumer's headroom requirement, and a slow
protobuf pass (or frame streaming's capture loop) can no longer block
unrelated system workqueue work (BLE housekeeping, HID reports,
keyscan-deferred work, ...) system-wide.

Also confirmed while debugging the above: repeatedly halting either board's
CPU via J-Link (e.g. for RTT log capture) while its BLE softdevice link is
live is itself capable of destabilizing the connection (observed as a
kernel oops / an apparent reconnect stall on the central) — a rig/tooling
caveat for future hardware sessions on this or any BLE-softdevice-based
board, not a firmware bug. Reading RTT logs without first zeroing the
`_SEGGER_RTT` control block's signature (forcing a fresh `WrOff`/`RdOff` on
next boot) is also unreliable across resets/reflashes: the ring buffer
retains stale bytes from earlier sessions at offsets the current session
hasn't reached yet, which briefly looked like a *recurring* crash before
this was understood to be one stale capture read twice.

Not yet exercised on hardware: a real sensor's persisted `cpi@<id>` value
across reboot, more than one PMW3610 on the same half, a real
`SetFrameStream` from a peripheral driven from the web UI's "Split source"
input, and `GetInfo{source: PMW3610_SOURCE_ALL}`'s broadcast-across-every-
peripheral path (F-f) with more than one peripheral attached. Re-run the
"Validation plan (hardware)" steps below for those once a second peripheral
or a persisted-settings scenario is available.

Facts about the dependencies below were verified against the checked-out
sources (paths cited).

### F.0 Facts this design builds on (verified in dependencies/)

- **Studio RPC runs only on the central**: `app/src/studio/Kconfig` has
  `select ZMK_STUDIO_RPC if !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL`. A
  peripheral never sees RPC requests directly.
- **ZMK fork has a generic split event relay**
  (`CONFIG_ZMK_SPLIT_RELAY_EVENT`, `app/include/zmk/event_manager.h`):
  - `ZMK_RELAY_EVENT_HANDLE(event_type, identifier, source_field_name)` —
    receive relayed events; on receive the event's `source_field_name` is set
    to `relay source + 1` (so 0 = raised locally … actually 0 is never set on
    receive; the *received* value is `peripheral slot + 1` on central, and
    central-originated events arrive at the peripheral with source `1`).
    Events raised locally must set the field to
    `ZMK_RELAY_EVENT_SOURCE_SELF` (0xFF) to be eligible for relaying.
  - `ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(type, id, src_field)` /
    `ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(type, id, src_field)` — auto-relay
    a locally-raised ZMK event across the link. Identifier ≤
    `CONFIG_ZMK_SPLIT_RELAY_EVENT_TYPE_NAME_LEN` (default 4) chars.
  - Payload ≤ `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` (default 128) bytes;
    the transport chunks larger-than-MTU payloads itself (sequence + final
    bit in `relay_event_header`), so raising this config is safe.
  - Delivery is **best-effort** (`k_msgq_put(..., K_NO_WAIT)` on central,
    bounded queues) — events can drop under pressure. The design must
    tolerate lost messages (timeouts, idempotent commands, per-chunk offsets).
- **zmk-feature-custom-settings already solves split settings**:
  `CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY`
  (`dependencies/zmk-feature-custom-settings/src/studio/custom_settings_handler.c`)
  relays settings RPC to peripherals over the event relay (identifiers
  `csr`/`csn`) and converts peripheral replies back into Studio
  *notifications* tagged with `source`. Source addressing is already defined:
  `ZMK_CUSTOM_SETTING_SOURCE_LOCAL` = 0, peripherals = slot+1,
  `ZMK_CUSTOM_SETTING_SOURCE_ALL` = UINT32_MAX
  (`include/cormoran/zmk/custom_settings.h`). Peripheral-targeted list/read
  results arrive **asynchronously as notifications**, not as the RPC
  response. Settings themselves live in *each half's own* section iterable +
  NVS — a peripheral stores and applies its own values locally.
- `struct zmk_custom_setting.key` is `const char *`
  (`include/cormoran/zmk/custom_settings.h`), so a key string may live in a
  module-owned static buffer filled at early boot (before `main()`'s
  `settings_load()`), enabling runtime-composed per-device keys.
  `CONFIG_ZMK_CUSTOM_SETTINGS_KEY_MAX_LEN` = 48 (max 48 with protobuf).

### F.1 Per-device settings (multi-sensor, one firmware image)

Today all PMW3610 devices of one image share a single set of global keys
(`cpi`, `swap_xy`, …) — the change listener re-applies every key to every
device. Replace with **one settings entry set per DT instance**:

- **Key naming**: `"<param>@<device-id>"` (e.g. `cpi@a3f2`,
  `rest1_downshift_ms@a3f2`). Longest is `report_interval_min_ms@` (23) +
  id — keep ids ≤ 8 chars to stay under KEY_MAX_LEN 48. This follows the
  suggestion to prefer stable name-derived keys over `array_index`:
  array-style keys (or reusing today's globals with an index) silently
  re-target when a sensor is added/removed/reordered in DT; name-derived
  keys survive DT reshuffles.
- **Device id**: an optional new DT property `settings-id` (string, added to
  `dts/bindings/cormoran,pmw3610.yml`) for a human-chosen stable id
  ("trackball", "thumb"); when absent, fall back to a short hash — 4 hex
  chars of FNV-1a over `DT_NODE_PATH(node)` (path, not node name: two
  `pmw3610@0` on different SPI buses must not collide). The hash is computed
  at boot (a `SYS_INIT` at `POST_KERNEL`, i.e. before `main()` runs
  `settings_load()`), written into per-entry static key buffers that the
  `zmk_custom_setting.key` pointers reference. Uniqueness is only needed
  *within one half* (each half has its own NVS + settings registry); the web
  UI disambiguates across halves by `source`.
- **Entry generation**: `DT_FOREACH_STATUS_OKAY(cormoran_pmw3610, ...)`
  expands the 13 per-parameter entries per instance (13 × N
  `STRUCT_SECTION_ITERABLE`s, same designated-initializer style as today —
  the nested-compound-literal restriction still applies). Bonus this
  unlocks: **per-device defaults from DT** — e.g. `force_awake`'s default
  can finally be that instance's `force-awake` DT property, removing the
  Phase B caveat where DT `force-awake;` was overridden to `false`.
- **Apply path**: `pmw3610_settings_apply_to_device(dev)` reads that
  device's own keys (compose key from the device's id). The
  `zmk_custom_setting_changed` listener parses the key suffix and re-applies
  to the *matching device only* (fall back to "apply all" is no longer
  needed).
- **API addition**: `pmw3610_get_settings_id(dev, buf, len)` (or expose the
  id table) in `pmw3610_api.h` so the RPC handler can report each device's
  id (see F.3 — the web UI needs it to group settings per device).
- **Migration / compatibility**: this is a breaking change for persisted
  values (old global `cpi` etc. become orphaned; devices fall back to
  defaults). Given the module is pre-1.0 and settings are trivially re-set
  from the web UI, accept the break and document it in README (mention
  `resetSettings` / re-save). A read-legacy-key-on-first-boot shim is
  possible but not worth the code.

Split note: **nothing else is needed for settings on peripherals.** The
peripheral image compiles its own entries from its own DT; custom-settings'
split relay (`CONFIG_ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY` on both halves +
`CONFIG_ZMK_SPLIT_RELAY_EVENT`) exposes them to the Studio client with
`source` addressing, values persist in the peripheral's own flash, and this
module's (peripheral-side) change listener applies them locally. The work is
config + docs + web UI plumbing, not firmware code.

### F.2 Split RPC bridge (info / diagnostics / registers / frames)

The `cormoran__pmw3610` Studio subsystem keeps running only on the central.
Peripheral sensors are reached via a module-owned pair of relayed events
mirroring the custom-settings pattern (`custom_settings_handler.c` is the
reference implementation to copy):

- **New proto file** `proto/cormoran/pmw3610/pmw3610_relay.proto` (nanopb on
  both halves):

  ```proto
  message RelayRequest  { uint32 request_id = 1; Request request  = 2; }
  message RelayResponse { uint32 request_id = 1; Response response = 2; }
  // FrameStreamChunk notifications are relayed as-is (no request_id).
  message RelayNotification { Notification notification = 1; }
  ```

- **New relayed ZMK events** (identifiers ≤ 4 chars, `source` field
  mandatory for bidirectional safety):

  ```c
  struct zmk_pmw3610_relay_request  { uint8_t source; uint8_t size; uint8_t payload[..]; };
  struct zmk_pmw3610_relay_response { uint8_t source; uint8_t size; uint8_t payload[..]; };
  ZMK_RELAY_EVENT_HANDLE(zmk_pmw3610_relay_request,  pmq, source);
  ZMK_RELAY_EVENT_HANDLE(zmk_pmw3610_relay_response, pmp, source);
  ZMK_RELAY_EVENT_CENTRAL_TO_PERIPHERAL(zmk_pmw3610_relay_request,  pmq, source);
  ZMK_RELAY_EVENT_PERIPHERAL_TO_CENTRAL(zmk_pmw3610_relay_response, pmp, source);
  ```

  `payload` is the nanopb-encoded Relay{Request,Response,Notification};
  `BUILD_ASSERT` the max encoded sizes against
  `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN` exactly like custom-settings does.

- **Addressing**: every device-targeted proto request gains a `source`
  field (`uint32`, 0 = local/central, N = peripheral slot N — **same
  convention as custom-settings** so one mental model covers both
  subsystems). `device_index` stays and is interpreted *within* that
  source's device list. `GetInfoRequest` stays broadcast (see below).

- **Async model** (per the "指示は relay、結果は notification" direction —
  and forced by the transport anyway): RPC dispatch is synchronous, relay
  round-trips are not, so a peripheral-targeted request returns
  **immediately** with a new `DeferredResponse { uint32 request_id; }`; the
  real payload arrives later as a Studio notification:

  ```proto
  message PeripheralResponse { uint32 source = 1; uint32 request_id = 2; Response response = 3; }
  message Notification {
      oneof notification_type {
          FrameStreamChunk frame_stream_chunk = 1;   // gains: uint32 source
          PeripheralResponse peripheral_response = 2;
          DeviceInventoryChanged device_inventory_changed = 3; // empty; “re-GetInfo”
      }
  }
  ```

  `request_id` is a central-assigned monotonically increasing counter.
  Requests targeting `source == 0` keep today's fully synchronous behavior
  (zero change for non-split users). Central keeps a small fixed-size table
  of in-flight relayed request ids (for logging/timeout only — correlation
  is the client's job; entries expire after ~5s, matching the frame-capture
  deadline).

- **Peripheral executor** (`src/split/pmw3610_relay_peripheral.c`): listener
  for `zmk_pmw3610_relay_request` decodes the inner `Request`, runs the
  *existing* handler logic against local devices (refactor
  `pmw3610_handler.c`'s per-request functions into a transport-independent
  core, e.g. `src/studio/pmw3610_request_exec.c`, so central RPC and
  peripheral relay share one implementation), encodes the `Response` into a
  `RelayResponse`, raises it with `source = ZMK_RELAY_EVENT_SOURCE_SELF` for
  relaying up. Executor work runs on the system workqueue (frame capture
  blocks up to ~5s — must not run in the BLE RX path; a 1-deep pending-work
  model with "busy" `ErrorResponse` for overlapping requests is enough).
- **Central bridge** (`src/split/pmw3610_relay_central.c`): forwards
  peripheral-targeted requests; on `zmk_pmw3610_relay_response`, wraps the
  decoded payload in `PeripheralResponse{source = ev.source}` and raises it
  as a custom Studio notification (same
  `raise_zmk_studio_custom_notification` path Phase E uses).

- **Device inventory**: `GetInfoResponse.DeviceInfo` gains `source`,
  `device_index`, `name` (`dev->name`), `settings_id` (F.1), and the
  response gains a per-source `reachable` flag. Central answers `GetInfo`
  from local devices + a **cached** peripheral inventory. The cache is
  filled by peripherals *announcing* their device list (an unsolicited
  `RelayResponse` carrying `GetInfoResponse`, request_id 0 = announce) when
  the split link connects (peripheral subscribes to
  `zmk_split_peripheral_status_changed`), and refreshed on demand: a
  peripheral-targeted `GetInfo{source=N}` follows the normal deferred path.
  Central raises `DeviceInventoryChanged` when the cache changes (announce
  received / peripheral disconnected) so the web UI re-queries.

- **Frame streaming from a peripheral**: `SetFrameStream{source=N}` is
  relayed like any command; the *existing* Phase E capture loop runs on the
  peripheral; each chunk is raised locally as `RelayNotification
  {FrameStreamChunk}` → relayed up → central re-raises it as the usual
  Studio notification with `source` filled in. Constraints:
  - Chunk payload must fit one relay event: require
    `CONFIG_ZMK_SPLIT_RELAY_EVENT_DATA_LEN ≥ 160` when
    `ZMK_PMW3610_SPLIT_RPC_RELAY` is on (BUILD_ASSERT), keeping the 128-byte
    chunk size uniform end-to-end (encoded chunk ≈ 128 + ~20B proto/envelope
    overhead). custom-settings documents the same "raise DATA_LEN" pattern.
  - Loss tolerance: chunks may drop (best-effort relay). The web assembler
    already only completes a frame when all bytes arrived; add a client-side
    stale-frame drop (new `frame_id` or timeout discards a partial frame).
    No firmware retransmit.
  - Keep the "one active stream per firmware image" rule per half
    (peripheral enforces locally), but the *central* additionally tracks a
    single global active stream target to keep UI semantics identical.
  - **Lock/disconnect stops**: central's existing lock listener additionally
    relays `SetFrameStream{enable:false}` to the streaming peripheral;
    the peripheral independently force-stops its stream when the split link
    drops (its `zmk_split_peripheral_status_changed` listener) — covers the
    "central rebooted / out of range while streaming" hole.

### F.3 Kconfig / build changes

- `ZMK_PMW3610_PROTOBUF` (internal, promptless): owns nanopb generation of
  `pmw3610.proto` + `pmw3610_relay.proto`. Selected by both options below.
  Today generation is tied to `ZMK_PMW3610_STUDIO_RPC`; a peripheral has no
  Studio, so this decoupling is required (mirrors
  `ZMK_CUSTOM_SETTINGS_PROTOBUF`).
- `ZMK_PMW3610_STUDIO_RPC` — unchanged semantics (central / non-split).
- `ZMK_PMW3610_SPLIT_RPC_RELAY` — `depends on ZMK_SPLIT_RELAY_EVENT`,
  selects `ZMK_PMW3610_PROTOBUF`. Compiles the common event glue plus, by
  role: central bridge (`ZMK_SPLIT_ROLE_CENTRAL`, additionally requires
  `ZMK_PMW3610_STUDIO_RPC`) or peripheral executor. Enabled on **both**
  halves.
- Settings: no new symbol — `ZMK_PMW3610_CUSTOM_SETTINGS` on both halves +
  `ZMK_CUSTOM_SETTINGS_SPLIT_RPC_RELAY` (dependency's own symbol) on both.
- README gains a split section: required configs per half, DATA_LEN ≥ 160,
  and the reminder that `&studio_unlock` lives on whichever half has the
  keymap position — lock state is central-only, unaffected by this design.

### F.4 Web UI changes

- **Device model**: selector becomes a flat list of
  `{source, device_index, name, settings_id}` built from the extended
  `GetInfo`; refreshed on `DeviceInventoryChanged` notifications.
- **Async correlation helper** (`web/src/relay.ts`, pure + unit-testable): a
  pending-request map `request_id → {resolve, reject, deadline}` fed by
  `PeripheralResponse` notifications; peripheral-targeted calls return a
  promise that resolves on the matching notification or rejects on timeout
  (~6s > firmware's 5s capture deadline). Local calls bypass it.
- **FrameViewer**: chunks are demuxed by `source`; one assembler per active
  stream; stale-frame drop as in F.2. Streaming FPS over BLE relay is
  bounded by capture time (~2s/frame) — no UI pacing needed.
- **SettingsPanel**: group keys by `settings_id` suffix into per-device
  cards; pass `source` through the custom-settings RPC calls (SettingRef/
  SettingScope already carry `source` in the dependency's proto); handle the
  dependency's async notification-based replies for peripheral sources.

### F.5 Tests

- **Unit (native_sim)**: keep `tests/studio` (central, source=0 unchanged);
  add `tests/split_peripheral` modeled on
  `dependencies/zmk-feature-custom-settings/tests/split_peripheral` (builds
  with `CONFIG_ZMK_SPLIT=y`, `CONFIG_ZMK_SPLIT_RELAY_EVENT=y`, peripheral
  role, zero devices): boots the executor, feeds it synthetic
  `zmk_pmw3610_relay_request` events (add a test-only injector, as the Dev
  Rules suggest), asserts encoded `RelayResponse` events come back
  (GetInfo announce, error path for bad device_index).
- **Build tests** (`tests/zmk-config`): add split artifacts — e.g.
  `pmw3610_split_central` (RPC + relay + settings relay) and
  `pmw3610_split_peripheral` (driver + relay + settings relay, sensor
  overlay on the peripheral) on a split-capable shield; `test.py` asserts
  the relay configs and device nodes are present in each half's build.
- **Web**: unit tests for the correlation map (resolve/timeout/unknown id)
  and per-source frame demux/stale-frame drop (extend `frame.ts` tests).

### F.6 Implementation order (per Dev Rules: proto → firmware → web)

1. **F-a** Per-device settings keys + `settings_id`/`name` in GetInfo
   (local-only; no split yet). Breaking-change note in README.
2. **F-b** Relay proto + Kconfig split (`ZMK_PMW3610_PROTOBUF`) + handler
   refactor into transport-independent executor. No behavior change.
3. **F-c** Peripheral executor + central bridge for
   GetInfo/ReadDiagnostics/Read+WriteRegister (deferred responses,
   inventory announce/cache). Unit + build tests.
4. **F-d** Frame capture + streaming over relay, lock/disconnect stop
   paths.
5. **F-e** Web UI (device model, correlation helper, per-device settings
   cards, frame demux) + web tests + README split guide.

### F.7 Risks / open questions

- **Best-effort relay**: dropped announce → central shows a reachable
  peripheral with no devices; mitigated by on-demand `GetInfo{source=N}`
  re-query from the UI. Dropped `RelayResponse` → client timeout (surfaced
  as a retryable error). Acceptable for a diagnostics tool.
- **Relay payload sizing**: `Response` for `GetInfo` with many devices may
  exceed one relay event even at DATA_LEN 160 — cap `.options`
  `max_count` for relayed GetInfo (e.g. 4 devices/half) and BUILD_ASSERT
  encoded max size. To be pinned down when writing the `.options` file.
- **Peripheral flash wear**: settings persist on the peripheral's NVS —
  same behavior as central today, no new mechanism.
- **Security**: relayed commands originate only from the central's SECURED
  RPC handlers, so the lock gate is preserved; the peripheral trusts the
  split link exactly as ZMK's own split features do (keystrokes flow over
  the same trust boundary).
- **ID collisions**: 4-hex FNV over node paths within one half — collision
  chance is negligible for realistic sensor counts (≤4/half); detect at
  boot (log + keep first) and let `settings-id` override as the escape
  hatch.

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
6. `CaptureFrame` + `GetFrameChunk` (primary Phase D task -- done) →
   Phase D found the Phase C guessed procedure never asserts PG_Valid;
   replaced with the official datasheet (R2.4) Pixel_Grab sequence (see
   "Frame capture" above) and validated on hardware: assembled frames,
   checked `complete`/PG_Valid, confirmed the 484 = 22x22 array size
   (datasheet Figure 17). Example CLI JSON request/response shapes are in
   README.md's "Frame capture" section.
7. Web UI smoke test (vite dev + Chrome WebSerial) if environment allows;
   otherwise CLI-driven equivalent via `custom-call`. In particular verify
   the Frame Viewer's streaming loop against real RPC round-trip latency
   (the web-side "streaming" is a sequential capture loop with no fixed
   interval, so FPS is bounded entirely by hardware/RPC speed, not
   simulated in any test so far).
