from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = SCRIPT_ROOT.parent
sys.path.insert(0, str(SCRIPT_ROOT))

from human_acceptance import (  # noqa: E402
    HumanAcceptanceError,
    validate_human_acceptance_record,
)


class HumanAcceptanceRecordTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary_directory = tempfile.TemporaryDirectory(
            dir=REPO_ROOT,
            prefix=".human-acceptance-test-",
        )
        self.root = Path(self._temporary_directory.name)
        self.manifest_path = self.root / "Comparison_Manifest.json"
        self.record_path = self.root / "Comparison_Manifest.HumanAcceptance.json"
        self.manifest = {
            "input_asset": "/Game/Audio/MarchingBand.MarchingBand",
            "source_actor": "/Game/Maps/Listening.Listening:PersistentLevel.Source",
            "listener_actor": "/Game/Maps/Listening.Listening:PersistentLevel.Listener",
            "scene_signature": "A1B2C3D4",
            "direct_preset": "clear",
            "reflection_environment": "enclosed",
            "automatic_checks_passed": True,
            "direct_to_reference_rms_ratio": 0.5,
            "wet_to_reference_rms_ratio": 0.25,
            "direct_wet_normalized_difference": 1.25,
            "modes_are_distinct": True,
        }
        self.manifest_path.write_text(
            json.dumps(self.manifest),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self._temporary_directory.cleanup()

    def make_record(self, **overrides: object) -> dict[str, object]:
        record: dict[str, object] = {
            "schema_version": 3,
            "recorded_at_utc": "2026-08-06T12:00:00.000Z",
            "comparison_manifest": self.manifest_path.name,
            "target_listening_device": "Reference Studio Headphones",
            "listening_notes": "No click/pop while switching modes.",
            "input_asset": self.manifest["input_asset"],
            "source_actor": self.manifest["source_actor"],
            "listener_actor": self.manifest["listener_actor"],
            "scene_signature": self.manifest["scene_signature"],
            "direct_preset": self.manifest["direct_preset"],
            "reflection_environment": self.manifest["reflection_environment"],
            "automatic_checks_passed": True,
            "human_listening_passed": True,
            "previewed_modes": ["Reference", "Direct", "Wet", "Full"],
            "last_previewed_mode": "Full",
            "human_confirmations": {
                "recognizable_direct": True,
                "audible_wet_full_difference": True,
                "moving_occlusion_continuity": True,
                "mode_switching_continuity": True,
                "environment_difference": True,
            },
            "direct_to_reference_rms_ratio": 0.5,
            "wet_to_reference_rms_ratio": 0.25,
            "direct_wet_normalized_difference": 1.25,
            "modes_are_distinct": True,
            "requirement": (
                "Direct and Full retain recognizable source content; Wet is "
                "spatial tail only and audibly distinct; moving occlusion and "
                "runtime mode switching have no click/pop, dropout, noise, or "
                "timing jump; environment differences are reasonable."
            ),
        }
        record.update(overrides)
        return record

    def write_record(self, **overrides: object) -> Path:
        self.record_path.write_text(
            json.dumps(self.make_record(**overrides)),
            encoding="utf-8",
        )
        return self.record_path

    def test_valid_final_pass_is_anchored_to_manifest(self) -> None:
        payload = validate_human_acceptance_record(
            self.write_record(),
            require_pass=True,
        )

        self.assertTrue(payload["human_listening_passed"])
        self.assertEqual(
            payload["target_listening_device"],
            "Reference Studio Headphones",
        )
        self.assertEqual(
            payload["previewed_modes"],
            ["Reference", "Direct", "Wet", "Full"],
        )
        self.assertTrue(all(payload["human_confirmations"].values()))

    def test_blank_target_device_is_rejected(self) -> None:
        with self.assertRaisesRegex(HumanAcceptanceError, "target listening device"):
            validate_human_acceptance_record(
                self.write_record(target_listening_device="  \t"),
            )

    def test_pass_missing_wet_preview_is_rejected(self) -> None:
        with self.assertRaisesRegex(HumanAcceptanceError, "Wet"):
            validate_human_acceptance_record(
                self.write_record(
                    previewed_modes=["Reference", "Direct", "Full"],
                ),
            )

    def test_pass_requires_every_structured_human_confirmation(self) -> None:
        confirmations = self.make_record()["human_confirmations"]
        self.assertIsInstance(confirmations, dict)
        for field in confirmations:
            with self.subTest(field=field):
                incomplete = dict(confirmations)
                incomplete[field] = False
                with self.assertRaisesRegex(HumanAcceptanceError, field):
                    validate_human_acceptance_record(
                        self.write_record(human_confirmations=incomplete),
                    )

    def test_schema_two_record_is_rejected(self) -> None:
        with self.assertRaisesRegex(HumanAcceptanceError, "schema_version"):
            validate_human_acceptance_record(
                self.write_record(schema_version=2),
            )

    def test_legacy_record_is_rejected(self) -> None:
        legacy = self.make_record()
        legacy.pop("schema_version")
        self.record_path.write_text(json.dumps(legacy), encoding="utf-8")

        with self.assertRaisesRegex(HumanAcceptanceError, "schema_version"):
            validate_human_acceptance_record(self.record_path)

    def test_manifest_provenance_mismatch_is_rejected(self) -> None:
        with self.assertRaisesRegex(HumanAcceptanceError, "input_asset"):
            validate_human_acceptance_record(
                self.write_record(input_asset="/Game/Audio/Wrong.Wrong"),
            )

    def test_early_human_fail_is_valid_but_not_a_final_pass(self) -> None:
        path = self.write_record(
            automatic_checks_passed=False,
            human_listening_passed=False,
            previewed_modes=["Reference"],
            last_previewed_mode="Reference",
            human_confirmations={
                "recognizable_direct": False,
                "audible_wet_full_difference": False,
                "moving_occlusion_continuity": False,
                "mode_switching_continuity": False,
                "environment_difference": False,
            },
        )
        self.manifest["automatic_checks_passed"] = False
        self.manifest_path.write_text(
            json.dumps(self.manifest),
            encoding="utf-8",
        )

        payload = validate_human_acceptance_record(path)
        self.assertFalse(payload["human_listening_passed"])

        with self.assertRaisesRegex(HumanAcceptanceError, "passing verdict"):
            validate_human_acceptance_record(path, require_pass=True)

    def test_cli_accepts_valid_required_pass(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(SCRIPT_ROOT / "validate_human_acceptance.py"),
                "--record",
                str(self.write_record()),
                "--require-pass",
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("HUMAN_ACCEPTANCE_PASS", completed.stdout)
        self.assertIn("Reference Studio Headphones", completed.stdout)

    def test_cli_rejects_legacy_record(self) -> None:
        legacy = self.make_record()
        legacy.pop("schema_version")
        self.record_path.write_text(json.dumps(legacy), encoding="utf-8")

        completed = subprocess.run(
            [
                sys.executable,
                str(SCRIPT_ROOT / "validate_human_acceptance.py"),
                "--record",
                str(self.record_path),
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(completed.returncode, 1)
        self.assertIn("schema_version", completed.stderr)


if __name__ == "__main__":
    unittest.main()
