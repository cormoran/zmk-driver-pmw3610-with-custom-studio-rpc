#!/usr/bin/env python3
"""Hardware-free end-to-end test: a Studio host talks to the split PERIPHERAL's
PMW3610 over an emulated USB+WIRED split, through the split event relay.

Topology (two real nRF52840 images on one Renode session; see
pmw3610_usb_wired_split_lib.py):

    host(USB CDC) --USB(Studio)--> CENTRAL --wired(split relay)--> PERIPHERAL (sim PMW3610)

The host (a Renode DualCdcAcmBridge on the central's USB CDC) issues a PMW3610
`GetInfo(source=0xFFFFFFFF)` custom Studio RPC to the central. The central
answers its own (empty) local devices synchronously AND broadcasts the request
to the peripheral over the split event relay; the peripheral's PMW3610 subsystem
answers and the central forwards it to the host as a `PeripheralResponse` custom
notification. This test asserts that notification carries the peripheral's real
`DeviceInfo` -- product_id 0x3E and ready=true, i.e. the simulated PMW3610 on the
peripheral's SPIM0 answered THROUGH the relay.

This is the "host <-> peripheral PMW3610 via event relay" proof. The usb+wired
path round-trips the large relayed response where a pure-Studio transport can't:
the wired split link (cormoran/zmk#34's relay-event transport) has no radio cap,
and Studio rides the emulated USB CDC. The central disables Studio locking
(CONFIG_ZMK_STUDIO_LOCKING=n in its shield conf) because a headless Renode run
has no physical unlock key -- otherwise every RPC is rejected with UNLOCK_REQUIRED
and no relay is ever dispatched.

Run via:
  west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-usb-split.yaml -af usb-wired-central
  west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-usb-split.yaml -af usb-wired-peripheral
  west zmk-renode-test --mode wired-split \
      --elf build/usb-wired-central/zephyr/zmk.elf \
      --peripheral-elf build/usb-wired-peripheral/zephyr/zmk.elf \
      tests/renode

Named `*_renode_test.py` (not `test_*.py`) so it stays out of `python3 -m
unittest` auto-discovery -- it needs real firmware ELFs. SKIPs if not built.
"""

from __future__ import annotations

import os
import sys
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

try:
    import renode_harness as rh
except ImportError:  # pragma: no cover - local-dev fallback
    for cand in (
        REPO_ROOT / "dependencies" / "zmk-west-commands" / "scripts" / "lib" / "renode",
        REPO_ROOT.parent / "zmk-west-commands" / "scripts" / "lib" / "renode",
    ):
        if cand.is_dir():
            sys.path.insert(0, str(cand))
            import renode_harness as rh  # noqa: F401

            break
    else:
        raise

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pmw3610_usb_wired_split_lib as splitlib  # noqa: E402

PMW3610_PRODUCT_ID = 0x3E
# Single custom subsystem on the central -> deterministic index 0 (see the
# template's KNOWN_SUBSYSTEM_INDEX note).
SUBSYSTEM_INDEX = 0
# GetInfo sentinel: "list every PMW3610 across the whole keyboard" -- answer the
# central's own local devices synchronously AND broadcast to every peripheral,
# each of which relays back a PeripheralResponse notification.
PMW3610_SOURCE_ALL = 0xFFFFFFFF


def _elf(env_name: str, default_rel: str) -> Path:
    val = os.environ.get(env_name)
    return Path(val) if val else REPO_ROOT / default_rel


def _load_protos():
    studio_proto_dir = rh.find_studio_proto_dir(REPO_ROOT)
    studio_pb2 = rh.load_studio_pb2(studio_proto_dir)  # also puts out_dir on sys.path
    import custom_pb2  # noqa: E402  (compiled alongside studio_pb2)

    out = rh.compile_protos(
        [REPO_ROOT / "proto" / "cormoran" / "pmw3610" / "pmw3610.proto"],
        include_dirs=[
            REPO_ROOT / "proto",
            studio_proto_dir,
            REPO_ROOT
            / "dependencies"
            / "modules"
            / "lib"
            / "nanopb"
            / "generator"
            / "proto",
        ],
    )
    sys.path.insert(0, str(out))
    from cormoran.pmw3610 import pmw3610_pb2  # noqa: E402

    return studio_pb2, custom_pb2, pmw3610_pb2


class PMW3610UsbWiredSplitRelayTest(unittest.TestCase):
    session = None

    @classmethod
    def setUpClass(cls):
        cls.central = _elf("ZMK_RENODE_ELF", "build/usb-wired-central/zephyr/zmk.elf")
        cls.peripheral = _elf(
            "ZMK_RENODE_PERIPHERAL_ELF", "build/usb-wired-peripheral/zephyr/zmk.elf"
        )
        for name, p in (("central", cls.central), ("peripheral", cls.peripheral)):
            if not p.is_file():
                raise unittest.SkipTest(
                    f"{name} ELF not built: {p} (see this file's header)"
                )

        cls.renode = rh.find_or_install_renode()
        if not cls.renode:
            raise unittest.SkipTest("Renode not available")

        cls.studio_pb2, cls.custom_pb2, cls.pmw3610_pb2 = _load_protos()

        cls.session, cls.c_con, cls.rpc, cls.p_con = splitlib.boot(
            cls.renode,
            cls.central,
            cls.peripheral,
            renode_log=Path("/tmp/pmw3610_usb_wired_split_renode.log"),
        )

        # Peripheral console (uart0) carries the split-link status + the PMW3610
        # driver's init/motion DBG lines. Collect it in the background as we drive
        # the RPC.
        cls.p_buf = ""

    @classmethod
    def tearDownClass(cls):
        if cls.session is not None:
            for sock in getattr(cls.session, "_cdc_sockets", []):
                try:
                    sock.close()
                except OSError:
                    pass
            try:
                cls.c_con.close()
                cls.p_con.close()
            except OSError:
                pass
            cls.session.stop()

    def _getinfo_request(self, source: int) -> bytes:
        """Unframed zmk.Request{custom:{call:{subsystem_index, payload=pmw3610
        Request{get_info{source}}}}} (RpcSocket.send() adds the SOF/ESC/EOF frame)."""
        pmw = self.pmw3610_pb2.Request()
        pmw.get_info.source = source
        cust = self.custom_pb2.Request()
        cust.call.subsystem_index = SUBSYSTEM_INDEX
        cust.call.payload = pmw.SerializeToString()
        req = self.studio_pb2.Request()
        req.request_id = 1
        req.custom.CopyFrom(cust)
        return req.SerializeToString()

    def _drain_peripheral(self):
        self.__class__.p_buf += rh.drain_text(self.p_con._sock, timeout=0.2)

    def test_01_peripheral_pmw3610_initialized(self):
        # Wait for the peripheral driver to init against the simulated sensor.
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline and "PMW3610 initialized" not in self.p_buf:
            self._drain_peripheral()
        self.assertIn(
            "PMW3610 initialized",
            self.p_buf,
            "peripheral's PMW3610 driver never initialized against the simulated "
            f"sensor (peripheral console tail:\n{self.p_buf[-800:]})",
        )

    def test_02_host_received_peripheral_pmw3610_via_relay(self):
        # Send GetInfo(source=ALL): the central relays to the peripheral and
        # forwards the answer back as a PeripheralResponse notification.
        self.rpc.send(self._getinfo_request(PMW3610_SOURCE_ALL))

        infos = []
        frames = []
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not infos:
            frame = self.rpc.read_frame(timeout=1.0)
            self._drain_peripheral()
            if frame is None:
                continue
            frames.append(frame)
            infos = self._peripheral_device_infos(frames)

        self.assertTrue(
            infos,
            "host received no relayed PMW3610 PeripheralResponse from the peripheral "
            f"({len(frames)} Studio frames seen; peripheral console tail:\n"
            f"{self.p_buf[-800:]})",
        )
        self.assertTrue(
            any(d.product_id == PMW3610_PRODUCT_ID and d.ready for d in infos),
            "relayed peripheral DeviceInfo did not show a ready PMW3610 "
            f"(product_id 0x{PMW3610_PRODUCT_ID:02x}); got: "
            + ", ".join(f"(pid=0x{d.product_id:02x},ready={d.ready})" for d in infos),
        )

    def _peripheral_device_infos(self, frames):
        """Decode every captured Studio frame and return the PMW3610 DeviceInfos
        delivered as a relayed PeripheralResponse notification. The notification
        rides a top-level zmk.Response whose `notification` oneof carries the
        custom payload:
          Response{notification:{custom:{custom_notification:{subsystem_index,
          payload = pmw3610 Notification{peripheral_response{response:{get_info:
          {devices}}}}}}}}."""
        infos = []
        for payload in frames:
            try:
                resp = self.studio_pb2.Response()
                resp.ParseFromString(payload)
            except Exception:
                continue
            if resp.WhichOneof("type") != "notification":
                continue
            note = resp.notification
            if note.WhichOneof("subsystem") != "custom":
                continue
            cn = note.custom.custom_notification
            if cn.subsystem_index != SUBSYSTEM_INDEX:
                continue
            pnote = self.pmw3610_pb2.Notification()
            try:
                pnote.ParseFromString(cn.payload)
            except Exception:
                continue
            if pnote.WhichOneof("notification_type") != "peripheral_response":
                continue
            pr = pnote.peripheral_response
            if pr.response.WhichOneof("response_type") != "get_info":
                continue
            infos.extend(pr.response.get_info.devices)
        return infos


if __name__ == "__main__":
    unittest.main(verbosity=2)
