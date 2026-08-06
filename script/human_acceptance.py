from __future__ import annotations

import json
from pathlib import Path
from typing import Any


class HumanAcceptanceError(RuntimeError):
    """Raised when a listening-acceptance record is not authoritative."""


_SCHEMA_VERSION = 3
_REQUIRED_PREVIEW_MODES = ("Reference", "Direct", "Wet", "Full")
_HUMAN_CONFIRMATION_FIELDS = (
    "recognizable_direct",
    "audible_wet_full_difference",
    "moving_occlusion_continuity",
    "mode_switching_continuity",
    "environment_difference",
)
_PROVENANCE_FIELDS = (
    "input_asset",
    "source_actor",
    "listener_actor",
    "scene_signature",
    "direct_preset",
    "reflection_environment",
)
_MIRRORED_BOOL_FIELDS = (
    "automatic_checks_passed",
    "modes_are_distinct",
)
_MIRRORED_NUMBER_FIELDS = (
    "direct_to_reference_rms_ratio",
    "wet_to_reference_rms_ratio",
    "direct_wet_normalized_difference",
)


def _load_json_object(path: Path, label: str) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise HumanAcceptanceError(f"{label} is not readable JSON: {path}") from exc
    if not isinstance(payload, dict):
        raise HumanAcceptanceError(f"{label} must contain a JSON object: {path}")
    return payload


def _require_string(payload: dict[str, Any], field: str, label: str) -> str:
    value = payload.get(field)
    if not isinstance(value, str) or not value.strip():
        raise HumanAcceptanceError(f"{label} requires non-empty {field}")
    return value


def _require_bool(payload: dict[str, Any], field: str, label: str) -> bool:
    value = payload.get(field)
    if not isinstance(value, bool):
        raise HumanAcceptanceError(f"{label} requires boolean {field}")
    return value


def _require_number(payload: dict[str, Any], field: str, label: str) -> int | float:
    value = payload.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise HumanAcceptanceError(f"{label} requires numeric {field}")
    return value


def _resolve_manifest_path(record_path: Path, value: str) -> Path:
    manifest_path = Path(value)
    if not manifest_path.is_absolute():
        manifest_path = record_path.parent / manifest_path
    try:
        return manifest_path.resolve(strict=True)
    except OSError as exc:
        raise HumanAcceptanceError(
            f"comparison manifest does not exist: {manifest_path}"
        ) from exc


def validate_human_acceptance_record(
    path: Path,
    *,
    require_pass: bool = False,
) -> dict[str, Any]:
    record_path = path.resolve()
    record = _load_json_object(record_path, "human acceptance record")

    schema_version = record.get("schema_version")
    if isinstance(schema_version, bool) or schema_version != _SCHEMA_VERSION:
        raise HumanAcceptanceError(
            f"human acceptance record schema_version must be {_SCHEMA_VERSION}"
        )

    _require_string(record, "recorded_at_utc", "human acceptance record")
    manifest_value = _require_string(
        record,
        "comparison_manifest",
        "human acceptance record",
    )
    target_device = record.get("target_listening_device")
    if not isinstance(target_device, str) or not target_device.strip():
        raise HumanAcceptanceError(
            "human acceptance record requires a target listening device"
        )
    notes = record.get("listening_notes")
    if not isinstance(notes, str):
        raise HumanAcceptanceError(
            "human acceptance record requires string listening_notes"
        )

    human_passed = _require_bool(
        record,
        "human_listening_passed",
        "human acceptance record",
    )
    previewed_modes = record.get("previewed_modes")
    if (
        not isinstance(previewed_modes, list)
        or not previewed_modes
        or any(not isinstance(mode, str) for mode in previewed_modes)
    ):
        raise HumanAcceptanceError(
            "human acceptance record requires non-empty string previewed_modes"
        )
    if len(set(previewed_modes)) != len(previewed_modes):
        raise HumanAcceptanceError(
            "human acceptance record previewed_modes must not contain duplicates"
        )
    unknown_modes = sorted(set(previewed_modes) - set(_REQUIRED_PREVIEW_MODES))
    if unknown_modes:
        raise HumanAcceptanceError(
            "human acceptance record contains unknown previewed mode(s): "
            + ", ".join(unknown_modes)
        )
    if human_passed:
        missing_modes = [
            mode for mode in _REQUIRED_PREVIEW_MODES if mode not in previewed_modes
        ]
        if missing_modes:
            raise HumanAcceptanceError(
                "passing human acceptance record is missing preview mode(s): "
                + ", ".join(missing_modes)
            )
    last_previewed_mode = _require_string(
        record,
        "last_previewed_mode",
        "human acceptance record",
    )
    if last_previewed_mode not in previewed_modes:
        raise HumanAcceptanceError(
            "last_previewed_mode must be present in previewed_modes"
        )

    human_confirmations = record.get("human_confirmations")
    if not isinstance(human_confirmations, dict):
        raise HumanAcceptanceError(
            "human acceptance record requires object human_confirmations"
        )
    for field in _HUMAN_CONFIRMATION_FIELDS:
        _require_bool(
            human_confirmations,
            field,
            "human acceptance human_confirmations",
        )
    if human_passed:
        missing_confirmations = [
            field
            for field in _HUMAN_CONFIRMATION_FIELDS
            if not human_confirmations[field]
        ]
        if missing_confirmations:
            raise HumanAcceptanceError(
                "passing human acceptance record is missing required human "
                "confirmation(s): " + ", ".join(missing_confirmations)
            )

    manifest_path = _resolve_manifest_path(record_path, manifest_value)
    manifest = _load_json_object(manifest_path, "comparison manifest")
    for field in _PROVENANCE_FIELDS:
        record_value = _require_string(record, field, "human acceptance record")
        manifest_field_value = _require_string(manifest, field, "comparison manifest")
        if record_value != manifest_field_value:
            raise HumanAcceptanceError(
                f"human acceptance {field} does not match comparison manifest"
            )
    for field in _MIRRORED_BOOL_FIELDS:
        record_value = _require_bool(record, field, "human acceptance record")
        manifest_field_value = _require_bool(manifest, field, "comparison manifest")
        if record_value != manifest_field_value:
            raise HumanAcceptanceError(
                f"human acceptance {field} does not match comparison manifest"
            )
    for field in _MIRRORED_NUMBER_FIELDS:
        record_value = _require_number(record, field, "human acceptance record")
        manifest_field_value = _require_number(manifest, field, "comparison manifest")
        if record_value != manifest_field_value:
            raise HumanAcceptanceError(
                f"human acceptance {field} does not match comparison manifest"
            )
    _require_string(record, "requirement", "human acceptance record")

    if require_pass:
        if not human_passed:
            raise HumanAcceptanceError(
                "final acceptance requires a passing verdict"
            )
        if not record["automatic_checks_passed"]:
            raise HumanAcceptanceError(
                "final acceptance requires automatic_checks_passed=true"
            )
        if not record["modes_are_distinct"]:
            raise HumanAcceptanceError(
                "final acceptance requires modes_are_distinct=true"
            )

    return record
