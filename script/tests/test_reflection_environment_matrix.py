from __future__ import annotations

import dataclasses
import hashlib
import json
import math
import re
import struct
import sys
import tempfile
import unittest
import wave
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

WAV_SAMPLES = {
    "reference": (1200, -1200, 600, -600),
    "direct": (600, -600, 300, -300),
    "open_space_wet": (0, 0, 0, 0),
    "near_wall_wet": (120, -120, 60, -60),
    "enclosed_wet": (240, -240, 120, -120),
    "near_wall_full": (720, -720, 360, -360),
    "enclosed_full": (840, -840, 420, -420),
}

FAILURE_LOG_PATTERNS = (
    "Fatal error",
    "Unhandled Exception",
    "Assertion failed",
    "LogWindows: Error",
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


def write_pcm16(path: Path, samples: list[int] | tuple[int, ...]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(16000)
        stream.writeframes(struct.pack(f"<{len(samples)}h", *samples))


def json_path_value(path: Path, base: Path, relative_paths: bool) -> str:
    if relative_paths:
        return str(path.relative_to(base))
    return str(path)


def make_complete_matrix(
    temporary_root: Path,
    *,
    relative_paths: bool = False,
    load_evidence: bool = True,
) -> tuple[
    dict[str, CaseManifest],
    dict[str, object],
    Path,
]:
    project_root = temporary_root / "TestProject"
    output_root = project_root / "Saved" / "ReflectionEnvironmentMatrix"
    cases: dict[str, CaseManifest] = {}
    result_paths: dict[str, Path] = {}

    for environment in reflection_environment_matrix.ENVIRONMENTS:
        environment_root = output_root / environment
        manifest_path = environment_root / f"{environment}_Manifest.json"
        result_path = environment_root / f"{environment}_Result.json"
        screenshot_path = environment_root / f"{environment}.png"
        log_path = environment_root / f"{environment}.log"
        environment_root.mkdir(parents=True, exist_ok=True)

        reference_path = environment_root / f"{environment}_Reference.wav"
        direct_path = environment_root / f"{environment}_Direct.wav"
        wet_path = environment_root / f"{environment}_Wet.wav"
        full_path = environment_root / f"{environment}_Full.wav"
        write_pcm16(reference_path, WAV_SAMPLES["reference"])
        write_pcm16(direct_path, WAV_SAMPLES["direct"])
        if environment == "open_space":
            write_pcm16(wet_path, WAV_SAMPLES["open_space_wet"])
            write_pcm16(full_path, WAV_SAMPLES["direct"])
        else:
            write_pcm16(wet_path, WAV_SAMPLES[f"{environment}_wet"])
            write_pcm16(full_path, WAV_SAMPLES[f"{environment}_full"])

        payload = make_payload(environment)
        payload["frames"] = 2
        for field, wav_path in (
            ("reference_wav", reference_path),
            ("direct_wav", direct_path),
            ("wet_wav", wet_path),
            ("full_wav", full_path),
        ):
            payload[field] = json_path_value(
                wav_path,
                manifest_path.parent,
                relative_paths,
            )
        manifest_path.write_text(
            json.dumps(payload, indent=2) + "\n",
            encoding="utf-8",
        )
        cases[environment] = CaseManifest(environment, manifest_path, payload)

        display_name = "".join(part.title() for part in environment.split("_"))
        ir_package = f"/Game/UERayTracingAudio/Validation/{display_name}IR"
        ir_path = (
            project_root
            / "Content"
            / "UERayTracingAudio"
            / "Validation"
            / f"{display_name}IR.uasset"
        )
        ir_path.parent.mkdir(parents=True, exist_ok=True)
        ir_path.write_bytes(b"uasset")
        screenshot_path.write_bytes(b"synthetic screenshot")
        log_path.write_text(
            "LogTemp: Display: R3 reflection fixture complete.\n",
            encoding="utf-8",
        )

        artifact_paths = {
            "reference": reference_path,
            "direct": direct_path,
            "wet": wet_path,
            "full": full_path,
            "manifest": manifest_path,
        }
        artifacts: dict[str, object] = {
            "hardware": 1,
            "auto_checks": int(environment != "open_space"),
            "distinct": int(environment != "open_space"),
            "input": "/Game/FirstPerson/Audio/MarchingBand.MarchingBand",
            "direct_preset": "clear",
            "reflection_environment": environment,
            "distance_cm": 200.0,
            "ir_asset": f"{ir_package}.{display_name}IR",
            "imported_assets": 4,
            "reflection_rays": 4096,
            "reflection_bounces": 32,
            "common_scale": 1.0,
        }
        for field, artifact_path in artifact_paths.items():
            artifacts[field] = json_path_value(
                artifact_path,
                result_path.parent,
                relative_paths,
            )

        result_payload = {
            "schema_version": 1,
            "passed": True,
            "scene": {
                "geometry": reflection_environment_matrix.EXPECTED_GEOMETRY[
                    environment
                ],
                "direct_preset": "clear",
                "reflection_environment": environment,
                "reflection_bounces": 32,
                "distance_cm": 200.0,
                "air_absorption_profile": "default",
                "air_absorption_per_meter": [0.0002, 0.0006, 0.0012],
            },
            "artifacts": artifacts,
            "image_metrics": {
                "width": 1280,
                "height": 720,
                "non_black_ratio": 0.75,
                "mean_luma": 80.0,
                "luma_stddev": 12.0,
            },
            "screenshot": json_path_value(
                screenshot_path,
                result_path.parent,
                relative_paths,
            ),
            "log": json_path_value(
                log_path,
                result_path.parent,
                relative_paths,
            ),
        }
        result_path.write_text(
            json.dumps(result_payload, indent=2) + "\n",
            encoding="utf-8",
        )
        result_paths[environment] = result_path

    evidence = {}
    if load_evidence:
        evidence = {
            environment: reflection_environment_matrix.load_case_evidence(
                environment,
                result_paths[environment],
            )
            for environment in reflection_environment_matrix.ENVIRONMENTS
        }
    return cases, evidence, project_root


def replace_case_payload(
    cases: dict[str, CaseManifest],
    environment: str,
    **changes: object,
) -> dict[str, CaseManifest]:
    payload = {**cases[environment].payload, **changes}
    return {
        **cases,
        environment: CaseManifest(
            environment,
            cases[environment].path,
            payload,
        ),
    }


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


class ReflectionEnvironmentMatrixManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.temporary_root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def make_cases(self, suffix: str = "matrix") -> dict[str, CaseManifest]:
        cases, _, _ = make_complete_matrix(
            self.temporary_root / suffix,
            load_evidence=False,
        )
        return cases

    def test_accepts_complete_real_pcm16_matrix_and_returns_summary(self) -> None:
        cases = self.make_cases()

        summary = reflection_environment_matrix.validate_matrix_manifests(cases)

        self.assertEqual(set(summary), {"thresholds", "cases", "comparisons"})
        self.assertTrue(
            all(
                isinstance(value, float)
                for value in summary["thresholds"].values()
            )
        )
        self.assertEqual(set(summary["cases"]), set(ENVIRONMENT_VALUES))
        open_summary = summary["cases"]["open_space"]
        open_case = cases["open_space"]
        reference_path = Path(str(open_case.payload["reference_wav"]))
        self.assertEqual(open_summary["manifest_path"], str(open_case.path.resolve()))
        self.assertEqual(open_summary["reference_wav"], str(reference_path.resolve()))
        self.assertEqual(
            open_summary["reference_sha256"],
            hashlib.sha256(reference_path.read_bytes()).hexdigest(),
        )
        for environment_summary in summary["cases"].values():
            for field in (
                "reference_wav",
                "direct_wav",
                "wet_wav",
                "full_wav",
                "reference_sha256",
                "direct_sha256",
                "wet_sha256",
                "full_sha256",
                "hardware_impulse_response_energy",
                "wet_to_reference_rms_ratio",
            ):
                self.assertIn(field, environment_summary)
        self.assertGreater(
            summary["comparisons"]["enclosed_to_near_wall_paths_ratio"],
            1.0,
        )
        self.assertGreaterEqual(
            summary["comparisons"]["enclosed_to_near_wall_ir_energy_ratio"],
            1.10,
        )

    def test_keeps_comparison_ratios_finite_for_huge_finite_growth(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(
            cases,
            "enclosed",
            hardware_late_reverb_gain=1.0e308,
            cpu_reference_late_reverb_gain=1.0e308,
        )

        summary = reflection_environment_matrix.validate_matrix_manifests(cases)

        self.assertTrue(
            all(
                math.isfinite(value)
                for value in summary["comparisons"].values()
            )
        )

    def test_accepts_manifest_wav_paths_relative_to_each_manifest(self) -> None:
        cases, _, _ = make_complete_matrix(
            self.temporary_root / "relative",
            relative_paths=True,
            load_evidence=False,
        )

        summary = reflection_environment_matrix.validate_matrix_manifests(cases)

        for environment, case in cases.items():
            expected_reference = (
                case.path.parent / str(case.payload["reference_wav"])
            ).resolve()
            self.assertEqual(
                summary["cases"][environment]["reference_wav"],
                str(expected_reference),
            )

    def test_rejects_nonzero_open_space_wet_pcm(self) -> None:
        cases = self.make_cases()
        write_pcm16(
            Path(str(cases["open_space"].payload["wet_wav"])),
            [1, 0, 0, 0],
        )

        with self.assertRaisesRegex(RuntimeError, "OpenSpace zero Wet PCM"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_different_open_space_direct_and_full_hashes(self) -> None:
        cases = self.make_cases()
        write_pcm16(
            Path(str(cases["open_space"].payload["full_wav"])),
            [601, -600, 300, -300],
        )

        with self.assertRaisesRegex(
            RuntimeError,
            "OpenSpace Direct/Full SHA-256",
        ):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_different_reference_hashes(self) -> None:
        cases = self.make_cases()
        write_pcm16(
            Path(str(cases["near_wall"].payload["reference_wav"])),
            [1201, -1200, 600, -600],
        )

        with self.assertRaisesRegex(RuntimeError, "identical Reference SHA-256"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_different_direct_hashes(self) -> None:
        cases = self.make_cases()
        write_pcm16(
            Path(str(cases["enclosed"].payload["direct_wav"])),
            [601, -600, 300, -300],
        )

        with self.assertRaisesRegex(RuntimeError, "identical Direct SHA-256"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_unequal_common_output_scale_beyond_tolerance(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(
            cases,
            "enclosed",
            common_output_scale=1.000002,
        )

        with self.assertRaisesRegex(RuntimeError, "common_output_scale"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_common_scale_span_beyond_tolerance(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(
            cases,
            "near_wall",
            common_output_scale=0.99999925,
        )
        cases = replace_case_payload(
            cases,
            "enclosed",
            common_output_scale=1.00000075,
        )

        with self.assertRaisesRegex(RuntimeError, "common_output_scale"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_unequal_frame_provenance(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(cases, "enclosed", frames=3)

        with self.assertRaisesRegex(RuntimeError, "common frames"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_each_missing_wav_file(self) -> None:
        for field in ("reference_wav", "direct_wav", "wet_wav", "full_wav"):
            with self.subTest(field=field):
                cases = self.make_cases(f"missing-{field}")
                Path(str(cases["near_wall"].payload[field])).unlink()

                with self.assertRaisesRegex(RuntimeError, "WAV file exists"):
                    reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_near_wall_or_enclosed_wet_and_full_matching_open_space(self) -> None:
        for environment in ("near_wall", "enclosed"):
            for artifact in ("wet", "full"):
                with self.subTest(environment=environment, artifact=artifact):
                    cases = self.make_cases(f"same-{environment}-{artifact}")
                    source_field = (
                        "wet_wav" if artifact == "wet" else "direct_wav"
                    )
                    source_path = Path(
                        str(cases["open_space"].payload[source_field])
                    )
                    target_path = Path(
                        str(cases[environment].payload[f"{artifact}_wav"])
                    )
                    target_path.write_bytes(source_path.read_bytes())

                    display_name = "".join(
                        part.title() for part in environment.split("_")
                    )
                    with self.assertRaisesRegex(
                        RuntimeError,
                        f"{display_name} {artifact.title()} differs from OpenSpace",
                    ):
                        reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_enclosed_path_count_without_strict_growth(self) -> None:
        cases = self.make_cases()
        near_paths = cases["near_wall"].payload[
            "hardware_indirect_valid_paths"
        ]
        cases = replace_case_payload(
            cases,
            "enclosed",
            hardware_indirect_valid_paths=near_paths,
            cpu_reference_indirect_valid_paths=near_paths,
        )

        with self.assertRaisesRegex(RuntimeError, "Enclosed path-count growth"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_enclosed_directional_bins_without_strict_growth(self) -> None:
        cases = self.make_cases()
        near_bins = cases["near_wall"].payload[
            "hardware_directional_bin_count"
        ]
        cases = replace_case_payload(
            cases,
            "enclosed",
            hardware_directional_bin_count=near_bins,
            cpu_reference_directional_bin_count=near_bins,
        )

        with self.assertRaisesRegex(RuntimeError, "Enclosed directional-bin growth"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_insufficient_enclosed_energy_growth(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(
            cases,
            "enclosed",
            hardware_impulse_response_energy=0.087,
            cpu_reference_impulse_response_energy=0.087,
        )

        with self.assertRaisesRegex(RuntimeError, "Enclosed IR energy growth"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_insufficient_enclosed_wet_ratio_growth(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(
            cases,
            "enclosed",
            wet_to_reference_rms_ratio=0.109,
        )

        with self.assertRaisesRegex(
            RuntimeError,
            "Enclosed Wet/Reference RMS growth",
        ):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_enclosed_late_gain_at_zero_tolerance(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(
            cases,
            "enclosed",
            hardware_late_reverb_gain=1.0e-10,
            cpu_reference_late_reverb_gain=1.0e-10,
        )

        with self.assertRaisesRegex(RuntimeError, "Enclosed late-reverb growth"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_insufficient_enclosed_late_growth_from_nonzero_near_wall(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(
            cases,
            "near_wall",
            hardware_late_reverb_gain=0.010,
            cpu_reference_late_reverb_gain=0.010,
        )
        cases = replace_case_payload(
            cases,
            "enclosed",
            hardware_late_reverb_gain=0.0105,
            cpu_reference_late_reverb_gain=0.0105,
        )

        with self.assertRaisesRegex(RuntimeError, "Enclosed late-reverb growth"):
            reflection_environment_matrix.validate_matrix_manifests(cases)


class ReflectionEnvironmentEvidenceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.temporary_root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def make_matrix(
        self,
        suffix: str = "evidence",
        *,
        relative_paths: bool = False,
    ) -> tuple[dict[str, CaseManifest], dict[str, object], Path]:
        return make_complete_matrix(
            self.temporary_root / suffix,
            relative_paths=relative_paths,
        )

    def replace_evidence_mapping(
        self,
        evidence: dict[str, object],
        environment: str,
        mapping_name: str,
        **changes: object,
    ) -> dict[str, object]:
        current = evidence[environment]
        mapping = dict(getattr(current, mapping_name))
        mapping.update(changes)
        return {
            **evidence,
            environment: dataclasses.replace(
                current,
                **{mapping_name: mapping},
            ),
        }

    def rewrite_result(
        self,
        result_path: Path,
        **changes: object,
    ) -> None:
        payload = json.loads(result_path.read_text(encoding="utf-8"))
        payload.update(changes)
        result_path.write_text(
            json.dumps(payload, indent=2) + "\n",
            encoding="utf-8",
        )

    def test_loads_frozen_case_evidence_and_accepts_complete_matrix(self) -> None:
        cases, evidence, project_root = self.make_matrix()

        self.assertIsInstance(
            evidence["open_space"],
            reflection_environment_matrix.CaseEvidence,
        )
        self.assertEqual(
            evidence["open_space"].case.path,
            cases["open_space"].path.resolve(),
        )
        with self.assertRaises(dataclasses.FrozenInstanceError):
            evidence["open_space"].log_path = Path("different.log")
        reflection_environment_matrix.validate_end_to_end_evidence(
            evidence,
            project_root,
        )

    def test_accepts_result_and_marker_paths_relative_to_result_json(self) -> None:
        _, evidence, project_root = self.make_matrix(
            "relative",
            relative_paths=True,
        )

        reflection_environment_matrix.validate_end_to_end_evidence(
            evidence,
            project_root,
        )

        for case_evidence in evidence.values():
            self.assertTrue(case_evidence.screenshot_path.is_absolute())
            self.assertTrue(case_evidence.log_path.is_absolute())
            self.assertTrue(case_evidence.case.path.is_absolute())

    def test_rejects_result_with_wrong_schema_version_or_pass_flag(self) -> None:
        for field, value, message in (
            ("schema_version", 2, "schema_version=1"),
            ("passed", False, "passed=true"),
        ):
            with self.subTest(field=field):
                cases, _, _ = make_complete_matrix(
                    self.temporary_root / f"result-{field}",
                    load_evidence=False,
                )
                result_path = (
                    cases["near_wall"].path.parent / "near_wall_Result.json"
                )
                self.rewrite_result(result_path, **{field: value})

                with self.assertRaisesRegex(RuntimeError, message):
                    reflection_environment_matrix.load_case_evidence(
                        "near_wall",
                        result_path,
                    )

    def test_rejects_malformed_result_evidence_objects(self) -> None:
        for field, value in (
            ("scene", []),
            ("artifacts", None),
            ("image_metrics", "not-an-object"),
        ):
            with self.subTest(field=field):
                cases, _, _ = make_complete_matrix(
                    self.temporary_root / f"malformed-{field}",
                    load_evidence=False,
                )
                result_path = (
                    cases["near_wall"].path.parent / "near_wall_Result.json"
                )
                self.rewrite_result(result_path, **{field: value})

                with self.assertRaisesRegex(RuntimeError, f"{field} object"):
                    reflection_environment_matrix.load_case_evidence(
                        "near_wall",
                        result_path,
                    )

    def test_rejects_each_missing_screenshot_or_log(self) -> None:
        for field, message in (
            ("screenshot_path", "screenshot exists"),
            ("log_path", "Editor log exists"),
        ):
            with self.subTest(field=field):
                _, evidence, project_root = self.make_matrix(f"missing-{field}")
                getattr(evidence["near_wall"], field).unlink()

                with self.assertRaisesRegex(RuntimeError, message):
                    reflection_environment_matrix.validate_end_to_end_evidence(
                        evidence,
                        project_root,
                    )

    def test_rejects_black_screenshot_metrics(self) -> None:
        for field, value, message in (
            ("non_black_ratio", 0.099, "non-black screenshot ratio"),
            ("luma_stddev", 7.999, "screenshot luma standard deviation"),
        ):
            with self.subTest(field=field):
                _, evidence, project_root = self.make_matrix(f"black-{field}")
                evidence = self.replace_evidence_mapping(
                    evidence,
                    "near_wall",
                    "image_metrics",
                    **{field: value},
                )

                with self.assertRaisesRegex(RuntimeError, message):
                    reflection_environment_matrix.validate_end_to_end_evidence(
                        evidence,
                        project_root,
                    )

    def test_rejects_wrong_geometry_for_each_environment(self) -> None:
        for environment in reflection_environment_matrix.ENVIRONMENTS:
            with self.subTest(environment=environment):
                _, evidence, project_root = self.make_matrix(
                    f"geometry-{environment}"
                )
                evidence = self.replace_evidence_mapping(
                    evidence,
                    environment,
                    "scene",
                    geometry=(
                        reflection_environment_matrix.EXPECTED_GEOMETRY[
                            environment
                        ]
                        + 1
                    ),
                )

                with self.assertRaisesRegex(
                    RuntimeError,
                    "fixture geometry count",
                ):
                    reflection_environment_matrix.validate_end_to_end_evidence(
                        evidence,
                        project_root,
                    )

    def test_rejects_wrong_fixed_scene_evidence(self) -> None:
        for field, value, message in (
            ("direct_preset", "soft_occluded", "scene Direct preset"),
            (
                "reflection_environment",
                "enclosed",
                "scene reflection environment",
            ),
            ("reflection_bounces", 31, "scene reflection bounces"),
            ("distance_cm", 201.0, "scene source/listener distance"),
            ("air_absorption_profile", "off", "scene default air profile"),
        ):
            with self.subTest(field=field):
                _, evidence, project_root = self.make_matrix(f"scene-{field}")
                evidence = self.replace_evidence_mapping(
                    evidence,
                    "near_wall",
                    "scene",
                    **{field: value},
                )

                with self.assertRaisesRegex(RuntimeError, message):
                    reflection_environment_matrix.validate_end_to_end_evidence(
                        evidence,
                        project_root,
                    )

    def test_rejects_wrong_fixed_artifact_evidence(self) -> None:
        for field, value, message in (
            ("hardware", 0, "artifact hardware ray tracing"),
            ("reflection_rays", 2048, "artifact reflection rays"),
            ("reflection_bounces", 31, "artifact reflection bounces"),
            ("imported_assets", 3, "four imported comparison assets"),
            (
                "reflection_environment",
                "enclosed",
                "artifact reflection environment",
            ),
            ("direct_preset", "soft_occluded", "artifact Direct preset"),
        ):
            with self.subTest(field=field):
                _, evidence, project_root = self.make_matrix(f"artifact-{field}")
                evidence = self.replace_evidence_mapping(
                    evidence,
                    "near_wall",
                    "artifacts",
                    **{field: value},
                )

                with self.assertRaisesRegex(RuntimeError, message):
                    reflection_environment_matrix.validate_end_to_end_evidence(
                        evidence,
                        project_root,
                    )

    def test_rejects_each_artifact_marker_path_mismatch(self) -> None:
        for field in ("reference", "direct", "wet", "full", "manifest"):
            with self.subTest(field=field):
                _, evidence, project_root = self.make_matrix(f"path-{field}")
                evidence = self.replace_evidence_mapping(
                    evidence,
                    "near_wall",
                    "artifacts",
                    **{field: f"mismatched-{field}.dat"},
                )

                with self.assertRaisesRegex(
                    RuntimeError,
                    f"artifact {field} path agrees with manifest",
                ):
                    reflection_environment_matrix.validate_end_to_end_evidence(
                        evidence,
                        project_root,
                    )

    def test_rejects_missing_ir_uasset(self) -> None:
        _, evidence, project_root = self.make_matrix()
        ir_path = (
            project_root
            / "Content"
            / "UERayTracingAudio"
            / "Validation"
            / "NearWallIR.uasset"
        )
        ir_path.unlink()

        with self.assertRaisesRegex(RuntimeError, "IR .uasset exists under Content"):
            reflection_environment_matrix.validate_end_to_end_evidence(
                evidence,
                project_root,
            )

    def test_rejects_each_project_failure_pattern_in_retained_log(self) -> None:
        for pattern in FAILURE_LOG_PATTERNS:
            with self.subTest(pattern=pattern):
                _, evidence, project_root = self.make_matrix(
                    "log-" + re.sub(r"[^a-z]+", "-", pattern.lower()).strip("-")
                )
                evidence["near_wall"].log_path.write_text(
                    f"LogTemp: Display: before\n{pattern}: sentinel\n",
                    encoding="utf-8",
                )

                with self.assertRaisesRegex(RuntimeError, re.escape(pattern)):
                    reflection_environment_matrix.validate_end_to_end_evidence(
                        evidence,
                        project_root,
                    )


if __name__ == "__main__":
    unittest.main()
