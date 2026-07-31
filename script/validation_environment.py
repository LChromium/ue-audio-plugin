from __future__ import annotations

import json
import os
from pathlib import Path


ENGINE_ENV_NAMES = ("UERAYTRACINGAUDIO_ENGINE_ROOT", "UE_ENGINE_ROOT")
PROJECT_ENV_NAMES = ("UERAYTRACINGAUDIO_PROJECT", "UE_PROJECT_PATH")


def _first_environment_path(names: tuple[str, ...]) -> Path | None:
    for name in names:
        value = os.environ.get(name)
        if value:
            return Path(value).expanduser()
    return None


def _is_engine_root(path: Path) -> bool:
    return (
        (path / "Engine" / "Build" / "BatchFiles" / "Build.bat").is_file()
        and (path / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe").is_file()
    )


def _engine_version_key(path: Path) -> tuple[int, ...]:
    version_text = path.name.removeprefix("UE_")
    parts: list[int] = []
    for part in version_text.split("."):
        try:
            parts.append(int(part))
        except ValueError:
            parts.append(-1)
    return tuple(parts)


def discover_engine_roots() -> list[Path]:
    candidates: list[Path] = []
    for drive_letter in "CDEFG":
        epic_root = Path(f"{drive_letter}:/Program Files/Epic Games")
        if not epic_root.is_dir():
            continue
        candidates.extend(path for path in epic_root.glob("UE_*") if _is_engine_root(path))
    return sorted(candidates, key=_engine_version_key, reverse=True)


def resolve_engine_root(explicit: Path | None) -> Path:
    candidate = explicit or _first_environment_path(ENGINE_ENV_NAMES)
    if candidate is not None:
        resolved = candidate.resolve()
        if not _is_engine_root(resolved):
            raise RuntimeError(f"Unreal Engine root is invalid: {resolved}")
        return resolved

    discovered = discover_engine_roots()
    if discovered:
        return discovered[0].resolve()

    environment_hint = " or ".join(ENGINE_ENV_NAMES)
    raise RuntimeError(
        "Could not discover an Unreal Engine installation. "
        f"Pass --engine-root or set {environment_hint}."
    )


def _project_enables_plugin(project_path: Path, plugin_name: str) -> bool:
    try:
        payload = json.loads(project_path.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return False

    for plugin in payload.get("Plugins", []):
        if plugin.get("Name") == plugin_name and plugin.get("Enabled", False):
            return True
    return False


def discover_project_paths(repo_root: Path, plugin_name: str) -> list[Path]:
    candidates = [
        path
        for path in repo_root.glob("TestProject/**/*.uproject")
        if _project_enables_plugin(path, plugin_name)
    ]
    return sorted(candidates, key=lambda path: path.stat().st_mtime_ns, reverse=True)


def resolve_project_path(explicit: Path | None, repo_root: Path, plugin_name: str) -> Path:
    candidate = explicit or _first_environment_path(PROJECT_ENV_NAMES)
    if candidate is not None:
        resolved = candidate.resolve()
        if resolved.suffix.lower() != ".uproject" or not resolved.is_file():
            raise RuntimeError(f"Project path must point to an existing .uproject file: {resolved}")
        return resolved

    discovered = discover_project_paths(repo_root, plugin_name)
    if discovered:
        return discovered[0].resolve()

    environment_hint = " or ".join(PROJECT_ENV_NAMES)
    raise RuntimeError(
        f"Could not discover a .uproject that enables {plugin_name}. "
        f"Pass --project or set {environment_hint}."
    )
