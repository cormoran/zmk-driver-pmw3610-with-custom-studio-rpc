#!/usr/bin/env python3
"""Hardware-free end-to-end test: a Studio host talks to the split PERIPHERAL's
PMW3610 over an emulated BLE wireless split, through the split event relay.

Topology (three real nRF52840 images on one Renode BLE medium; see
pmw3610_ble_split_lib.py):

    host  --BLE(Studio)-->  CENTRAL  --BLE(split relay)-->  PERIPHERAL (sim PMW3610)

The host issues a PMW3610 `GetInfo(source=0xFFFFFFFF)` custom Studio RPC to the
central. The central answers its own (empty) local devices synchronously AND
broadcasts the request to the peripheral over the split event relay; the
peripheral's PMW3610 subsystem answers and the central forwards it to the host as
a `PeripheralResponse` custom notification. This test asserts that notification
carries the peripheral's real `DeviceInfo` -- product_id 0x3E and ready=true,
i.e. the simulated PMW3610 on the peripheral's SPIM0 answered THROUGH the relay.

This is the "host <-> peripheral PMW3610 via event relay" proof. It also asserts
the encrypted split link came up (peripheral RTT "Security changed ... level 2")
and the peripheral's driver initialized against the simulated sensor.

*** PENDING INTEGRATION (see the module README's ble-split section) ***
This test needs two things from cormoran/zmk-west-commands that are landing in a
separate PR:
  1. The Renode fake-BLE fix so a multi-fragment (large) Studio response
     round-trips (the relay's DeviceInfo response is well over the ~20-byte
     single-PDU size that works today).
  2. The renode-ble-host built with CONFIG_RENODE_BLE_HOST_REQUEST_HEX set to the
     framed PMW3610 GetInfo request (REQUEST_HEX below) and
     CONFIG_RENODE_BLE_HOST_TARGET_NAME="PMW3610", so it writes that request and
     prints each response/notification frame as `STAGE:S8-RPC-RESPONSE OK hex=..`.
Until both land, this test SKIPs (host ELF not built / no S8 markers). Build:
  west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-ble-split.yaml -af ble-split-central
  west zmk-build tests/zmk-config --build-yaml tests/zmk-config/build-ble-split.yaml -af ble-split-peripheral
  west build -b nrf52840dk/nrf52840 -s <zmk-west-commands>/renode-ble-host -d build/ble-host -- \
      -DCONFIG_RENODE_BLE_HOST_TARGET_NAME='"PMW3610"' \
      -DCONFIG_RENODE_BLE_HOST_REQUEST_HEX='"ab0801a2060c120a12080a0608ffffffff0fad"'
  west zmk-renode-test --mode ble-split \
      --elf build/ble-split-central/zephyr/zmk.elf \
      --peripheral-elf build/ble-split-peripheral/zephyr/zmk.elf \
      --host-elf build/ble-host/zephyr/zephyr.elf \
      tests/renode
"""

from __future__ import annotations

import os
import re
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
import pmw3610_ble_split_lib as splitlib  # noqa: E402

# Framed PMW3610 GetInfo(source=0xFFFFFFFF) Studio request (zmk.Request{custom:
# {call:{subsystem_index=0, payload=pmw3610.Request{get_info{source=-1}}}}}),
# SOF/ESC/EOF-framed. The host is built to WRITE these exact bytes -- kept here
# only for reference / to regenerate; see the module header.
REQUEST_HEX = "ab0801a2060c120a12080a0608ffffffff0fad"

PMW3610_PRODUCT_ID = 0x3E
# Single custom subsystem on the central -> deterministic index 0 (see the
# template's KNOWN_SUBSYSTEM_INDEX note; ListCustomSubsystem discovery would be
# a large response, so the request hardcodes index 0).
SUBSYSTEM_INDEX = 0


def _elf(env_name: str, default_rel: str) -> Path:
    val = os.environ.get(env_name)
    return Path(val) if val else REPO_ROOT / default_rel


def _load_protos():
    """Compile the studio + custom + pmw3610 protos on the fly and return the
    modules needed to decode response frames."""
    studio_proto_dir = rh.find_studio_proto_dir(REPO_ROOT)
    studio_pb2 = rh.load_studio_pb2(studio_proto_dir)  # also puts out_dir on sys.path
    import custom_pb2  # noqa: E402  (compiled alongside studio_pb2)

    # pmw3610 proto lives in the module's own proto/ tree.
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


class PMW3610BleSplitRelayTest(unittest.TestCase):
    session = None

    @classmethod
    def setUpClass(cls):
        cls.central = _elf("ZMK_RENODE_ELF", "build/ble-split-central/zephyr/zmk.elf")
        cls.peripheral = _elf(
            "ZMK_RENODE_PERIPHERAL_ELF", "build/ble-split-peripheral/zephyr/zmk.elf"
        )
        cls.host = _elf("ZMK_RENODE_HOST_ELF", "build/ble-host/zephyr/zephyr.elf")
        for name, p in (
            ("central", cls.central),
            ("peripheral", cls.peripheral),
            ("host", cls.host),
        ):
            if not p.is_file():
                raise unittest.SkipTest(
                    f"{name} ELF not built: {p} (see this file's header)"
                )

        cls.renode = rh.find_or_install_renode()
        if not cls.renode:
            raise unittest.SkipTest("Renode not available")

        cls.studio_pb2, cls.custom_pb2, cls.pmw3610_pb2 = _load_protos()

        cls.session, cls.c_con, cls.c_rpc, cls.p_rtt, cls.h_con = splitlib.boot(
            cls.renode,
            cls.central,
            cls.peripheral,
            cls.host,
            renode_log=Path("/tmp/pmw3610_ble_split_renode.log"),
        )

        # Collect for up to a generous wall budget (three machines at 10us
        # quantum are slow): peripheral RTT (split L2 + PMW3610 init) and host
        # console (S4 + S8 response frames).
        cls.rtt_buf = ""
        cls.host_buf = ""
        deadline = time.monotonic() + 1400
        while time.monotonic() < deadline:
            if cls.p_rtt is not None:
                cls.rtt_buf += rh.drain_text(cls.p_rtt._sock, timeout=0.3)
            cls.host_buf += rh.drain_text(cls.h_con._sock, timeout=0.3)
            if "STAGE:S8-RPC-RESPONSE" in cls.host_buf and "level 2" in cls.rtt_buf:
                # give a moment for the async PeripheralResponse notification too
                cls.host_buf += rh.drain_text(cls.h_con._sock, timeout=2.0)
                break

    @classmethod
    def tearDownClass(cls):
        if cls.session is not None:
            cls.session.stop()

    def _response_payloads(self):
        return [
            bytes.fromhex(m)
            for m in re.findall(
                r"STAGE:S8-RPC-RESPONSE OK[^\n]*hex=([0-9a-fA-F]+)", self.host_buf
            )
        ]

    def _peripheral_device_infos(self):
        """Decode every response/notification frame the host captured and return
        the PMW3610 DeviceInfos delivered as a relayed PeripheralResponse."""
        infos = []
        for payload in self._response_payloads():
            # The relayed peripheral answer arrives as a Studio Notification ->
            # custom_notification -> pmw3610 Notification -> peripheral_response.
            try:
                note = self.studio_pb2.Notification()
                note.ParseFromString(payload)
            except Exception:
                continue
            if not note.HasField("custom"):
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
            infos.extend(pnote.peripheral_response.response.devices)
        return infos

    def test_01_split_link_encrypted(self):
        self.assertIn(
            "level 2",
            self.rtt_buf,
            "split peripheral<->central link never reached BT_SECURITY_L2 "
            f"(peripheral RTT tail:\n{self.rtt_buf[-500:]})",
        )

    def test_02_peripheral_pmw3610_initialized(self):
        self.assertIn(
            "PMW3610 initialized",
            self.rtt_buf,
            "peripheral's PMW3610 driver never initialized against the simulated "
            f"sensor (peripheral RTT tail:\n{self.rtt_buf[-500:]})",
        )

    def test_03_host_received_peripheral_pmw3610_via_relay(self):
        self.assertIn(
            "STAGE:S4-SECURITY-CHANGED OK",
            self.host_buf,
            "host never reached an encrypted link to the central",
        )
        infos = self._peripheral_device_infos()
        self.assertTrue(
            infos,
            "host received no relayed PMW3610 PeripheralResponse from the peripheral; "
            "host STAGE tail:\n"
            + "\n".join(ln for ln in self.host_buf.splitlines() if "STAGE:" in ln)[
                -1500:
            ],
        )
        self.assertTrue(
            any(d.product_id == PMW3610_PRODUCT_ID and d.ready for d in infos),
            "relayed peripheral DeviceInfo did not show a ready PMW3610 "
            f"(product_id 0x{PMW3610_PRODUCT_ID:02x}); got: "
            + ", ".join(f"(pid=0x{d.product_id:02x},ready={d.ready})" for d in infos),
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
