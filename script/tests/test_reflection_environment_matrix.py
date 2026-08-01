from __future__ import annotations

import contextlib
import dataclasses
import hashlib
import io
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
from pathlib import Path
from unittest import mock


SCRIPT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPT_ROOT))

import reflection_environment_matrix
import validate_reflection_environment_matrix
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
    "impulse_response_frames",
    "impulse_response_duration_seconds",
    "wet_mix",
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
    "Ensure condition failed",
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
        "impulse_response_frames": 16000,
        "impulse_response_duration_seconds": 1.0,
        "wet_mix": 0.8,
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


def write_wave(
    path: Path,
    samples: list[int] | tuple[int, ...],
    *,
    channels: int = 2,
    sample_width: int = 2,
    sample_rate: int = 16000,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(channels)
        stream.setsampwidth(sample_width)
        stream.setframerate(sample_rate)
        if sample_width == 1:
            frame_data = bytes(samples)
        else:
            frame_data = struct.pack(f"<{len(samples)}h", *samples)
        stream.writeframes(frame_data)


def write_pcm16(path: Path, samples: list[int] | tuple[int, ...]) -> None:
    write_wave(path, samples)


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
            ("impulse_response_frames", 8000, "16000 impulse-response frames"),
            (
                "impulse_response_duration_seconds",
                0.5,
                "1.0-second impulse-response duration",
            ),
            ("wet_mix", 0.123, "0.8 validation Wet mix"),
        ):
            with self.subTest(field=field):
                self.assert_rejected("near_wall", field, value, message)

    def test_rejects_missing_or_non_finite_ir_duration_and_wet_mix(self) -> None:
        for field in ("impulse_response_duration_seconds", "wet_mix"):
            for invalid_name, invalid_value in (
                ("missing", None),
                ("NaN", math.nan),
                ("infinity", math.inf),
            ):
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

    def test_rejects_ir_duration_inconsistent_with_frames_and_rate(self) -> None:
        self.assert_rejected(
            "near_wall",
            "impulse_response_duration_seconds",
            1.0 + 5.0e-7,
            "impulse-response duration matches frames/sample rate",
        )

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

    def test_rejects_manifest_frames_disagreeing_with_wav_headers(self) -> None:
        cases = self.make_cases()
        cases = replace_case_payload(cases, "enclosed", frames=3)

        with self.assertRaisesRegex(RuntimeError, "actual WAV frame count"):
            reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_each_missing_wav_file(self) -> None:
        for field in ("reference_wav", "direct_wav", "wet_wav", "full_wav"):
            with self.subTest(field=field):
                cases = self.make_cases(f"missing-{field}")
                Path(str(cases["near_wall"].payload[field])).unlink()

                with self.assertRaisesRegex(RuntimeError, "WAV file exists"):
                    reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_corrupt_wav_for_every_case_and_artifact(self) -> None:
        for environment in reflection_environment_matrix.ENVIRONMENTS:
            for field in (
                "reference_wav",
                "direct_wav",
                "wet_wav",
                "full_wav",
            ):
                with self.subTest(environment=environment, field=field):
                    cases = self.make_cases(f"corrupt-{environment}-{field}")
                    Path(str(cases[environment].payload[field])).write_bytes(
                        b"not a RIFF/WAVE file"
                    )
                    display_name = "".join(
                        part.title() for part in environment.split("_")
                    )

                    with self.assertRaisesRegex(
                        RuntimeError,
                        f"{display_name} {field} valid uncompressed PCM16 WAV",
                    ):
                        reflection_environment_matrix.validate_matrix_manifests(
                            cases
                        )

    def test_rejects_actual_wav_header_mismatch(self) -> None:
        mutations = (
            (
                "channels",
                {"samples": [120, -120], "channels": 1},
                "actual WAV channels",
            ),
            (
                "sample-rate",
                {"samples": [120, -120, 60, -60], "sample_rate": 48000},
                "actual WAV sample rate",
            ),
            (
                "sample-width",
                {
                    "samples": [128, 129, 127, 128],
                    "sample_width": 1,
                },
                "valid uncompressed PCM16 WAV",
            ),
            (
                "frames",
                {"samples": [120, -120]},
                "actual WAV frame count",
            ),
        )
        for label, options, message in mutations:
            with self.subTest(label=label):
                cases = self.make_cases(f"header-{label}")
                target = Path(str(cases["near_wall"].payload["wet_wav"]))
                write_wave(target, **options)

                with self.assertRaisesRegex(RuntimeError, message):
                    reflection_environment_matrix.validate_matrix_manifests(cases)

    def test_rejects_truncated_wav_frame_data(self) -> None:
        cases = self.make_cases("truncated-frame-data")
        target = Path(str(cases["near_wall"].payload["wet_wav"]))
        target.write_bytes(target.read_bytes()[:-2])

        with self.assertRaisesRegex(RuntimeError, "complete PCM frame data"):
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

    def test_load_rejects_existing_copied_manifest_outside_reference_directory(
        self,
    ) -> None:
        cases, _, _ = make_complete_matrix(
            self.temporary_root / "copied-manifest",
            load_evidence=False,
        )
        manifest_path = cases["near_wall"].path
        result_path = manifest_path.parent / "near_wall_Result.json"
        copied_manifest = (
            manifest_path.parent / "copied" / manifest_path.name
        )
        copied_manifest.parent.mkdir(parents=True)
        copied_manifest.write_bytes(manifest_path.read_bytes())
        payload = json.loads(result_path.read_text(encoding="utf-8"))
        payload["artifacts"]["manifest"] = str(copied_manifest)
        result_path.write_text(
            json.dumps(payload, indent=2) + "\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(
            RuntimeError,
            "Reference/Manifest naming contract",
        ):
            reflection_environment_matrix.load_case_evidence(
                "near_wall",
                result_path,
            )

    def test_load_rejects_noncanonical_reference_wav_suffix(self) -> None:
        cases, _, _ = make_complete_matrix(
            self.temporary_root / "reference-suffix",
            load_evidence=False,
        )
        manifest_path = cases["near_wall"].path
        result_path = manifest_path.parent / "near_wall_Result.json"
        reference_path = manifest_path.parent / "near_wall_Reference.wav"
        noncanonical_reference = manifest_path.parent / "near_wall_Input.wav"
        noncanonical_reference.write_bytes(reference_path.read_bytes())
        payload = json.loads(result_path.read_text(encoding="utf-8"))
        payload["artifacts"]["reference"] = str(noncanonical_reference)
        result_path.write_text(
            json.dumps(payload, indent=2) + "\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(
            RuntimeError,
            "canonical Reference WAV suffix",
        ):
            reflection_environment_matrix.load_case_evidence(
                "near_wall",
                result_path,
            )

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

    def test_rejects_non_json_numbers_for_scene_distance_and_luma_fields(
        self,
    ) -> None:
        fields = (
            (
                "scene",
                "distance_cm",
                "200.0",
                "scene source/listener distance",
            ),
            (
                "image_metrics",
                "non_black_ratio",
                "0.75",
                "finite non-black screenshot ratio",
            ),
            (
                "image_metrics",
                "luma_stddev",
                "12.0",
                "finite screenshot luma standard deviation",
            ),
        )
        invalid_kinds = (
            ("string", None),
            ("boolean", True),
            ("nan", math.nan),
            ("infinity", math.inf),
        )
        for mapping_name, field, numeric_string, message in fields:
            for kind, value in invalid_kinds:
                with self.subTest(field=field, kind=kind):
                    _, evidence, project_root = self.make_matrix(
                        f"number-{field}-{kind}"
                    )
                    evidence = self.replace_evidence_mapping(
                        evidence,
                        "near_wall",
                        mapping_name,
                        **{
                            field: (
                                numeric_string if kind == "string" else value
                            )
                        },
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


class ReflectionEnvironmentMatrixCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.temporary_root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def make_cli_fixture(
        self,
        suffix: str,
    ) -> tuple[
        dict[str, CaseManifest],
        dict[str, object],
        Path,
        Path,
    ]:
        cases, evidence, project_root = make_complete_matrix(
            self.temporary_root / suffix,
        )
        project_path = project_root / "UERayTracingAudioTest.uproject"
        project_path.write_text("{}\n", encoding="utf-8")
        engine_root = self.temporary_root / "UE_5.7"
        engine_root.mkdir(exist_ok=True)
        return cases, evidence, project_path, engine_root

    def cli_patches(
        self,
        arguments: list[str],
        engine_root: Path,
        project_path: Path,
    ) -> contextlib.ExitStack:
        stack = contextlib.ExitStack()
        stack.enter_context(
            mock.patch.object(
                sys,
                "argv",
                ["validate_reflection_environment_matrix.py", *arguments],
            )
        )
        stack.enter_context(
            mock.patch.object(
                validate_reflection_environment_matrix.validation_environment,
                "resolve_engine_root",
                return_value=engine_root,
            )
        )
        stack.enter_context(
            mock.patch.object(
                validate_reflection_environment_matrix.validation_environment,
                "resolve_project_path",
                return_value=project_path,
            )
        )
        return stack

    @staticmethod
    def environment_from_command(command: list[str]) -> str:
        return command[command.index("--reflection-environment") + 1]

    @staticmethod
    def path_from_command(command: list[str], option: str) -> Path:
        return Path(command[command.index(option) + 1])

    def successful_case_result(
        self,
        command: list[str],
        evidence: dict[str, object],
        *,
        stderr: str = "",
    ) -> subprocess.CompletedProcess[str]:
        environment = self.environment_from_command(command)
        result_path = self.path_from_command(command, "--result-json")
        screenshot_path = self.path_from_command(command, "--screenshot")
        payload = json.loads(
            evidence[environment].result_path.read_text(encoding="utf-8")
        )
        screenshot_path.parent.mkdir(parents=True, exist_ok=True)
        screenshot_path.write_bytes(b"synthetic Editor screenshot")
        payload["screenshot"] = str(screenshot_path)
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(
            json.dumps(payload, indent=2) + "\n",
            encoding="utf-8",
        )
        return subprocess.CompletedProcess(
            command,
            0,
            stdout=f"{environment} helper stdout\n",
            stderr=stderr,
        )

    @staticmethod
    def summary_temporary_path(output_path: Path) -> Path:
        return output_path.with_suffix(output_path.suffix + ".tmp")

    def seed_stale_summary(self, output_path: Path) -> tuple[bytes, bytes]:
        output_payload = b'{"passed": true, "end_to_end": true}\n'
        temporary_payload = b"stale temporary summary\n"
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(output_payload)
        self.summary_temporary_path(output_path).write_bytes(temporary_payload)
        return output_payload, temporary_payload

    @staticmethod
    def manifest_only_arguments(
        cases: dict[str, CaseManifest],
        project_path: Path,
        output_path: Path,
    ) -> list[str]:
        return [
            "--project",
            str(project_path),
            "--output",
            str(output_path),
            "--open-space-manifest",
            str(cases["open_space"].path),
            "--near-wall-manifest",
            str(cases["near_wall"].path),
            "--enclosed-manifest",
            str(cases["enclosed"].path),
        ]

    def test_build_case_command_uses_exact_list_arguments(self) -> None:
        repo_root = self.temporary_root / "repo"
        engine_root = self.temporary_root / "UE_5.7"
        project_path = self.temporary_root / "project" / "Test.uproject"
        output_root = self.temporary_root / "matrix"

        command = validate_reflection_environment_matrix.build_case_command(
            repo_root,
            engine_root,
            project_path,
            "near_wall",
            180.0,
            output_root / "NearWall.png",
            output_root / "NearWall_Result.json",
        )

        self.assertEqual(
            command,
            [
                sys.executable,
                str(repo_root / "script" / "validate_visible_editor_ab_scene.py"),
                "--artifacts",
                "--direct-preset",
                "clear",
                "--reflection-environment",
                "near_wall",
                "--reflection-bounces",
                "32",
                "--timeout",
                "180.0",
                "--screenshot",
                str(output_root / "NearWall.png"),
                "--result-json",
                str(output_root / "NearWall_Result.json"),
                "--engine-root",
                str(engine_root),
                "--project",
                str(project_path),
            ],
        )

    def test_manifest_arguments_are_all_or_none(self) -> None:
        cases, _, project_path, engine_root = self.make_cli_fixture(
            "all-or-none"
        )
        manifest_options = (
            ("--open-space-manifest", cases["open_space"].path),
            ("--near-wall-manifest", cases["near_wall"].path),
            ("--enclosed-manifest", cases["enclosed"].path),
        )

        for mask in range(1, 7):
            arguments = [
                value
                for index, (option, path) in enumerate(manifest_options)
                if mask & (1 << index)
                for value in (option, str(path))
            ]
            with self.subTest(mask=mask):
                with self.cli_patches(arguments, engine_root, project_path):
                    with mock.patch.object(
                        validate_reflection_environment_matrix.subprocess,
                        "run",
                        side_effect=AssertionError("Editor must not launch"),
                    ) as run_mock:
                        with self.assertRaisesRegex(
                            RuntimeError,
                            "must be supplied together",
                        ):
                            validate_reflection_environment_matrix.main()
                run_mock.assert_not_called()

    def test_full_mode_runs_three_cases_in_order_and_writes_atomic_summary(
        self,
    ) -> None:
        cases, evidence, project_path, engine_root = self.make_cli_fixture(
            "full-success"
        )
        timestamp = "20260802-120102"
        output_path = (
            project_path.parent
            / "Saved"
            / "UERayTracingAudio"
            / "ListeningAcceptance"
            / "ReflectionEnvironmentMatrix"
            / timestamp
            / "ReflectionEnvironmentMatrix_Manifest.json"
        )
        stdout = io.StringIO()
        stderr = io.StringIO()

        def run_success(
            command: list[str],
            **_: object,
        ) -> subprocess.CompletedProcess[str]:
            environment = self.environment_from_command(command)
            return self.successful_case_result(
                command,
                evidence,
                stderr=f"{environment} helper stderr\n",
            )

        arguments = [
            "--engine-root",
            str(engine_root),
            "--project",
            str(project_path),
        ]
        with self.cli_patches(arguments, engine_root, project_path):
            with mock.patch.object(
                validate_reflection_environment_matrix.time,
                "strftime",
                return_value=timestamp,
            ):
                with mock.patch.object(
                    validate_reflection_environment_matrix.subprocess,
                    "run",
                    side_effect=run_success,
                ) as run_mock:
                    with contextlib.redirect_stdout(stdout):
                        with contextlib.redirect_stderr(stderr):
                            result = validate_reflection_environment_matrix.main()

        self.assertEqual(result, 0)
        commands = [call.args[0] for call in run_mock.call_args_list]
        self.assertEqual(
            [self.environment_from_command(command) for command in commands],
            ["open_space", "near_wall", "enclosed"],
        )
        display_names = ("OpenSpace", "NearWall", "Enclosed")
        for call, display_name in zip(
            run_mock.call_args_list,
            display_names,
            strict=True,
        ):
            command = call.args[0]
            self.assertIsInstance(command, list)
            self.assertEqual(call.kwargs["cwd"], Path(__file__).resolve().parents[2])
            self.assertIs(call.kwargs["capture_output"], True)
            self.assertIs(call.kwargs["text"], True)
            self.assertIs(call.kwargs["check"], False)
            self.assertEqual(call.kwargs["timeout"], 240.0)
            self.assertNotIn("shell", call.kwargs)
            self.assertEqual(
                self.path_from_command(command, "--screenshot"),
                output_path.parent / f"{display_name}.png",
            )
            self.assertEqual(
                self.path_from_command(command, "--result-json"),
                output_path.parent / f"{display_name}_Result.json",
            )

        output_lines = stdout.getvalue().splitlines()
        self.assertEqual(
            [line for line in output_lines if "helper stdout" in line],
            [
                "open_space helper stdout",
                "near_wall helper stdout",
                "enclosed helper stdout",
            ],
        )
        self.assertEqual(
            stderr.getvalue().splitlines(),
            [
                "open_space helper stderr",
                "near_wall helper stderr",
                "enclosed helper stderr",
            ],
        )
        pass_lines = [
            line
            for line in output_lines
            if line.startswith("R3_REFLECTION_MATRIX_PASS ")
        ]
        self.assertEqual(
            pass_lines,
            [
                "R3_REFLECTION_MATRIX_PASS bounces=32 "
                f'manifest="{output_path}"'
            ],
        )

        summary = json.loads(output_path.read_text(encoding="utf-8"))
        self.assertEqual(summary["schema_version"], 1)
        self.assertIs(summary["passed"], True)
        self.assertIs(summary["end_to_end"], True)
        self.assertEqual(
            summary["fixed_config"],
            {
                "input_asset": "/Game/FirstPerson/Audio/MarchingBand.MarchingBand",
                "direct_preset": "clear",
                "distance_cm": 200,
                "air_absorption_profile": "default",
                "reflection_rays": 4096,
                "reflection_bounces": 32,
                "bake_output_sample_rate": 16000,
                "bake_output_channels": 2,
                "impulse_response_channels": 2,
                "impulse_response_frames": 16000,
                "impulse_response_duration_seconds": 1.0,
                "validation_wet_mix": 0.8,
            },
        )
        self.assertEqual(
            summary["thresholds"],
            {
                "fixed_float_tolerance": 1.0e-6,
                "ir_duration_consistency_tolerance": 1.0e-9,
                "zero_tolerance": 1.0e-9,
                "max_cpu_relative_delta": 0.05,
                "min_direction_dot": 0.99,
                "min_directional_energy_ratio": 0.05,
                "min_wet_to_reference_ratio": 0.05,
                "min_wet_stereo_difference": 0.01,
                "common_output_scale_tolerance": 1.0e-6,
                "min_enclosed_growth_ratio": 1.10,
                "min_non_black_ratio": 0.10,
                "min_luma_stddev": 8.0,
            },
        )
        self.assertEqual(
            set(summary["cases"]),
            {"open_space", "near_wall", "enclosed"},
        )
        near_wall = summary["cases"]["near_wall"]
        self.assertEqual(
            near_wall["manifest_path"],
            str(cases["near_wall"].path.resolve()),
        )
        self.assertEqual(
            near_wall["result"],
            str(output_path.parent / "NearWall_Result.json"),
        )
        self.assertEqual(
            near_wall["screenshot"],
            str(output_path.parent / "NearWall.png"),
        )
        self.assertEqual(near_wall["log"], str(evidence["near_wall"].log_path))
        self.assertEqual(near_wall["hardware_indirect_valid_paths"], 1000)
        self.assertEqual(near_wall["impulse_response_frames"], 16000)
        self.assertEqual(near_wall["impulse_response_duration_seconds"], 1.0)
        self.assertEqual(near_wall["wet_mix"], 0.8)
        self.assertEqual(
            near_wall["reference_sha256"],
            hashlib.sha256(
                Path(str(cases["near_wall"].payload["reference_wav"])).read_bytes()
            ).hexdigest(),
        )
        self.assertGreater(
            summary["comparisons"]["enclosed_to_near_wall_paths_ratio"],
            1.0,
        )
        self.assertFalse(
            output_path.with_suffix(output_path.suffix + ".tmp").exists()
        )

    def test_full_mode_invalidates_stale_summary_and_stops_after_first_nonzero_case(
        self,
    ) -> None:
        _, evidence, project_path, engine_root = self.make_cli_fixture(
            "first-failure"
        )
        output_path = self.temporary_root / "failure" / "summary.json"
        self.seed_stale_summary(output_path)
        stdout = io.StringIO()
        stderr = io.StringIO()

        def run_until_failure(
            command: list[str],
            **_: object,
        ) -> subprocess.CompletedProcess[str]:
            environment = self.environment_from_command(command)
            if environment == "open_space":
                return self.successful_case_result(command, evidence)
            return subprocess.CompletedProcess(
                command,
                17,
                stdout="near_wall helper stdout\n",
                stderr="near_wall helper failed\n",
            )

        arguments = [
            "--engine-root",
            str(engine_root),
            "--project",
            str(project_path),
            "--output",
            str(output_path),
        ]
        with self.cli_patches(arguments, engine_root, project_path):
            with mock.patch.object(
                validate_reflection_environment_matrix.subprocess,
                "run",
                side_effect=run_until_failure,
            ) as run_mock:
                with contextlib.redirect_stdout(stdout):
                    with contextlib.redirect_stderr(stderr):
                        with self.assertRaisesRegex(
                            RuntimeError,
                            "NearWall.*code 17",
                        ):
                            validate_reflection_environment_matrix.main()

        commands = [call.args[0] for call in run_mock.call_args_list]
        self.assertEqual(
            [self.environment_from_command(command) for command in commands],
            ["open_space", "near_wall"],
        )
        self.assertIn("near_wall helper stdout", stdout.getvalue())
        self.assertIn("near_wall helper failed", stderr.getvalue())
        self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())
        self.assertFalse(output_path.exists())
        self.assertFalse(self.summary_temporary_path(output_path).exists())

    def test_run_editor_case_rejects_success_without_result_json(self) -> None:
        output_root = self.temporary_root / "missing-result"
        completed = subprocess.CompletedProcess(
            ["helper"],
            0,
            stdout="helper completed\n",
            stderr="helper warning\n",
        )
        stdout = io.StringIO()
        stderr = io.StringIO()

        with mock.patch.object(
            validate_reflection_environment_matrix.subprocess,
            "run",
            return_value=completed,
        ) as run_mock:
            with contextlib.redirect_stdout(stdout):
                with contextlib.redirect_stderr(stderr):
                    with self.assertRaisesRegex(
                        RuntimeError,
                        "OpenSpace result JSON does not exist",
                    ):
                        validate_reflection_environment_matrix.run_editor_case(
                            self.temporary_root / "repo",
                            self.temporary_root / "engine",
                            self.temporary_root / "project" / "Test.uproject",
                            "open_space",
                            12.5,
                            output_root,
                        )

        self.assertEqual(run_mock.call_args.kwargs["timeout"], 72.5)
        self.assertEqual(stdout.getvalue(), "helper completed\n")
        self.assertEqual(stderr.getvalue(), "helper warning\n")

    def test_timeout_is_reported_as_fail_and_never_writes_summary(self) -> None:
        _, _, project_path, engine_root = self.make_cli_fixture("timeout")
        output_path = self.temporary_root / "timeout" / "summary.json"
        self.seed_stale_summary(output_path)
        stdout = io.StringIO()
        stderr = io.StringIO()
        arguments = [
            "--engine-root",
            str(engine_root),
            "--project",
            str(project_path),
            "--timeout",
            "5.5",
            "--output",
            str(output_path),
        ]

        with self.cli_patches(arguments, engine_root, project_path):
            with mock.patch.object(
                validate_reflection_environment_matrix.subprocess,
                "run",
                side_effect=subprocess.TimeoutExpired(
                    ["helper"],
                    65.5,
                    output=b"partial helper stdout\n",
                    stderr="partial helper stderr\n",
                ),
            ) as run_mock:
                with contextlib.redirect_stdout(stdout):
                    with contextlib.redirect_stderr(stderr):
                        result = validate_reflection_environment_matrix.entrypoint()

        self.assertEqual(result, 1)
        self.assertEqual(run_mock.call_args.kwargs["timeout"], 65.5)
        self.assertEqual(stdout.getvalue(), "partial helper stdout\n")
        self.assertEqual(
            stderr.getvalue().splitlines()[0],
            "partial helper stderr",
        )
        self.assertTrue(
            stderr.getvalue().splitlines()[1].startswith(
                "R3_REFLECTION_MATRIX_FAIL "
            )
        )
        self.assertFalse(output_path.exists())
        self.assertFalse(self.summary_temporary_path(output_path).exists())

    def test_manifest_only_load_failure_removes_stale_summary_and_temp(
        self,
    ) -> None:
        cases, _, project_path, engine_root = self.make_cli_fixture(
            "manifest-load-failure"
        )
        cases["enclosed"].path.unlink()
        output_path = self.temporary_root / "load-failure" / "summary.json"
        self.seed_stale_summary(output_path)
        stdout = io.StringIO()
        stderr = io.StringIO()

        with self.cli_patches(
            self.manifest_only_arguments(cases, project_path, output_path),
            engine_root,
            project_path,
        ):
            with contextlib.redirect_stdout(stdout):
                with contextlib.redirect_stderr(stderr):
                    result = validate_reflection_environment_matrix.entrypoint()

        self.assertEqual(result, 1)
        self.assertIn("R3_REFLECTION_MATRIX_FAIL", stderr.getvalue())
        self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())
        self.assertFalse(output_path.exists())
        self.assertFalse(self.summary_temporary_path(output_path).exists())

    def test_manifest_only_validation_failure_removes_stale_summary_and_temp(
        self,
    ) -> None:
        cases, _, project_path, engine_root = self.make_cli_fixture(
            "manifest-validation-failure"
        )
        enclosed_payload = json.loads(
            cases["enclosed"].path.read_text(encoding="utf-8")
        )
        enclosed_payload["reflection_bounce_count"] = 31
        cases["enclosed"].path.write_text(
            json.dumps(enclosed_payload, indent=2) + "\n",
            encoding="utf-8",
        )
        output_path = self.temporary_root / "validation-failure" / "summary.json"
        self.seed_stale_summary(output_path)
        stdout = io.StringIO()
        stderr = io.StringIO()

        with self.cli_patches(
            self.manifest_only_arguments(cases, project_path, output_path),
            engine_root,
            project_path,
        ):
            with contextlib.redirect_stdout(stdout):
                with contextlib.redirect_stderr(stderr):
                    result = validate_reflection_environment_matrix.entrypoint()

        self.assertEqual(result, 1)
        self.assertIn("R3_REFLECTION_MATRIX_FAIL", stderr.getvalue())
        self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())
        self.assertFalse(output_path.exists())
        self.assertFalse(self.summary_temporary_path(output_path).exists())

    def test_serialization_failure_removes_stale_summary_and_temp(self) -> None:
        cases, _, project_path, engine_root = self.make_cli_fixture(
            "serialization-failure"
        )
        output_path = self.temporary_root / "serialization" / "summary.json"
        self.seed_stale_summary(output_path)
        stdout = io.StringIO()
        stderr = io.StringIO()

        with self.cli_patches(
            self.manifest_only_arguments(cases, project_path, output_path),
            engine_root,
            project_path,
        ):
            with mock.patch.object(
                validate_reflection_environment_matrix.json,
                "dumps",
                side_effect=TypeError("not JSON serializable"),
            ):
                with contextlib.redirect_stdout(stdout):
                    with contextlib.redirect_stderr(stderr):
                        result = validate_reflection_environment_matrix.entrypoint()

        self.assertEqual(result, 1)
        self.assertIn("R3_REFLECTION_MATRIX_FAIL", stderr.getvalue())
        self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())
        self.assertFalse(output_path.exists())
        self.assertFalse(self.summary_temporary_path(output_path).exists())

    def test_manifest_only_rejects_output_equal_to_input_manifest_unchanged(
        self,
    ) -> None:
        cases, _, project_path, engine_root = self.make_cli_fixture(
            "manifest-output-collision"
        )
        output_path = cases["near_wall"].path.resolve()
        manifest_bytes = output_path.read_bytes()
        temporary_path = self.summary_temporary_path(output_path)
        temporary_bytes = b"pre-existing unrelated temporary data\n"
        temporary_path.write_bytes(temporary_bytes)
        stdout = io.StringIO()

        with self.cli_patches(
            self.manifest_only_arguments(cases, project_path, output_path),
            engine_root,
            project_path,
        ):
            with self.assertRaisesRegex(
                RuntimeError,
                "summary output.*collides.*NearWall manifest",
            ):
                with contextlib.redirect_stdout(stdout):
                    validate_reflection_environment_matrix.main()

        self.assertEqual(output_path.read_bytes(), manifest_bytes)
        self.assertEqual(temporary_path.read_bytes(), temporary_bytes)
        self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())

    def test_full_mode_rejects_output_or_temp_aliasing_reserved_case_artifacts(
        self,
    ) -> None:
        collisions = (
            ("output", "OpenSpace_Result.json", "OpenSpace result"),
            ("output", "NearWall.png", "NearWall screenshot"),
            ("temporary", "OpenSpace_Result.json", "OpenSpace result"),
            ("temporary", "NearWall.png", "NearWall screenshot"),
        )
        for summary_kind, reserved_name, evidence_label in collisions:
            with self.subTest(
                summary_kind=summary_kind,
                reserved_name=reserved_name,
            ):
                _, _, project_path, engine_root = self.make_cli_fixture(
                    f"reserved-{summary_kind}-{reserved_name}"
                )
                output_root = (
                    self.temporary_root
                    / f"reserved-output-{summary_kind}-{reserved_name}"
                )
                output_root.mkdir(parents=True)
                protected_bytes = b"protected collision evidence\n"
                if summary_kind == "output":
                    output_path = output_root / reserved_name
                    output_path.write_bytes(protected_bytes)
                    protected_path = output_path
                else:
                    output_path = output_root / "summary.json"
                    output_path.write_bytes(b"protected prior summary\n")
                    protected_path = self.summary_temporary_path(output_path)
                    protected_path.write_bytes(protected_bytes)
                    os.link(protected_path, output_root / reserved_name)

                arguments = [
                    "--engine-root",
                    str(engine_root),
                    "--project",
                    str(project_path),
                    "--output",
                    str(output_path),
                ]
                stdout = io.StringIO()
                completed = subprocess.CompletedProcess(
                    ["helper"],
                    17,
                    stdout="",
                    stderr="",
                )
                with self.cli_patches(arguments, engine_root, project_path):
                    with mock.patch.object(
                        validate_reflection_environment_matrix.subprocess,
                        "run",
                        return_value=completed,
                    ) as run_mock:
                        with self.assertRaisesRegex(
                            RuntimeError,
                            f"summary {summary_kind}.*collides.*{evidence_label}",
                        ):
                            with contextlib.redirect_stdout(stdout):
                                validate_reflection_environment_matrix.main()

                run_mock.assert_not_called()
                self.assertEqual(protected_path.read_bytes(), protected_bytes)
                self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())

    def test_full_mode_rejects_later_loaded_manifest_or_wav_output_collision(
        self,
    ) -> None:
        for artifact, evidence_label in (
            ("manifest", "OpenSpace manifest"),
            ("reference_wav", "OpenSpace reference_wav"),
        ):
            with self.subTest(artifact=artifact):
                cases, evidence, project_path, engine_root = self.make_cli_fixture(
                    f"loaded-{artifact}-collision"
                )
                output_path = (
                    cases["open_space"].path.resolve()
                    if artifact == "manifest"
                    else Path(
                        str(cases["open_space"].payload[artifact])
                    ).resolve()
                )
                evidence_bytes = output_path.read_bytes()
                arguments = [
                    "--engine-root",
                    str(engine_root),
                    "--project",
                    str(project_path),
                    "--output",
                    str(output_path),
                ]
                stdout = io.StringIO()

                def run_success(
                    command: list[str],
                    **_: object,
                ) -> subprocess.CompletedProcess[str]:
                    if self.environment_from_command(command) == "open_space":
                        output_path.write_bytes(evidence_bytes)
                    return self.successful_case_result(command, evidence)

                with self.cli_patches(arguments, engine_root, project_path):
                    with mock.patch.object(
                        validate_reflection_environment_matrix.subprocess,
                        "run",
                        side_effect=run_success,
                    ) as run_mock:
                        with self.assertRaisesRegex(
                            RuntimeError,
                            f"summary output.*collides.*{evidence_label}",
                        ):
                            with contextlib.redirect_stdout(stdout):
                                validate_reflection_environment_matrix.main()

                self.assertEqual(run_mock.call_count, 1)
                self.assertEqual(output_path.read_bytes(), evidence_bytes)
                self.assertFalse(
                    self.summary_temporary_path(output_path).exists()
                )
                self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())

    def test_manifest_only_rechecks_semantics_without_end_to_end_pass_marker(
        self,
    ) -> None:
        cases, evidence, project_path, engine_root = self.make_cli_fixture(
            "manifest-only"
        )
        for case_evidence in evidence.values():
            case_evidence.result_path.unlink()
            case_evidence.screenshot_path.unlink()
            case_evidence.log_path.unlink()
        for ir_path in project_path.parent.rglob("*IR.uasset"):
            ir_path.unlink()

        output_path = self.temporary_root / "recheck" / "summary.json"
        output_path.parent.mkdir(parents=True)
        output_path.write_text("stale summary\n", encoding="utf-8")
        arguments = [
            "--project",
            str(project_path),
            "--output",
            str(output_path),
            "--open-space-manifest",
            str(cases["open_space"].path),
            "--near-wall-manifest",
            str(cases["near_wall"].path),
            "--enclosed-manifest",
            str(cases["enclosed"].path),
        ]
        stdout = io.StringIO()
        stderr = io.StringIO()

        with self.cli_patches(arguments, engine_root, project_path):
            with mock.patch.object(
                validate_reflection_environment_matrix.subprocess,
                "run",
                side_effect=AssertionError("Editor must not launch"),
            ) as run_mock:
                with contextlib.redirect_stdout(stdout):
                    with contextlib.redirect_stderr(stderr):
                        result = validate_reflection_environment_matrix.main()

        self.assertEqual(result, 0)
        run_mock.assert_not_called()
        self.assertEqual(stderr.getvalue(), "")
        self.assertEqual(
            stdout.getvalue().splitlines(),
            [
                "R3_REFLECTION_MATRIX_RECHECK_PASS bounces=32 "
                f'manifest="{output_path}"'
            ],
        )
        self.assertNotIn("R3_REFLECTION_MATRIX_PASS", stdout.getvalue())
        summary = json.loads(output_path.read_text(encoding="utf-8"))
        self.assertIs(summary["passed"], True)
        self.assertIs(summary["end_to_end"], False)
        for case_payload in summary["cases"].values():
            self.assertNotIn("result", case_payload)
            self.assertNotIn("screenshot", case_payload)
            self.assertNotIn("log", case_payload)
        self.assertFalse(
            output_path.with_suffix(output_path.suffix + ".tmp").exists()
        )


if __name__ == "__main__":
    unittest.main()
