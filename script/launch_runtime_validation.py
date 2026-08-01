from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
import time
from pathlib import Path

import validation_environment

DEFAULT_GAME_SECONDS = 180.0
DEFAULT_EDITOR_READY_TIMEOUT_SECONDS = 120.0
MIN_INTEGRATED_WET_INPUT_RMS_RATIO = 0.05
MIN_CONTINUOUS_WET_BUFFER_COUNT = 24
MIN_WET_PRESENCE_FRACTION = 0.80
MAX_SILENT_WET_RUN_FRACTION = 0.20
FAILURE_PATTERNS = (
    "Fatal error",
    "Unhandled Exception",
    "Assertion failed",
    "LogWindows: Error",
)
EDITOR_READY_PATTERN = "LogLoad: (Engine Initialization) Total time:"
RUNTIME_MODULE_MARKERS = (
    "UERayTracingAudioSDK module initialized.",
    "UERayTracingAudio runtime module initialized.",
)
EDITOR_MODULE_MARKERS = RUNTIME_MODULE_MARKERS + (
    "UERayTracingAudioEditor module initialized.",
)
AUDIO_DIRECT_PATH_MARKERS = (
    "submits direct sound visibility queries asynchronously to the render thread",
    "uses hardware ray tracing for direct sound visibility queries",
    "falls back to physics line traces for direct sound visibility queries",
)
AUDIO_INDIRECT_PATH_MARKERS = (
    "submits indirect sound energy-field queries asynchronously to the render thread",
    "uses hardware ray tracing for indirect sound path queries",
    "falls back to CPU acoustic scene queries for indirect sound path tracing",
)
DIRECT_SWEEP_MARKER = "UERayTracingAudio direct sweep:"
DIRECT_SWEEP_PATTERN = re.compile(
    r"UERayTracingAudio direct sweep: passed=(?P<passed>[01]) "
    r"generations=(?P<generations>[0-9]+) "
    r"distance_min_cm=(?P<distance_min>[0-9.eE+-]+) "
    r"distance_max_cm=(?P<distance_max>[0-9.eE+-]+) "
    r"visibility_min=(?P<visibility_min>[0-9.eE+-]+) "
    r"visibility_max=(?P<visibility_max>[0-9.eE+-]+) "
    r"gain_min=(?P<gain_min>[0-9.eE+-]+) "
    r"gain_max=(?P<gain_max>[0-9.eE+-]+) "
    r"max_gain_step=(?P<max_gain_step>[0-9.eE+-]+) "
    r"direct_dropouts=(?P<direct_dropouts>[0-9]+) "
    r"restored=(?P<restored>[01]) hardware=(?P<hardware>[01])"
)
VALIDATION_RESULT_MARKER = "UERayTracingAudio validation result:"
VISIBLE_SCENE_MARKER = "UERayTracingAudio validation visible scene ready:"
PRIMARY_INPUT_MARKER = "UERayTracingAudio validation primary input:"
PRIMARY_INPUT_PATTERN = re.compile(
    r"UERayTracingAudio validation primary input:.*?"
    r"real_soundwave=(?P<real_soundwave>[01]).*?asset=\"(?P<asset>[^\"]+)\""
)
AUDIO_PIPELINE_MARKER = "UERayTracingAudio validation audio pipeline:"
AUDIO_PIPELINE_PATTERN = re.compile(
    r"UERayTracingAudio validation audio pipeline:.*?"
    r"max_actual_volume=(?P<max_actual_volume>[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?).*?"
    r"pre_distance_buffers=(?P<pre_distance_buffers>[0-9]+).*?"
    r"pre_distance_non_silent=(?P<pre_distance_non_silent>[0-9]+)"
)
DATA_SOURCE_BAKE_MARKER = (
    "UERayTracingAudio validation data-source bake started"
)
DATA_SOURCE_VALIDATION_MARKER = "UERayTracingAudio validation data sources:"
_DATA_SOURCE_INTEGER_FIELDS = (
    "passed",
    *(
        field
        for mode in ("baked", "realtime", "hybrid")
        for field in (
            f"{mode}_buffers",
            f"{mode}_input_non_silent",
            f"{mode}_non_silent",
            f"{mode}_rms_measured",
            f"{mode}_wet_present",
            f"{mode}_max_silent_run",
            f"{mode}_audible_wet",
            f"{mode}_max_inaudible_run",
            f"{mode}_over_unit",
        )
    ),
    "non_finite",
    "stereo_ir",
    "baked_kernels",
    "realtime_kernels",
    "hybrid_kernels",
    "audio_playing",
    "interactive_hybrid_reverb",
    "parametric_tail",
)
_DATA_SOURCE_FLOAT_FIELDS = (
    *(
        field
        for mode in ("baked", "realtime", "hybrid")
        for field in (
            f"{mode}_wet_input_rms_ratio",
            f"{mode}_integrated_wet_input_rms_ratio",
            f"{mode}_full_peak",
        )
    ),
    "minimum_wet_input_rms_ratio",
    "minimum_wet_presence_fraction",
)
DATA_SOURCE_VALIDATION_PATTERN = re.compile(
    r"UERayTracingAudio validation data sources:"
    + "".join(
        rf"(?=[^\r\n]*\b{name}=(?P<{name}>[0-9]+))"
        for name in _DATA_SOURCE_INTEGER_FIELDS
    )
    + "".join(
        rf"(?=[^\r\n]*\b{name}=(?P<{name}>[0-9.eE+-]+))"
        for name in _DATA_SOURCE_FLOAT_FIELDS
    )
    + r"[^\r\n]*"
)
HARD_REALTIME_MARKER = "UERayTracingAudio hard realtime:"
HARD_REALTIME_PATTERN = re.compile(
    r"UERayTracingAudio hard realtime: "
    r"passed=(?P<passed>[01]) "
    r"callbacks=(?P<callbacks>[0-9]+) "
    r"callback_capacity_misses=(?P<callback_capacity_misses>[0-9]+) "
    r"convolution_prepare_drops=(?P<convolution_prepare_drops>[0-9]+)\."
)
INTERACTIVE_SMOKE_MARKER = "UERayTracingAudio interactive smoke:"
INTERACTIVE_SMOKE_PATTERN = re.compile(
    r"UERayTracingAudio interactive smoke:"
    r"(?=[^\r\n]*\bab_restart_count=(?P<ab_restart_count>[0-9]+)).*?"
    r"passed=(?P<passed>[01]).*?"
    r"moved_cm=(?P<moved_cm>[0-9.eE+-]+).*?"
    r"listener_camera_error_cm=(?P<listener_camera_error_cm>[0-9.eE+-]+).*?"
    r"origin_error_cm=(?P<origin_error_cm>[0-9.eE+-]+).*?"
    r"realtime=(?P<realtime>[01]).*?"
    r"baked=(?P<baked>[01]).*?"
    r"hybrid=(?P<hybrid>[01]).*?"
    r"rendered_ab=(?P<rendered_ab>[01]).*?"
    r"reference_ab=(?P<reference_ab>[01]).*?"
    r"fixed_view=(?P<fixed_view>[01]).*?"
    r"interactive_view=(?P<interactive_view>[01]).*?"
    r"audio_playing=(?P<audio_playing>[01]).*?"
    r"reference_playing=(?P<reference_playing>[01]).*?"
    r"ab_base_levels_matched=(?P<ab_base_levels_matched>[01]).*?"
    r"foreign_audio_playing=(?P<foreign_audio_playing>[0-9]+).*?"
    r"muted_foreign_audio=(?P<muted_foreign_audio>[0-9]+)"
)
EDITOR_VISIBLE_SCENE_MARKER = "UERayTracingAudioEditor validation scene ready:"
EDITOR_INTERACTIVE_READY_MARKER = "UERayTracingAudioEditor interactive validation ready:"
EDITOR_VALIDATION_MARKERS = EDITOR_MODULE_MARKERS + (EDITOR_VISIBLE_SCENE_MARKER,)
EDITOR_AB_ARTIFACTS_MARKER = "UERayTracingAudioEditor A/B artifacts ready:"
EDITOR_AB_ARTIFACTS_FAILURE_MARKER = "UERayTracingAudioEditor A/B artifacts failed:"
EDITOR_LISTENING_UI_MARKER = "UERayTracingAudioEditor listening acceptance ready:"
EDITOR_DIRECT_PRESETS = ("clear", "soft_occluded", "hard_occluded")
EDITOR_REFLECTION_ENVIRONMENTS = ("enclosed", "open_space", "near_wall")
EDITOR_VALIDATION_DISTANCES_CM = (100, 200, 400)
EDITOR_AIR_ABSORPTION_PROFILES = ("off", "default", "stress")
EDITOR_AIR_ABSORPTION_VECTORS = {
    "off": (0.0, 0.0, 0.0),
    "default": (0.0002, 0.0006, 0.0012),
    "stress": (0.01, 0.04, 0.12),
}
EDITOR_VISIBLE_SCENE_PATTERN = re.compile(
    r"UERayTracingAudioEditor validation scene ready: "
    r"source=1 listener=1 geometry=(?P<geometry>[0-9]+) "
    r"lighting=1 bake_ui=1 "
    r"direct_preset=(?P<direct_preset>[a-z_]+) "
    r"reflection_environment=(?P<reflection_environment>[a-z_]+) "
    r"source_listener_distance_cm=(?P<distance_cm>[0-9.eE+-]+) "
    r"air_absorption_profile=(?P<air_absorption_profile>[a-z_]+) "
    r"air_absorption_per_meter=\("
    r"(?P<air_low>[0-9.eE+-]+),"
    r"(?P<air_mid>[0-9.eE+-]+),"
    r"(?P<air_high>[0-9.eE+-]+)\)\."
)
EDITOR_AB_ARTIFACTS_PATTERN = re.compile(
    r"UERayTracingAudioEditor A/B artifacts ready: "
    r"hardware=(?P<hardware>[01]) auto_checks=(?P<auto_checks>[01]) "
    r"distinct=(?P<distinct>[01]) input=\"(?P<input>[^\"]+)\" "
    r"direct_preset=\"(?P<direct_preset>[^\"]+)\" "
    r"reflection_environment=\"(?P<reflection_environment>[^\"]+)\" "
    r"distance_cm=(?P<distance_cm>[0-9.eE+-]+) "
    r"visibility=(?P<visibility>[0-9.eE+-]+) "
    r"occlusion=(?P<occlusion>[0-9.eE+-]+) "
    r"distance_attenuation=(?P<distance_attenuation>[0-9.eE+-]+) "
    r"ir_asset=\"(?P<ir_asset>[^\"]+)\" imported_assets=(?P<imported_assets>[0-9]+) "
    r"reference=\"(?P<reference>[^\"]+)\" direct=\"(?P<direct>[^\"]+)\" "
    r"wet=\"(?P<wet>[^\"]+)\" full=\"(?P<full>[^\"]+)\" "
    r"manifest=\"(?P<manifest>[^\"]+)\".*?"
    r"direct_level=(?P<direct_level>[0-9.eE+-]+) "
    r"wet_level=(?P<wet_level>[0-9.eE+-]+).*?"
    r"direct_wet_difference=(?P<direct_wet_difference>[0-9.eE+-]+).*?"
    r"wet_stereo_difference=(?P<wet_stereo_difference>[0-9.eE+-]+) "
    r"directional_wet=(?P<directional_wet>[01]) "
    r"common_scale=(?P<common_scale>[0-9.eE+-]+)\."
)
VALIDATION_RESULT_PATTERN = re.compile(
    r"UERayTracingAudio validation result:.*?sources=(?P<sources>[0-9]+).*?"
    r"direct_batch_sources=(?P<direct_batch_sources>[0-9]+).*?"
    r"indirect_batch_sources=(?P<indirect_batch_sources>[0-9]+).*?"
    r"visibility=(?P<visibility>[0-9.]+).*?"
    r"valid_paths=(?P<valid_paths>[0-9]+).*?indirect_gain=(?P<indirect_gain>[0-9.]+)"
)
CPU_REFERENCE_MARKER = "UERayTracingAudio validation CPU reference:"
CPU_REFERENCE_PATTERN = re.compile(
    r"UERayTracingAudio validation CPU reference:.*?"
    r"hardware_paths=(?P<hardware_paths>[0-9]+).*?cpu_paths=(?P<cpu_paths>[0-9]+).*?"
    r"path_relative_delta=(?P<path_relative_delta>[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?).*?"
    r"hardware_gain=(?P<hardware_gain>[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?).*?"
    r"cpu_gain=(?P<cpu_gain>[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?).*?"
    r"gain_relative_delta=(?P<gain_relative_delta>[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)"
)
MAX_CPU_REFERENCE_PATH_RELATIVE_DELTA = 0.05
MAX_CPU_REFERENCE_GAIN_RELATIVE_DELTA = 0.10
BAKE_REPEATABILITY_MARKER = "UERayTracingAudio validation bake repeatability:"
BAKE_REPEATABILITY_PATTERN = re.compile(
    r"UERayTracingAudio validation bake repeatability:.*?passed=(?P<passed>[01]).*?"
    r"samples=(?P<samples>[0-9]+).*?duration=(?P<duration>[0-9]+(?:\.[0-9]+)?).*?"
    r"first_energy=(?P<first_energy>[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?).*?"
    r"second_energy=(?P<second_energy>[0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?).*?"
    r"energy_relative_delta=(?P<energy_relative_delta>[0-9]+(?:\.[0-9]+)?).*?"
    r"sample_relative_rms=(?P<sample_relative_rms>[0-9]+(?:\.[0-9]+)?)"
)
MAX_BAKE_RELATIVE_DELTA = 0.05
CSV_CAPTURE_PATTERN = re.compile(r"Capture Ended\. Writing CSV to file\s*:\s*(?P<path>[^\r\n]+)")
VALIDATION_AUDIO_OVERRIDES = (
    "-ini:Engine:[/Script/WindowsTargetPlatform.WindowsTargetSettings]:SpatializationPlugin=UE Ray Tracing Audio Spatialization",
    "-ini:Engine:[/Script/WindowsTargetPlatform.WindowsTargetSettings]:OcclusionPlugin=UE Ray Tracing Audio Occlusion",
    "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--game-seconds", type=float, default=DEFAULT_GAME_SECONDS)
    parser.add_argument("--source-count", type=int, default=4)
    parser.add_argument("--csv-profile", action="store_true")
    parser.add_argument("--bake-repeatability", action="store_true")
    parser.add_argument("--editor-ab-artifacts", action="store_true")
    parser.add_argument(
        "--interactive-runtime",
        action="store_true",
        help="Leave Editor ready for an interactive PIE session with player-driven Listener and runtime mode hotkeys.",
    )
    parser.add_argument(
        "--interactive-smoke",
        action="store_true",
        help="Require the Game phase to move the Pawn/Listener and exercise Realtime/Baked/Hybrid plus fixed/interactive views.",
    )
    parser.add_argument(
        "--editor-direct-preset",
        choices=EDITOR_DIRECT_PRESETS,
        default="clear",
    )
    parser.add_argument(
        "--editor-distance-cm",
        type=int,
        choices=EDITOR_VALIDATION_DISTANCES_CM,
        default=200,
    )
    parser.add_argument(
        "--editor-air-absorption-profile",
        choices=EDITOR_AIR_ABSORPTION_PROFILES,
        default="default",
    )
    parser.add_argument("--editor-ready-timeout-seconds", type=float, default=DEFAULT_EDITOR_READY_TIMEOUT_SECONDS)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def build_editor_path(engine_root: Path) -> Path:
    return engine_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"


def is_original_project_input_asset(asset_path: str) -> bool:
    return (
        asset_path.startswith("/Game/")
        and not asset_path.startswith("/Game/UERayTracingAudio/Validation")
    )


def _extract_exactly_one_strict_marker(
    log_text: str,
    *,
    marker: str,
    pattern: re.Pattern[str],
    label: str,
) -> re.Match[str]:
    marker_lines: list[str] = []
    for line in log_text.splitlines():
        marker_index = line.find(marker)
        if marker_index >= 0:
            marker_lines.append(line[marker_index:].strip())

    if len(marker_lines) != 1:
        raise RuntimeError(
            f"validation requires exactly one strict {label} marker "
            f"(found {len(marker_lines)})"
        )
    match = pattern.fullmatch(marker_lines[0])
    if match is None:
        raise RuntimeError(
            f"validation requires exactly one strict {label} marker "
            "(found one malformed marker)"
        )
    return match


def validate_editor_scene_ready(
    log_text: str,
    *,
    expected_direct_preset: str,
    expected_reflection_environment: str,
    expected_distance_cm: int,
    expected_air_absorption_profile: str,
) -> dict[str, object]:
    if expected_direct_preset not in EDITOR_DIRECT_PRESETS:
        raise ValueError(
            f"Unknown Editor direct preset {expected_direct_preset!r}; "
            f"expected one of {EDITOR_DIRECT_PRESETS}."
        )
    if expected_reflection_environment not in EDITOR_REFLECTION_ENVIRONMENTS:
        raise ValueError(
            "Unknown Editor reflection environment "
            f"{expected_reflection_environment!r}; "
            f"expected one of {EDITOR_REFLECTION_ENVIRONMENTS}."
        )
    if expected_distance_cm not in EDITOR_VALIDATION_DISTANCES_CM:
        raise ValueError(
            f"Unknown Editor validation distance {expected_distance_cm!r}; "
            f"expected one of {EDITOR_VALIDATION_DISTANCES_CM}."
        )
    if expected_air_absorption_profile not in EDITOR_AIR_ABSORPTION_PROFILES:
        raise ValueError(
            "Unknown Editor air absorption profile "
            f"{expected_air_absorption_profile!r}; "
            f"expected one of {EDITOR_AIR_ABSORPTION_PROFILES}."
        )

    marker_lines: list[str] = []
    for line in log_text.splitlines():
        marker_index = line.find(EDITOR_VISIBLE_SCENE_MARKER)
        if marker_index < 0:
            continue
        marker_lines.append(line[marker_index:].strip())

    if len(marker_lines) != 1:
        raise RuntimeError(
            "fixed Editor validation requires exactly one strict Editor "
            f"validation scene marker (found {len(marker_lines)})"
        )

    match = EDITOR_VISIBLE_SCENE_PATTERN.fullmatch(marker_lines[0])
    if not match:
        raise RuntimeError(
            "fixed Editor validation requires exactly one strict Editor "
            "validation scene marker (found 0 parseable)"
        )

    distance_cm = float(match.group("distance_cm"))
    profile = match.group("air_absorption_profile")
    air_absorption = tuple(
        float(match.group(group))
        for group in ("air_low", "air_mid", "air_high")
    )
    failures: list[str] = []
    if match.group("direct_preset") != expected_direct_preset:
        failures.append(
            "direct preset "
            f"({match.group('direct_preset')} != {expected_direct_preset})"
        )
    if (
        match.group("reflection_environment")
        != expected_reflection_environment
    ):
        failures.append(
            "reflection environment "
            f"({match.group('reflection_environment')} != "
            f"{expected_reflection_environment})"
        )
    if abs(distance_cm - expected_distance_cm) > 0.1:
        failures.append(
            f"effective distance ({distance_cm} != {expected_distance_cm})"
        )
    if profile != expected_air_absorption_profile:
        failures.append(
            "air absorption profile "
            f"({profile} != {expected_air_absorption_profile})"
        )
    expected_vector = EDITOR_AIR_ABSORPTION_VECTORS[
        expected_air_absorption_profile
    ]
    if any(
        abs(actual - expected) > 0.0000005
        for actual, expected in zip(air_absorption, expected_vector)
    ):
        failures.append(
            "air absorption vector "
            f"({air_absorption} != {expected_vector})"
        )
    if failures:
        raise RuntimeError(
            "Editor validation scene evidence failed: "
            + "; ".join(failures)
        )

    return {
        "geometry": int(match.group("geometry")),
        "direct_preset": match.group("direct_preset"),
        "reflection_environment": match.group(
            "reflection_environment"
        ),
        "distance_cm": distance_cm,
        "air_absorption_profile": profile,
        "air_absorption_per_meter": air_absorption,
    }


def validate_editor_ab_artifacts_marker(
    log_text: str,
    *,
    expected_direct_preset: str,
    expected_reflection_environment: str,
) -> dict[str, object]:
    match = _extract_exactly_one_strict_marker(
        log_text,
        marker=EDITOR_AB_ARTIFACTS_MARKER,
        pattern=EDITOR_AB_ARTIFACTS_PATTERN,
        label="Editor A/B artifact",
    )
    failures: list[str] = []
    if match.group("direct_preset") != expected_direct_preset:
        failures.append(
            "direct preset "
            f"({match.group('direct_preset')} != {expected_direct_preset})"
        )
    if (
        match.group("reflection_environment")
        != expected_reflection_environment
    ):
        failures.append(
            "reflection environment "
            f"({match.group('reflection_environment')} != "
            f"{expected_reflection_environment})"
        )
    if failures:
        raise RuntimeError(
            "Editor A/B artifact evidence failed: "
            + "; ".join(failures)
        )

    values: dict[str, object] = dict(match.groupdict())
    for name in (
        "distance_cm",
        "visibility",
        "occlusion",
        "distance_attenuation",
        "direct_level",
        "wet_level",
        "direct_wet_difference",
        "wet_stereo_difference",
        "common_scale",
    ):
        values[name] = float(match.group(name))
    for name in (
        "hardware",
        "auto_checks",
        "distinct",
        "imported_assets",
        "directional_wet",
    ):
        values[name] = int(match.group(name))
    return values


def validate_direct_sweep(log_text: str) -> dict[str, float | int]:
    marker_lines = []
    for line in log_text.splitlines():
        marker_index = line.find(DIRECT_SWEEP_MARKER)
        if marker_index >= 0:
            marker_lines.append(line[marker_index:].strip())

    if not marker_lines:
        raise RuntimeError("missing parseable hardware Direct sweep")
    if len(marker_lines) != 1:
        raise RuntimeError(
            "ambiguous hardware Direct sweep markers "
            f"(found {len(marker_lines)}, expected exactly one)"
        )

    match = DIRECT_SWEEP_PATTERN.fullmatch(marker_lines[0])
    if match is None:
        raise RuntimeError("missing parseable hardware Direct sweep")

    integer_fields = {
        "passed",
        "generations",
        "direct_dropouts",
        "restored",
        "hardware",
    }
    try:
        values: dict[str, float | int] = {
            key: int(value) if key in integer_fields else float(value)
            for key, value in match.groupdict().items()
        }
    except ValueError as error:
        raise RuntimeError("missing parseable hardware Direct sweep") from error

    failures: list[str] = []
    for field in (
        "distance_min",
        "distance_max",
        "visibility_min",
        "visibility_max",
        "gain_min",
        "gain_max",
        "max_gain_step",
    ):
        if not math.isfinite(float(values[field])):
            failures.append(f"finite {field}")

    if values["passed"] != 1:
        failures.append("passing Direct sweep")
    if values["generations"] < 8:
        failures.append("at least eight Direct generations")
    if (
        values["distance_min"] < 198.0
        or values["distance_max"] > 202.0
        or values["distance_min"] > values["distance_max"]
    ):
        failures.append("constant two-metre distance")
    if (
        values["visibility_min"] > 0.10
        or values["visibility_max"] < 0.90
    ):
        failures.append("Clear and Occluded visibility endpoints")
    if not (
        0.0 <= values["visibility_min"]
        <= values["visibility_max"]
        <= 1.0
    ):
        failures.append("ordered normalized visibility range")
    if not (
        0.0 < values["gain_min"]
        <= values["gain_max"]
        <= 1.0
    ):
        failures.append(
            "nonzero Soft Occlusion gain in an "
            "ordered normalized Direct gain range"
        )
    if not 0.0 <= values["max_gain_step"] <= 0.01:
        failures.append(
            "non-negative bounded per-sample gain step"
        )
    if values["direct_dropouts"] != 0:
        failures.append("zero Direct dropouts")
    if values["restored"] != 1:
        failures.append("restored Source state")
    if values["hardware"] != 1:
        failures.append("hardware provenance")

    direct_marker_offset = log_text.find(DIRECT_SWEEP_MARKER)
    for evidence_name, evidence_marker in (
        ("data-source Bake", DATA_SOURCE_BAKE_MARKER),
        ("hard-real-time result", HARD_REALTIME_MARKER),
        ("final data-source result", DATA_SOURCE_VALIDATION_MARKER),
    ):
        evidence_offset = log_text.find(evidence_marker)
        if (
            evidence_offset >= 0
            and direct_marker_offset >= evidence_offset
        ):
            failures.append(
                f"Direct sweep terminal before {evidence_name}"
            )
    if failures:
        raise RuntimeError("Direct sweep failed: " + ", ".join(failures))
    return values


def build_game_command(
    editor_exe: Path,
    project_path: Path,
    log_path: Path,
    source_count: int = 4,
    csv_profile: bool = False,
    bake_repeatability: bool = False,
    interactive_smoke: bool = False,
) -> list[str]:
    command = [
        str(editor_exe),
        str(project_path),
        "/Game/FirstPerson/Lvl_FirstPerson",
        "-game",
        "-NoSplash",
        "-dx12",
        "-raytracing",
        "-UERayTracingAudioValidationScenario",
        "-UERayTracingAudioValidationDirectSweep",
        f"-UERayTracingAudioValidationSourceCount={max(2, min(source_count, 32))}",
        *VALIDATION_AUDIO_OVERRIDES,
        f"-abslog={log_path}",
    ]
    if csv_profile:
        command.extend(
            (
                "-UERayTracingAudioPerformanceProfile",
                "-csvStartOnEvent=UERayTracingAudioPerformanceStart",
                "-csvCaptureOnEventFrameCount=600",
                "-csvCategories=Audio",
            )
        )
    if bake_repeatability:
        command.append("-UERayTracingAudioValidationBakeRepeatability")
    if interactive_smoke:
        command.extend(
            (
                "-UERayTracingAudioInteractiveValidation",
                "-UERayTracingAudioInteractiveSmoke",
            )
        )
    return command


def build_editor_command(
    editor_exe: Path,
    project_path: Path,
    log_path: Path,
    source_count: int = 4,
    bake_repeatability: bool = False,
    editor_ab_artifacts: bool = False,
    editor_direct_preset: str = "clear",
    editor_distance_cm: int = 200,
    editor_air_absorption_profile: str = "default",
    editor_reflection_bounces: int = 8,
    interactive_runtime: bool = False,
) -> list[str]:
    if editor_direct_preset not in EDITOR_DIRECT_PRESETS:
        raise ValueError(
            f"Unknown Editor direct preset {editor_direct_preset!r}; "
            f"expected one of {EDITOR_DIRECT_PRESETS}."
        )
    if editor_distance_cm not in EDITOR_VALIDATION_DISTANCES_CM:
        raise ValueError(
            f"Unknown Editor validation distance {editor_distance_cm!r}; "
            f"expected one of {EDITOR_VALIDATION_DISTANCES_CM}."
        )
    if editor_air_absorption_profile not in EDITOR_AIR_ABSORPTION_PROFILES:
        raise ValueError(
            "Unknown Editor air absorption profile "
            f"{editor_air_absorption_profile!r}; "
            f"expected one of {EDITOR_AIR_ABSORPTION_PROFILES}."
        )
    command = [
        str(editor_exe),
        str(project_path),
        "-NoSplash",
        "-dx12",
        "-raytracing",
        "-UERayTracingAudioValidationScenario",
        f"-UERayTracingAudioValidationDirectPreset={editor_direct_preset}",
        f"-UERayTracingAudioValidationDistanceCm={editor_distance_cm:g}",
        "-UERayTracingAudioValidationAirAbsorptionProfile="
        f"{editor_air_absorption_profile}",
        f"-UERayTracingAudioValidationReflectionBounces={max(1, min(editor_reflection_bounces, 64))}",
        f"-UERayTracingAudioValidationSourceCount={max(2, min(source_count, 32))}",
        *VALIDATION_AUDIO_OVERRIDES,
        f"-abslog={log_path}",
    ]
    if bake_repeatability:
        command.append("-UERayTracingAudioValidationBakeRepeatability")
    if editor_ab_artifacts:
        command.append("-UERayTracingAudioValidationEditorBake")
    if interactive_runtime:
        command.append("-UERayTracingAudioInteractiveValidation")
    return command


def editor_required_markers(
    editor_ab_artifacts: bool,
    interactive_runtime: bool = False,
) -> tuple[str, ...]:
    if interactive_runtime:
        return (EDITOR_INTERACTIVE_READY_MARKER,)
    markers = (EDITOR_VISIBLE_SCENE_MARKER,)
    if editor_ab_artifacts:
        markers += (EDITOR_AB_ARTIFACTS_MARKER, EDITOR_LISTENING_UI_MARKER)
    return markers


def ensure_paths(editor_exe: Path, project_path: Path) -> None:
    if not editor_exe.exists():
        raise RuntimeError(f"UnrealEditor.exe does not exist: {editor_exe}")
    if project_path.suffix.lower() != ".uproject":
        raise RuntimeError(f"Project path must point to a .uproject file: {project_path}")
    if not project_path.exists():
        raise RuntimeError(f"Project file does not exist: {project_path}")


def snapshot_files(root: Path) -> dict[Path, tuple[int, int]]:
    if not root.is_dir():
        return {}
    return {
        path: (path.stat().st_mtime_ns, path.stat().st_size)
        for path in root.rglob("*")
        if path.is_file()
    }


def changed_files(root: Path, before: dict[Path, tuple[int, int]]) -> list[Path]:
    if not root.is_dir():
        return []
    changed: list[Path] = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        signature = (path.stat().st_mtime_ns, path.stat().st_size)
        if before.get(path) != signature:
            changed.append(path)
    return changed


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def validation_log_path(project_path: Path, phase: str) -> Path:
    timestamp = time.time_ns()
    return project_path.parent / "Saved" / "Logs" / f"UERayTracingAudioValidation-{phase}-{timestamp}.log"


def assert_runtime_log_evidence(
    project_path: Path,
    log_snapshot: dict[Path, tuple[int, int]],
    crash_snapshot: dict[Path, tuple[int, int]],
    phase: str,
    required_markers: tuple[str, ...] = (),
    phase_log_path: Path | None = None,
) -> str:
    saved_root = project_path.parent / "Saved"
    changed_logs = [phase_log_path] if phase_log_path is not None and phase_log_path.is_file() else []
    if phase_log_path is None:
        changed_logs = changed_files(saved_root / "Logs", log_snapshot)
    new_crash_files = changed_files(saved_root / "Crashes", crash_snapshot)

    failures: list[str] = []
    combined_log = ""
    for log_path in changed_logs:
        text = read_text(log_path)
        combined_log += text
        matched = [pattern for pattern in FAILURE_PATTERNS if pattern in text]
        if matched:
            failures.append(f"{log_path}: {', '.join(matched)}")

    if new_crash_files:
        failures.append("new crash artifacts: " + ", ".join(str(path) for path in new_crash_files))

    if not changed_logs:
        failures.append("no new or changed runtime log was produced")

    missing_markers = [marker for marker in required_markers if marker not in combined_log]
    if missing_markers:
        failures.append("missing module markers: " + ", ".join(missing_markers))

    if failures:
        raise RuntimeError(f"{phase} runtime validation failed: " + " | ".join(failures))
    return combined_log


def print_audio_path_summary(
    log_text: str,
    phase: str,
    require_validation: bool = False,
    require_data_sources: bool = False,
    require_bake_repeatability: bool = False,
    require_interactive_smoke: bool = False,
) -> None:
    direct_sweep_values = (
        validate_direct_sweep(log_text)
        if require_data_sources
        else None
    )
    direct = [marker for marker in AUDIO_DIRECT_PATH_MARKERS if marker in log_text]
    indirect = [marker for marker in AUDIO_INDIRECT_PATH_MARKERS if marker in log_text]
    result_match = VALIDATION_RESULT_PATTERN.search(log_text)
    cpu_reference_match = CPU_REFERENCE_PATTERN.search(log_text)
    primary_input_match = PRIMARY_INPUT_PATTERN.search(log_text)
    audio_pipeline_match = AUDIO_PIPELINE_PATTERN.search(log_text)
    if require_data_sources:
        hard_realtime_match = _extract_exactly_one_strict_marker(
            log_text,
            marker=HARD_REALTIME_MARKER,
            pattern=HARD_REALTIME_PATTERN,
            label="hard-real-time",
        )
        data_source_match = _extract_exactly_one_strict_marker(
            log_text,
            marker=DATA_SOURCE_VALIDATION_MARKER,
            pattern=DATA_SOURCE_VALIDATION_PATTERN,
            label="data-source",
        )
    else:
        data_source_match = DATA_SOURCE_VALIDATION_PATTERN.search(log_text)
        hard_realtime_match = HARD_REALTIME_PATTERN.search(log_text)
    bake_repeatability_match = BAKE_REPEATABILITY_PATTERN.search(log_text)
    interactive_smoke_match = INTERACTIVE_SMOKE_PATTERN.search(log_text)
    has_result = result_match is not None
    if direct or indirect:
        print(f"{phase} audio path evidence:")
        for marker in direct + indirect:
            print(f"  - {marker}")
        if has_result:
            print(
                f"  - {VALIDATION_RESULT_MARKER} "
                f"sources={result_match.group('sources')} "
                f"direct_batch_sources={result_match.group('direct_batch_sources')} "
                f"indirect_batch_sources={result_match.group('indirect_batch_sources')} "
                f"visibility={result_match.group('visibility')} "
                f"valid_paths={result_match.group('valid_paths')} "
                f"indirect_gain={result_match.group('indirect_gain')}"
            )
        if cpu_reference_match:
            print(
                f"  - {CPU_REFERENCE_MARKER} "
                f"hardware_paths={cpu_reference_match.group('hardware_paths')} "
                f"cpu_paths={cpu_reference_match.group('cpu_paths')} "
                f"path_relative_delta={cpu_reference_match.group('path_relative_delta')} "
                f"hardware_gain={cpu_reference_match.group('hardware_gain')} "
                f"cpu_gain={cpu_reference_match.group('cpu_gain')} "
                f"gain_relative_delta={cpu_reference_match.group('gain_relative_delta')}"
            )
        if direct_sweep_values is not None:
            print(
                f"  - {DIRECT_SWEEP_MARKER} "
                f"passed={direct_sweep_values['passed']} "
                f"generations={direct_sweep_values['generations']} "
                f"distance_min_cm={direct_sweep_values['distance_min']:.3f} "
                f"distance_max_cm={direct_sweep_values['distance_max']:.3f} "
                f"visibility_min={direct_sweep_values['visibility_min']:.6f} "
                f"visibility_max={direct_sweep_values['visibility_max']:.6f} "
                f"gain_min={direct_sweep_values['gain_min']:.6f} "
                f"gain_max={direct_sweep_values['gain_max']:.6f} "
                f"max_gain_step={direct_sweep_values['max_gain_step']:.8f} "
                f"direct_dropouts={direct_sweep_values['direct_dropouts']} "
                f"restored={direct_sweep_values['restored']} "
                f"hardware={direct_sweep_values['hardware']}"
            )
        if data_source_match:
            print(
                f"  - {DATA_SOURCE_VALIDATION_MARKER} "
                f"passed={data_source_match.group('passed')} "
                f"baked=input:{data_source_match.group('baked_input_non_silent')},"
                f"wet:{data_source_match.group('baked_non_silent')}/"
                f"{data_source_match.group('baked_buffers')},"
                f"present:{data_source_match.group('baked_wet_present')}/"
                f"{data_source_match.group('baked_rms_measured')},"
                f"silent_run:{data_source_match.group('baked_max_silent_run')},"
                f"integrated_ratio:"
                f"{data_source_match.group('baked_integrated_wet_input_rms_ratio')},"
                f"max_ratio:{data_source_match.group('baked_wet_input_rms_ratio')},"
                f"full_peak:{data_source_match.group('baked_full_peak')},"
                f"over_unit:{data_source_match.group('baked_over_unit')},"
                f"threshold_blocks:{data_source_match.group('baked_audible_wet')} "
                f"realtime=input:{data_source_match.group('realtime_input_non_silent')},"
                f"wet:{data_source_match.group('realtime_non_silent')}/"
                f"{data_source_match.group('realtime_buffers')},"
                f"present:{data_source_match.group('realtime_wet_present')}/"
                f"{data_source_match.group('realtime_rms_measured')},"
                f"silent_run:{data_source_match.group('realtime_max_silent_run')},"
                f"integrated_ratio:"
                f"{data_source_match.group('realtime_integrated_wet_input_rms_ratio')},"
                f"max_ratio:{data_source_match.group('realtime_wet_input_rms_ratio')},"
                f"full_peak:{data_source_match.group('realtime_full_peak')},"
                f"over_unit:{data_source_match.group('realtime_over_unit')},"
                f"threshold_blocks:{data_source_match.group('realtime_audible_wet')} "
                f"hybrid=input:{data_source_match.group('hybrid_input_non_silent')},"
                f"wet:{data_source_match.group('hybrid_non_silent')}/"
                f"{data_source_match.group('hybrid_buffers')},"
                f"present:{data_source_match.group('hybrid_wet_present')}/"
                f"{data_source_match.group('hybrid_rms_measured')},"
                f"silent_run:{data_source_match.group('hybrid_max_silent_run')},"
                f"integrated_ratio:"
                f"{data_source_match.group('hybrid_integrated_wet_input_rms_ratio')},"
                f"max_ratio:{data_source_match.group('hybrid_wet_input_rms_ratio')},"
                f"full_peak:{data_source_match.group('hybrid_full_peak')},"
                f"over_unit:{data_source_match.group('hybrid_over_unit')},"
                f"threshold_blocks:{data_source_match.group('hybrid_audible_wet')} "
                f"non_finite={data_source_match.group('non_finite')} "
                f"kernels={data_source_match.group('baked_kernels')}/"
                f"{data_source_match.group('realtime_kernels')}/"
                f"{data_source_match.group('hybrid_kernels')}"
            )
        if hard_realtime_match:
            print(
                f"  - {HARD_REALTIME_MARKER} "
                f"passed={hard_realtime_match.group('passed')} "
                f"callbacks={hard_realtime_match.group('callbacks')} "
                "callback_capacity_misses="
                f"{hard_realtime_match.group('callback_capacity_misses')} "
                "convolution_prepare_drops="
                f"{hard_realtime_match.group('convolution_prepare_drops')}"
            )
        if primary_input_match:
            print(
                f"  - {PRIMARY_INPUT_MARKER} "
                f"real_soundwave={primary_input_match.group('real_soundwave')} "
                f"asset={primary_input_match.group('asset')}"
            )
        if audio_pipeline_match:
            print(
                f"  - {AUDIO_PIPELINE_MARKER} "
                f"max_actual_volume={audio_pipeline_match.group('max_actual_volume')} "
                f"pre_distance_non_silent={audio_pipeline_match.group('pre_distance_non_silent')}/"
                f"{audio_pipeline_match.group('pre_distance_buffers')}"
            )
        if bake_repeatability_match:
            print(
                f"  - {BAKE_REPEATABILITY_MARKER} "
                f"passed={bake_repeatability_match.group('passed')} "
                f"samples={bake_repeatability_match.group('samples')} "
                f"duration={bake_repeatability_match.group('duration')} "
                f"energy_relative_delta={bake_repeatability_match.group('energy_relative_delta')} "
                f"sample_relative_rms={bake_repeatability_match.group('sample_relative_rms')}"
            )
        if interactive_smoke_match:
            print(
                f"  - {INTERACTIVE_SMOKE_MARKER} "
                f"passed={interactive_smoke_match.group('passed')} "
                f"moved_cm={interactive_smoke_match.group('moved_cm')} "
                f"listener_camera_error_cm={interactive_smoke_match.group('listener_camera_error_cm')} "
                f"origin_error_cm={interactive_smoke_match.group('origin_error_cm')} "
                f"modes={interactive_smoke_match.group('realtime')}/"
                f"{interactive_smoke_match.group('baked')}/"
                f"{interactive_smoke_match.group('hybrid')} "
                f"ab={interactive_smoke_match.group('rendered_ab')}/"
                f"{interactive_smoke_match.group('reference_ab')} "
                f"views={interactive_smoke_match.group('fixed_view')}/"
                f"{interactive_smoke_match.group('interactive_view')} "
                f"ab_base_levels_matched="
                f"{interactive_smoke_match.group('ab_base_levels_matched')} "
                f"ab_restart_count="
                f"{interactive_smoke_match.group('ab_restart_count')} "
                f"foreign_audio_playing="
                f"{interactive_smoke_match.group('foreign_audio_playing')} "
                f"muted_foreign_audio="
                f"{interactive_smoke_match.group('muted_foreign_audio')}"
            )
    else:
        print(
            f"WARNING: {phase} loaded the plugin but produced no direct/indirect path evidence. "
            "The current map may not contain an active Listener/Source/Geometry validation setup."
        )
    if require_validation:
        missing: list[str] = []
        if AUDIO_DIRECT_PATH_MARKERS[0] not in log_text:
            missing.append("direct hardware submission marker")
        if AUDIO_INDIRECT_PATH_MARKERS[0] not in log_text:
            missing.append("indirect hardware submission marker")
        if not has_result:
            missing.append(VALIDATION_RESULT_MARKER)
        else:
            sources = int(result_match.group("sources"))
            direct_batch_sources = int(result_match.group("direct_batch_sources"))
            indirect_batch_sources = int(result_match.group("indirect_batch_sources"))
            visibility = float(result_match.group("visibility"))
            valid_paths = int(result_match.group("valid_paths"))
            indirect_gain = float(result_match.group("indirect_gain"))
            if sources < 2:
                missing.append(f"multi-source scenario (sources={sources})")
            if direct_batch_sources < 2:
                missing.append(f"direct RHI batch (sources={direct_batch_sources})")
            if indirect_batch_sources < 2:
                missing.append(f"indirect RHI batch (sources={indirect_batch_sources})")
            if visibility >= 0.99:
                missing.append(f"occlusion response (visibility={visibility:.4f})")
            if valid_paths <= 0:
                missing.append(f"positive indirect valid_paths ({valid_paths})")
            if indirect_gain <= 0.0:
                missing.append(f"positive indirect_gain ({indirect_gain:.6f})")
        if cpu_reference_match is None:
            missing.append(CPU_REFERENCE_MARKER)
        else:
            path_relative_delta = float(cpu_reference_match.group("path_relative_delta"))
            gain_relative_delta = float(cpu_reference_match.group("gain_relative_delta"))
            if path_relative_delta > MAX_CPU_REFERENCE_PATH_RELATIVE_DELTA:
                missing.append(
                    "CPU reference path tolerance "
                    f"({path_relative_delta:.4f} > {MAX_CPU_REFERENCE_PATH_RELATIVE_DELTA:.4f})"
                )
            if gain_relative_delta > MAX_CPU_REFERENCE_GAIN_RELATIVE_DELTA:
                missing.append(
                    "CPU reference gain tolerance "
                    f"({gain_relative_delta:.4f} > {MAX_CPU_REFERENCE_GAIN_RELATIVE_DELTA:.4f})"
                )
        if missing:
            raise RuntimeError(f"{phase} acoustic validation missing: " + ", ".join(missing))
    if require_data_sources:
        data_source_failures: list[str] = []
        if hard_realtime_match is None:
            data_source_failures.append(
                "parseable hard-real-time runtime counters"
            )
        else:
            hard_realtime_passed = int(
                hard_realtime_match.group("passed")
            )
            callback_count = int(
                hard_realtime_match.group("callbacks")
            )
            callback_capacity_misses = int(
                hard_realtime_match.group(
                    "callback_capacity_misses"
                )
            )
            convolution_prepare_drops = int(
                hard_realtime_match.group(
                    "convolution_prepare_drops"
                )
            )
            if hard_realtime_passed != 1:
                data_source_failures.append(
                    "successful hard-real-time runtime gate"
                )
            if callback_count <= 0:
                data_source_failures.append(
                    "observed audio callbacks"
                )
            if callback_capacity_misses != 0:
                data_source_failures.append(
                    "zero callback capacity misses "
                    f"({callback_capacity_misses} != 0)"
                )
            if convolution_prepare_drops != 0:
                data_source_failures.append(
                    "zero convolution prepare drops "
                    f"({convolution_prepare_drops} != 0)"
                )
        if DATA_SOURCE_VALIDATION_MARKER not in log_text:
            data_source_failures.append(DATA_SOURCE_VALIDATION_MARKER)
        elif data_source_match is None:
            data_source_failures.append("parseable Realtime/Baked/Hybrid result")
        else:
            values = {
                name: int(data_source_match.group(name))
                for name in (
                    "passed",
                    "baked_buffers",
                    "baked_input_non_silent",
                    "baked_non_silent",
                    "baked_rms_measured",
                    "baked_wet_present",
                    "baked_max_silent_run",
                    "baked_audible_wet",
                    "baked_max_inaudible_run",
                    "baked_over_unit",
                    "realtime_buffers",
                    "realtime_input_non_silent",
                    "realtime_non_silent",
                    "realtime_rms_measured",
                    "realtime_wet_present",
                    "realtime_max_silent_run",
                    "realtime_audible_wet",
                    "realtime_max_inaudible_run",
                    "realtime_over_unit",
                    "hybrid_buffers",
                    "hybrid_input_non_silent",
                    "hybrid_non_silent",
                    "hybrid_rms_measured",
                    "hybrid_wet_present",
                    "hybrid_max_silent_run",
                    "hybrid_audible_wet",
                    "hybrid_max_inaudible_run",
                    "hybrid_over_unit",
                    "non_finite",
                    "stereo_ir",
                    "baked_kernels",
                    "realtime_kernels",
                    "hybrid_kernels",
                    "audio_playing",
                    "interactive_hybrid_reverb",
                    "parametric_tail",
                )
            }
            if values["passed"] != 1:
                data_source_failures.append("successful Realtime/Baked/Hybrid validation")
            for mode in ("baked", "realtime", "hybrid"):
                if values[f"{mode}_buffers"] <= 0:
                    data_source_failures.append(f"{mode} audio-thread buffers")
                if values[f"{mode}_input_non_silent"] <= 0:
                    data_source_failures.append(f"{mode} non-silent source input")
                if values[f"{mode}_non_silent"] <= 0:
                    data_source_failures.append(f"{mode} non-silent wet audio")
                measured_buffers = values[f"{mode}_rms_measured"]
                wet_present_buffers = values[f"{mode}_wet_present"]
                maximum_silent_run = values[f"{mode}_max_silent_run"]
                if measured_buffers < MIN_CONTINUOUS_WET_BUFFER_COUNT:
                    data_source_failures.append(
                        f"{mode} continuous wet observation "
                        f"({measured_buffers} < "
                        f"{MIN_CONTINUOUS_WET_BUFFER_COUNT} buffers)"
                    )
                elif wet_present_buffers * 5 < measured_buffers * 4:
                    data_source_failures.append(
                        f"{mode} sustained wet presence "
                        f"({wet_present_buffers}/{measured_buffers} < "
                        f"{MIN_WET_PRESENCE_FRACTION:.2f})"
                    )
                if (
                    measured_buffers > 0
                    and maximum_silent_run * 5 > measured_buffers
                ):
                    data_source_failures.append(
                        f"{mode} maximum silent wet run "
                        f"({maximum_silent_run}/{measured_buffers} > "
                        f"{MAX_SILENT_WET_RUN_FRACTION:.2f})"
                    )
                integrated_wet_input_rms_ratio = float(
                    data_source_match.group(
                        f"{mode}_integrated_wet_input_rms_ratio"
                    )
                )
                if (
                    integrated_wet_input_rms_ratio
                    < MIN_INTEGRATED_WET_INPUT_RMS_RATIO
                ):
                    data_source_failures.append(
                        f"{mode} integrated wet/input RMS ratio "
                        f"({integrated_wet_input_rms_ratio:.6f} < "
                        f"{MIN_INTEGRATED_WET_INPUT_RMS_RATIO:.6f})"
                    )
                full_peak = float(
                    data_source_match.group(f"{mode}_full_peak")
                )
                if full_peak > 1.0 + 1.0e-6:
                    data_source_failures.append(
                        f"{mode} Full peak without clipping "
                        f"({full_peak:.6f} > 1.000000)"
                    )
                if values[f"{mode}_over_unit"] != 0:
                    data_source_failures.append(
                        f"{mode} over-unit Full samples "
                        f"({values[f'{mode}_over_unit']} != 0)"
                    )
            if values["non_finite"] != 0:
                data_source_failures.append(
                    f"finite wet output (non_finite={values['non_finite']})"
                )
            if values["stereo_ir"] != 1:
                data_source_failures.append("stereo hardware Bake IR")
            expected_kernels = {
                "baked_kernels": 2,
                "realtime_kernels": 2,
                "hybrid_kernels": 4,
            }
            for name, expected in expected_kernels.items():
                if values[name] != expected:
                    data_source_failures.append(
                        f"{name}={expected} (observed {values[name]})"
                    )
            if values["audio_playing"] != 1:
                data_source_failures.append("active SoundWave playback")
            if values["interactive_hybrid_reverb"] != 1:
                data_source_failures.append(
                    "interactive HybridReverb mode restored"
                )
            if values["parametric_tail"] != 1:
                data_source_failures.append("active parametric reverb tail")
        if primary_input_match is None:
            data_source_failures.append("parseable real project SoundWave input")
        elif int(primary_input_match.group("real_soundwave")) != 1:
            data_source_failures.append("real project SoundWave input")
        elif not is_original_project_input_asset(primary_input_match.group("asset")):
            data_source_failures.append(
                f"original project input asset ({primary_input_match.group('asset')})"
            )
        if audio_pipeline_match is None:
            data_source_failures.append("parseable pre-distance audio pipeline")
        else:
            max_actual_volume = float(audio_pipeline_match.group("max_actual_volume"))
            pre_distance_buffers = int(audio_pipeline_match.group("pre_distance_buffers"))
            pre_distance_non_silent = int(audio_pipeline_match.group("pre_distance_non_silent"))
            if max_actual_volume <= 0.0:
                data_source_failures.append(
                    f"positive source gain before mixing (max_actual_volume={max_actual_volume:.6f})"
                )
            if pre_distance_buffers <= 0:
                data_source_failures.append("pre-distance source buffers")
            if pre_distance_non_silent <= 0:
                data_source_failures.append("non-silent pre-distance source audio")
        if data_source_failures:
            raise RuntimeError(
                f"{phase} runtime IR data-source validation missing: "
                + ", ".join(data_source_failures)
            )
    if require_bake_repeatability:
        bake_failures: list[str] = []
        if BAKE_REPEATABILITY_MARKER not in log_text:
            bake_failures.append(BAKE_REPEATABILITY_MARKER)
        elif bake_repeatability_match is None:
            bake_failures.append("parseable bake repeatability result")
        else:
            passed = int(bake_repeatability_match.group("passed"))
            samples = int(bake_repeatability_match.group("samples"))
            duration = float(bake_repeatability_match.group("duration"))
            first_energy = float(bake_repeatability_match.group("first_energy"))
            second_energy = float(bake_repeatability_match.group("second_energy"))
            energy_relative_delta = float(bake_repeatability_match.group("energy_relative_delta"))
            sample_relative_rms = float(bake_repeatability_match.group("sample_relative_rms"))
            if passed != 1:
                bake_failures.append("successful hardware bake comparison")
            if samples <= 0 or duration <= 0.0:
                bake_failures.append(f"non-empty bake output (samples={samples}, duration={duration:.6f})")
            if first_energy <= 0.0 or second_energy <= 0.0:
                bake_failures.append(
                    f"positive bake energy (first={first_energy:.9g}, second={second_energy:.9g})"
                )
            if energy_relative_delta > MAX_BAKE_RELATIVE_DELTA:
                bake_failures.append(
                    f"bake energy tolerance ({energy_relative_delta:.6f} > {MAX_BAKE_RELATIVE_DELTA:.6f})"
                )
            if sample_relative_rms > MAX_BAKE_RELATIVE_DELTA:
                bake_failures.append(
                    f"bake sample tolerance ({sample_relative_rms:.6f} > {MAX_BAKE_RELATIVE_DELTA:.6f})"
                )
        if bake_failures:
            raise RuntimeError(f"{phase} bake repeatability validation missing: " + ", ".join(bake_failures))
    if require_interactive_smoke:
        smoke_failures: list[str] = []
        if INTERACTIVE_SMOKE_MARKER not in log_text:
            smoke_failures.append(INTERACTIVE_SMOKE_MARKER)
        elif interactive_smoke_match is None:
            smoke_failures.append("parseable interactive smoke result")
        else:
            values = {
                name: int(interactive_smoke_match.group(name))
                for name in (
                    "passed",
                    "realtime",
                    "baked",
                    "hybrid",
                    "rendered_ab",
                    "reference_ab",
                    "fixed_view",
                    "interactive_view",
                    "audio_playing",
                    "reference_playing",
                    "ab_base_levels_matched",
                )
            }
            foreign_audio_playing = int(
                interactive_smoke_match.group("foreign_audio_playing")
            )
            ab_restart_count = int(
                interactive_smoke_match.group("ab_restart_count")
            )
            moved_cm = float(interactive_smoke_match.group("moved_cm"))
            listener_error_cm = float(
                interactive_smoke_match.group("listener_camera_error_cm")
            )
            origin_error_cm = float(interactive_smoke_match.group("origin_error_cm"))
            if values["passed"] != 1:
                smoke_failures.append("successful interactive smoke")
            if moved_cm < 25.0:
                smoke_failures.append(f"Pawn movement >=25 cm (observed {moved_cm:.3f})")
            if listener_error_cm > 1.0:
                smoke_failures.append(
                    f"Listener/camera error <=1 cm (observed {listener_error_cm:.3f})"
                )
            if origin_error_cm > 1.0:
                smoke_failures.append(
                    f"baked-origin return error <=1 cm (observed {origin_error_cm:.3f})"
                )
            for name in (
                "realtime",
                "baked",
                "hybrid",
                "rendered_ab",
                "reference_ab",
                "fixed_view",
                "interactive_view",
                "audio_playing",
                "reference_playing",
                "ab_base_levels_matched",
            ):
                if values[name] != 1:
                    smoke_failures.append(name)
            if foreign_audio_playing != 0:
                smoke_failures.append(
                    "no foreign audio playing "
                    f"(observed {foreign_audio_playing})"
                )
            if ab_restart_count != 0:
                smoke_failures.append(
                    "zero A/B playback restarts "
                    f"(observed {ab_restart_count})"
                )
        if smoke_failures:
            raise RuntimeError(
                f"{phase} interactive runtime smoke missing: "
                + ", ".join(smoke_failures)
            )

def wait_for_editor_ready(
    process: subprocess.Popen[bytes | str],
    log_path: Path,
    timeout_seconds: float,
    required_markers: tuple[str, ...] = (),
) -> None:
    deadline = time.monotonic() + timeout_seconds

    while time.monotonic() < deadline:
        exit_code = process.poll()
        if exit_code is not None:
            raise RuntimeError(f"Editor exited before initialization completed with exit code {exit_code}.")

        log_text = read_text(log_path)
        if EDITOR_AB_ARTIFACTS_FAILURE_MARKER in log_text:
            failure_line = next(
                line.strip()
                for line in log_text.splitlines()
                if EDITOR_AB_ARTIFACTS_FAILURE_MARKER in line
            )
            raise RuntimeError(failure_line)
        b_artifact_marker_complete = True
        if EDITOR_AB_ARTIFACTS_MARKER in required_markers:
            artifact_marker_lines = [
                line[
                    line.find(EDITOR_AB_ARTIFACTS_MARKER)
                :].strip()
                for line in log_text.splitlines()
                if EDITOR_AB_ARTIFACTS_MARKER in line
            ]
            if len(artifact_marker_lines) > 1:
                raise RuntimeError(
                    "Editor emitted more than one A/B artifact marker "
                    "before initialization completed."
                )
            b_artifact_marker_complete = (
                len(artifact_marker_lines) == 1
                and EDITOR_AB_ARTIFACTS_PATTERN.fullmatch(
                    artifact_marker_lines[0]
                )
                is not None
            )
        if (
            EDITOR_READY_PATTERN in log_text
            and all(marker in log_text for marker in required_markers)
            and b_artifact_marker_complete
        ):
            return
        time.sleep(1.0)

    log_text = read_text(log_path)
    missing_markers = [
        marker
        for marker in (EDITOR_READY_PATTERN, *required_markers)
        if marker not in log_text
    ]
    if (
        EDITOR_AB_ARTIFACTS_MARKER in required_markers
        and not any(
            EDITOR_AB_ARTIFACTS_PATTERN.fullmatch(
                line[line.find(EDITOR_AB_ARTIFACTS_MARKER):].strip()
            )
            for line in log_text.splitlines()
            if EDITOR_AB_ARTIFACTS_MARKER in line
        )
    ):
        missing_markers.append(
            "complete strict Editor A/B artifact marker"
        )
    raise RuntimeError(
        f"Editor did not report initialization completion within {timeout_seconds:.1f} seconds; "
        f"missing markers: {missing_markers}."
    )


def stop_process(process: subprocess.Popen[bytes | str], timeout_seconds: float = 10.0) -> None:
    if process.poll() is not None:
        return

    process.terminate()
    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=timeout_seconds)


def start_game_then_kill(
    editor_exe: Path,
    project_path: Path,
    game_seconds: float,
    source_count: int,
    csv_profile: bool,
    bake_repeatability: bool,
    interactive_smoke: bool,
    dry_run: bool,
) -> None:
    phase_log_path = validation_log_path(project_path, "Game")
    phase_log_path.parent.mkdir(parents=True, exist_ok=True)
    command = build_game_command(
        editor_exe,
        project_path,
        phase_log_path,
        source_count=source_count,
        csv_profile=csv_profile,
        bake_repeatability=bake_repeatability,
        interactive_smoke=interactive_smoke,
    )
    print("\n=== Launch Game Validation ===")
    print(" ".join(command))
    if dry_run:
        return

    saved_root = project_path.parent / "Saved"
    log_snapshot = snapshot_files(saved_root / "Logs")
    crash_snapshot = snapshot_files(saved_root / "Crashes")
    process = subprocess.Popen(
        command,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        close_fds=True,
    )
    try:
        time.sleep(game_seconds)
        exit_code = process.poll()
        if exit_code is not None and exit_code != 0:
            raise RuntimeError(f"Game validation exited early with exit code {exit_code}.")
    finally:
        stop_process(process)
    game_log = assert_runtime_log_evidence(
        project_path,
        log_snapshot,
        crash_snapshot,
        "Game",
        RUNTIME_MODULE_MARKERS
        + (
            VISIBLE_SCENE_MARKER,
            DIRECT_SWEEP_MARKER,
            DATA_SOURCE_BAKE_MARKER,
            PRIMARY_INPUT_MARKER,
            AUDIO_PIPELINE_MARKER,
            DATA_SOURCE_VALIDATION_MARKER,
        ),
        phase_log_path,
    )
    print_audio_path_summary(
        game_log,
        "Game",
        require_validation=True,
        require_data_sources=True,
        require_bake_repeatability=bake_repeatability,
        require_interactive_smoke=interactive_smoke,
    )
    if csv_profile:
        capture_match = CSV_CAPTURE_PATTERN.search(game_log)
        if not capture_match:
            raise RuntimeError("Game performance validation did not finish a CSV capture.")
        print(f"Game CSV profile: {capture_match.group('path').strip()}")


def start_editor_for_user(
    editor_exe: Path,
    project_path: Path,
    editor_ready_timeout_seconds: float,
    source_count: int,
    bake_repeatability: bool,
    editor_ab_artifacts: bool,
    editor_direct_preset: str,
    editor_distance_cm: int,
    editor_air_absorption_profile: str,
    interactive_runtime: bool,
    dry_run: bool,
) -> None:
    phase_log_path = validation_log_path(project_path, "Editor")
    phase_log_path.parent.mkdir(parents=True, exist_ok=True)
    command = build_editor_command(
        editor_exe,
        project_path,
        phase_log_path,
        source_count=source_count,
        bake_repeatability=bake_repeatability,
        editor_ab_artifacts=editor_ab_artifacts,
        editor_direct_preset=editor_direct_preset,
        editor_distance_cm=editor_distance_cm,
        editor_air_absorption_profile=editor_air_absorption_profile,
        interactive_runtime=interactive_runtime,
    )
    print("\n=== Launch Editor For User ===")
    print(" ".join(command))
    if dry_run:
        return

    creation_flags = 0
    if sys.platform == "win32":
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS

    saved_root = project_path.parent / "Saved"
    log_snapshot = snapshot_files(saved_root / "Logs")
    crash_snapshot = snapshot_files(saved_root / "Crashes")
    process = subprocess.Popen(
        command,
        creationflags=creation_flags,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        close_fds=True,
    )
    required_editor_markers = editor_required_markers(
        editor_ab_artifacts,
        interactive_runtime,
    )
    wait_for_editor_ready(
        process,
        phase_log_path,
        editor_ready_timeout_seconds,
        required_markers=required_editor_markers,
    )
    editor_log = assert_runtime_log_evidence(
        project_path,
        log_snapshot,
        crash_snapshot,
        "Editor",
        EDITOR_MODULE_MARKERS + required_editor_markers,
        phase_log_path,
    )
    if interactive_runtime:
        interactive_line = next(
            line.strip()
            for line in editor_log.splitlines()
            if EDITOR_INTERACTIVE_READY_MARKER in line
        )
        print("Editor interactive runtime readiness:")
        print(f"  - {interactive_line}")
        print("  - Press Play, wait for the IR mode gate, then use WASD+Mouse; F3 toggles Rendered/Original and F1/F2/F5 select the IR source.")
    else:
        editor_scene_values = validate_editor_scene_ready(
            editor_log,
            expected_direct_preset=editor_direct_preset,
            expected_reflection_environment="enclosed",
            expected_distance_cm=editor_distance_cm,
            expected_air_absorption_profile=(
                editor_air_absorption_profile
            ),
        )
        editor_scene_line = next(
            line.strip()
            for line in editor_log.splitlines()
            if EDITOR_VISIBLE_SCENE_MARKER in line
        )
        print("Editor A/B scene evidence:")
        print(f"  - {editor_scene_line}")
        print(
            "  - Parsed validation fixture: "
            f"distance_cm={editor_scene_values['distance_cm']:.2f} "
            "air_absorption_profile="
            f"{editor_scene_values['air_absorption_profile']} "
            "air_absorption_per_meter="
            f"{editor_scene_values['air_absorption_per_meter']}"
        )

    if editor_ab_artifacts:
        validate_editor_ab_artifacts_marker(
            editor_log,
            expected_direct_preset=editor_direct_preset,
            expected_reflection_environment="enclosed",
        )
        artifacts_match = EDITOR_AB_ARTIFACTS_PATTERN.search(editor_log)
        assert artifacts_match is not None
        failures: list[str] = []
        if int(artifacts_match.group("hardware")) != 1:
            failures.append("hardware ray tracing provenance")
        if int(artifacts_match.group("auto_checks")) != 1:
            failures.append("automatic A/B checks")
        if int(artifacts_match.group("distinct")) != 1:
            failures.append("direct/wet distinction")
        if int(artifacts_match.group("imported_assets")) != 4:
            failures.append("four imported comparison assets")
        if artifacts_match.group("direct_preset") != editor_direct_preset:
            failures.append(
                "requested direct preset "
                f"({artifacts_match.group('direct_preset')} != {editor_direct_preset})"
            )
        if not is_original_project_input_asset(artifacts_match.group("input")):
            failures.append(
                f"original project input ({artifacts_match.group('input')})"
            )
        artifact_files = [
            Path(artifacts_match.group(name))
            for name in ("reference", "direct", "wet", "full", "manifest")
        ]
        missing_files = [str(path) for path in artifact_files if not path.is_file()]
        if missing_files:
            failures.append(f"artifact files exist ({missing_files})")
        ir_object_path = artifacts_match.group("ir_asset")
        ir_package_path = ir_object_path.split(".", 1)[0]
        if not ir_package_path.startswith("/Game/"):
            failures.append(f"valid IR object path ({ir_object_path})")
        else:
            ir_filename = project_path.parent / "Content" / (
                ir_package_path.removeprefix("/Game/") + ".uasset"
            )
            if not ir_filename.is_file():
                failures.append(f"saved IR asset ({ir_filename})")
        manifest_path = Path(artifacts_match.group("manifest"))
        if manifest_path.is_file():
            manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
            if not manifest.get("hardware_ray_tracing", False):
                failures.append("manifest hardware provenance")
            if not manifest.get("modes_are_distinct", False):
                failures.append("manifest mode distinction")
            if not manifest.get("automatic_checks_passed", False):
                failures.append("manifest automatic checks")
            if not manifest.get("audio_safety_checks_passed", False):
                failures.append("manifest audio safety checks")
            if not manifest.get("samples_finite", False):
                failures.append("manifest finite audio samples")
            if int(manifest.get("impulse_response_channels", 0)) != 2:
                failures.append("manifest directional stereo impulse response")
            if editor_direct_preset == "clear":
                if not manifest.get("directional_wet_is_distinct", False):
                    failures.append("manifest directional wet distinction")
                if float(manifest.get("wet_stereo_normalized_difference", 0.0)) < 0.01:
                    failures.append("manifest measurable wet stereo difference")
            if not manifest.get("direct_semantics_passed", False):
                failures.append("manifest direct preset semantics")
            if manifest.get("direct_preset") != editor_direct_preset:
                failures.append("manifest direct preset provenance")
            for field in (
                "direct_distance_cm",
                "direct_visibility",
                "direct_occlusion",
                "direct_distance_attenuation",
                "hardware_indirect_valid_paths",
                "hardware_indirect_gain",
                "hardware_impulse_response_energy",
                "cpu_reference_indirect_valid_paths",
                "cpu_reference_indirect_gain",
                "cpu_reference_impulse_response_energy",
                "reflection_ray_count",
                "reflection_bounce_count",
                "hardware_early_reflection_gain",
                "hardware_late_reverb_gain",
                "hardware_directional_energy_ratio",
                "hardware_directional_bin_count",
                "cpu_reference_early_reflection_gain",
                "cpu_reference_late_reverb_gain",
                "cpu_reference_directional_energy_ratio",
                "cpu_reference_directional_bin_count",
                "post_scale_peak",
                "clipped_sample_count",
                "direct_active_window_count",
                "direct_dropout_window_count",
                "direct_model_residual_rms",
                "full_mix_residual_rms",
                "max_direct_discontinuity_residual",
            ):
                if not isinstance(manifest.get(field), (int, float)):
                    failures.append(f"manifest {field}")
            if int(manifest.get("clipped_sample_count", -1)) != 0:
                failures.append("manifest zero clipped samples")
            if int(manifest.get("direct_dropout_window_count", -1)) != 0:
                failures.append("manifest zero direct dropout windows")
            if int(manifest.get("reflection_ray_count", 0)) <= 0:
                failures.append("manifest positive reflection rays")
            if int(manifest.get("reflection_bounce_count", 0)) < 2:
                failures.append("manifest multi-bounce provenance")
            if int(manifest.get("hardware_directional_bin_count", 0)) <= 0:
                failures.append("manifest hardware directional bins")
            if int(manifest.get("cpu_reference_directional_bin_count", 0)) <= 0:
                failures.append("manifest CPU directional bins")
            if not manifest.get("has_cpu_reference", False):
                failures.append("manifest CPU reference provenance")
            hardware_paths = int(manifest.get("hardware_indirect_valid_paths", 0))
            cpu_paths = int(manifest.get("cpu_reference_indirect_valid_paths", 0))
            hardware_gain = float(manifest.get("hardware_indirect_gain", 0.0))
            cpu_gain = float(manifest.get("cpu_reference_indirect_gain", 0.0))
            hardware_energy = float(manifest.get("hardware_impulse_response_energy", 0.0))
            cpu_energy = float(manifest.get("cpu_reference_impulse_response_energy", 0.0))
            for label, hardware_value, cpu_value in (
                ("paths", float(hardware_paths), float(cpu_paths)),
                ("gain", hardware_gain, cpu_gain),
                ("IR energy", hardware_energy, cpu_energy),
                (
                    "early reflection",
                    float(manifest.get("hardware_early_reflection_gain", 0.0)),
                    float(manifest.get("cpu_reference_early_reflection_gain", 0.0)),
                ),
                (
                    "late reverb",
                    float(manifest.get("hardware_late_reverb_gain", 0.0)),
                    float(manifest.get("cpu_reference_late_reverb_gain", 0.0)),
                ),
                (
                    "directional energy",
                    float(manifest.get("hardware_directional_energy_ratio", 0.0)),
                    float(manifest.get("cpu_reference_directional_energy_ratio", 0.0)),
                ),
            ):
                denominator = max(abs(hardware_value), abs(cpu_value), 1.0e-12)
                relative_delta = abs(hardware_value - cpu_value) / denominator
                if hardware_value <= 0.0 or cpu_value <= 0.0 or relative_delta > 0.05:
                    failures.append(f"manifest hardware/CPU {label} agreement")
        if failures:
            raise RuntimeError("Editor A/B artifact validation failed: " + ", ".join(failures))
        artifact_line = next(
            line.strip()
            for line in editor_log.splitlines()
            if EDITOR_AB_ARTIFACTS_MARKER in line
        )
        print("Editor hardware A/B artifact evidence:")
        print(f"  - {artifact_line}")
        listening_ui_line = next(
            line.strip()
            for line in editor_log.splitlines()
            if EDITOR_LISTENING_UI_MARKER in line
        )
        print("Editor listening acceptance UI evidence:")
        print(f"  - {listening_ui_line}")

    print("Editor completed initialization and was left running for manual verification.")


def main() -> int:
    args = parse_args()
    game_seconds = args.game_seconds

    repo_root = Path(__file__).resolve().parent.parent
    plugin_files = sorted(repo_root.glob("*.uplugin"))
    if len(plugin_files) != 1:
        raise RuntimeError(f"Expected exactly one .uplugin file in {repo_root}, found {len(plugin_files)}.")
    engine_root = validation_environment.resolve_engine_root(args.engine_root)
    project_path = validation_environment.resolve_project_path(args.project, repo_root, plugin_files[0].stem)
    editor_exe = build_editor_path(engine_root)

    ensure_paths(editor_exe, project_path)
    if args.interactive_runtime and args.editor_ab_artifacts:
        raise RuntimeError(
            "--interactive-runtime and --editor-ab-artifacts are separate acceptance flows; run them separately."
        )

    print(f"Engine root          : {engine_root}")
    print(f"Project file         : {project_path}")
    print(f"Editor executable    : {editor_exe}")
    print(f"Game validation secs : {game_seconds}")
    print(f"Validation sources   : {max(2, min(args.source_count, 32))}")
    print(f"CSV performance      : {args.csv_profile}")
    print(f"Bake repeatability   : {args.bake_repeatability}")
    print(f"Editor A/B artifacts : {args.editor_ab_artifacts}")
    print(f"Interactive runtime  : {args.interactive_runtime}")
    print(f"Interactive smoke    : {args.interactive_smoke}")
    print(f"Editor direct preset : {args.editor_direct_preset}")
    print(f"Editor distance cm   : {args.editor_distance_cm}")
    print(
        "Editor air profile   : "
        f"{args.editor_air_absorption_profile}"
    )
    print(f"Editor ready timeout : {args.editor_ready_timeout_seconds}")
    print(f"Dry run              : {args.dry_run}")

    start_game_then_kill(
        editor_exe=editor_exe,
        project_path=project_path,
        game_seconds=game_seconds,
        source_count=args.source_count,
        csv_profile=args.csv_profile,
        bake_repeatability=args.bake_repeatability,
        interactive_smoke=args.interactive_smoke,
        dry_run=args.dry_run,
    )
    start_editor_for_user(
        editor_exe=editor_exe,
        project_path=project_path,
        editor_ready_timeout_seconds=args.editor_ready_timeout_seconds,
        source_count=args.source_count,
        bake_repeatability=args.bake_repeatability,
        editor_ab_artifacts=args.editor_ab_artifacts,
        editor_direct_preset=args.editor_direct_preset,
        editor_distance_cm=args.editor_distance_cm,
        editor_air_absorption_profile=(
            args.editor_air_absorption_profile
        ),
        interactive_runtime=args.interactive_runtime,
        dry_run=args.dry_run,
    )

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
