from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import launch_runtime_validation
import validation_environment
from validate_visible_runtime_scene import (
    capture_metrics,
    client_bounds,
    stop_process,
    visible_unreal_windows,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--artifacts", action="store_true")
    parser.add_argument(
        "--direct-preset",
        choices=launch_runtime_validation.EDITOR_DIRECT_PRESETS,
        default="clear",
    )
    parser.add_argument(
        "--reflection-environment",
        choices=launch_runtime_validation.EDITOR_REFLECTION_ENVIRONMENTS,
        default="enclosed",
    )
    parser.add_argument("--reflection-bounces", type=int, default=8)
    parser.add_argument(
        "--screenshot",
        type=Path,
        default=Path("Saved/Validation/editor-ab-scene.png"),
    )
    parser.add_argument("--result-json", type=Path)
    return parser.parse_args()


def read_log(log_path: Path) -> str:
    try:
        return log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


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
    editor_exe = launch_runtime_validation.build_editor_path(engine_root)
    screenshot_path = args.screenshot
    if not screenshot_path.is_absolute():
        screenshot_path = repo_root / screenshot_path
    screenshot_path.parent.mkdir(parents=True, exist_ok=True)
    phase_log_path = project_path.parent / "Saved" / "Logs" / (
        f"UERayTracingAudioEditorVisible-{time.time_ns()}.log"
    )
    command = launch_runtime_validation.build_editor_command(
        editor_exe,
        project_path,
        phase_log_path,
        editor_ab_artifacts=args.artifacts,
        editor_direct_preset=args.direct_preset,
        editor_reflection_environment=args.reflection_environment,
        editor_reflection_bounces=args.reflection_bounces,
    )

    existing_process_ids = {
        process_id for _, process_id, _ in visible_unreal_windows()
    }
    creation_flags = 0
    if sys.platform == "win32":
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP
    process = subprocess.Popen(
        command,
        creationflags=creation_flags,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        close_fds=True,
    )
    try:
        deadline = time.monotonic() + args.timeout
        log_text = ""
        target_marker = (
            launch_runtime_validation.EDITOR_AB_ARTIFACTS_MARKER
            if args.artifacts
            else launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER
        )
        while time.monotonic() < deadline:
            if process.poll() is not None:
                print(
                    f"EDITOR_AB_SCENE_FAIL Unreal Editor exited early with code {process.returncode}; "
                    f"log='{phase_log_path}'.",
                    file=sys.stderr,
                )
                return 1
            log_text = read_log(phase_log_path)
            if launch_runtime_validation.EDITOR_AB_ARTIFACTS_FAILURE_MARKER in log_text:
                failure_line = next(
                    line.strip()
                    for line in log_text.splitlines()
                    if launch_runtime_validation.EDITOR_AB_ARTIFACTS_FAILURE_MARKER in line
                )
                print(f"EDITOR_AB_SCENE_FAIL {failure_line}", file=sys.stderr)
                return 1
            marker_is_complete = (
                launch_runtime_validation.EDITOR_AB_ARTIFACTS_PATTERN.search(log_text)
                if args.artifacts
                else target_marker in log_text
            )
            if marker_is_complete:
                break
            time.sleep(0.25)
        else:
            print(
                f"EDITOR_AB_SCENE_FAIL missing marker "
                f"{target_marker!r}; "
                f"log='{phase_log_path}'.",
                file=sys.stderr,
            )
            return 1

        effective_reflection_bounces = max(
            1,
            min(args.reflection_bounces, 64),
        )
        scene_values = launch_runtime_validation.validate_editor_scene_ready(
            log_text,
            expected_direct_preset=args.direct_preset,
            expected_reflection_environment=args.reflection_environment,
            expected_distance_cm=200,
            expected_air_absorption_profile="default",
            expected_reflection_bounces=effective_reflection_bounces,
        )
        artifact_values: dict[str, object] | None = None
        if args.artifacts:
            artifact_values = (
                launch_runtime_validation.validate_editor_ab_artifacts_marker(
                    log_text,
                    expected_direct_preset=args.direct_preset,
                    expected_reflection_environment=args.reflection_environment,
                    expected_reflection_bounces=effective_reflection_bounces,
                )
            )

        scene_line = next(
            line.strip()
            for line in log_text.splitlines()
            if launch_runtime_validation.EDITOR_VISIBLE_SCENE_MARKER in line
        )
        artifact_line = None
        if args.artifacts:
            artifact_line = next(
                line.strip()
                for line in log_text.splitlines()
                if launch_runtime_validation.EDITOR_AB_ARTIFACTS_MARKER in line
            )

        runtime_window: tuple[int, int, str] | None = None
        window_deadline = time.monotonic() + 15.0
        while time.monotonic() < window_deadline:
            candidates = [
                window
                for window in visible_unreal_windows()
                if window[1] == process.pid and window[1] not in existing_process_ids
            ]
            candidates = [
                window
                for window in candidates
                if client_bounds(window[0])[2] >= 640
                and client_bounds(window[0])[3] >= 360
            ]
            if candidates:
                runtime_window = max(
                    candidates,
                    key=lambda window: client_bounds(window[0])[2]
                    * client_bounds(window[0])[3],
                )
                break
            time.sleep(0.25)

        if runtime_window is None:
            print(
                f"EDITOR_AB_SCENE_FAIL no visible main Editor window for pid={process.pid}; "
                f"log='{phase_log_path}'.",
                file=sys.stderr,
            )
            return 1

        window_handle, process_id, title = runtime_window
        width, height, non_black_ratio, mean_luma, luma_stddev = capture_metrics(
            window_handle,
            screenshot_path,
        )
        print(f"EDITOR_AB_SCENE_MARKER {scene_line}")
        if artifact_line is not None:
            print(f"EDITOR_AB_ARTIFACT_MARKER {artifact_line}")
        print(
            "EDITOR_AB_SCENE_METRICS "
            f"pid={process_id} title={title!r} size={width}x{height} "
            f"non_black_ratio={non_black_ratio:.4f} mean_luma={mean_luma:.2f} "
            f"luma_stddev={luma_stddev:.2f} screenshot='{screenshot_path}'"
        )
        if non_black_ratio < 0.10 or luma_stddev < 8.0:
            print(
                "EDITOR_AB_SCENE_FAIL Editor window is black or lacks a visibly rendered scene.",
                file=sys.stderr,
            )
            return 1

        result_json_path = args.result_json
        if result_json_path is not None and not result_json_path.is_absolute():
            result_json_path = repo_root / result_json_path
        payload = {
            "schema_version": 1,
            "passed": True,
            "scene": scene_values,
            "artifacts": artifact_values if args.artifacts else None,
            "image_metrics": {
                "width": width,
                "height": height,
                "non_black_ratio": non_black_ratio,
                "mean_luma": mean_luma,
                "luma_stddev": luma_stddev,
            },
            "screenshot": str(screenshot_path),
            "log": str(phase_log_path),
        }
        if result_json_path is not None:
            result_json_path.parent.mkdir(parents=True, exist_ok=True)
            temporary_path = result_json_path.with_suffix(
                result_json_path.suffix + ".tmp"
            )
            temporary_path.write_text(
                json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            temporary_path.replace(result_json_path)

        print("EDITOR_AB_SCENE_PASS Editor contains the actual A/B scene and Bake UI.")
        return 0
    finally:
        if process.poll() is None:
            stop_process(process.pid)


if __name__ == "__main__":
    raise SystemExit(main())
