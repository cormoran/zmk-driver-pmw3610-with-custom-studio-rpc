#!/usr/bin/env python3
"""Boot a USB+wired PMW3610 split (central answering Studio over USB CDC +
peripheral with a simulated PMW3610 on SPIM0) under Renode for the PMW3610 relay
round-trip test.

This is a module-local variant of zmk-west-commands' renode_harness.boot_usb_wired_split:
that helper puts the peripheral on the plain sensor-less wired-split repl, whereas
the PMW3610 relay test needs the split PERIPHERAL machine to carry a *simulated
PMW3610* on SPIM0 (platforms/PMW3610.cs + the LATCH-aware GPIO) so the
peripheral's ZMK driver inits for real and answers relayed requests. The central
is unchanged: the real studio-rpc-usb-uart image on the NRF_USBD_Full USB
platform, so it answers Studio RPC over the emulated USB CDC -- the transport
that round-trips the large relayed PMW3610 response.

Topology:

    host(CDC bridge) --USB(Studio)--> CENTRAL --wired(split relay)--> PERIPHERAL (sim PMW3610)

Requires `renode_harness` on PYTHONPATH (provided by `west zmk-renode-test`); it
reuses that module's materialization + USB-CDC-bridge helpers so this file stays
a thin wrapper.
"""

from __future__ import annotations

import os
import time
from pathlib import Path

import renode_harness as rh

MODULE_RENODE_DIR = Path(__file__).resolve().parent
PLATFORMS_DIR = MODULE_RENODE_DIR / "platforms"
RESC = PLATFORMS_DIR / "pmw3610_usb_wired_split.resc"

# The central presents a SINGLE USB CDC (Studio) because the shield disables the
# board console CDC (CONFIG_BOARD_SERIAL_BACKEND_CDC_ACM=n) -- so the Studio RPC
# rides cdc0. A dual-CDC image (board console CDC on) would put Studio on cdc1;
# we detect that via the bridge's IsWired flags, mirroring renode_smoke.
USB_BRIDGE_NAME = "bridge"


def boot(
    renode_path: str,
    central_elf: Path,
    peripheral_elf: Path,
    storage_addr: int = rh.STORAGE_ADDR_DEFAULT,
    storage_size: int = rh.STORAGE_SIZE_DEFAULT,
    boot_wait: float = 4.0,
    usb_settle: float = 2.0,
    port_base: int = 32000,
    renode_log: Path | None = None,
):
    """Boot the two machines, attach the USB CDC bridge to the central, and return
    (session, central_console, studio_rpc, peripheral_console). studio_rpc is the
    framed RpcSocket for the central's Studio CDC (host side). Caller owns cleanup
    (session.stop() + closing sockets)."""
    # BLE is OFF on both halves (wired split), so -- unlike boot_ble_split -- no
    # per-machine FICR BLE identity and no fake CCM are needed. The central still
    # boots the real image (USB/QSPI/FICR/NVMC stubs), so materialize the USB repl
    # and preload its erased NVS, exactly as usb mode does.
    central_repl = rh._materialize_real_repl(template_name="xiao_nrf52840_usb.repl")
    ff_path = rh._write_ff_binary(storage_size)
    tmps = [central_repl, ff_path]

    session = rh.RenodeSession(
        renode_path,
        RESC,
        monitor_port=port_base,
        variables={
            "central_bin": f"@{central_elf}",
            "peripheral_bin": f"@{peripheral_elf}",
            "central_platform": f"@{central_repl}",
            "central_console_port": port_base + 1,
            "peripheral_console_port": port_base + 2,
        },
        cwd=MODULE_RENODE_DIR,
    )
    try:
        session.start(boot_wait=boot_wait)
        assert session.mon is not None
        if renode_log is not None:
            session.mon.execute(f"logFile @{renode_log}")
        central_console = session.connect_uart(port_base + 1)
        peripheral_console = session.connect_uart(port_base + 2)

        # Preload the CENTRAL's erased NVS before the CPUs run. LoadBinary is
        # machine-scoped; the resc leaves the peripheral (created last) selected,
        # so select the central first. Leave it selected so attach_dual_cdc_bridge
        # (sysbus.usbd ...) targets the central.
        session.mon.execute('mach set "central"')
        session.mon.execute(f"sysbus LoadBinary @{ff_path} {hex(storage_addr)}")
        session.go()
    except Exception:
        session.stop()
        for tmp in tmps:
            try:
                os.unlink(tmp)
            except OSError:
                pass
        raise

    for tmp in tmps:
        try:
            os.unlink(tmp)
        except OSError:
            pass

    # Let the central settle its USB init, then attach the CDC bridge (central is
    # selected). A single-CDC image -> Studio on cdc0; dual-CDC -> cdc1.
    time.sleep(usb_settle)
    cdc = list(
        rh.attach_dual_cdc_bridge(
            session, port_base + 3, port_base + 4, name=USB_BRIDGE_NAME
        )
    )
    dual_cdc = bool(_mon_flag(session.mon, f"sysbus.{USB_BRIDGE_NAME}_cdc1 IsWired"))
    studio_rpc = cdc[1] if dual_cdc else cdc[0]
    session._cdc_sockets = cdc  # keep both alive for the session's lifetime
    return session, central_console, studio_rpc, peripheral_console


def _mon_flag(mon, cmd: str, timeout: float = 5.0):
    """Return True/False for a monitor command that prints a boolean, else None."""
    import re

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        out = mon.execute(cmd, settle=0.3)
        m = re.search(r"\b(True|False)\b", out)
        if m:
            return m.group(1) == "True"
    return None
