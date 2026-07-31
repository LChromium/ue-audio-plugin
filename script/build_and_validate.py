from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import sync_plugin_to_test_project
import validate_audio_realtime_safety
import validation_environment


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", type=Path)
    parser.add_argument("--project", type=Path)
    parser.add_argument("--target", type=str)
    parser.add_argument("--configuration", type=str, default="Development")
    parser.add_argument("--platform", type=str, default="Win64")
    parser.add_argument(
        "--max-parallel-actions",
        type=int,
        default=4,
        help="Limit UnrealBuildTool parallelism to avoid exhausting memory during the standalone plugin build.",
    )
    parser.add_argument("--skip-sync", action="store_true")
    parser.add_argument("--skip-project-build", action="store_true")
    parser.add_argument("--skip-plugin-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def build_command(
    build_bat: Path,
    target: str,
    platform: str,
    configuration: str,
    project_path: Path,
    max_parallel_actions: int,
    plugin_path: Path | None = None,
    plugin_name: str | None = None,
) -> list[str]:
    command = [
        str(build_bat),
        target,
        platform,
        configuration,
        f"-Project={project_path}",
        "-NoXGE",
        "-WaitMutex",
        "-FromMsBuild",
        f"-MaxParallelActions={max(1, max_parallel_actions)}",
    ]
    if plugin_path is not None:
        command.append(f"-plugin={plugin_path}")
    if plugin_name is not None:
        command.append(f"-BuildPlugin={plugin_name}")
    return command


def run_step(title: str, command: list[str], cwd: Path, dry_run: bool) -> None:
    print(f"\n=== {title} ===")
    print(" ".join(command))
    if dry_run:
        return
    completed = subprocess.run(command, cwd=cwd, shell=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{title} failed with exit code {completed.returncode}.")


def main() -> int:
    args = parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    plugin_file = sync_plugin_to_test_project.find_plugin_file(repo_root)
    project_path = validation_environment.resolve_project_path(args.project, repo_root, plugin_file.stem)
    engine_root = validation_environment.resolve_engine_root(args.engine_root)
    build_bat = engine_root / "Engine" / "Build" / "BatchFiles" / "Build.bat"

    if project_path.suffix.lower() != ".uproject":
        raise RuntimeError(f"Project path must point to a .uproject file: {project_path}")
    if not project_path.exists():
        raise RuntimeError(f"Project file does not exist: {project_path}")
    if not build_bat.exists():
        raise RuntimeError(f"Build.bat does not exist: {build_bat}")

    plugin_name = plugin_file.stem
    target = args.target or f"{project_path.stem}Editor"

    print(f"Repository root : {repo_root}")
    print(f"Engine root     : {engine_root}")
    print(f"Project file    : {project_path}")
    print(f"Plugin file     : {plugin_file}")
    print(f"Target          : {target}")
    print(f"Platform        : {args.platform}")
    print(f"Configuration   : {args.configuration}")
    print(f"Parallel actions: {max(1, args.max_parallel_actions)}")
    print(f"Dry run         : {args.dry_run}")

    realtime_report = validate_audio_realtime_safety.validate_repo(
        repo_root
    )
    print(
        "Audio RT audit  : "
        f"{realtime_report.audited_functions} functions, "
        f"{realtime_report.audited_bodies} bodies, "
        "0 forbidden operations"
    )

    if not args.skip_sync:
        destination_root = project_path.parent / "Plugins" / plugin_name
        print(f"Sync destination: {destination_root}")
        sync_plugin_to_test_project.sync_repo(repo_root, destination_root, args.dry_run)
        print("Sync complete.")

    if not args.skip_project_build:
        project_build_command = build_command(
            build_bat=build_bat,
            target=target,
            platform=args.platform,
            configuration=args.configuration,
            project_path=project_path,
            max_parallel_actions=args.max_parallel_actions,
        )
        run_step("Build Project", project_build_command, repo_root, args.dry_run)

    if not args.skip_plugin_build:
        plugin_build_command = build_command(
            build_bat=build_bat,
            target=target,
            platform=args.platform,
            configuration=args.configuration,
            project_path=project_path,
            max_parallel_actions=args.max_parallel_actions,
            plugin_path=plugin_file,
            plugin_name=plugin_name,
        )
        run_step("Build Plugin", plugin_build_command, repo_root, args.dry_run)

    print("\nBuild and validation complete.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
