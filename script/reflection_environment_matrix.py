from __future__ import annotations

import hashlib
import json
import math
import os
import sys
import wave
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ENVIRONMENTS = ("open_space", "near_wall", "enclosed")
EXPECTED_GEOMETRY = {"open_space": 0, "near_wall": 1, "enclosed": 7}
REFLECTION_RAYS = 4096
REFLECTION_BOUNCES = 32
BAKE_OUTPUT_SAMPLE_RATE = 16000
BAKE_OUTPUT_CHANNELS = 2
IMPULSE_RESPONSE_CHANNELS = 2
IMPULSE_RESPONSE_FRAMES = 16000
IMPULSE_RESPONSE_DURATION_SECONDS = 1.0
VALIDATION_WET_MIX = 0.8
FIXED_FLOAT_TOLERANCE = 1.0e-6
IR_DURATION_CONSISTENCY_TOLERANCE = 1.0e-9
ZERO_TOLERANCE = 1.0e-9
MAX_CPU_RELATIVE_DELTA = 0.05
MIN_DIRECTION_DOT = 0.99
MIN_DIRECTIONAL_ENERGY_RATIO = 0.05
MIN_WET_TO_REFERENCE_RATIO = 0.05
MIN_WET_STEREO_DIFFERENCE = 0.01
COMMON_OUTPUT_SCALE_TOLERANCE = 1.0e-6
MIN_ENCLOSED_GROWTH_RATIO = 1.10
MIN_NON_BLACK_RATIO = 0.10
MIN_LUMA_STDDEV = 8.0
RESULT_SCHEMA_VERSION = 1

WAV_FIELDS = ("reference_wav", "direct_wav", "wet_wav", "full_wav")
COMMON_PROVENANCE_FIELDS = (
    "input_asset",
    "direct_preset",
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
)
FAILURE_PATTERNS = (
    "Fatal error",
    "Unhandled Exception",
    "Assertion failed",
    "Ensure condition failed",
    "LogWindows: Error",
)

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


@dataclass(frozen=True)
class CaseEvidence:
    case: CaseManifest
    result_path: Path
    scene: Mapping[str, Any]
    artifacts: Mapping[str, Any]
    image_metrics: Mapping[str, Any]
    screenshot_path: Path
    log_path: Path


def load_case_manifest(environment: str, path: Path) -> CaseManifest:
    if environment not in ENVIRONMENTS:
        raise RuntimeError(f"Unknown reflection environment: {environment!r}")
    payload = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(payload, dict):
        raise RuntimeError(f"Reflection manifest must contain a JSON object: {path}")
    return CaseManifest(environment=environment, path=path, payload=payload)


def _display_name(environment: str) -> str:
    return "".join(part.title() for part in environment.split("_"))


def _resolve_path(value: object, base: Path) -> Path | None:
    if not isinstance(value, (str, os.PathLike)) or not str(value):
        return None
    path = Path(value)
    if not path.is_absolute():
        path = base / path
    return path.resolve()


def _paths_equal(first: Path, second: Path) -> bool:
    return os.path.normcase(str(first.resolve())) == os.path.normcase(
        str(second.resolve())
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def pcm16_is_zero(path: Path) -> bool:
    with wave.open(str(path), "rb") as stream:
        if stream.getsampwidth() != 2:
            return False
        return all(
            byte == 0
            for byte in stream.readframes(stream.getnframes())
        )


def _validate_pcm16_wav(
    path: Path,
    *,
    expected_channels: float,
    expected_sample_rate: float,
    expected_frames: float,
    label: str,
    failures: list[str],
) -> None:
    try:
        with wave.open(str(path), "rb") as stream:
            actual_channels = stream.getnchannels()
            actual_sample_width = stream.getsampwidth()
            actual_sample_rate = stream.getframerate()
            actual_frames = stream.getnframes()
            compression_type = stream.getcomptype()
            frame_data = stream.readframes(actual_frames)
    except (EOFError, OSError, wave.Error):
        failures.append(f"{label} valid uncompressed PCM16 WAV")
        return

    if compression_type != "NONE" or actual_sample_width != 2:
        failures.append(f"{label} valid uncompressed PCM16 WAV")
    if actual_channels != expected_channels:
        failures.append(
            f"{label} actual WAV channels "
            f"({actual_channels} != {expected_channels:g})"
        )
    if actual_sample_rate != expected_sample_rate:
        failures.append(
            f"{label} actual WAV sample rate "
            f"({actual_sample_rate} != {expected_sample_rate:g})"
        )
    if actual_frames != expected_frames:
        failures.append(
            f"{label} actual WAV frame count "
            f"({actual_frames} != {expected_frames:g})"
        )
    expected_data_size = actual_frames * actual_channels * actual_sample_width
    if len(frame_data) != expected_data_size:
        failures.append(
            f"{label} complete PCM frame data "
            f"({len(frame_data)} != {expected_data_size} bytes)"
        )


def load_case_evidence(environment: str, result_path: Path) -> CaseEvidence:
    if environment not in ENVIRONMENTS:
        raise RuntimeError(f"Unknown reflection environment: {environment!r}")

    resolved_result_path = result_path.resolve()
    if not resolved_result_path.is_file():
        raise RuntimeError(
            f"{_display_name(environment)} result JSON exists: "
            f"{resolved_result_path}"
        )
    try:
        payload = json.loads(
            resolved_result_path.read_text(encoding="utf-8-sig")
        )
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(
            f"{_display_name(environment)} result JSON is readable: "
            f"{resolved_result_path} ({exc})"
        ) from exc
    if not isinstance(payload, dict):
        raise RuntimeError(
            f"{_display_name(environment)} result JSON must contain an object"
        )

    failures: list[str] = []
    if (
        type(payload.get("schema_version")) is not int
        or payload.get("schema_version") != RESULT_SCHEMA_VERSION
    ):
        failures.append("schema_version=1")
    if payload.get("passed") is not True:
        failures.append("passed=true")

    mappings: dict[str, Mapping[str, Any]] = {}
    for field in ("scene", "artifacts", "image_metrics"):
        value = payload.get(field)
        if not isinstance(value, Mapping):
            failures.append(f"{field} object")
        else:
            mappings[field] = value

    screenshot_path = _resolve_path(
        payload.get("screenshot"),
        resolved_result_path.parent,
    )
    if screenshot_path is None:
        failures.append("screenshot path")
    log_path = _resolve_path(
        payload.get("log"),
        resolved_result_path.parent,
    )
    if log_path is None:
        failures.append("Editor log path")

    reference_path = None
    manifest_path = None
    expected_manifest_path = None
    artifacts = mappings.get("artifacts")
    if artifacts is not None:
        reference_path = _resolve_path(
            artifacts.get("reference"),
            resolved_result_path.parent,
        )
        if reference_path is None:
            failures.append("artifact Reference path")
        else:
            reference_suffix = "_Reference.wav"
            reference_name = reference_path.name
            safe_prefix = reference_name[: -len(reference_suffix)]
            if not reference_name.endswith(reference_suffix) or not safe_prefix:
                failures.append("canonical Reference WAV suffix")
            else:
                expected_manifest_path = reference_path.with_name(
                    f"{safe_prefix}_Manifest.json"
                ).resolve()

        manifest_path = _resolve_path(
            artifacts.get("manifest"),
            resolved_result_path.parent,
        )
        if manifest_path is None:
            failures.append("artifact manifest path")
        elif (
            expected_manifest_path is not None
            and not _paths_equal(manifest_path, expected_manifest_path)
        ):
            failures.append("Reference/Manifest naming contract")
        elif not manifest_path.is_file():
            failures.append(f"artifact manifest exists ({manifest_path})")

    if failures:
        raise RuntimeError(
            f"{_display_name(environment)} result evidence validation failed: "
            + ", ".join(failures)
        )

    assert manifest_path is not None
    assert screenshot_path is not None
    assert log_path is not None
    case = load_case_manifest(environment, manifest_path)
    return CaseEvidence(
        case=case,
        result_path=resolved_result_path,
        scene=mappings["scene"],
        artifacts=mappings["artifacts"],
        image_metrics=mappings["image_metrics"],
        screenshot_path=screenshot_path,
        log_path=log_path,
    )


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
    if numbers["sample_rate"] != BAKE_OUTPUT_SAMPLE_RATE:
        failures.append("16000 Hz sample rate")
    if numbers["channels"] != BAKE_OUTPUT_CHANNELS:
        failures.append("stereo channels")
    if numbers["impulse_response_channels"] != IMPULSE_RESPONSE_CHANNELS:
        failures.append("directional-stereo impulse response")
    if numbers["impulse_response_frames"] != IMPULSE_RESPONSE_FRAMES:
        failures.append("16000 impulse-response frames")
    if not math.isclose(
        numbers["impulse_response_duration_seconds"],
        IMPULSE_RESPONSE_DURATION_SECONDS,
        rel_tol=0.0,
        abs_tol=FIXED_FLOAT_TOLERANCE,
    ):
        failures.append("1.0-second impulse-response duration")
    if not math.isclose(
        numbers["wet_mix"],
        VALIDATION_WET_MIX,
        rel_tol=0.0,
        abs_tol=FIXED_FLOAT_TOLERANCE,
    ):
        failures.append("0.8 validation Wet mix")
    if numbers["sample_rate"] <= 0.0 or not math.isclose(
        numbers["impulse_response_duration_seconds"],
        numbers["impulse_response_frames"] / numbers["sample_rate"]
        if numbers["sample_rate"] > 0.0
        else 0.0,
        rel_tol=0.0,
        abs_tol=IR_DURATION_CONSISTENCY_TOLERANCE,
    ):
        failures.append("impulse-response duration matches frames/sample rate")

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
        "impulse_response_frames",
        "impulse_response_duration_seconds",
        "wet_mix",
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


def _validate_environment_keys(
    cases: Mapping[str, object],
    label: str,
) -> None:
    actual = set(cases)
    expected = set(ENVIRONMENTS)
    if actual != expected:
        missing = sorted(expected - actual)
        unexpected = sorted(str(value) for value in actual - expected)
        raise RuntimeError(
            f"{label} requires exactly OpenSpace, NearWall, and Enclosed "
            f"(missing={missing}, unexpected={unexpected})"
        )


def _ratio(numerator: float, denominator: float) -> float:
    ratio = numerator / max(abs(denominator), ZERO_TOLERANCE)
    if not math.isfinite(ratio):
        return math.copysign(sys.float_info.max, numerator)
    return ratio


def validate_matrix_manifests(
    cases: Mapping[str, CaseManifest],
) -> dict[str, object]:
    _validate_environment_keys(cases, "Reflection environment matrix")

    metrics_by_environment: dict[
        str,
        dict[str, float | int | str],
    ] = {}
    paths_by_environment: dict[str, dict[str, Path]] = {}
    hashes_by_environment: dict[str, dict[str, str]] = {}
    failures: list[str] = []

    for environment in ENVIRONMENTS:
        display_name = _display_name(environment)
        case = cases[environment]
        if not isinstance(case, CaseManifest):
            failures.append(f"{display_name} CaseManifest")
            continue
        if case.environment != environment:
            failures.append(
                f"{display_name} mapping/case environment agreement "
                f"({case.environment!r})"
            )
            continue

        metrics = validate_case_manifest(case)
        metrics_by_environment[environment] = metrics
        wav_paths: dict[str, Path] = {}
        for field in WAV_FIELDS:
            path = _resolve_path(case.payload.get(field), case.path.parent)
            if path is None:
                failures.append(f"{display_name} {field} WAV path")
                continue
            wav_paths[field] = path
            if not path.is_file():
                failures.append(
                    f"{display_name} WAV file exists ({field}={path})"
                )
                continue
            _validate_pcm16_wav(
                path,
                expected_channels=float(metrics["channels"]),
                expected_sample_rate=float(metrics["sample_rate"]),
                expected_frames=float(metrics["frames"]),
                label=f"{display_name} {field}",
                failures=failures,
            )
        paths_by_environment[environment] = wav_paths

    if failures:
        raise RuntimeError(
            "Reflection environment matrix validation failed: "
            + ", ".join(failures)
        )

    for environment in ENVIRONMENTS:
        hashes_by_environment[environment] = {
            field: sha256_file(paths_by_environment[environment][field])
            for field in WAV_FIELDS
        }

    open_case = cases["open_space"]
    for environment in ("near_wall", "enclosed"):
        display_name = _display_name(environment)
        case = cases[environment]
        for field in COMMON_PROVENANCE_FIELDS:
            if case.payload.get(field) != open_case.payload.get(field):
                if field == "frames":
                    failures.append(f"{display_name} common frames")
                elif field == "channels":
                    failures.append(f"{display_name} common channels")
                else:
                    failures.append(f"{display_name} common {field} provenance")
    common_scales = [
        float(metrics_by_environment[environment]["common_output_scale"])
        for environment in ENVIRONMENTS
    ]
    if max(common_scales) - min(common_scales) > COMMON_OUTPUT_SCALE_TOLERANCE:
        failures.append(
            "common_output_scale span within "
            f"{COMMON_OUTPUT_SCALE_TOLERANCE:.0e}"
        )

    if len(
        {
            hashes_by_environment[environment]["reference_wav"]
            for environment in ENVIRONMENTS
        }
    ) != 1:
        failures.append("identical Reference SHA-256 across environments")
    if len(
        {
            hashes_by_environment[environment]["direct_wav"]
            for environment in ENVIRONMENTS
        }
    ) != 1:
        failures.append("identical Direct SHA-256 across environments")

    open_hashes = hashes_by_environment["open_space"]
    if open_hashes["direct_wav"] != open_hashes["full_wav"]:
        failures.append("OpenSpace Direct/Full SHA-256 equality")
    try:
        open_wet_is_zero = pcm16_is_zero(
            paths_by_environment["open_space"]["wet_wav"]
        )
    except (OSError, EOFError, wave.Error):
        open_wet_is_zero = False
    if not open_wet_is_zero:
        failures.append("OpenSpace zero Wet PCM")

    for environment in ("near_wall", "enclosed"):
        display_name = _display_name(environment)
        hashes = hashes_by_environment[environment]
        for artifact in ("wet", "full"):
            if hashes[f"{artifact}_wav"] == open_hashes[f"{artifact}_wav"]:
                failures.append(
                    f"{display_name} {artifact.title()} differs from OpenSpace"
                )

    near = metrics_by_environment["near_wall"]
    enclosed = metrics_by_environment["enclosed"]
    near_paths = float(near["hardware_indirect_valid_paths"])
    enclosed_paths = float(enclosed["hardware_indirect_valid_paths"])
    near_bins = float(near["hardware_directional_bin_count"])
    enclosed_bins = float(enclosed["hardware_directional_bin_count"])
    near_energy = float(near["hardware_impulse_response_energy"])
    enclosed_energy = float(enclosed["hardware_impulse_response_energy"])
    near_wet_ratio = float(near["wet_to_reference_rms_ratio"])
    enclosed_wet_ratio = float(enclosed["wet_to_reference_rms_ratio"])
    near_late = float(near["hardware_late_reverb_gain"])
    enclosed_late = float(enclosed["hardware_late_reverb_gain"])

    if enclosed_paths <= near_paths:
        failures.append("Enclosed path-count growth")
    if enclosed_bins <= near_bins:
        failures.append("Enclosed directional-bin growth")
    if enclosed_energy < near_energy * MIN_ENCLOSED_GROWTH_RATIO:
        failures.append("Enclosed IR energy growth")
    if enclosed_wet_ratio < near_wet_ratio * MIN_ENCLOSED_GROWTH_RATIO:
        failures.append("Enclosed Wet/Reference RMS growth")
    minimum_enclosed_late = (
        near_late * MIN_ENCLOSED_GROWTH_RATIO
        if near_late > ZERO_TOLERANCE
        else ZERO_TOLERANCE
    )
    if enclosed_late <= minimum_enclosed_late:
        failures.append("Enclosed late-reverb growth")

    if failures:
        raise RuntimeError(
            "Reflection environment matrix validation failed: "
            + ", ".join(failures)
        )

    case_summary: dict[str, dict[str, object]] = {}
    for environment in ENVIRONMENTS:
        case = cases[environment]
        summary: dict[str, object] = dict(metrics_by_environment[environment])
        summary["manifest_path"] = str(case.path.resolve())
        for field in WAV_FIELDS:
            stem = field.removesuffix("_wav")
            summary[field] = str(paths_by_environment[environment][field])
            summary[f"{stem}_sha256"] = hashes_by_environment[environment][field]
        case_summary[environment] = summary

    thresholds: dict[str, float] = {
        "fixed_float_tolerance": FIXED_FLOAT_TOLERANCE,
        "ir_duration_consistency_tolerance": IR_DURATION_CONSISTENCY_TOLERANCE,
        "zero_tolerance": ZERO_TOLERANCE,
        "max_cpu_relative_delta": MAX_CPU_RELATIVE_DELTA,
        "min_direction_dot": MIN_DIRECTION_DOT,
        "min_directional_energy_ratio": MIN_DIRECTIONAL_ENERGY_RATIO,
        "min_wet_to_reference_ratio": MIN_WET_TO_REFERENCE_RATIO,
        "min_wet_stereo_difference": MIN_WET_STEREO_DIFFERENCE,
        "common_output_scale_tolerance": COMMON_OUTPUT_SCALE_TOLERANCE,
        "min_enclosed_growth_ratio": MIN_ENCLOSED_GROWTH_RATIO,
        "min_non_black_ratio": MIN_NON_BLACK_RATIO,
        "min_luma_stddev": MIN_LUMA_STDDEV,
    }
    comparisons: dict[str, float] = {
        "enclosed_to_near_wall_paths_ratio": _ratio(
            enclosed_paths,
            near_paths,
        ),
        "enclosed_to_near_wall_directional_bins_ratio": _ratio(
            enclosed_bins,
            near_bins,
        ),
        "enclosed_to_near_wall_ir_energy_ratio": _ratio(
            enclosed_energy,
            near_energy,
        ),
        "enclosed_to_near_wall_wet_ratio": _ratio(
            enclosed_wet_ratio,
            near_wet_ratio,
        ),
        "enclosed_to_near_wall_late_reverb_ratio": _ratio(
            enclosed_late,
            near_late,
        ),
    }
    return {
        "thresholds": thresholds,
        "cases": case_summary,
        "comparisons": comparisons,
    }


def _strict_number(
    values: Mapping[str, Any],
    field: str,
    failures: list[str],
    failure: str,
) -> float | None:
    value = values.get(field)
    if type(value) not in (int, float):
        failures.append(failure)
        return None
    try:
        number = float(value)
    except (OverflowError, TypeError, ValueError):
        failures.append(failure)
        return None
    if not math.isfinite(number):
        failures.append(failure)
        return None
    return number


def _strict_integer(
    values: Mapping[str, Any],
    field: str,
    failures: list[str],
    failure: str,
) -> int | None:
    value = values.get(field)
    if type(value) is not int:
        failures.append(failure)
        return None
    return value


def _ir_asset_path(
    object_path: object,
    content_root: Path,
) -> Path | None:
    if not isinstance(object_path, str) or not object_path.startswith("/Game/"):
        return None
    package_path, separator, object_name = object_path.partition(".")
    if not separator or not object_name:
        return None
    relative_package = package_path.removeprefix("/Game/")
    if not relative_package:
        return None
    candidate = Path(str(content_root / relative_package) + ".uasset").resolve()
    try:
        candidate.relative_to(content_root)
    except ValueError:
        return None
    return candidate


def validate_end_to_end_evidence(
    cases: Mapping[str, CaseEvidence],
    project_root: Path,
) -> None:
    _validate_environment_keys(cases, "End-to-end reflection evidence")
    resolved_project_root = project_root.resolve()
    content_root = (resolved_project_root / "Content").resolve()
    failures: list[str] = []

    for environment in ENVIRONMENTS:
        display_name = _display_name(environment)
        evidence = cases[environment]
        if not isinstance(evidence, CaseEvidence):
            failures.append(f"{display_name} CaseEvidence")
            continue
        if evidence.case.environment != environment:
            failures.append(f"{display_name} evidence/case environment agreement")
        if not evidence.result_path.is_file():
            failures.append(f"{display_name} result JSON exists")

        scene = evidence.scene
        if scene.get("direct_preset") != "clear":
            failures.append(f"{display_name} scene Direct preset")
        if scene.get("reflection_environment") != environment:
            failures.append(f"{display_name} scene reflection environment")
        geometry = _strict_integer(
            scene,
            "geometry",
            failures,
            f"{display_name} fixture geometry count",
        )
        if (
            geometry is not None
            and geometry != EXPECTED_GEOMETRY[environment]
        ):
            failures.append(
                f"{display_name} fixture geometry count "
                f"({geometry} != {EXPECTED_GEOMETRY[environment]})"
            )
        scene_bounces = _strict_integer(
            scene,
            "reflection_bounces",
            failures,
            f"{display_name} scene reflection bounces",
        )
        if scene_bounces is not None and scene_bounces != REFLECTION_BOUNCES:
            failures.append(f"{display_name} scene reflection bounces")
        scene_distance = _strict_number(
            scene,
            "distance_cm",
            failures,
            f"{display_name} scene source/listener distance",
        )
        if scene_distance is not None and scene_distance != 200.0:
            failures.append(f"{display_name} scene source/listener distance")
        if scene.get("air_absorption_profile") != "default":
            failures.append(f"{display_name} scene default air profile")

        artifacts = evidence.artifacts
        artifact_hardware = _strict_integer(
            artifacts,
            "hardware",
            failures,
            f"{display_name} artifact hardware ray tracing",
        )
        if artifact_hardware is not None and artifact_hardware != 1:
            failures.append(f"{display_name} artifact hardware ray tracing")
        artifact_rays = _strict_integer(
            artifacts,
            "reflection_rays",
            failures,
            f"{display_name} artifact reflection rays",
        )
        if artifact_rays is not None and artifact_rays != REFLECTION_RAYS:
            failures.append(f"{display_name} artifact reflection rays")
        artifact_bounces = _strict_integer(
            artifacts,
            "reflection_bounces",
            failures,
            f"{display_name} artifact reflection bounces",
        )
        if (
            artifact_bounces is not None
            and artifact_bounces != REFLECTION_BOUNCES
        ):
            failures.append(f"{display_name} artifact reflection bounces")
        imported_assets = _strict_integer(
            artifacts,
            "imported_assets",
            failures,
            f"{display_name} four imported comparison assets",
        )
        if imported_assets is not None and imported_assets != 4:
            failures.append(f"{display_name} four imported comparison assets")
        if artifacts.get("reflection_environment") != environment:
            failures.append(f"{display_name} artifact reflection environment")
        if artifacts.get("direct_preset") != "clear":
            failures.append(f"{display_name} artifact Direct preset")

        result_base = evidence.result_path.parent
        artifact_manifest = _resolve_path(
            artifacts.get("manifest"),
            result_base,
        )
        if artifact_manifest is None or not _paths_equal(
            artifact_manifest,
            evidence.case.path,
        ):
            failures.append(
                f"{display_name} artifact manifest path agrees with manifest"
            )
        for artifact_field, manifest_field in (
            ("reference", "reference_wav"),
            ("direct", "direct_wav"),
            ("wet", "wet_wav"),
            ("full", "full_wav"),
        ):
            artifact_path = _resolve_path(
                artifacts.get(artifact_field),
                result_base,
            )
            manifest_path = _resolve_path(
                evidence.case.payload.get(manifest_field),
                evidence.case.path.parent,
            )
            if (
                artifact_path is None
                or manifest_path is None
                or not _paths_equal(artifact_path, manifest_path)
            ):
                failures.append(
                    f"{display_name} artifact {artifact_field} path agrees "
                    "with manifest"
                )

        ir_path = _ir_asset_path(artifacts.get("ir_asset"), content_root)
        if ir_path is None:
            failures.append(f"{display_name} valid IR object path under Content")
        elif not ir_path.is_file():
            failures.append(
                f"{display_name} IR .uasset exists under Content ({ir_path})"
            )

        if not evidence.screenshot_path.is_file():
            failures.append(f"{display_name} screenshot exists")
        if not evidence.log_path.is_file():
            failures.append(f"{display_name} Editor log exists")

        non_black_ratio = _strict_number(
            evidence.image_metrics,
            "non_black_ratio",
            failures,
            f"{display_name} finite non-black screenshot ratio",
        )
        if (
            non_black_ratio is not None
            and non_black_ratio < MIN_NON_BLACK_RATIO
        ):
            failures.append(f"{display_name} non-black screenshot ratio")
        luma_stddev = _strict_number(
            evidence.image_metrics,
            "luma_stddev",
            failures,
            f"{display_name} finite screenshot luma standard deviation",
        )
        if luma_stddev is not None and luma_stddev < MIN_LUMA_STDDEV:
            failures.append(
                f"{display_name} screenshot luma standard deviation"
            )

        if evidence.log_path.is_file():
            try:
                log_text = evidence.log_path.read_text(
                    encoding="utf-8",
                    errors="replace",
                )
            except OSError as exc:
                failures.append(f"{display_name} Editor log readable ({exc})")
            else:
                for pattern in FAILURE_PATTERNS:
                    if pattern in log_text:
                        failures.append(
                            f"{display_name} Editor log contains {pattern}"
                        )

    if failures:
        raise RuntimeError(
            "R3 end-to-end evidence validation failed: "
            + ", ".join(failures)
        )
