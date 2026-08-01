from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from collections.abc import Iterable
from pathlib import Path
from typing import TextIO

import validation_environment
from reflection_environment_matrix import (
    ENVIRONMENTS,
    WAV_FIELDS,
    CaseEvidence,
    CaseManifest,
    load_case_evidence,
    load_case_manifest,
    validate_end_to_end_evidence,
    validate_matrix_manifests,
)


DISPLAY_NAMES = {
    "open_space": "OpenSpace",
    "near_wall": "NearWall",
    "enclosed": "Enclosed",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--open-space-manifest", type=Path)
    parser.add_argument("--near-wall-manifest", type=Path)
    parser.add_argument("--enclosed-manifest", type=Path)
    return parser.parse_args()


def _display_name(environment: str) -> str:
    try:
        return DISPLAY_NAMES[environment]
    except KeyError as exc:
        raise RuntimeError(
            f"Unknown reflection environment: {environment!r}"
        ) from exc


def build_case_command(
    repo_root: Path,
    engine_root: Path,
    project_path: Path,
    environment: str,
    timeout_seconds: float,
    screenshot_path: Path,
    result_path: Path,
) -> list[str]:
    _display_name(environment)
    return [
        sys.executable,
        str(repo_root / "script" / "validate_visible_editor_ab_scene.py"),
        "--artifacts",
        "--direct-preset",
        "clear",
        "--reflection-environment",
        environment,
        "--reflection-bounces",
        "32",
        "--timeout",
        str(timeout_seconds),
        "--screenshot",
        str(screenshot_path),
        "--result-json",
        str(result_path),
        "--engine-root",
        str(engine_root),
        "--project",
        str(project_path),
    ]


def run_editor_case(
    repo_root: Path,
    engine_root: Path,
    project_path: Path,
    environment: str,
    timeout_seconds: float,
    output_root: Path,
) -> CaseEvidence:
    display_name = _display_name(environment)
    screenshot_path = output_root / f"{display_name}.png"
    result_path = output_root / f"{display_name}_Result.json"
    output_root.mkdir(parents=True, exist_ok=True)
    result_path.unlink(missing_ok=True)
    command = build_case_command(
        repo_root,
        engine_root,
        project_path,
        environment,
        timeout_seconds,
        screenshot_path,
        result_path,
    )
    try:
        completed = subprocess.run(
            command,
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout_seconds + 60.0,
        )
    except subprocess.TimeoutExpired as exc:
        _forward_captured_output(exc.stdout, sys.stdout)
        _forward_captured_output(exc.stderr, sys.stderr)
        raise
    _forward_captured_output(completed.stdout, sys.stdout)
    _forward_captured_output(completed.stderr, sys.stderr)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{display_name} Editor validation exited with code "
            f"{completed.returncode}."
        )
    if not result_path.is_file():
        raise RuntimeError(
            f"{display_name} result JSON does not exist: {result_path}"
        )
    return load_case_evidence(environment, result_path)


def _forward_captured_output(
    output: str | bytes | None,
    stream: TextIO,
) -> None:
    if not output:
        return
    if isinstance(output, bytes):
        output = output.decode("utf-8", errors="replace")
    print(output, end="", file=stream)


def _manifest_paths(args: argparse.Namespace) -> dict[str, Path] | None:
    paths = {
        "open_space": args.open_space_manifest,
        "near_wall": args.near_wall_manifest,
        "enclosed": args.enclosed_manifest,
    }
    supplied = sum(path is not None for path in paths.values())
    if supplied not in (0, len(paths)):
        raise RuntimeError(
            "--open-space-manifest, --near-wall-manifest, and "
            "--enclosed-manifest must be supplied together."
        )
    if supplied == 0:
        return None
    return {
        environment: path.resolve()
        for environment, path in paths.items()
        if path is not None
    }


def _output_path(
    requested_output: Path | None,
    repo_root: Path,
    project_path: Path,
) -> Path:
    if requested_output is None:
        return (
            project_path.parent
            / "Saved"
            / "UERayTracingAudio"
            / "ListeningAcceptance"
            / "ReflectionEnvironmentMatrix"
            / time.strftime("%Y%m%d-%H%M%S")
            / "ReflectionEnvironmentMatrix_Manifest.json"
        )
    if requested_output.is_absolute():
        return requested_output
    return repo_root / requested_output


def _summary_temporary_path(output_path: Path) -> Path:
    return output_path.with_suffix(output_path.suffix + ".tmp")


def _paths_alias(first: Path, second: Path) -> bool:
    if os.path.normcase(str(first.resolve())) == os.path.normcase(
        str(second.resolve())
    ):
        return True
    try:
        return first.samefile(second)
    except OSError:
        return False


def _reject_summary_collisions(
    output_path: Path,
    authoritative_paths: Iterable[tuple[str, Path]],
) -> None:
    evidence_paths = tuple(authoritative_paths)
    summary_paths = (
        ("summary output", output_path),
        ("summary temporary output", _summary_temporary_path(output_path)),
    )
    for summary_label, summary_path in summary_paths:
        for evidence_label, evidence_path in evidence_paths:
            if _paths_alias(summary_path, evidence_path):
                raise RuntimeError(
                    f"{summary_label} collides with {evidence_label}: "
                    f"{summary_path}"
                )


def _reserved_case_paths(output_root: Path) -> tuple[tuple[str, Path], ...]:
    paths: list[tuple[str, Path]] = []
    for environment in ENVIRONMENTS:
        display_name = _display_name(environment)
        paths.extend(
            (
                (
                    f"{display_name} result",
                    output_root / f"{display_name}_Result.json",
                ),
                (
                    f"{display_name} screenshot",
                    output_root / f"{display_name}.png",
                ),
            )
        )
    return tuple(paths)


def _manifest_authoritative_paths(
    environment: str,
    case: CaseManifest,
) -> tuple[tuple[str, Path], ...]:
    display_name = _display_name(environment)
    paths: list[tuple[str, Path]] = [
        (f"{display_name} manifest", case.path.resolve())
    ]
    for field in WAV_FIELDS:
        value = case.payload.get(field)
        if not isinstance(value, (str, os.PathLike)) or not str(value):
            continue
        path = Path(value)
        if not path.is_absolute():
            path = case.path.parent / path
        paths.append((f"{display_name} {field}", path.resolve()))
    return tuple(paths)


def _evidence_authoritative_paths(
    environment: str,
    evidence: CaseEvidence,
) -> tuple[tuple[str, Path], ...]:
    display_name = _display_name(environment)
    return (
        *_manifest_authoritative_paths(environment, evidence.case),
        (f"{display_name} result", evidence.result_path.resolve()),
        (f"{display_name} screenshot", evidence.screenshot_path.resolve()),
        (f"{display_name} log", evidence.log_path.resolve()),
    )


def _invalidate_summary(output_path: Path) -> None:
    output_path.unlink(missing_ok=True)
    _summary_temporary_path(output_path).unlink(missing_ok=True)


def _write_summary_atomic(output_path: Path, summary: dict[str, object]) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = _summary_temporary_path(output_path)
    temporary_path.unlink(missing_ok=True)
    try:
        temporary_path.write_text(
            json.dumps(
                summary,
                indent=2,
                ensure_ascii=False,
                allow_nan=False,
            )
            + "\n",
            encoding="utf-8",
        )
        temporary_path.replace(output_path)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise


def main() -> int:
    args = parse_args()
    manifest_paths = _manifest_paths(args)
    repo_root = Path(__file__).resolve().parent.parent
    plugin_files = sorted(repo_root.glob("*.uplugin"))
    if len(plugin_files) != 1:
        raise RuntimeError(f"Expected exactly one .uplugin file in {repo_root}.")

    project_path = validation_environment.resolve_project_path(
        args.project,
        repo_root,
        plugin_files[0].stem,
    )
    output_path = _output_path(args.output, repo_root, project_path).resolve()
    output_root = output_path.parent

    if manifest_paths is None:
        early_authoritative_paths = _reserved_case_paths(output_root)
    else:
        early_authoritative_paths = tuple(
            (
                f"{_display_name(environment)} manifest",
                manifest_paths[environment],
            )
            for environment in ENVIRONMENTS
        )
    _reject_summary_collisions(output_path, early_authoritative_paths)
    _invalidate_summary(output_path)

    case_evidence: dict[str, CaseEvidence] = {}
    if manifest_paths is None:
        engine_root = validation_environment.resolve_engine_root(args.engine_root)
        for environment in ENVIRONMENTS:
            case_evidence[environment] = run_editor_case(
                repo_root,
                engine_root,
                project_path,
                environment,
                args.timeout,
                output_root,
            )
            _reject_summary_collisions(
                output_path,
                _evidence_authoritative_paths(
                    environment,
                    case_evidence[environment],
                ),
            )
        case_manifests = {
            environment: evidence.case
            for environment, evidence in case_evidence.items()
        }
        end_to_end = True
    else:
        case_manifests = {
            environment: load_case_manifest(environment, manifest_paths[environment])
            for environment in ENVIRONMENTS
        }
        for environment in ENVIRONMENTS:
            _reject_summary_collisions(
                output_path,
                _manifest_authoritative_paths(
                    environment,
                    case_manifests[environment],
                ),
            )
        end_to_end = False

    matrix_validation = validate_matrix_manifests(case_manifests)
    if end_to_end:
        validate_end_to_end_evidence(case_evidence, project_path.parent)

    case_payload = dict(matrix_validation["cases"])
    if end_to_end:
        for environment, evidence in case_evidence.items():
            case_payload[environment] = {
                **case_payload[environment],
                "result": str(evidence.result_path),
                "screenshot": str(evidence.screenshot_path),
                "log": str(evidence.log_path),
            }
    threshold_payload = matrix_validation["thresholds"]
    comparison_payload = matrix_validation["comparisons"]
    summary = {
        "schema_version": 1,
        "passed": True,
        "end_to_end": end_to_end,
        "fixed_config": {
            "input_asset": "/Game/FirstPerson/Audio/MarchingBand.MarchingBand",
            "direct_preset": "clear",
            "distance_cm": 200,
            "air_absorption_profile": "default",
            "reflection_rays": 4096,
            "reflection_bounces": 32,
        },
        "thresholds": threshold_payload,
        "cases": case_payload,
        "comparisons": comparison_payload,
    }
    _write_summary_atomic(output_path, summary)

    marker = (
        "R3_REFLECTION_MATRIX_PASS"
        if end_to_end
        else "R3_REFLECTION_MATRIX_RECHECK_PASS"
    )
    print(f'{marker} bounces=32 manifest="{output_path}"')
    return 0


def entrypoint() -> int:
    try:
        return main()
    except Exception as exc:
        print(f"R3_REFLECTION_MATRIX_FAIL {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(entrypoint())
