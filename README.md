# zmk-driver-pmw3610-with-custom-studio-rpc

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-module-template/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-module-template/actions/workflows/zmk-module.yml) [![Devcontainer](https://github.com/cormoran/zmk-module-template/actions/workflows/devcontainer.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-module-template/actions/workflows/devcontainer.yml)

ZMK module providing the [PMW3610](https://github.com/cormoran/zmk-pmw3610-driver)
optical mouse sensor driver, with a Web UI and an **unofficial** custom ZMK
Studio RPC protocol for diagnostics (and, in later phases, runtime settings
and frame capture).

This is a drop-in replacement for `cormoran/zmk-pmw3610-driver` (same
devicetree compatible `cormoran,pmw3610`) — do not use both modules together.

See [DESIGN.md](./DESIGN.md) for the full module design and roadmap. This
module is under active development; the sections below will be filled in as
features land.

## Summary

This module currently includes (Phase A):

- **Firmware**: PMW3610 sensor driver (`src/pmw3610.c`) and a minimal custom
  Studio RPC handler (`src/studio/pmw3610_handler.c`) exposing sensor
  presence/diagnostics via `GetInfo`.
- **Protocol**: Protobuf definition (`proto/cormoran/pmw3610/pmw3610.proto`)
- **Web UI**: React + TypeScript app (`web/`) using
  [@cormoran/zmk-studio-react-hook](https://github.com/cormoran/react-zmk-studio)
- **Tests**: Firmware unit tests (`tests/studio/`) and build tests
  (`tests/zmk-config/`)

Planned next: runtime-configurable settings via
[zmk-feature-custom-settings](https://github.com/cormoran/zmk-feature-custom-settings),
raw register access, frame (image) capture streaming, and a full web UI.

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

   # Optionally enable custom Studio RPC diagnostics
   CONFIG_ZMK_STUDIO=y
   CONFIG_ZMK_PMW3610_STUDIO_RPC=y
   CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256
   ```

3. Explore the protocol / firmware handler / web UI:
   - `proto/cormoran/pmw3610/pmw3610.proto` — message types
   - `src/studio/pmw3610_handler.c` — firmware RPC handler
   - `web/src/App.tsx` — web UI

### Web UI

See [web/README.md](./web/README.md) for web UI development instructions.

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
