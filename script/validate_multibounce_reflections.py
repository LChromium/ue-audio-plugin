from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import launch_runtime_validation
import validation_environment


SINGLE_BOUNCE_COUNT = 1
MULTI_BOUNCE_COUNT = 8
MAX_CPU_RELATIVE_DELTA = 0.05
MIN_DIRECTIONAL_ENERGY_RATIO = 0.05
MIN_DIRECTION_DOT = 0.99


def relative_delta(first: float, second: float) -> float:
    denominator = max(abs(first), abs(second), 1.0e-12)
    return abs(first - second) / denominator


def normalized_direction(manifest: dict[str, Any], prefix: str) -> tuple[float, float, float]:
    values = tuple(
        float(manifest.get(f"{prefix}_dominant_arrival_direction_{axis}", 0.0))
        for axis in ("x", "y", "z")
    )
    length = math.sqrt(sum(value * value for value in values))
    if length <= 1.0e-12:
        return (0.0, 0.0, 0.0)
    return tuple(value / length for value in values)


def direction_dot(first: tuple[float, float, float], second: tuple[float, float, float]) -> float:
    return sum(a * b for a, b in zip(first, second, strict=True))


def validate_multibounce_reflections(
    single: dict[str, Any],
    multi: dict[str, Any],
) -> dict[str, float | str]:
    failures: list[str] = []
    for label, manifest, expected_bounces in (
        ("single", single, SINGLE_BOUNCE_COUNT),
        ("multi", multi, MULTI_BOUNCE_COUNT),
    ):
        if manifest.get("direct_preset") != "clear":
            failures.append(f"{label} clear-scene provenance")
        if int(manifest.get("reflection_bounce_count", 0)) != expected_bounces:
            failures.append(f"{label} reflection bounce provenance")
        if int(manifest.get("reflection_ray_count", 0)) <= 0:
            failures.append(f"{label} positive reflection ray count")
        for field in (
            "hardware_ray_tracing",
            "has_cpu_reference",
            "automatic_checks_passed",
            "audio_safety_checks_passed",
            "samples_finite",
            "directional_wet_is_distinct",
        ):
            if not manifest.get(field, False):
                failures.append(f"{label} {field}")
        if int(manifest.get("impulse_response_channels", 0)) != 2:
            failures.append(f"{label} directional stereo impulse response")
        if float(manifest.get("wet_stereo_normalized_difference", 0.0)) < 0.01:
            failures.append(f"{label} measurable wet stereo difference")

        hardware_paths = int(manifest.get("hardware_indirect_valid_paths", 0))
        cpu_paths = int(manifest.get("cpu_reference_indirect_valid_paths", 0))
        if hardware_paths <= 0 or cpu_paths <= 0:
            failures.append(f"{label} positive hardware/CPU paths")
        elif relative_delta(float(hardware_paths), float(cpu_paths)) > MAX_CPU_RELATIVE_DELTA:
            failures.append(f"{label} hardware/CPU path agreement")

        hardware_ir_energy = float(manifest.get("hardware_impulse_response_energy", 0.0))
        cpu_ir_energy = float(manifest.get("cpu_reference_impulse_response_energy", 0.0))
        if hardware_ir_energy <= 0.0 or cpu_ir_energy <= 0.0:
            failures.append(f"{label} positive hardware/CPU IR energy")
        elif relative_delta(hardware_ir_energy, cpu_ir_energy) > MAX_CPU_RELATIVE_DELTA:
            failures.append(f"{label} hardware/CPU IR energy agreement")

        for metric in (
            "indirect_gain",
            "early_reflection_gain",
            "late_reverb_gain",
            "directional_energy_ratio",
        ):
            hardware_value = float(manifest.get(f"hardware_{metric}", 0.0))
            cpu_value = float(manifest.get(f"cpu_reference_{metric}", 0.0))
            if metric != "late_reverb_gain" and (hardware_value <= 0.0 or cpu_value <= 0.0):
                failures.append(f"{label} positive hardware/CPU {metric}")
            if relative_delta(hardware_value, cpu_value) > MAX_CPU_RELATIVE_DELTA:
                failures.append(f"{label} hardware/CPU {metric} agreement")

        hardware_bins = int(manifest.get("hardware_directional_bin_count", 0))
        cpu_bins = int(manifest.get("cpu_reference_directional_bin_count", 0))
        hardware_ratio = float(manifest.get("hardware_directional_energy_ratio", 0.0))
        cpu_ratio = float(manifest.get("cpu_reference_directional_energy_ratio", 0.0))
        if hardware_bins <= 0 or cpu_bins <= 0:
            failures.append(f"{label} non-zero directional bins")
        if min(hardware_ratio, cpu_ratio) < MIN_DIRECTIONAL_ENERGY_RATIO:
            failures.append(f"{label} meaningful directional energy")

        hardware_direction = normalized_direction(manifest, "hardware")
        cpu_direction = normalized_direction(manifest, "cpu_reference")
        hardware_cpu_direction_dot = direction_dot(hardware_direction, cpu_direction)
        if hardware_cpu_direction_dot < MIN_DIRECTION_DOT:
            failures.append(
                f"{label} hardware/CPU arrival direction ({hardware_cpu_direction_dot:.6f})"
            )

    if not single.get("input_asset") == multi.get("input_asset"):
        failures.append("same original input asset")
    if not single.get("scene_signature") == multi.get("scene_signature"):
        failures.append("same acoustic scene signature")
    if abs(float(single.get("direct_distance_cm", -1.0)) - float(multi.get("direct_distance_cm", -1.0))) > 1.0:
        failures.append("same source/listener distance")
    if int(single.get("reflection_ray_count", 0)) != int(multi.get("reflection_ray_count", -1)):
        failures.append("same reflection ray count")

    single_paths = int(single.get("hardware_indirect_valid_paths", 0))
    multi_paths = int(multi.get("hardware_indirect_valid_paths", 0))
    single_energy = float(single.get("hardware_impulse_response_energy", 0.0))
    multi_energy = float(multi.get("hardware_impulse_response_energy", 0.0))
    single_late = float(single.get("hardware_late_reverb_gain", 0.0))
    multi_late = float(multi.get("hardware_late_reverb_gain", 0.0))
    single_directional_bins = int(single.get("hardware_directional_bin_count", 0))
    multi_directional_bins = int(multi.get("hardware_directional_bin_count", 0))
    single_delay = float(single.get("hardware_average_delay_seconds", 0.0))
    multi_delay = float(multi.get("hardware_average_delay_seconds", 0.0))
    if multi_paths <= single_paths:
        failures.append("multi-bounce adds valid reflection paths")
    if multi_energy <= single_energy:
        failures.append("multi-bounce adds impulse-response energy")
    if multi_late <= max(single_late, 0.0):
        failures.append("multi-bounce adds a non-zero late-reverb tail")
    if multi_directional_bins <= single_directional_bins:
        failures.append("multi-bounce adds directional delay bins")
    if multi_delay <= single_delay:
        failures.append("multi-bounce increases average path delay")

    if failures:
        raise RuntimeError("Multi-bounce reflection validation failed: " + ", ".join(failures))

    return {
        "input_asset": str(single.get("input_asset", "")),
        "scene_signature": str(single.get("scene_signature", "")),
        "single_paths": float(single_paths),
        "multi_paths": float(multi_paths),
        "single_ir_energy": single_energy,
        "multi_ir_energy": multi_energy,
        "single_late_reverb_gain": single_late,
        "multi_late_reverb_gain": multi_late,
        "single_directional_bins": float(single_directional_bins),
        "multi_directional_bins": float(multi_directional_bins),
        "single_average_delay_seconds": single_delay,
        "multi_average_delay_seconds": multi_delay,
        "multi_directional_energy_ratio": float(multi["hardware_directional_energy_ratio"]),
        "single_wet_stereo_normalized_difference": float(
            single["wet_stereo_normalized_difference"]
        ),
        "multi_wet_stereo_normalized_difference": float(
            multi["wet_stereo_normalized_difference"]
        ),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--single-manifest", type=Path)
    parser.add_argument("--multi-manifest", type=Path)
    return parser.parse_args()


def run_editor_case(
    repo_root: Path,
    bounces: int,
    timeout_seconds: float,
    engine_root: Path,
    project_path: Path,
) -> tuple[Path, Path]:
    screenshot = repo_root / "Saved" / "Validation" / f"multibounce-{bounces}-clear.png"
    command = [
        sys.executable,
        str(repo_root / "script" / "validate_visible_editor_ab_scene.py"),
        "--artifacts",
        "--direct-preset",
        "clear",
        "--reflection-bounces",
        str(bounces),
        "--timeout",
        str(timeout_seconds),
        "--screenshot",
        str(screenshot),
        "--engine-root",
        str(engine_root),
        "--project",
        str(project_path),
    ]
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
        raise RuntimeError(f"Editor {bounces}-bounce validation exited with code {completed.returncode}.")
    matches = list(launch_runtime_validation.EDITOR_AB_ARTIFACTS_PATTERN.finditer(completed.stdout))
    if not matches:
        raise RuntimeError(f"Could not parse the {bounces}-bounce artifact marker.")
    manifest = Path(matches[-1].group("manifest"))
    if not manifest.is_file():
        raise RuntimeError(f"The {bounces}-bounce manifest does not exist: {manifest}")
    return manifest, screenshot


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    plugin_files = sorted(repo_root.glob("*.uplugin"))
    if len(plugin_files) != 1:
        raise RuntimeError(f"Expected exactly one .uplugin file in {repo_root}.")
    engine_root = validation_environment.resolve_engine_root(args.engine_root)
    project_path = validation_environment.resolve_project_path(
        args.project, repo_root, plugin_files[0].stem
    )

    if (args.single_manifest is None) != (args.multi_manifest is None):
        raise RuntimeError("--single-manifest and --multi-manifest must be supplied together.")
    if args.single_manifest is not None and args.multi_manifest is not None:
        single_path = args.single_manifest.resolve()
        multi_path = args.multi_manifest.resolve()
        single_screenshot = repo_root / "Saved" / "Validation" / "multibounce-1-clear.png"
        multi_screenshot = repo_root / "Saved" / "Validation" / "multibounce-8-clear.png"
    else:
        single_path, single_screenshot = run_editor_case(
            repo_root, SINGLE_BOUNCE_COUNT, args.timeout, engine_root, project_path
        )
        multi_path, multi_screenshot = run_editor_case(
            repo_root, MULTI_BOUNCE_COUNT, args.timeout, engine_root, project_path
        )
    if not single_path.is_file() or not multi_path.is_file():
        raise RuntimeError("One or both multi-bounce input manifests do not exist.")
    single = json.loads(single_path.read_text(encoding="utf-8-sig"))
    multi = json.loads(multi_path.read_text(encoding="utf-8-sig"))
    metrics = validate_multibounce_reflections(single, multi)

    single_reference_hash = sha256_file(Path(str(single["reference_wav"])))
    multi_reference_hash = sha256_file(Path(str(multi["reference_wav"])))
    if single_reference_hash != multi_reference_hash:
        raise RuntimeError("Multi-bounce validation failed: Reference WAVs are not byte-identical.")

    output_path = args.output
    if output_path is None:
        output_path = (
            project_path.parent
            / "Saved"
            / "UERayTracingAudio"
            / "ListeningAcceptance"
            / "MultiBounce"
            / time.strftime("%Y%m%d-%H%M%S")
            / "MultiBounce_Manifest.json"
        )
    elif not output_path.is_absolute():
        output_path = repo_root / output_path
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(
            {
                "passed": True,
                "single_manifest": str(single_path),
                "multi_manifest": str(multi_path),
                "single_screenshot": str(single_screenshot),
                "multi_screenshot": str(multi_screenshot),
                "reference_sha256": single_reference_hash,
                **metrics,
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n",
        encoding="utf-8",
    )
    print(
        "MULTIBOUNCE_REFLECTION_PASS "
        f"single_paths={metrics['single_paths']:.0f} "
        f"multi_paths={metrics['multi_paths']:.0f} "
        f"single_late={metrics['single_late_reverb_gain']:.9f} "
        f"multi_late={metrics['multi_late_reverb_gain']:.9f} "
        f"directional_ratio={metrics['multi_directional_energy_ratio']:.6f} "
        f"wet_stereo_difference={metrics['multi_wet_stereo_normalized_difference']:.6f} "
        f"manifest='{output_path}'"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"MULTIBOUNCE_REFLECTION_FAIL {exc}", file=sys.stderr)
        raise SystemExit(1)
