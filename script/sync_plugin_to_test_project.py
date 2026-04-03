from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


DEFAULT_PROJECT_PATH = Path(r"C:\Projects\MyProject\MyProject.uproject")
EXCLUDED_NAMES = {
    ".git",
    ".vs",
    "__pycache__",
    "Binaries",
    "BuildValidation",
    "DerivedDataCache",
    "Intermediate",
    "Saved",
}
EXCLUDED_SUFFIXES = {".pyc", ".pyo"}


def is_excluded(path: Path) -> bool:
    return path.name in EXCLUDED_NAMES or path.suffix.lower() in EXCLUDED_SUFFIXES


def find_plugin_file(repo_root: Path) -> Path:
    plugin_files = sorted(repo_root.glob("*.uplugin"))
    if len(plugin_files) != 1:
        raise RuntimeError(f"Expected exactly one .uplugin file in {repo_root}, found {len(plugin_files)}.")
    return plugin_files[0]


def ensure_directory(path: Path, dry_run: bool) -> None:
    if dry_run:
        return
    path.mkdir(parents=True, exist_ok=True)


def remove_path(path: Path, dry_run: bool) -> None:
    print(f"REMOVE {path}")
    if dry_run or not path.exists():
        return
    if path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink()


def copy_file(source: Path, destination: Path, dry_run: bool) -> None:
    print(f"COPY   {source} -> {destination}")
    if dry_run:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def sync_directory(source_dir: Path, destination_dir: Path, dry_run: bool) -> None:
    ensure_directory(destination_dir, dry_run)

    source_entries = {entry.name: entry for entry in source_dir.iterdir() if not is_excluded(entry)}
    destination_entries = {entry.name: entry for entry in destination_dir.iterdir() if not is_excluded(entry)} if destination_dir.exists() else {}

    for name, destination_entry in destination_entries.items():
        if name not in source_entries:
            remove_path(destination_entry, dry_run)

    for name, source_entry in source_entries.items():
        destination_entry = destination_dir / name
        if source_entry.is_dir():
            sync_directory(source_entry, destination_entry, dry_run)
        else:
            copy_file(source_entry, destination_entry, dry_run)


def sync_repo(repo_root: Path, destination_root: Path, dry_run: bool) -> None:
    ensure_directory(destination_root, dry_run)

    source_entries = {entry.name: entry for entry in repo_root.iterdir() if not is_excluded(entry)}
    destination_entries = {entry.name: entry for entry in destination_root.iterdir() if not is_excluded(entry)} if destination_root.exists() else {}

    for name, destination_entry in destination_entries.items():
        if name not in source_entries:
            remove_path(destination_entry, dry_run)

    for name, source_entry in source_entries.items():
        destination_entry = destination_root / name
        if source_entry.is_dir():
            sync_directory(source_entry, destination_entry, dry_run)
        else:
            copy_file(source_entry, destination_entry, dry_run)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT_PATH)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parent.parent
    plugin_file = find_plugin_file(repo_root)
    project_path = args.project.resolve()

    if project_path.suffix.lower() != ".uproject":
        raise RuntimeError(f"Project path must point to a .uproject file: {project_path}")
    if not project_path.exists():
        raise RuntimeError(f"Project file does not exist: {project_path}")

    plugin_name = plugin_file.stem
    destination_root = project_path.parent / "Plugins" / plugin_name

    print(f"Repository root : {repo_root}")
    print(f"Project file    : {project_path}")
    print(f"Plugin file     : {plugin_file}")
    print(f"Destination     : {destination_root}")
    print(f"Dry run         : {args.dry_run}")

    sync_repo(repo_root, destination_root, args.dry_run)
    print("Sync complete.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
