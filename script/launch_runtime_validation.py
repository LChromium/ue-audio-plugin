from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_ENGINE_ROOT = Path(r"C:\Projects\ZeroEngine")
DEFAULT_PROJECT_PATH = Path(r"C:\Projects\MyProject\MyProject.uproject")
DEFAULT_GAME_SECONDS = 15.0
DEFAULT_EDITOR_GRACE_SECONDS = 8.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine-root", type=Path, default=DEFAULT_ENGINE_ROOT)
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT_PATH)
    parser.add_argument("--game-seconds", type=float, default=DEFAULT_GAME_SECONDS)
    parser.add_argument("--editor-grace-seconds", type=float, default=DEFAULT_EDITOR_GRACE_SECONDS)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def build_editor_path(engine_root: Path) -> Path:
    return engine_root / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"


def build_game_command(editor_exe: Path, project_path: Path) -> list[str]:
    return [
        str(editor_exe),
        str(project_path),
        "-game",
        "-NoSplash",
        "-NoSound",
    ]


def build_editor_command(editor_exe: Path, project_path: Path) -> list[str]:
    return [
        str(editor_exe),
        str(project_path),
        "-NoSplash",
        "-NoSound",
    ]


def ensure_paths(editor_exe: Path, project_path: Path) -> None:
    if not editor_exe.exists():
        raise RuntimeError(f"UnrealEditor.exe does not exist: {editor_exe}")
    if project_path.suffix.lower() != ".uproject":
        raise RuntimeError(f"Project path must point to a .uproject file: {project_path}")
    if not project_path.exists():
        raise RuntimeError(f"Project file does not exist: {project_path}")


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
    dry_run: bool,
) -> None:
    command = build_game_command(editor_exe, project_path)
    print("\n=== Launch Game Validation ===")
    print(" ".join(command))
    if dry_run:
        return

    process = subprocess.Popen(command)
    try:
        time.sleep(game_seconds)
        exit_code = process.poll()
        if exit_code is not None and exit_code != 0:
            raise RuntimeError(f"Game validation exited early with exit code {exit_code}.")
    finally:
        stop_process(process)


def start_editor_for_user(
    editor_exe: Path,
    project_path: Path,
    editor_grace_seconds: float,
    dry_run: bool,
) -> None:
    command = build_editor_command(editor_exe, project_path)
    print("\n=== Launch Editor For User ===")
    print(" ".join(command))
    if dry_run:
        return

    creation_flags = 0
    if sys.platform == "win32":
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS

    process = subprocess.Popen(command, creationflags=creation_flags)
    time.sleep(editor_grace_seconds)

    exit_code = process.poll()
    if exit_code is not None and exit_code != 0:
        raise RuntimeError(f"Editor exited early with exit code {exit_code}.")

    print("Editor launched and left running for manual verification.")


def main() -> int:
    args = parse_args()

    engine_root = args.engine_root.resolve()
    project_path = args.project.resolve()
    editor_exe = build_editor_path(engine_root)

    ensure_paths(editor_exe, project_path)

    print(f"Engine root          : {engine_root}")
    print(f"Project file         : {project_path}")
    print(f"Editor executable    : {editor_exe}")
    print(f"Game validation secs : {args.game_seconds}")
    print(f"Editor grace secs    : {args.editor_grace_seconds}")
    print(f"Dry run              : {args.dry_run}")

    start_game_then_kill(
        editor_exe=editor_exe,
        project_path=project_path,
        game_seconds=args.game_seconds,
        dry_run=args.dry_run,
    )
    start_editor_for_user(
        editor_exe=editor_exe,
        project_path=project_path,
        editor_grace_seconds=args.editor_grace_seconds,
        dry_run=args.dry_run,
    )

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
