from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import launch_runtime_validation
import validation_environment


MAX_DISTANCE_DELTA_CM = 1.0
MAX_DISTANCE_ATTENUATION_DELTA = 0.005
MIN_CLEAR_VISIBILITY = 0.95
MAX_SOFT_OCCLUDED_VISIBILITY = 0.05
MAX_HARD_OCCLUDED_VISIBILITY = 0.05
MIN_SOFT_OCCLUSION = 0.15
MAX_SOFT_OCCLUSION = 0.35
MAX_HARD_OCCLUSION = 0.01
MAX_HARD_DIRECT_GAIN = 1.0e-4
MIN_HARD_FULL_RMS_RATIO = 0.001
MIN_AUDIBLE_DIRECT_GAIN = 0.05
MIN_SOFT_TO_CLEAR_GAIN_RATIO = 0.15
MAX_SOFT_TO_CLEAR_GAIN_RATIO = 0.40
MAX_CPU_REFERENCE_RELATIVE_DELTA = 0.05


def relative_delta(first: float, second: float) -> float:
    denominator = max(abs(first), abs(second), 1.0e-12)
    return abs(first - second) / denominator


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--output",
        type=Path,
        help="Optional pair manifest path; defaults under the test project's Saved directory.",
    )
    return parser.parse_args()


def read_manifest(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_direct_occlusion_pair(
    clear: dict[str, Any],
    soft_occluded: dict[str, Any],
    hard_occluded: dict[str, Any],
) -> dict[str, float | str]:
    failures: list[str] = []
    for label, manifest, expected_preset in (
        ("clear", clear, "clear"),
        ("soft_occluded", soft_occluded, "soft_occluded"),
        ("hard_occluded", hard_occluded, "hard_occluded"),
    ):
        if manifest.get("direct_preset") != expected_preset:
            failures.append(f"{label} preset provenance")
        if not manifest.get("hardware_ray_tracing", False):
            failures.append(f"{label} hardware provenance")
        if not manifest.get("automatic_checks_passed", False):
            failures.append(f"{label} automatic A/B checks")
        if not manifest.get("modes_are_distinct", False):
            failures.append(f"{label} direct/wet distinction")
        if not manifest.get("audio_safety_checks_passed", False):
            failures.append(f"{label} audio safety checks")
        if not manifest.get("samples_finite", False):
            failures.append(f"{label} finite audio samples")
        if not manifest.get("direct_semantics_passed", False):
            failures.append(f"{label} direct preset semantics")
        if int(manifest.get("clipped_sample_count", -1)) != 0:
            failures.append(f"{label} zero clipped samples")
        if int(manifest.get("direct_dropout_window_count", -1)) != 0:
            failures.append(f"{label} zero direct dropout windows")
        if expected_preset != "hard_occluded" and float(manifest.get("direct_dry_correlation", 0.0)) < 0.99:
            failures.append(f"{label} direct dry preservation")
        if not manifest.get("has_cpu_reference", False):
            failures.append(f"{label} CPU reference provenance")
        if int(manifest.get("reflection_bounce_count", 0)) < 2:
            failures.append(f"{label} multi-bounce provenance")
        if int(manifest.get("reflection_ray_count", 0)) <= 0:
            failures.append(f"{label} positive reflection rays")
        if int(manifest.get("hardware_directional_bin_count", 0)) <= 0:
            failures.append(f"{label} hardware directional bins")
        if int(manifest.get("cpu_reference_directional_bin_count", 0)) <= 0:
            failures.append(f"{label} CPU directional bins")
        if int(manifest.get("impulse_response_channels", 0)) != 2:
            failures.append(f"{label} directional stereo impulse response")
        if expected_preset == "clear":
            if not manifest.get("directional_wet_is_distinct", False):
                failures.append(f"{label} directional wet distinction")
            if float(manifest.get("wet_stereo_normalized_difference", 0.0)) < 0.01:
                failures.append(f"{label} measurable wet stereo difference")

        hardware_paths = int(manifest.get("hardware_indirect_valid_paths", 0))
        cpu_paths = int(manifest.get("cpu_reference_indirect_valid_paths", 0))
        path_delta = relative_delta(float(hardware_paths), float(cpu_paths))
        if hardware_paths <= 0 or cpu_paths <= 0:
            failures.append(f"{label} positive hardware/CPU indirect paths")
        elif path_delta > MAX_CPU_REFERENCE_RELATIVE_DELTA:
            failures.append(f"{label} hardware/CPU path agreement ({path_delta:.6f})")

        hardware_gain = float(manifest.get("hardware_indirect_gain", 0.0))
        cpu_gain = float(manifest.get("cpu_reference_indirect_gain", 0.0))
        gain_delta = relative_delta(hardware_gain, cpu_gain)
        if hardware_gain <= 0.0 or cpu_gain <= 0.0:
            failures.append(f"{label} positive hardware/CPU indirect gain")
        elif gain_delta > MAX_CPU_REFERENCE_RELATIVE_DELTA:
            failures.append(f"{label} hardware/CPU gain agreement ({gain_delta:.6f})")

        hardware_energy = float(manifest.get("hardware_impulse_response_energy", 0.0))
        cpu_energy = float(manifest.get("cpu_reference_impulse_response_energy", 0.0))
        energy_delta = relative_delta(hardware_energy, cpu_energy)
        if hardware_energy <= 0.0 or cpu_energy <= 0.0:
            failures.append(f"{label} positive hardware/CPU IR energy")
        elif energy_delta > MAX_CPU_REFERENCE_RELATIVE_DELTA:
            failures.append(f"{label} hardware/CPU IR energy agreement ({energy_delta:.6f})")

        for metric in (
            "early_reflection_gain",
            "late_reverb_gain",
            "directional_energy_ratio",
        ):
            hardware_value = float(manifest.get(f"hardware_{metric}", 0.0))
            cpu_value = float(manifest.get(f"cpu_reference_{metric}", 0.0))
            metric_delta = relative_delta(hardware_value, cpu_value)
            if hardware_value <= 0.0 or cpu_value <= 0.0:
                failures.append(f"{label} positive hardware/CPU {metric}")
            elif metric_delta > MAX_CPU_REFERENCE_RELATIVE_DELTA:
                failures.append(
                    f"{label} hardware/CPU {metric} agreement ({metric_delta:.6f})"
                )

    if not clear.get("input_asset") == soft_occluded.get("input_asset") == hard_occluded.get("input_asset"):
        failures.append("same original input asset")
    if not clear.get("scene_signature") == soft_occluded.get("scene_signature") == hard_occluded.get("scene_signature"):
        failures.append("same acoustic geometry signature")

    clear_distance = float(clear.get("direct_distance_cm", -1.0))
    soft_distance = float(soft_occluded.get("direct_distance_cm", -1.0))
    hard_distance = float(hard_occluded.get("direct_distance_cm", -1.0))
    distance_delta = max(
        abs(clear_distance - soft_distance),
        abs(clear_distance - hard_distance),
    )
    if min(clear_distance, soft_distance, hard_distance) <= 0.0 or distance_delta > MAX_DISTANCE_DELTA_CM:
        failures.append(
            f"equal positive source/listener distance (delta={distance_delta:.3f} cm)"
        )

    clear_attenuation = float(clear.get("direct_distance_attenuation", -1.0))
    soft_attenuation = float(soft_occluded.get("direct_distance_attenuation", -1.0))
    hard_attenuation = float(hard_occluded.get("direct_distance_attenuation", -1.0))
    attenuation_delta = max(
        abs(clear_attenuation - soft_attenuation),
        abs(clear_attenuation - hard_attenuation),
    )
    if (
        min(clear_attenuation, soft_attenuation, hard_attenuation) <= 0.0
        or attenuation_delta > MAX_DISTANCE_ATTENUATION_DELTA
    ):
        failures.append(
            "equal positive distance attenuation "
            f"(delta={attenuation_delta:.6f})"
        )

    clear_visibility = float(clear.get("direct_visibility", -1.0))
    soft_visibility = float(soft_occluded.get("direct_visibility", -1.0))
    hard_visibility = float(hard_occluded.get("direct_visibility", -1.0))
    if clear_visibility < MIN_CLEAR_VISIBILITY:
        failures.append(f"clear visibility ({clear_visibility:.6f})")
    if soft_visibility < 0.0 or soft_visibility > MAX_SOFT_OCCLUDED_VISIBILITY:
        failures.append(f"soft-occluded visibility ({soft_visibility:.6f})")
    if hard_visibility < 0.0 or hard_visibility > MAX_HARD_OCCLUDED_VISIBILITY:
        failures.append(f"hard-occluded visibility ({hard_visibility:.6f})")

    clear_occlusion = float(clear.get("direct_occlusion", -1.0))
    soft_occlusion = float(soft_occluded.get("direct_occlusion", -1.0))
    hard_occlusion = float(hard_occluded.get("direct_occlusion", -1.0))
    if clear_occlusion < MIN_CLEAR_VISIBILITY:
        failures.append(f"clear occlusion transfer ({clear_occlusion:.6f})")
    if not MIN_SOFT_OCCLUSION <= soft_occlusion <= MAX_SOFT_OCCLUSION:
        failures.append(f"soft occlusion floor ({soft_occlusion:.6f})")
    if hard_occlusion < 0.0 or hard_occlusion > MAX_HARD_OCCLUSION:
        failures.append(f"hard occlusion transfer ({hard_occlusion:.6f})")

    clear_gain = float(clear.get("direct_gain", 0.0))
    soft_gain = float(soft_occluded.get("direct_gain", 0.0))
    hard_gain = float(hard_occluded.get("direct_gain", -1.0))
    if clear_gain < MIN_AUDIBLE_DIRECT_GAIN:
        failures.append(f"audible clear direct gain ({clear_gain:.6f})")
    if soft_gain < MIN_AUDIBLE_DIRECT_GAIN:
        failures.append(f"audible soft-occluded direct gain ({soft_gain:.6f})")
    gain_ratio = soft_gain / clear_gain if clear_gain > 0.0 else 0.0
    if not MIN_SOFT_TO_CLEAR_GAIN_RATIO <= gain_ratio <= MAX_SOFT_TO_CLEAR_GAIN_RATIO:
        failures.append(f"occlusion-only direct gain ratio ({gain_ratio:.6f})")
    if hard_gain < 0.0 or hard_gain > MAX_HARD_DIRECT_GAIN:
        failures.append(f"hard-occluded direct silence ({hard_gain:.6f})")
    hard_full_ratio = float(hard_occluded.get("full_to_reference_rms_ratio", 0.0))
    if hard_full_ratio < MIN_HARD_FULL_RMS_RATIO:
        failures.append(f"hard-occluded reflected Full energy ({hard_full_ratio:.6f})")

    if failures:
        raise RuntimeError("Direct/occlusion pair validation failed: " + ", ".join(failures))

    return {
        "input_asset": str(clear.get("input_asset", "")),
        "scene_signature": str(clear.get("scene_signature", "")),
        "distance_delta_cm": distance_delta,
        "distance_attenuation_delta": attenuation_delta,
        "clear_visibility": clear_visibility,
        "soft_occluded_visibility": soft_visibility,
        "hard_occluded_visibility": hard_visibility,
        "clear_direct_gain": clear_gain,
        "soft_occluded_direct_gain": soft_gain,
        "hard_occluded_direct_gain": hard_gain,
        "hard_occluded_full_to_reference_rms_ratio": hard_full_ratio,
        "soft_to_clear_direct_gain_ratio": gain_ratio,
        "clear_hardware_cpu_path_relative_delta": relative_delta(
            float(clear["hardware_indirect_valid_paths"]),
            float(clear["cpu_reference_indirect_valid_paths"]),
        ),
        "soft_hardware_cpu_path_relative_delta": relative_delta(
            float(soft_occluded["hardware_indirect_valid_paths"]),
            float(soft_occluded["cpu_reference_indirect_valid_paths"]),
        ),
        "hard_hardware_cpu_path_relative_delta": relative_delta(
            float(hard_occluded["hardware_indirect_valid_paths"]),
            float(hard_occluded["cpu_reference_indirect_valid_paths"]),
        ),
    }


def run_editor_case(
    repo_root: Path,
    preset: str,
    timeout_seconds: float,
    engine_root: Path | None,
    project_path: Path | None,
) -> tuple[Path, str]:
    screenshot = repo_root / "Saved" / "Validation" / f"direct-{preset}.png"
    command = [
        sys.executable,
        str(repo_root / "script" / "validate_visible_editor_ab_scene.py"),
        "--artifacts",
        "--direct-preset",
        preset,
        "--timeout",
        str(timeout_seconds),
        "--screenshot",
        str(screenshot),
    ]
    if engine_root is not None:
        command.extend(("--engine-root", str(engine_root)))
    if project_path is not None:
        command.extend(("--project", str(project_path)))

    completed = subprocess.run(
        command,
        cwd=repo_root,
        capture_output=True,
        text=True,
        timeout=timeout_seconds + 45.0,
        check=False,
    )
    if completed.stdout:
        print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)
    if completed.returncode != 0:
        raise RuntimeError(
            f"Editor {preset} validation exited with code {completed.returncode}."
        )

    matches = list(
        launch_runtime_validation.EDITOR_AB_ARTIFACTS_PATTERN.finditer(completed.stdout)
    )
    if not matches:
        raise RuntimeError(f"Could not parse the {preset} Editor artifact marker.")
    match = matches[-1]
    if match.group("direct_preset") != preset:
        raise RuntimeError(
            f"Editor returned preset {match.group('direct_preset')!r} instead of {preset!r}."
        )
    manifest_path = Path(match.group("manifest"))
    if not manifest_path.is_file():
        raise RuntimeError(f"The {preset} manifest does not exist: {manifest_path}")
    return manifest_path, str(screenshot)


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    plugin_files = sorted(repo_root.glob("*.uplugin"))
    if len(plugin_files) != 1:
        raise RuntimeError(f"Expected exactly one .uplugin file in {repo_root}.")

    engine_root = validation_environment.resolve_engine_root(args.engine_root)
    project_path = validation_environment.resolve_project_path(
        args.project,
        repo_root,
        plugin_files[0].stem,
    )

    clear_manifest_path, clear_screenshot = run_editor_case(
        repo_root,
        "clear",
        args.timeout,
        engine_root,
        project_path,
    )
    soft_manifest_path, soft_screenshot = run_editor_case(
        repo_root,
        "soft_occluded",
        args.timeout,
        engine_root,
        project_path,
    )
    hard_manifest_path, hard_screenshot = run_editor_case(
        repo_root,
        "hard_occluded",
        args.timeout,
        engine_root,
        project_path,
    )
    clear = read_manifest(clear_manifest_path)
    soft_occluded = read_manifest(soft_manifest_path)
    hard_occluded = read_manifest(hard_manifest_path)
    metrics = validate_direct_occlusion_pair(clear, soft_occluded, hard_occluded)

    clear_reference = Path(str(clear["reference_wav"]))
    soft_reference = Path(str(soft_occluded["reference_wav"]))
    hard_reference = Path(str(hard_occluded["reference_wav"]))
    clear_reference_sha256 = sha256_file(clear_reference)
    soft_reference_sha256 = sha256_file(soft_reference)
    hard_reference_sha256 = sha256_file(hard_reference)
    if not clear_reference_sha256 == soft_reference_sha256 == hard_reference_sha256:
        raise RuntimeError(
            "Direct/occlusion validation failed: Reference WAVs are not byte-identical."
        )

    output_path = args.output
    if output_path is None:
        output_path = (
            project_path.parent
            / "Saved"
            / "UERayTracingAudio"
            / "ListeningAcceptance"
            / "DirectOcclusionPair"
            / time.strftime("%Y%m%d-%H%M%S")
            / "DirectOcclusionPair_Manifest.json"
        )
    elif not output_path.is_absolute():
        output_path = repo_root / output_path
    output_path.parent.mkdir(parents=True, exist_ok=True)
    pair_manifest = {
        "passed": True,
        "clear_manifest": str(clear_manifest_path),
        "soft_occluded_manifest": str(soft_manifest_path),
        "hard_occluded_manifest": str(hard_manifest_path),
        "clear_screenshot": clear_screenshot,
        "soft_occluded_screenshot": soft_screenshot,
        "hard_occluded_screenshot": hard_screenshot,
        "reference_sha256": clear_reference_sha256,
        **metrics,
    }
    output_path.write_text(
        json.dumps(pair_manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(
        "DIRECT_OCCLUSION_PAIR_PASS "
        f"input={metrics['input_asset']!r} "
        f"distance_delta_cm={metrics['distance_delta_cm']:.3f} "
        f"attenuation_delta={metrics['distance_attenuation_delta']:.6f} "
        f"clear_visibility={metrics['clear_visibility']:.6f} "
        f"soft_visibility={metrics['soft_occluded_visibility']:.6f} "
        f"hard_visibility={metrics['hard_occluded_visibility']:.6f} "
        f"clear_gain={metrics['clear_direct_gain']:.6f} "
        f"soft_gain={metrics['soft_occluded_direct_gain']:.6f} "
        f"hard_gain={metrics['hard_occluded_direct_gain']:.6f} "
        f"gain_ratio={metrics['soft_to_clear_direct_gain_ratio']:.6f} "
        f"reference_sha256={clear_reference_sha256} "
        f"manifest='{output_path}'"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"DIRECT_OCCLUSION_PAIR_FAIL {exc}", file=sys.stderr)
        raise SystemExit(1)
