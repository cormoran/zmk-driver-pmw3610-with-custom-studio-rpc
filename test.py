from __future__ import annotations

import platform
import shutil
import subprocess
import unittest
from pathlib import Path

from dataclasses import dataclass

THIS_DIR = Path(__file__).parent.resolve()
TEST_BUILD_DIR_NAME = "tests-pmw3610"


def run_west(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["west", *args],
        capture_output=True,
        text=True,
        cwd=THIS_DIR,
    )


@dataclass
class NotFound:
    text: str


@dataclass
class ConfigAndDeviceTree:
    # Expected rows in .config
    config: list[str | NotFound]
    # Expected rows in devicetree_generated.h
    device: list[str | NotFound]
    # Expected (and NotFound) symbol names in zephyr/zmk.elf, checked via `nm`
    elf_symbols: list[str | NotFound] | None = None


class WestCommandsTests(unittest.TestCase):
    WEST_TOPDIR: Path
    BUILD_DIR: Path

    @classmethod
    def setUpClass(cls):
        cls.WEST_TOPDIR = Path(run_west(["topdir"]).stdout.strip())
        cls.BUILD_DIR = cls.WEST_TOPDIR / "build"

    @unittest.skipUnless(
        platform.system() == "Linux", "zmk-test is only supported on Linux"
    )
    def test_zmk_test(self):
        test_build_dir = self.BUILD_DIR / TEST_BUILD_DIR_NAME
        shutil.rmtree(test_build_dir, ignore_errors=True)

        result = run_west(["zmk-test", "tests", "-m", ".", "-d", str(test_build_dir)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS: test", result.stdout, result.stdout + result.stderr)
        self.assertIn("PASS: studio", result.stdout, result.stdout + result.stderr)
        self.assertIn(
            "PASS: split_peripheral", result.stdout, result.stdout + result.stderr
        )
        self.assertNotIn("FAILED: ", result.stdout, result.stdout + result.stderr)

    def test_zmk_build(self):
        self._test_zmk_build(
            {
                "pmw3610_disabled": ConfigAndDeviceTree(
                    config=[
                        'CONFIG_ZMK_KEYBOARD_NAME="Module Test"',
                        # No devicetree node -> DT_HAS_CORMORAN_PMW3610_ENABLED=n, so
                        # Kconfig doesn't even emit a "# CONFIG_PMW3610 is not set" line.
                        NotFound("CONFIG_PMW3610=y"),
                        NotFound("CONFIG_ZMK_PMW3610_STUDIO_RPC"),
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_zmk_keymap",
                        NotFound("DT_COMPAT_HAS_OKAY_cormoran_pmw3610"),
                    ],
                ),
                "pmw3610_plain": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_PMW3610=y",
                        "CONFIG_INPUT=y",
                        "CONFIG_ZMK_POINTING=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                        NotFound("CONFIG_ZMK_PMW3610_STUDIO_RPC"),
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_cormoran_pmw3610",
                    ],
                ),
                # Burst read OFF variant: same driver config as pmw3610_plain
                # (burst read is a devicetree property, not a Kconfig), just
                # built from the pmw3610-trackball-no-burst snippet.
                "pmw3610_plain_no_burst": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_PMW3610=y",
                        "CONFIG_INPUT=y",
                        "CONFIG_ZMK_POINTING=y",
                        "# CONFIG_ZMK_STUDIO is not set",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_cormoran_pmw3610",
                    ],
                ),
                "pmw3610_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_PMW3610=y",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_PMW3610_STUDIO_RPC=y",
                        "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256",
                        # Frame capture buffer (Phase C): default size, present
                        # whenever CONFIG_ZMK_PMW3610_STUDIO_RPC=y.
                        "CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE=484",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_cormoran_pmw3610",
                    ],
                ),
                "pmw3610_settings_rpc": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_PMW3610=y",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_PMW3610_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS_STUDIO_RPC=y",
                        "CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE=256",
                        "CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128",
                        "CONFIG_ZMK_PMW3610_STUDIO_RPC_FRAME_BUF_SIZE=484",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_cormoran_pmw3610",
                    ],
                ),
                # Two PMW3610 devices in one firmware image (DESIGN.md Phase
                # F): asserts each devicetree instance gets its own
                # `zmk_custom_setting` entries (no symbol collisions), by
                # name-mangled symbol in the linked ELF -- `nm` output looks
                # like "200018e0 D pmw3610_setting_0_cpi".
                "pmw3610_settings_rpc_dual": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_PMW3610=y",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_PMW3610_STUDIO_RPC=y",
                        "CONFIG_ZMK_CUSTOM_SETTINGS=y",
                        "CONFIG_ZMK_PMW3610_CUSTOM_SETTINGS=y",
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_cormoran_pmw3610",
                    ],
                    elf_symbols=[
                        "pmw3610_setting_0_cpi",
                        "pmw3610_setting_1_cpi",
                        "pmw3610_setting_0_force_awake",
                        "pmw3610_setting_1_force_awake",
                        "pmw3610_settings_id_0",
                        "pmw3610_settings_id_1",
                        # A hypothetical third instance's entries must not
                        # accidentally exist (catches an off-by-one in the
                        # DT_INST_FOREACH_STATUS_OKAY expansion).
                        NotFound("pmw3610_setting_2_cpi"),
                    ],
                ),
                # Split keyboard build coverage (DESIGN.md Phase F). Peripheral
                # role: no Studio (ZMK_STUDIO only selects ZMK_STUDIO_RPC for
                # !ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL), but the relay bridge +
                # a real sensor still compile.
                "pmw3610_split_peripheral": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_PMW3610=y",
                        "CONFIG_ZMK_SPLIT=y",
                        NotFound("CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y"),
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY=y",
                        NotFound("CONFIG_ZMK_STUDIO=y"),
                    ],
                    device=[
                        "DT_COMPAT_HAS_OKAY_cormoran_pmw3610",
                    ],
                ),
                # Central role: local Studio RPC + the relay bridge dispatch
                # side, no local sensor required to reach split peripheral
                # devices.
                "pmw3610_split_central": ConfigAndDeviceTree(
                    config=[
                        "CONFIG_ZMK_SPLIT=y",
                        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL=y",
                        "CONFIG_ZMK_SPLIT_RELAY_EVENT=y",
                        "CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY=y",
                        "CONFIG_ZMK_PMW3610_SPLIT_RPC_RELAY_TEST=y",
                        "CONFIG_ZMK_STUDIO=y",
                        "CONFIG_ZMK_PMW3610_STUDIO_RPC=y",
                    ],
                    device=[],
                    elf_symbols=[
                        "pmw3610_split_relay_central_test_init",
                        "pmw3610_relay_broadcast_request",
                    ],
                ),
            }
        )

    def _test_zmk_build(
        self, artifacts_and_expected_build_params: dict[str, ConfigAndDeviceTree]
    ):

        for artifact in artifacts_and_expected_build_params.keys():
            shutil.rmtree(self.BUILD_DIR / artifact, ignore_errors=True)

        result = run_west(["zmk-build", "tests/zmk-config", "-q"])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for artifact, entries in artifacts_and_expected_build_params.items():
            artifact_dir = self.BUILD_DIR / artifact / "zephyr"
            config_path = artifact_dir / ".config"
            device_tree_path = (
                artifact_dir
                / "include"
                / "generated"
                / "zephyr"
                / "devicetree_generated.h"
            )
            self._test_strings_in_file(
                config_path, entries.config, f"{artifact} config"
            )
            if entries.device:
                self._test_strings_in_file(
                    device_tree_path, entries.device, f"{artifact} device tree"
                )
            self.assertTrue(
                (artifact_dir / "zmk.uf2").exists(),
                f"{artifact} zmk.uf2 is missing in {artifact_dir}",
            )
            if entries.elf_symbols:
                self._test_elf_symbols(
                    artifact_dir / "zmk.elf",
                    entries.elf_symbols,
                    f"{artifact} elf symbols",
                )

    def _test_elf_symbols(
        self, elf_path: Path, expected_symbols: list[str | NotFound], hint: str
    ):
        self.assertTrue(elf_path.exists(), f"{hint}: {elf_path} is missing")
        # `nm` (not the arm-specific variant) is enough to list symbol names
        # from an ELF's symbol table regardless of target architecture.
        result = subprocess.run(["nm", str(elf_path)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        symbol_names = {
            line.split()[-1] for line in result.stdout.splitlines() if line.strip()
        }

        for expected in expected_symbols:
            if isinstance(expected, NotFound):
                if expected.text in symbol_names:
                    self.fail(
                        f"{hint}: symbol {expected.text} found, but it should not be present"
                    )
            else:
                if expected not in symbol_names:
                    self.fail(f"{hint}: symbol {expected} not found in {elf_path}")

    def _test_strings_in_file(
        self, file_path: Path, expected_strings: list[str | NotFound], hint: str
    ):
        self.assertTrue(file_path.exists(), f"{hint}: {file_path} is missing")
        file_text = file_path.read_text()

        for expected in expected_strings:
            if isinstance(expected, NotFound):
                if expected.text in file_text:
                    self.fail(
                        f"{hint}: {expected.text} found in {file_path}, but it should not be present"
                    )
            else:
                if expected not in file_text:
                    self.fail(f"{hint}: {expected} not found in {file_path}")


if __name__ == "__main__":
    unittest.main()
