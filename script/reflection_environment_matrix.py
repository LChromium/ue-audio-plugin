from __future__ import annotations

import json
import math
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ENVIRONMENTS = ("open_space", "near_wall", "enclosed")
EXPECTED_GEOMETRY = {"open_space": 0, "near_wall": 1, "enclosed": 7}
REFLECTION_RAYS = 4096
REFLECTION_BOUNCES = 32
ZERO_TOLERANCE = 1.0e-9
MAX_CPU_RELATIVE_DELTA = 0.05
MIN_DIRECTION_DOT = 0.99
MIN_DIRECTIONAL_ENERGY_RATIO = 0.05
MIN_WET_TO_REFERENCE_RATIO = 0.05
MIN_WET_STEREO_DIFFERENCE = 0.01

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
    (
        "indirect_valid_paths",
        "hardware_indirect_valid_paths",
        "cpu_reference_indirect_valid_paths",
    ),
    ("indirect_gain", "hardware_indirect_gain", "cpu_reference_indirect_gain"),
    (
        "early_reflection_gain",
        "hardware_early_reflection_gain",
        "cpu_reference_early_reflection_gain",
    ),
    (
        "late_reverb_gain",
        "hardware_late_reverb_gain",
        "cpu_reference_late_reverb_gain",
    ),
    (
        "impulse_response_energy",
        "hardware_impulse_response_energy",
        "cpu_reference_impulse_response_energy",
    ),
    (
        "directional_energy_ratio",
        "hardware_directional_energy_ratio",
        "cpu_reference_directional_energy_ratio",
    ),
    (
        "directional_bin_count",
        "hardware_directional_bin_count",
        "cpu_reference_directional_bin_count",
    ),
)

POSITIVE_NEAR_WALL_METRICS = {
    "indirect_valid_paths",
    "indirect_gain",
    "early_reflection_gain",
    "impulse_response_energy",
    "directional_energy_ratio",
    "directional_bin_count",
}


@dataclass(frozen=True)
class CaseManifest:
    environment: str
    path: Path
    payload: Mapping[str, Any]


def load_case_manifest(environment: str, path: Path) -> CaseManifest:
    if environment not in ENVIRONMENTS:
        raise RuntimeError(f"Unknown reflection environment: {environment!r}")
    payload = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"Reflection manifest must contain a JSON object: {path}")
    return CaseManifest(environment=environment, path=path, payload=payload)


def relative_delta(first: float, second: float) -> float:
    return abs(first - second) / max(abs(first), abs(second), 1.0e-12)


def _finite_number(
    payload: Mapping[str, Any],
    field: str,
    failures: list[str],
) -> float:
    value = payload.get(field)
    if isinstance(value, bool) or value is None:
        failures.append(f"finite numeric {field}")
        return 0.0
    try:
        number = float(value)
    except (OverflowError, TypeError, ValueError):
        failures.append(f"finite numeric {field}")
        return 0.0
    if not math.isfinite(number):
        failures.append(f"finite numeric {field}")
        return 0.0
    return number


def _direction(
    numbers: Mapping[str, float],
    prefix: str,
) -> tuple[float, float, float]:
    return (
        numbers[f"{prefix}_dominant_arrival_direction_x"],
        numbers[f"{prefix}_dominant_arrival_direction_y"],
        numbers[f"{prefix}_dominant_arrival_direction_z"],
    )


def _normalized_direction_dot(
    first: tuple[float, float, float],
    second: tuple[float, float, float],
) -> float | None:
    first_scale = max(abs(component) for component in first)
    second_scale = max(abs(component) for component in second)
    if first_scale <= ZERO_TOLERANCE or second_scale <= ZERO_TOLERANCE:
        return None

    first_scaled = tuple(component / first_scale for component in first)
    second_scaled = tuple(component / second_scale for component in second)
    first_length = math.sqrt(
        sum(component * component for component in first_scaled)
    )
    second_length = math.sqrt(
        sum(component * component for component in second_scaled)
    )
    return sum(
        first_component * second_component
        for first_component, second_component in zip(first_scaled, second_scaled)
    ) / (first_length * second_length)


def validate_case_manifest(case: CaseManifest) -> dict[str, float | int | str]:
    environment = case.environment
    if environment not in ENVIRONMENTS:
        raise RuntimeError(f"Unknown reflection environment: {environment!r}")

    payload = case.payload
    failures: list[str] = []
    numbers = {
        field: _finite_number(payload, field, failures) for field in FINITE_FIELDS
    }
    if failures:
        raise RuntimeError(
            f"{environment} reflection environment validation failed: "
            + ", ".join(failures)
        )

    if payload.get("input_asset") != "/Game/FirstPerson/Audio/MarchingBand.MarchingBand":
        failures.append("exact MarchingBand input asset")
    if "ravel" in str(payload.get("input_asset", "")).lower():
        failures.append("no Ravel input")
    if payload.get("direct_preset") != "clear":
        failures.append("clear Direct preset")
    if payload.get("reflection_environment") != environment:
        failures.append(f"{environment} environment provenance")
    if numbers["direct_distance_cm"] != 200.0:
        failures.append("200 cm Direct distance")
    if numbers["reflection_ray_count"] != REFLECTION_RAYS:
        failures.append("4096 reflection rays")
    if numbers["reflection_bounce_count"] != REFLECTION_BOUNCES:
        failures.append("32 reflection bounces")
    if numbers["sample_rate"] != 16000:
        failures.append("16000 Hz sample rate")
    if numbers["channels"] != 2:
        failures.append("stereo channels")
    if numbers["impulse_response_channels"] != 2:
        failures.append("directional-stereo impulse response")

    for field, failure in (
        ("hardware_ray_tracing", "hardware ray tracing provenance"),
        ("has_cpu_reference", "CPU reference provenance"),
        ("samples_finite", "finite audio samples"),
        ("audio_safety_checks_passed", "audio safety checks"),
        ("direct_semantics_passed", "Direct semantics"),
    ):
        if payload.get(field) is not True:
            failures.append(failure)
    if numbers["clipped_sample_count"] != 0:
        failures.append("zero clipped samples")
    if numbers["post_scale_peak"] > 0.99001:
        failures.append("safe post-scale peak")
    if numbers["direct_dropout_window_count"] != 0:
        failures.append("zero Direct dropout windows")

    relative_deltas: dict[str, float] = {}
    for metric, hardware_field, cpu_field in HARDWARE_CPU_PAIRS:
        hardware_value = numbers[hardware_field]
        cpu_value = numbers[cpu_field]
        metric_delta = relative_delta(hardware_value, cpu_value)
        relative_deltas[metric] = metric_delta
        if environment == "open_space":
            if (
                abs(hardware_value) > ZERO_TOLERANCE
                or abs(cpu_value) > ZERO_TOLERANCE
            ):
                failures.append(f"open_space zero hardware/CPU {metric}")
            continue

        requires_positive = (
            environment == "enclosed"
            or metric in POSITIVE_NEAR_WALL_METRICS
            or abs(hardware_value) > ZERO_TOLERANCE
            or abs(cpu_value) > ZERO_TOLERANCE
        )
        if not requires_positive:
            continue
        if hardware_value <= 0.0 or cpu_value <= 0.0:
            failures.append(f"{environment} positive hardware/CPU {metric}")
        elif metric_delta > MAX_CPU_RELATIVE_DELTA:
            failures.append(
                f"{environment} hardware/CPU {metric} agreement ({metric_delta:.6f})"
            )

    direction_dot = 0.0
    if environment != "open_space":
        direction_result = _normalized_direction_dot(
            _direction(numbers, "hardware"),
            _direction(numbers, "cpu_reference"),
        )
        if direction_result is None:
            failures.append(f"{environment} positive hardware/CPU dominant direction")
        else:
            direction_dot = direction_result
            if not math.isfinite(direction_dot):
                failures.append(
                    f"{environment} hardware/CPU dominant direction agreement "
                    "(non-finite)"
                )
            elif direction_dot < MIN_DIRECTION_DOT:
                failures.append(
                    f"{environment} hardware/CPU dominant direction agreement "
                    f"({direction_dot:.6f})"
                )

    if environment == "open_space":
        for field in (
            "modes_are_distinct",
            "directional_wet_is_distinct",
            "automatic_checks_passed",
        ):
            if payload.get(field) is not False:
                failures.append(f"open_space {field}=false")
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
            if abs(numbers[field]) > ZERO_TOLERANCE:
                failures.append(f"open_space zero {field}")
    else:
        for field in (
            "modes_are_distinct",
            "directional_wet_is_distinct",
            "automatic_checks_passed",
        ):
            if payload.get(field) is not True:
                failures.append(f"{environment} {field}=true")
        if numbers["wet_to_reference_rms_ratio"] < MIN_WET_TO_REFERENCE_RATIO:
            failures.append(f"{environment} audible Wet")
        if numbers["wet_stereo_normalized_difference"] < MIN_WET_STEREO_DIFFERENCE:
            failures.append(f"{environment} stereo Wet distinction")
        if numbers["hardware_directional_energy_ratio"] < MIN_DIRECTIONAL_ENERGY_RATIO:
            failures.append(f"{environment} directional energy")

    if failures:
        raise RuntimeError(
            f"{environment} reflection environment validation failed: "
            + ", ".join(failures)
        )

    metrics: dict[str, float | int | str] = {
        "environment": environment,
        "manifest_path": str(case.path),
        "input_asset": str(payload["input_asset"]),
        "hardware_cpu_direction_dot": direction_dot,
    }
    for field in (
        "direct_distance_cm",
        "reflection_ray_count",
        "reflection_bounce_count",
        "sample_rate",
        "channels",
        "impulse_response_channels",
        "frames",
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
    ):
        metrics[field] = numbers[field]
    for metric, delta in relative_deltas.items():
        metrics[f"hardware_cpu_{metric}_relative_delta"] = delta
    return metrics
