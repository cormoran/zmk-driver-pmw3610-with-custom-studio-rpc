#!/usr/bin/env python3
"""Boot a wireless BLE split (peripheral w/ a simulated PMW3610 + central + host)
under Renode for the PMW3610 relay round-trip test.

This is a module-local variant of zmk-west-commands' renode_harness.boot_ble_split:
that helper uses a fixed, sensor-less real platform for all three machines,
whereas the PMW3610 relay test needs the split PERIPHERAL machine to carry a
*simulated PMW3610* on SPIM0 (platforms/PMW3610.cs + the LATCH-aware GPIO) so the
peripheral's ZMK driver inits for real and answers relayed requests. Everything
else (fake AES-CCM in all three machines, per-machine FICR BLE identity, erased
NVS preload, peripheral SEGGER-RTT capture) mirrors boot_ble_split.

Requires `renode_harness` on PYTHONPATH (provided by `west zmk-renode-test`); it
reuses that module's materialization helpers so this file stays a thin wrapper.
"""

from __future__ import annotations

import os
import tempfile
from pathlib import Path

import renode_harness as rh

MODULE_RENODE_DIR = Path(__file__).resolve().parent
PLATFORMS_DIR = MODULE_RENODE_DIR / "platforms"
RESC = PLATFORMS_DIR / "pmw3610_ble_split.resc"
CS_PMW = PLATFORMS_DIR / "PMW3610.cs"
CS_LATCH = PLATFORMS_DIR / "NRF52840_GPIO_WithLatch.cs"
PERIPHERAL_REPL = PLATFORMS_DIR / "xiao_nrf52840_real_pmw3610.repl"


def _materialize_sensor_peripheral_repl(ficr_path: str) -> str:
    """Same rewrite as renode_harness._materialize_real_repl, but on the
    sensor-equipped peripheral platform (xiao_nrf52840_real_pmw3610.repl): make
    the PythonPeripheral model `filename:`s absolute (they live in
    zmk-west-commands' platforms/models) and point the FICR model at the
    per-machine ficr .py so the peripheral gets its own BLE address."""
    abs_models = str((rh.PLATFORMS_DIR / "models").resolve())
    repl = PERIPHERAL_REPL.read_text().replace(
        'filename: "platforms/models/', f'filename: "{abs_models}/'
    )
    repl = repl.replace(f'filename: "{abs_models}/ficr.py"', f'filename: "{ficr_path}"')
    fd, path = tempfile.mkstemp(prefix="xiao_real_pmw3610-", suffix=".repl")
    with os.fdopen(fd, "w") as fh:
        fh.write(repl)
    return path


def boot(
    renode_path: str,
    central_elf: Path,
    peripheral_elf: Path,
    host_elf: Path,
    storage_addr: int = rh.STORAGE_ADDR_DEFAULT,
    storage_size: int = rh.STORAGE_SIZE_DEFAULT,
    boot_wait: float = 5.0,
    port_base: int = 31000,
    renode_log: Path | None = None,
):
    """Boot the three machines and return
    (session, central_console, central_rpc, peripheral_rtt, host_console).
    Caller owns cleanup (session.stop() + closing sockets)."""
    central_ficr = rh._materialize_ficr(rh.device_addr_for_machine(0))
    peripheral_ficr = rh._materialize_ficr(rh.device_addr_for_machine(1))
    host_ficr = rh._materialize_ficr(rh.device_addr_for_machine(2))
    central_repl = rh._materialize_real_repl(central_ficr)
    peripheral_repl = _materialize_sensor_peripheral_repl(peripheral_ficr)
    host_repl = rh._materialize_real_repl(host_ficr)
    ccm_repl = rh._materialize_ccm_repl()
    ff_path = rh._write_ff_binary(storage_size)
    tmps = [
        central_ficr,
        peripheral_ficr,
        host_ficr,
        central_repl,
        peripheral_repl,
        host_repl,
        ccm_repl,
        ff_path,
    ]

    session = rh.RenodeSession(
        renode_path,
        RESC,
        monitor_port=port_base,
        variables={
            "central_bin": f"@{central_elf}",
            "peripheral_bin": f"@{peripheral_elf}",
            "host_bin": f"@{host_elf}",
            "central_platform": f"@{central_repl}",
            "peripheral_platform": f"@{peripheral_repl}",
            "host_platform": f"@{host_repl}",
            "ccm": f"@{ccm_repl}",
            "cs_pmw": f"@{CS_PMW}",
            "cs_latch": f"@{CS_LATCH}",
            "c_console": port_base + 1,
            "c_rpc": port_base + 2,
            "p_console": port_base + 3,
            "h_console": port_base + 4,
        },
        # resc @relative + the .cs/platform vars are absolute; cwd only needs to
        # contain the resc so RenodeSession can form its relative path.
        cwd=MODULE_RENODE_DIR,
    )
    session.peripheral_rtt = None
    try:
        session.start(boot_wait=boot_wait)
        assert session.mon is not None
        if renode_log is not None:
            session.mon.execute(f"logFile @{renode_log}")
        central_console = session.connect_uart(port_base + 1)
        central_rpc = session.connect_uart(port_base + 2)
        host_console = session.connect_uart(port_base + 4)
        session._idle_sockets = [
            session.connect_uart(port_base + 3)
        ]  # peripheral console (silent)

        # SEGGER RTT capture on the peripheral (its USB-CDC console is silent),
        # mirroring renode_harness.boot_ble_split.
        rtt_port = port_base + 5
        session.mon.execute('mach set "peripheral"')
        session.mon.execute(f"include @{rh.SEGGER_RTT_HELPER}")
        session.mon.execute('machine CreateVirtualConsole "segger_rtt"')
        session.mon.execute("setup_segger_rtt_wskip sysbus.segger_rtt")
        session.mon.execute(
            f'emulation CreateServerSocketTerminal {rtt_port} "prtt_term" false'
        )
        session.mon.execute("connector Connect sysbus.segger_rtt prtt_term")
        session.peripheral_rtt = session.connect_uart(rtt_port)

        for mach in ("central", "peripheral"):
            session.mon.execute(f'mach set "{mach}"')
            session.mon.execute(f"sysbus LoadBinary @{ff_path} {hex(storage_addr)}")
        session.go()
    finally:
        for tmp in tmps:
            try:
                os.unlink(tmp)
            except OSError:
                pass
    return session, central_console, central_rpc, session.peripheral_rtt, host_console
