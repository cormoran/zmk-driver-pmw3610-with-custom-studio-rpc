# PMW3610 Module - Web Frontend

This is a web application (sensor configurator + frame viewer) for the
PMW3610 ZMK module's custom Studio RPC subsystem.

## Features

- **Device Connection**: Connect to ZMK devices via Serial (WebSerial)
- **Sensor info & diagnostics**: `GetInfo` (ready/product id/revision/init
  error/runtime config) and `ReadDiagnostics` (SQUAL/shutter/pixel min-avg-max),
  with optional 1s auto-refresh for diagnostics
- **Settings panel**: generic editor for the sensor's own custom settings
  (subsystem `cormoran__pmw3610`), talking to the separate
  `cormoran_custom_settings` subsystem provided by
  [zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings)
  (list/edit/write with memory or persist mode, save/discard/reset)
- **Frame viewer**: capture a still image from the sensor (`CaptureFrame` +
  chunked `GetFrameChunk`) and render it to a `<canvas>`, with a
  capture-once button, a streaming toggle (sequential capture loop, not a
  fixed-interval timer), an FPS counter, an invalid-byte (PG_VALID clear)
  warning count, and an advanced panel for tuning the (hardware-unvalidated
  as of this writing) capture procedure
- **React + TypeScript**: Modern web development with Vite for fast builds
- **react-zmk-studio**: Uses the `@cormoran/zmk-studio-react-hook` library for
  simplified ZMK integration

## Quick Start

```bash
# Install dependencies
npm install

# Generate TypeScript types from proto
npm run generate

# Run development server
npm run dev

# Build for production
npm run build

# Run tests
npm test
```

## Project Structure

```
src/
├── main.tsx              # React entry point
├── App.tsx               # Main application: connection + feature cards
├── App.css               # Styles
├── SensorInfo.tsx         # "Sensor" + "Diagnostics" cards (GetInfo/ReadDiagnostics)
├── SettingsPanel.tsx      # "Settings" card (generic custom-settings editor)
├── FrameViewer.tsx        # "Frame Viewer" card (CaptureFrame/GetFrameChunk + canvas)
├── frame.ts               # Pure frame-chunk reassembly + pixel conversion helpers
├── settingsJson.ts        # Settings export/import JSON document helpers
└── proto/                 # Generated protobuf TypeScript types (buf generate)
    └── cormoran/
        ├── pmw3610/
        │   └── pmw3610.ts
        └── zmk/custom_settings/
            └── custom_settings.ts

test/
├── App.spec.tsx              # Tests for App component
├── SensorInfo.spec.tsx        # Tests for the sensor/diagnostics cards
├── SettingsPanel.spec.tsx     # Tests for the settings editor
├── FrameViewer.spec.tsx       # Tests for the frame viewer controls
└── frame.spec.ts               # Tests for frame reassembly/pixel conversion (pure functions)
```

## How It Works

### 1. Protocol Definition

This module's own protobuf schema is defined in
`../proto/cormoran/pmw3610/pmw3610.proto`. The settings panel additionally
talks to zmk-feature-custom-settings' generic custom-settings subsystem,
whose schema (`cormoran/zmk/custom_settings/custom_settings.proto`) lives in
that dependency's own repo, not this one.

### 2. Code Generation

TypeScript types are generated using `ts-proto`:

```bash
npm run generate
```

This runs `buf generate`, which uses the configuration in `buf.gen.yaml`.
`buf.gen.yaml` lists **two** input directories: this repo's own `../proto`,
and the west dependency checkout at
`../dependencies/zmk-feature-custom-settings/proto` (generated straight from
there, not vendored into this repo's `proto/` tree -- vendoring it there
would make this module's own firmware-side nanopb glob
(`CMakeLists.txt: proto/*.proto`) pick it up too and generate a second,
conflicting copy of the `cormoran.zmk.custom_settings` package, which would
fail to link into the `pmw3610_settings_rpc` test artifact that includes
both modules).

### 3. Using react-zmk-studio

The app uses the `@cormoran/zmk-studio-react-hook` library:

```typescript
import { useZMKApp, ZMKCustomSubsystem } from "@cormoran/zmk-studio-react-hook";

// Connect to device
const { state, connect, findSubsystem, isConnected } = useZMKApp();

// Find the PMW3610 subsystem
const subsystem = findSubsystem("cormoran__pmw3610");

// Create service and make RPC calls
const service = new ZMKCustomSubsystem(state.connection, subsystem.index);
const response = await service.callRPC(payload);
```

The settings panel does the same thing but against a different subsystem:
`findSubsystem("cormoran_custom_settings")` (zmk-feature-custom-settings'
generic settings RPC), using `SettingRef.customSubsystemIndex` to point at
this module's `findSubsystem("cormoran__pmw3610")` result for `list_settings`
/ `write_setting` scopes.

## Testing

```bash
# Run all tests
npm test

# Run tests in watch mode
npm run test:watch

# Run tests with coverage
npm run test:coverage
```

### Writing Tests

Use the test helpers from `@cormoran/zmk-studio-react-hook/testing`:

```typescript
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";

const mockZMKApp = createConnectedMockZMKApp({
  deviceName: "Test Device",
  subsystems: ["cormoran__pmw3610"],
});

render(
  <ZMKAppProvider value={mockZMKApp}>
    <YourComponent />
  </ZMKAppProvider>
);
```

## Frame viewer notes

- The firmware side has a fixed-size static frame buffer
  (`CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE`, default 484 = 22x22).
  `CaptureFrame`'s `pixel_count` is clamped to this size.
- `GetFrameChunk` returns up to 128 bytes per call; the web app loops
  `GetFrameChunk` calls (see `chunkOffsets()` in `src/frame.ts`) until the
  whole frame is collected.
- "Streaming" is a sequential capture loop (capture, fetch all chunks,
  render, repeat), not a fixed-interval timer -- actual frame rate depends
  on RPC round-trip time and is shown by the FPS counter.
- The capture procedure (PIXEL_GRAB/FRAME_GRAB register sequence) is not
  documented in the public datasheet and is unvalidated against real
  hardware as of this writing -- use the "Advanced" panel to experiment
  with `write_frame_grab`/`frame_grab_value`/`skip_pixel_grab_reset`/
  `max_invalid_retries` if the captured image looks wrong.
- Each raw pixel byte's bit7 is PG_VALID; the viewer masks it off for
  display (`(byte & 0x7f) << 1` for 8-bit grayscale) but also counts and
  shows bytes with bit7 clear as a data-quality warning.

## Roadmap

This UI is feature-complete for Phase C (sensor info/diagnostics, settings,
frame viewer). Hardware validation of the frame-capture procedure is a
follow-up phase.
