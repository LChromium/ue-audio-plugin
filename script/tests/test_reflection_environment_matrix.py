from __future__ import annotations

import json
import math
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_ROOT))

import reflection_environment_matrix
from reflection_environment_matrix import CaseManifest


ENVIRONMENT_VALUES = {
    "open_space": {
        "paths": 0,
        "indirect_gain": 0.0,
        "early_gain": 0.0,
        "late_gain": 0.0,
        "ir_energy": 0.0,
        "directional_ratio": 0.0,
        "directional_bins": 0,
        "wet_rms": 0.0,
        "wet_ratio": 0.0,
        "automatic": False,
        "distinct": False,
        "earliest_arrival": 0.0,
        "average_delay": 0.0,
        "reverb_times": (0.0, 0.0, 0.0),
        "direction": (0.0, 0.0, 0.0),
    },
    "near_wall": {
        "paths": 1000,
        "indirect_gain": 0.08,
        "early_gain": 0.08,
        "late_gain": 0.0,
        "ir_energy": 0.08,
        "directional_ratio": 0.60,
        "directional_bins": 100,
        "wet_rms": 0.10,
        "wet_ratio": 0.10,
        "automatic": True,
        "distinct": True,
        "earliest_arrival": 0.005,
        "average_delay": 0.009,
        "reverb_times": (0.0, 0.0, 0.0),
        "direction": (0.0, 1.0, 0.0),
    },
    "enclosed": {
        "paths": 1600,
        "indirect_gain": 0.12,
        "early_gain": 0.10,
        "late_gain": 0.02,
        "ir_energy": 0.12,
        "directional_ratio": 0.55,
        "directional_bins": 180,
        "wet_rms": 0.14,
        "wet_ratio": 0.14,
        "automatic": True,
        "distinct": True,
        "earliest_arrival": 0.004,
        "average_delay": 0.020,
        "reverb_times": (0.7, 0.6, 0.5),
        "direction": (0.2, 0.97, 0.1),
    },
}

FINITE_FIELDS = (
    "direct_distance_cm",
    "reflection_ray_count",
    "reflection_bounce_count",
    "sample_rate",
    "channels",
    "impulse_response_channels",
    "frames",
    "post_scale_peak",
    "clipped_sample_count",
    "direct_dropout_window_count",
    "common_output_scale",
    "hardware_indirect_valid_paths",
    "cpu_reference_indirect_valid_paths",
    "hardware_indirect_gain",
    "cpu_reference_indirect_gain",
    "hardware_early_reflection_gain",
    "cpu_reference_early_reflection_gain",
    "hardware_late_reverb_gain",
    "cpu_reference_late_reverb_gain",
    "hardware_impulse_response_energy",
    "cpu_reference_impulse_response_energy",
    "hardware_directional_energy_ratio",
    "cpu_reference_directional_energy_ratio",
    "hardware_directional_bin_count",
    "cpu_reference_directional_bin_count",
    "hardware_dominant_arrival_direction_x",
    "hardware_dominant_arrival_direction_y",
    "hardware_dominant_arrival_direction_z",
    "cpu_reference_dominant_arrival_direction_x",
    "cpu_reference_dominant_arrival_direction_y",
    "cpu_reference_dominant_arrival_direction_z",
    "hardware_earliest_arrival_seconds",
    "hardware_average_delay_seconds",
    "hardware_reverb_time_low_seconds",
    "hardware_reverb_time_mid_seconds",
    "hardware_reverb_time_high_seconds",
    "reference_rms",
    "direct_rms",
    "wet_rms",
    "full_rms",
    "direct_to_reference_rms_ratio",
    "wet_to_reference_rms_ratio",
    "full_to_reference_rms_ratio",
    "direct_dry_correlation",
    "full_dry_correlation",
    "wet_dry_correlation",
    "direct_wet_normalized_difference",
    "wet_stereo_normalized_difference",
)

HARDWARE_CPU_PAIRS = (
    ("hardware_indirect_valid_paths", "cpu_reference_indirect_valid_paths"),
    ("hardware_indirect_gain", "cpu_reference_indirect_gain"),
    ("hardware_early_reflection_gain", "cpu_reference_early_reflection_gain"),
    ("hardware_late_reverb_gain", "cpu_reference_late_reverb_gain"),
    ("hardware_impulse_response_energy", "cpu_reference_impulse_response_energy"),
    ("hardware_directional_energy_ratio", "cpu_reference_directional_energy_ratio"),
    ("hardware_directional_bin_count", "cpu_reference_directional_bin_count"),
)


def normalized(direction: tuple[float, float, float]) -> tuple[float, float, float]:
    magnitude = math.sqrt(sum(component * component for component in direction))
    if magnitude == 0.0:
        return direction
    return tuple(component / magnitude for component in direction)


def make_payload(environment: str) -> dict[str, object]:
    values = ENVIRONMENT_VALUES[environment]
    direction = normalized(values["direction"])
    reverb_low, reverb_mid, reverb_high = values["reverb_times"]
    payload: dict[str, object] = {
        "input_asset": "/Game/FirstPerson/Audio/MarchingBand.MarchingBand",
        "direct_preset": "clear",
        "reflection_environment": environment,
        "direct_distance_cm": 200.0,
        "reflection_ray_count": 4096,
        "reflection_bounce_count": 32,
        "hardware_ray_tracing": True,
        "has_cpu_reference": True,
        "sample_rate": 16000,
        "channels": 2,
        "impulse_response_channels": 2,
        "frames": 160000,
        "samples_finite": True,
        "audio_safety_checks_passed": True,
        "direct_semantics_passed": True,
        "clipped_sample_count": 0,
        "post_scale_peak": 0.8,
        "direct_dropout_window_count": 0,
        "common_output_scale": 1.0,
        "hardware_indirect_valid_paths": values["paths"],
        "cpu_reference_indirect_valid_paths": values["paths"],
        "hardware_indirect_gain": values["indirect_gain"],
        "cpu_reference_indirect_gain": values["indirect_gain"],
        "hardware_early_reflection_gain": values["early_gain"],
        "cpu_reference_early_reflection_gain": values["early_gain"],
        "hardware_late_reverb_gain": values["late_gain"],
        "cpu_reference_late_reverb_gain": values["late_gain"],
        "hardware_impulse_response_energy": values["ir_energy"],
        "cpu_reference_impulse_response_energy": values["ir_energy"],
        "hardware_directional_energy_ratio": values["directional_ratio"],
        "cpu_reference_directional_energy_ratio": values["directional_ratio"],
        "hardware_directional_bin_count": values["directional_bins"],
        "cpu_reference_directional_bin_count": values["directional_bins"],
        "hardware_earliest_arrival_seconds": values["earliest_arrival"],
        "hardware_average_delay_seconds": values["average_delay"],
        "hardware_reverb_time_low_seconds": reverb_low,
        "hardware_reverb_time_mid_seconds": reverb_mid,
        "hardware_reverb_time_high_seconds": reverb_high,
    }
    for prefix in ("hardware", "cpu_reference"):
        payload[f"{prefix}_dominant_arrival_direction_x"] = direction[0]
        payload[f"{prefix}_dominant_arrival_direction_y"] = direction[1]
        payload[f"{prefix}_dominant_arrival_direction_z"] = direction[2]
    payload.update(
        {
            "reference_rms": 0.20,
            "direct_rms": 0.10,
            "wet_rms": values["wet_rms"],
            "full_rms": 0.10 + values["wet_rms"],
            "direct_to_reference_rms_ratio": 0.50,
            "wet_to_reference_rms_ratio": values["wet_ratio"],
            "full_to_reference_rms_ratio": (0.10 + values["wet_rms"]) / 0.20,
            "direct_dry_correlation": 1.0,
            "full_dry_correlation": 1.0 if environment == "open_space" else 0.70,
            "wet_dry_correlation": 0.0,
            "direct_wet_normalized_difference": 0.0 if environment == "open_space" else 0.80,
            "wet_stereo_normalized_difference": 0.0 if environment == "open_space" else 0.30,
            "modes_are_distinct": values["distinct"],
            "directional_wet_is_distinct": values["distinct"],
            "automatic_checks_passed": values["automatic"],
        }
    )
    return payload


def make_case(environment: str, payload: dict[str, object] | None = None) -> CaseManifest:
    return CaseManifest(
        environment=environment,
        path=Path(f"{environment}.json"),
        payload=make_payload(environment) if payload is None else payload,
    )


class ReflectionEnvironmentCaseTests(unittest.TestCase):
    def assert_rejected(
        self,
        environment: str,
        field: str,
        value: object,
        message: str,
    ) -> None:
        payload = make_payload(environment)
        payload[field] = value
        with self.assertRaisesRegex(RuntimeError, message):
            reflection_environment_matrix.validate_case_manifest(
                make_case(environment, payload)
            )

    def test_loads_case_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "near-wall.json"
            payload = make_payload("near_wall")
            path.write_text(json.dumps(payload), encoding="utf-8")

            case = reflection_environment_matrix.load_case_manifest("near_wall", path)

        self.assertEqual(case.environment, "near_wall")
        self.assertEqual(case.path, path)
        self.assertEqual(case.payload, payload)

    def test_accepts_open_space_zero_indirect_semantics(self) -> None:
        metrics = reflection_environment_matrix.validate_case_manifest(
            make_case("open_space")
        )
        self.assertEqual(metrics["environment"], "open_space")
        self.assertEqual(metrics["hardware_indirect_valid_paths"], 0)
        self.assertEqual(metrics["hardware_indirect_gain"], 0.0)

    def test_accepts_near_wall_early_reflection_without_late_tail(self) -> None:
        metrics = reflection_environment_matrix.validate_case_manifest(
            make_case("near_wall")
        )
        self.assertEqual(metrics["environment"], "near_wall")
        self.assertEqual(metrics["hardware_late_reverb_gain"], 0.0)
        self.assertAlmostEqual(metrics["hardware_cpu_direction_dot"], 1.0)

    def test_accepts_enclosed_early_and_late_reflections(self) -> None:
        metrics = reflection_environment_matrix.validate_case_manifest(
            make_case("enclosed")
        )
        self.assertEqual(metrics["environment"], "enclosed")
        self.assertGreater(metrics["hardware_late_reverb_gain"], 0.0)
        self.assertAlmostEqual(metrics["hardware_cpu_direction_dot"], 1.0)

    def test_rejects_non_marching_band_and_ravel_input(self) -> None:
        self.assert_rejected(
            "near_wall",
            "input_asset",
            "/Game/FirstPerson/Audio/Ravel.Ravel",
            "exact MarchingBand input asset.*no Ravel input",
        )

    def test_rejects_wrong_environment_provenance(self) -> None:
        self.assert_rejected(
            "near_wall",
            "reflection_environment",
            "enclosed",
            "near_wall environment provenance",
        )

    def test_rejects_non_32_bounce_manifest(self) -> None:
        self.assert_rejected(
            "near_wall",
            "reflection_bounce_count",
            8,
            "32 reflection bounces",
        )

    def test_rejects_wrong_reflection_ray_count(self) -> None:
        self.assert_rejected(
            "near_wall",
            "reflection_ray_count",
            2048,
            "4096 reflection rays",
        )

    def test_rejects_cpu_only_manifest(self) -> None:
        self.assert_rejected(
            "near_wall",
            "hardware_ray_tracing",
            False,
            "hardware ray tracing provenance",
        )

    def test_rejects_missing_cpu_reference(self) -> None:
        self.assert_rejected(
            "near_wall",
            "has_cpu_reference",
            False,
            "CPU reference provenance",
        )

    def test_rejects_every_missing_boolean_gate(self) -> None:
        for field, message in (
            ("hardware_ray_tracing", "hardware ray tracing provenance"),
            ("has_cpu_reference", "CPU reference provenance"),
            ("samples_finite", "finite audio samples"),
            ("audio_safety_checks_passed", "audio safety checks"),
            ("direct_semantics_passed", "Direct semantics"),
        ):
            with self.subTest(field=field):
                payload = make_payload("near_wall")
                del payload[field]
                with self.assertRaisesRegex(RuntimeError, message):
                    reflection_environment_matrix.validate_case_manifest(
                        make_case("near_wall", payload)
                    )

    def test_rejects_truthy_non_boolean_common_gates(self) -> None:
        for field, message in (
            ("hardware_ray_tracing", "hardware ray tracing provenance"),
            ("has_cpu_reference", "CPU reference provenance"),
            ("samples_finite", "finite audio samples"),
            ("audio_safety_checks_passed", "audio safety checks"),
            ("direct_semantics_passed", "Direct semantics"),
        ):
            with self.subTest(field=field):
                self.assert_rejected("near_wall", field, 1, message)

    def test_rejects_missing_boolean_nan_and_infinite_numeric_fields(self) -> None:
        invalid_values = (
            ("missing", None),
            ("boolean", True),
            ("NaN", math.nan),
            ("infinity", math.inf),
        )
        for field in FINITE_FIELDS:
            for invalid_name, invalid_value in invalid_values:
                with self.subTest(field=field, invalid=invalid_name):
                    payload = make_payload("near_wall")
                    if invalid_name == "missing":
                        del payload[field]
                    else:
                        payload[field] = invalid_value
                    with self.assertRaisesRegex(
                        RuntimeError,
                        f"finite numeric {field}",
                    ):
                        reflection_environment_matrix.validate_case_manifest(
                            make_case("near_wall", payload)
                        )

    def test_rejects_oversized_integer_as_non_finite_numeric_data(self) -> None:
        self.assert_rejected(
            "near_wall",
            "frames",
            10**400,
            "finite numeric frames",
        )

    def test_rejects_clipping(self) -> None:
        self.assert_rejected(
            "near_wall", "clipped_sample_count", 1, "zero clipped samples"
        )

    def test_rejects_excessive_post_scale_peak(self) -> None:
        self.assert_rejected(
            "near_wall", "post_scale_peak", 0.99002, "safe post-scale peak"
        )

    def test_rejects_direct_dropout(self) -> None:
        self.assert_rejected(
            "near_wall",
            "direct_dropout_window_count",
            1,
            "zero Direct dropout windows",
        )

    def test_rejects_failed_direct_semantics(self) -> None:
        self.assert_rejected(
            "near_wall", "direct_semantics_passed", False, "Direct semantics"
        )

    def test_rejects_wrong_fixed_case_configuration(self) -> None:
        for field, value, message in (
            ("direct_preset", "soft_occluded", "clear Direct preset"),
            ("direct_distance_cm", 201.0, "200 cm Direct distance"),
            ("sample_rate", 48000, "16000 Hz sample rate"),
            ("channels", 1, "stereo channels"),
            ("impulse_response_channels", 1, "directional-stereo impulse response"),
        ):
            with self.subTest(field=field):
                self.assert_rejected("near_wall", field, value, message)

    def test_rejects_hardware_cpu_disagreement_for_every_compared_metric(self) -> None:
        for hardware_field, cpu_field in HARDWARE_CPU_PAIRS:
            environment = "enclosed" if "late_reverb" in hardware_field else "near_wall"
            with self.subTest(field=hardware_field):
                payload = make_payload(environment)
                payload[cpu_field] = float(payload[hardware_field]) * 0.80
                with self.assertRaisesRegex(RuntimeError, "hardware/CPU .* agreement"):
                    reflection_environment_matrix.validate_case_manifest(
                        make_case(environment, payload)
                    )

    def test_rejects_open_space_nonzero_hardware_or_cpu_pair_value(self) -> None:
        for hardware_field, cpu_field in HARDWARE_CPU_PAIRS:
            for field in (hardware_field, cpu_field):
                with self.subTest(field=field):
                    self.assert_rejected(
                        "open_space",
                        field,
                        1.0e-8,
                        "open_space zero hardware/CPU",
                    )

    def test_rejects_open_space_nonzero_wet_or_indirect_metric(self) -> None:
        for field in (
            "wet_rms",
            "wet_to_reference_rms_ratio",
            "hardware_indirect_gain",
            "hardware_early_reflection_gain",
            "hardware_late_reverb_gain",
            "hardware_impulse_response_energy",
            "cpu_reference_indirect_gain",
            "cpu_reference_early_reflection_gain",
            "cpu_reference_late_reverb_gain",
            "cpu_reference_impulse_response_energy",
        ):
            with self.subTest(field=field):
                self.assert_rejected(
                    "open_space", field, 1.0e-8, f"open_space zero {field}"
                )

    def test_rejects_open_space_nonzero_tail_or_direction_metadata(self) -> None:
        for field in (
            "hardware_earliest_arrival_seconds",
            "hardware_average_delay_seconds",
            "hardware_reverb_time_low_seconds",
            "hardware_reverb_time_mid_seconds",
            "hardware_reverb_time_high_seconds",
            "hardware_dominant_arrival_direction_x",
            "hardware_dominant_arrival_direction_y",
            "hardware_dominant_arrival_direction_z",
            "cpu_reference_dominant_arrival_direction_x",
            "cpu_reference_dominant_arrival_direction_y",
            "cpu_reference_dominant_arrival_direction_z",
        ):
            with self.subTest(field=field):
                self.assert_rejected(
                    "open_space", field, 1.0e-8, f"open_space zero {field}"
                )

    def test_rejects_open_space_distinction_or_generic_automatic_success(self) -> None:
        for field in (
            "modes_are_distinct",
            "directional_wet_is_distinct",
            "automatic_checks_passed",
        ):
            with self.subTest(field=field):
                self.assert_rejected(
                    "open_space", field, True, f"open_space {field}=false"
                )

    def test_rejects_non_boolean_open_space_distinction_flags(self) -> None:
        for field in (
            "modes_are_distinct",
            "directional_wet_is_distinct",
            "automatic_checks_passed",
        ):
            with self.subTest(field=field):
                self.assert_rejected(
                    "open_space", field, 0, f"open_space {field}=false"
                )

    def test_rejects_missing_nonzero_environment_distinction_or_automatic_gate(self) -> None:
        for environment in ("near_wall", "enclosed"):
            for field in (
                "modes_are_distinct",
                "directional_wet_is_distinct",
                "automatic_checks_passed",
            ):
                with self.subTest(environment=environment, field=field):
                    self.assert_rejected(
                        environment, field, False, f"{environment} {field}=true"
                    )

    def test_rejects_inaudible_or_nondirectional_nonzero_environment(self) -> None:
        for field, value, message in (
            ("wet_to_reference_rms_ratio", 0.049, "audible Wet"),
            ("wet_stereo_normalized_difference", 0.009, "stereo Wet distinction"),
            ("hardware_directional_energy_ratio", 0.049, "directional energy"),
        ):
            with self.subTest(field=field):
                self.assert_rejected("near_wall", field, value, message)

    def test_rejects_zero_required_near_wall_physics(self) -> None:
        for hardware_field, cpu_field in (
            ("hardware_indirect_valid_paths", "cpu_reference_indirect_valid_paths"),
            ("hardware_indirect_gain", "cpu_reference_indirect_gain"),
            ("hardware_early_reflection_gain", "cpu_reference_early_reflection_gain"),
            ("hardware_impulse_response_energy", "cpu_reference_impulse_response_energy"),
            ("hardware_directional_energy_ratio", "cpu_reference_directional_energy_ratio"),
            ("hardware_directional_bin_count", "cpu_reference_directional_bin_count"),
        ):
            for field in (hardware_field, cpu_field):
                with self.subTest(field=field):
                    self.assert_rejected(
                        "near_wall", field, 0, "near_wall positive hardware/CPU"
                    )

    def test_rejects_zero_enclosed_late_gain(self) -> None:
        for field in (
            "hardware_late_reverb_gain",
            "cpu_reference_late_reverb_gain",
        ):
            with self.subTest(field=field):
                self.assert_rejected(
                    "enclosed", field, 0.0, "enclosed positive hardware/CPU late_reverb_gain"
                )

    def test_rejects_nonmatching_dominant_directions(self) -> None:
        self.assert_rejected(
            "near_wall",
            "cpu_reference_dominant_arrival_direction_x",
            1.0,
            "hardware/CPU dominant direction agreement",
        )

    def test_accepts_huge_finite_matching_dominant_directions(self) -> None:
        payload = make_payload("near_wall")
        for prefix in ("hardware", "cpu_reference"):
            payload[f"{prefix}_dominant_arrival_direction_x"] = 1.0e308
            payload[f"{prefix}_dominant_arrival_direction_y"] = 1.0e308
            payload[f"{prefix}_dominant_arrival_direction_z"] = 0.0

        metrics = reflection_environment_matrix.validate_case_manifest(
            make_case("near_wall", payload)
        )

        direction_dot = float(metrics["hardware_cpu_direction_dot"])
        self.assertTrue(math.isfinite(direction_dot))
        self.assertAlmostEqual(direction_dot, 1.0)

    def test_rejects_huge_finite_divergent_dominant_directions(self) -> None:
        payload = make_payload("near_wall")
        payload["hardware_dominant_arrival_direction_x"] = 1.0e308
        payload["hardware_dominant_arrival_direction_y"] = 1.0e308
        payload["hardware_dominant_arrival_direction_z"] = 0.0
        payload["cpu_reference_dominant_arrival_direction_x"] = 1.0e308
        payload["cpu_reference_dominant_arrival_direction_y"] = -1.0e308
        payload["cpu_reference_dominant_arrival_direction_z"] = 0.0
        with self.assertRaisesRegex(
            RuntimeError,
            "hardware/CPU dominant direction agreement",
        ):
            reflection_environment_matrix.validate_case_manifest(
                make_case("near_wall", payload)
            )

    def test_rejects_zero_dominant_direction_for_nonzero_environment(self) -> None:
        self.assert_rejected(
            "near_wall",
            "cpu_reference_dominant_arrival_direction_y",
            0.0,
            "positive hardware/CPU dominant direction",
        )


if __name__ == "__main__":
    unittest.main()
