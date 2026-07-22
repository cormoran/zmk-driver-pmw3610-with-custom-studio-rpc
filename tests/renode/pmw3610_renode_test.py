#!/usr/bin/env python3
"""Hardware-free functional test: boot this module's firmware in the Renode
emulator with a *simulated PMW3610 sensor* on SPIM0 and drive the driver end
to end -- power-up/self-test handshake, then motion reporting -- with no
J-Link and no real trackball.

What makes this different from the generic Renode smoke test (which only
proves the image boots and speaks core Studio RPC over UART) is the
simulated sensor: `platforms/PMW3610.cs` is a small C# ISPIPeripheral that
answers the driver's SPI register protocol (src/pmw3610.c) -- product id
0x3E, the OBSERVATION self-test nibble, MOTION_BURST reports -- and drives
the motion IRQ line. `platforms/xiao_pmw3610.repl` wires it onto SPIM0 with
the same chip-select (P1.15) and IRQ (P1.14) pins the test snippet's
devicetree uses, and `platforms/pmw3610_single.resc` compiles + loads it.

The sensor exposes Renode-monitor test hooks (QueueMotion, PendingMotionCount,
PowerUpResetCount, ...) so the test can inject motion and inspect what the
firmware wrote, entirely over the monitor socket.

Wiring: run via `west zmk-renode-test tests/renode --elf <ELF> --skip-smoke`
(the default smoke test uses a sensor-less platform, so skip it here), which
sets ZMK_RENODE_ELF and puts `renode_harness` (zmk-west-commands'
scripts/lib/renode) on PYTHONPATH. Build the ELF first with
tests/zmk-config/build-renode.yaml -- see that file's header. The test can
also be run directly with `python3 tests/renode/pmw3610_renode_test.py` once
those are in place.

Named `*_renode_test.py`, not `test_*.py`, on purpose: it needs a real
firmware ELF, so it must stay out of `python3 -m unittest`'s auto-discovery.
"""

from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMS_DIR = Path(__file__).resolve().parent / "platforms"

# renode_harness comes from the zmk-west-commands checkout that
# `west zmk-renode-test` puts on PYTHONPATH. Support running this file
# directly too by falling back to the west dependency this repo already
# fetches, then a sibling checkout.
try:
    import renode_harness
except ImportError:  # pragma: no cover - convenience fallback for local dev
    fallback_candidates = [
        REPO_ROOT / "dependencies" / "zmk-west-commands" / "scripts" / "lib" / "renode",
        REPO_ROOT.parent / "zmk-west-commands" / "scripts" / "lib" / "renode",
    ]
    for fallback in fallback_candidates:
        if fallback.is_dir():
            sys.path.insert(0, str(fallback))
            import renode_harness

            break
    else:
        raise


def _resolve_elf() -> Path:
    env = os.environ.get("ZMK_RENODE_ELF")
    if env:
        return Path(env)
    # `west zmk-build` writes to <west topdir>/build/<artifact>/ and this
    # repo is its own west workspace, so the renode artifact lands here.
    return REPO_ROOT / "build" / "renode" / "zephyr" / "zmk.elf"


class PMW3610RenodeTests(unittest.TestCase):
    """Boots the PMW3610-simulation ELF once for the whole class (boot is the
    slow part) and drives the driver through init + motion reporting."""

    MACHINE = "pmw3610"
    # Sensor peripheral monitor path: it is registered on the SPI controller
    # (`trackball: SPI.PMW3610 @ spi0` in xiao_pmw3610.repl), so it lives
    # under spi0 in the peripheral tree.
    SENSOR = "spi0.trackball"

    renode_path: str
    elf: Path
    session = None
    console = None

    @classmethod
    def setUpClass(cls):
        # This is the single-board sensor test. `west zmk-renode-test` globs every
        # *_test.py in tests/renode/, so a split run (e.g. --mode wired-split for
        # the relay test) would otherwise try to boot this against the split
        # central ELF. Skip when a split link is active; the split relay test owns
        # that case.
        if os.environ.get("ZMK_RENODE_SPLIT_LINK", "none") != "none":
            raise unittest.SkipTest(
                "single-board sensor test; skipped for split runs "
                f"(ZMK_RENODE_SPLIT_LINK={os.environ.get('ZMK_RENODE_SPLIT_LINK')})"
            )

        cls.elf = _resolve_elf()
        if not cls.elf.is_file():
            raise unittest.SkipTest(
                f"firmware ELF not found at {cls.elf}; build it first with "
                "tests/zmk-config/build-renode.yaml (see that file's header)"
            )

        renode_path = renode_harness.find_or_install_renode(
            install_script=PLATFORMS_DIR.parent
            / ".."
            / "dependencies"
            / "zmk-west-commands"
            / "scripts"
            / "lib"
            / "renode"
            / "install_renode.sh"
        )
        if not renode_path:
            raise unittest.SkipTest(
                "Renode is not installed and could not be auto-installed"
            )
        cls.renode_path = renode_path

        # Random-ish but deterministic port base derived from the PID, to
        # avoid collisions when tests run in parallel without using
        # Math.random-style nondeterminism.
        port_base = 27000 + (os.getpid() % 2000)

        cls.session = renode_harness.RenodeSession(
            cls.renode_path,
            PLATFORMS_DIR / "pmw3610_single.resc",
            monitor_port=port_base,
            variables={
                "bin": f"@{cls.elf}",
                "console_port": port_base + 1,
                "rpc_port": port_base + 2,
            },
            # `.resc` @relative paths (PMW3610.cs, xiao_pmw3610.repl) resolve
            # against Renode's cwd -- launch it from tests/renode/ so
            # `@platforms/...` finds them.
            cwd=PLATFORMS_DIR.parent,
        )
        cls.session.start(boot_wait=3.0)
        cls.console = cls.session.connect_uart(port_base + 1)
        cls.session.go()

        # Boot banner first, then wait for the driver's async init to finish.
        # Emulated boot is fast enough that a single drain window can capture
        # everything through init, so accumulate rather than assume the two
        # needles land in separate reads.
        boot_log = renode_harness.wait_for_text(
            cls.console._sock, "Welcome to ZMK", timeout=20.0
        )
        if "Welcome to ZMK" not in boot_log:
            raise AssertionError(
                f"never saw ZMK boot banner on console UART; got:\n{boot_log}"
            )
        if "PMW3610 initialized" not in boot_log:
            boot_log += renode_harness.wait_for_text(
                cls.console._sock, "PMW3610 initialized", timeout=20.0
            )
        cls._boot_log = boot_log
        if "PMW3610 initialized" not in boot_log:
            raise AssertionError(
                "PMW3610 driver never reported successful init (self-test / product-id "
                f"handshake failed under the simulated sensor); console log:\n{cls._boot_log}"
            )

    @classmethod
    def tearDownClass(cls):
        if cls.session is not None:
            cls.session.stop()

    def _mon(self, command: str) -> str:
        assert self.session is not None and self.session.mon is not None
        return self.session.mon.execute(f'mach set "{self.MACHINE}"; {command}')

    def _sensor(self, method_and_args: str) -> str:
        return self._mon(f"{self.SENSOR} {method_and_args}")

    def _sensor_int(self, method_and_args: str) -> int:
        """Run a sensor monitor method and parse its integer return value
        (Renode prints these as e.g. `0x00000001`)."""
        out = self._sensor(method_and_args)
        # Strip ANSI colour codes, drop the echoed command line, then take the
        # last numeric token the monitor printed as the return value.
        clean = re.sub(r"\x1b\[[0-9;]*m", "", out)
        tokens = re.findall(
            r"0x[0-9a-fA-F]+|\b\d+\b", clean.replace(method_and_args, "")
        )
        if not tokens:
            raise AssertionError(
                f"no numeric return from `{method_and_args}`; got:\n{out}"
            )
        return int(tokens[-1], 0)

    def test_01_init_handshake_ran(self):
        """The driver's power-up reset (0x3A=0x5A) reached the sensor at least
        once -- affirmative proof the SPI write path works, complementing the
        setUpClass assertion that the read path (self-test + product id) did."""
        self.assertGreaterEqual(
            self._sensor_int("PowerUpResetCount"),
            1,
            "expected >=1 power-up reset written to the sensor",
        )

    def test_02_configure_wrote_cpi(self):
        """CONFIGURE wrote the resolution step via the paged-write sequence
        (0x7F page select). The driver names RES_STEP 0x85, but bit7 is the
        SPI write flag, so on the wire the address masks to 0x05 on page 1 --
        that's where the model records the write."""
        self.assertGreaterEqual(
            self._sensor_int("WriteCountTo 1 0x05"),
            1,
            "expected >=1 write to page1 RES_STEP",
        )

    def test_03_motion_reported(self):
        """Injecting a motion report through the sensor drives the IRQ line,
        the driver reads MOTION_BURST and emits the decoded delta."""
        # Drain anything already on the console so the assertion below only
        # sees output caused by this injection.
        renode_harness.drain_text(self.console._sock, timeout=0.5)

        self._sensor("QueueMotion 100 -50")
        log = renode_harness.wait_for_text(
            self.console._sock, "x/y: 100/-50", timeout=10.0
        )
        self.assertIn(
            "x/y: 100/-50",
            log,
            "driver did not report the injected motion (100, -50); "
            f"console after injection:\n{log}",
        )
        # The sensor's motion queue should have been drained by the read.
        self.assertEqual(
            self._sensor_int("PendingMotionCount"),
            0,
            "motion queue was not drained by the read",
        )

    def test_04_second_motion_reported(self):
        """A second, differently-signed report also round-trips (proves the
        level IRQ re-arms and 12-bit two's-complement decode is stable)."""
        renode_harness.drain_text(self.console._sock, timeout=0.5)
        self._sensor("QueueMotion -7 33")
        log = renode_harness.wait_for_text(
            self.console._sock, "x/y: -7/33", timeout=10.0
        )
        self.assertIn(
            "x/y: -7/33",
            log,
            f"driver did not report the second injected motion (-7, 33):\n{log}",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
