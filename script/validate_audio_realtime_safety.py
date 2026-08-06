from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class AuditSpec:
    relative_path: str
    qualified_name: str
    minimum_bodies: int = 1
    allow_bounded_resize: bool = False


@dataclass(frozen=True)
class AuditViolation:
    relative_path: str
    qualified_name: str
    category: str
    token: str


@dataclass(frozen=True)
class AuditReport:
    audited_functions: int
    audited_bodies: int
    audited_lines: int
    violations: tuple[AuditViolation, ...]


AUDIO_CALLBACK_SPECS = (
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp",
        "FUERayTracingAudioOcclusionPlugin::ProcessAudio",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp",
        "ResetOcclusionSourceState",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp",
        "FUERayTracingAudioOcclusionPlugin::OnReleaseSource",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.cpp",
        "FUERayTracingAudioThreeBandAirAbsorption::Reset",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.cpp",
        "FUERayTracingAudioThreeBandAirAbsorption::CanProcess",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.cpp",
        "FUERayTracingAudioThreeBandAirAbsorption::ProcessSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioSpatialization.cpp",
        "FUERayTracingAudioSpatializationPlugin::ProcessAudio",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioSpatialization.cpp",
        "FUERayTracingAudioSpatializationPlugin::OnReleaseSource",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioSimulationSnapshot.cpp",
        "FUERayTracingAudioSimulationSnapshotRegistry::FindEntry",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioSimulationSnapshot.cpp",
        "FUERayTracingAudioSimulationSnapshotRegistry::Read",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioSimulationSnapshot.cpp",
        "FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::Release",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioIndirectRenderer::ConfigurePrepared",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioIndirectRenderer::ProcessSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioEarlyReflectionRenderer::ConfigurePrepared",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioEarlyReflectionRenderer::ProcessSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioEarlyReflectionRenderer::ReleasePreparedStates",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioLateReverbRenderer::Configure",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioLateReverbRenderer::ProcessSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "FUERayTracingAudioLateReverbRenderer::ReadDelayedSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectAudioBridge.cpp",
        "FUERayTracingAudioIndirectAudioBridge::BeginWriteInternal",
        allow_bounded_resize=True,
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectAudioBridge.cpp",
        "FUERayTracingAudioIndirectAudioBridge::EndWrite",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectAudioBridge.cpp",
        "FUERayTracingAudioIndirectAudioBridge::Consume",
        minimum_bodies=2,
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectAudioBridge.cpp",
        "FUERayTracingAudioIndirectAudioBridge::ConfigureConvolver",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectAudioBridge.cpp",
        "FUERayTracingAudioIndirectAudioBridge::ReleaseConvolver",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPartitionedConvolver::ProcessSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPartitionedConvolver::ProcessBlock",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPreparedConvolverState::ProcessSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPreparedCrossfadingConvolver::CanAcceptTransition",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPreparedCrossfadingConvolver::StartTransition",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPreparedCrossfadingConvolver::AdoptPreparedState",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPreparedCrossfadingConvolver::AdoptSilence",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPreparedCrossfadingConvolver::ProcessSample",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioConvolution.cpp",
        "FUERayTracingAudioPreparedCrossfadingConvolver::DetachAllStates",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp",
        "FUERayTracingAudioAudioDiagnostics::RecordBuffer",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp",
        "FUERayTracingAudioAudioDiagnostics::IsEnabledFor",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp",
        "FUERayTracingAudioAudioDiagnostics::RecordDirectBuffer",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp",
        "FUERayTracingAudioAudioDiagnosticsInternal::CaptureTarget",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp",
        "FUERayTracingAudioAudioDiagnosticsInternal::RecordDirectBuffer",
    ),
    AuditSpec(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp",
        "FUERayTracingAudioAudioDiagnostics::RecordFinalOutput",
    ),
)


FORBIDDEN_PATTERNS = (
    (
        "lock",
        re.compile(
            r"\b(?:FScopeLock|FRWScopeLock|FReadScopeLock|FWriteScopeLock|"
            r"FCriticalSection|FRWLock)\b|(?:\.|->)(?:Lock|ReadLock|WriteLock)\s*\("
        ),
    ),
    (
        "heap",
        re.compile(
            r"\b(?:MakeShared|MakeUnique|Malloc|Realloc|Free)\s*(?:<|\()|"
            r"\bnew\s+|\bdelete\b|"
            r"(?:\.|->)(?:Add|AddDefaulted|Emplace|Insert|Reserve|"
            r"SetNum|SetNumZeroed|SetNumUninitialized)\s*\("
        ),
    ),
    (
        "shared-ownership",
        re.compile(r"\b(?:TSharedPtr|TSharedRef|MakeShareable)\b"),
    ),
    (
        "blocking",
        re.compile(
            r"\b(?:FlushRenderingCommands|WaitForCompletion|WaitUntilTaskCompletes|"
            r"Sleep|ConditionalSleep)\s*\("
        ),
    ),
    (
        "uobject",
        re.compile(
            r"\b(?:GetWorld|GetManager|LoadObject|StaticLoadObject|FindObject|"
            r"GetDefault|GetMutableDefault)\s*(?:<|\()"
        ),
    ),
    (
        "logging",
        re.compile(r"\b(?:UE_LOG|UE_CLOG|UE_LOGFMT|UE_CLOGFMT)\s*\("),
    ),
)

COMMENTS_AND_LITERALS = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)


def strip_comments_and_literals(source: str) -> str:
    return COMMENTS_AND_LITERALS.sub(
        lambda match: "\n" * match.group(0).count("\n"),
        source,
    )


def _selector_pattern(qualified_name: str) -> re.Pattern[str]:
    pieces = tuple(re.escape(piece) for piece in qualified_name.split("::"))
    return re.compile(r"::\s*".join(pieces) + r"\s*\(")


FUNCTION_SELECTOR = re.compile(
    r"(?P<qualified>(?:~?[A-Za-z_]\w*::)*~?[A-Za-z_]\w*)\s*\("
)

CALL_SELECTOR = re.compile(r"(?<![:~\w.>])(?P<name>[A-Za-z_]\w*)\s*\(")

NON_TRANSITIVE_CALLS = frozenset(
    {
        "alignas",
        "alignof",
        "case",
        "catch",
        "decltype",
        "delete",
        "for",
        "if",
        "new",
        "return",
        "sizeof",
        "static_cast",
        "reinterpret_cast",
        "const_cast",
        "dynamic_cast",
        "switch",
        "throw",
        "while",
    }
)


def _find_matching(
    source: str,
    opening_index: int,
    opening: str,
    closing: str,
) -> int:
    depth = 0
    in_string: str | None = None
    escaped = False
    index = opening_index
    while index < len(source):
        character = source[index]
        if in_string is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == in_string:
                in_string = None
        elif character in ('"', "'"):
            in_string = character
        elif source.startswith("//", index):
            newline = source.find("\n", index + 2)
            index = len(source) if newline < 0 else newline
            continue
        elif source.startswith("/*", index):
            comment_end = source.find("*/", index + 2)
            index = len(source) if comment_end < 0 else comment_end + 2
            continue
        elif character == opening:
            depth += 1
        elif character == closing:
            depth -= 1
            if depth == 0:
                return index
        index += 1
    raise RuntimeError(
        f"Unbalanced {opening}{closing} pair at source offset {opening_index}."
    )


def extract_function_bodies(source: str, qualified_name: str) -> tuple[str, ...]:
    bodies: list[str] = []
    for match in _selector_pattern(qualified_name).finditer(source):
        opening_parenthesis = source.find("(", match.start())
        closing_parenthesis = _find_matching(
            source,
            opening_parenthesis,
            "(",
            ")",
        )
        opening_brace = source.find("{", closing_parenthesis + 1)
        semicolon = source.find(";", closing_parenthesis + 1)
        if opening_brace < 0 or (semicolon >= 0 and semicolon < opening_brace):
            continue
        closing_brace = _find_matching(source, opening_brace, "{", "}")
        bodies.append(source[opening_brace + 1 : closing_brace])
    return tuple(bodies)


def extract_named_function_bodies(source: str) -> dict[str, tuple[str, ...]]:
    bodies_by_name: dict[str, list[str]] = {}
    for match in FUNCTION_SELECTOR.finditer(source):
        opening_parenthesis = source.find("(", match.start())
        try:
            closing_parenthesis = _find_matching(
                source,
                opening_parenthesis,
                "(",
                ")",
            )
        except RuntimeError:
            continue

        cursor = closing_parenthesis + 1
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1
        if cursor < len(source) and source[cursor] == ":":
            opening_brace = source.find("{", cursor + 1)
            semicolon = source.find(";", cursor + 1)
        else:
            opening_brace = source.find("{", cursor)
            semicolon = source.find(";", cursor)
        if opening_brace < 0 or (semicolon >= 0 and semicolon < opening_brace):
            continue

        try:
            closing_brace = _find_matching(source, opening_brace, "{", "}")
        except RuntimeError:
            continue

        qualified_name = match.group("qualified")
        body = source[opening_brace + 1 : closing_brace]
        bodies_by_name.setdefault(qualified_name, []).append(body)
        unqualified_name = qualified_name.split("::")[-1]
        if unqualified_name != qualified_name:
            bodies_by_name.setdefault(unqualified_name, []).append(body)

    return {
        name: tuple(bodies)
        for name, bodies in bodies_by_name.items()
    }


def extract_same_file_calls(body: str) -> tuple[str, ...]:
    stripped_body = strip_comments_and_literals(body)
    calls: list[str] = []
    for match in CALL_SELECTOR.finditer(stripped_body):
        name = match.group("name")
        if name not in NON_TRANSITIVE_CALLS:
            calls.append(name)
    return tuple(calls)


def audit_body(
    relative_path: str,
    qualified_name: str,
    body: str,
    *,
    allow_bounded_resize: bool,
) -> tuple[AuditViolation, ...]:
    inspected_body = strip_comments_and_literals(body)
    violations: list[AuditViolation] = []
    if allow_bounded_resize:
        bounded_resize = re.compile(
            r"State\.StereoWet\.SetNumUninitialized\s*\(\s*"
            r"NumFrames\s*,\s*EAllowShrinking::No\s*\)\s*;"
        )
        resize_matches = tuple(bounded_resize.finditer(inspected_body))
        guard_present = (
            "NumFrames > MaxFramesPerCallback" in inspected_body
            and "State.StereoWet.Max()" in inspected_body
            and "return " in inspected_body
        )
        if len(resize_matches) != 1 or not guard_present:
            violations.append(
                AuditViolation(
                    relative_path,
                    qualified_name,
                    "bounded-resize-invariant",
                    "StereoWet.SetNumUninitialized",
                )
            )
        inspected_body = bounded_resize.sub(
            "/* audited bounded resize */",
            inspected_body,
        )

    for category, pattern in FORBIDDEN_PATTERNS:
        for match in pattern.finditer(inspected_body):
            violations.append(
                AuditViolation(
                    relative_path,
                    qualified_name,
                    category,
                    match.group(0),
                )
            )
    return tuple(violations)


def audit_body_and_same_file_helpers(
    relative_path: str,
    qualified_name: str,
    body: str,
    *,
    allow_bounded_resize: bool,
    same_file_bodies: dict[str, tuple[str, ...]],
    visited: set[tuple[str, int]],
) -> tuple[AuditViolation, ...]:
    violations = list(
        audit_body(
            relative_path,
            qualified_name,
            body,
            allow_bounded_resize=allow_bounded_resize,
        )
    )
    for call_name in extract_same_file_calls(body):
        for called_body in same_file_bodies.get(call_name, ()):
            visit_key = (call_name, id(called_body))
            if visit_key in visited:
                continue
            visited.add(visit_key)
            violations.extend(
                audit_body_and_same_file_helpers(
                    relative_path,
                    call_name,
                    called_body,
                    allow_bounded_resize=False,
                    same_file_bodies=same_file_bodies,
                    visited=visited,
                )
            )
    return tuple(violations)


def audit_repo(
    repo_root: Path,
    specs: tuple[AuditSpec, ...] | None = None,
) -> AuditReport:
    repo_root = repo_root.resolve()
    audit_specs = AUDIO_CALLBACK_SPECS if specs is None else specs
    violations: list[AuditViolation] = []
    audited_bodies = 0
    audited_lines = 0
    cache: dict[str, str] = {}
    function_cache: dict[str, dict[str, tuple[str, ...]]] = {}
    for spec in audit_specs:
        path = repo_root / spec.relative_path
        if not path.is_file():
            violations.append(
                AuditViolation(
                    spec.relative_path,
                    spec.qualified_name,
                    "missing-file",
                    str(path),
                )
            )
            continue
        source = cache.setdefault(
            spec.relative_path,
            path.read_text(encoding="utf-8"),
        )
        same_file_bodies = function_cache.setdefault(
            spec.relative_path,
            extract_named_function_bodies(source),
        )
        bodies = extract_function_bodies(source, spec.qualified_name)
        if len(bodies) < spec.minimum_bodies:
            violations.append(
                AuditViolation(
                    spec.relative_path,
                    spec.qualified_name,
                    "missing-function",
                    f"found={len(bodies)} expected>={spec.minimum_bodies}",
                )
            )
            continue
        for body in bodies:
            audited_bodies += 1
            audited_lines += body.count("\n") + 1
            violations.extend(
                audit_body_and_same_file_helpers(
                    spec.relative_path,
                    spec.qualified_name,
                    body,
                    allow_bounded_resize=spec.allow_bounded_resize,
                    same_file_bodies=same_file_bodies,
                    visited={(spec.qualified_name, id(body))},
                )
            )

    if specs is not None:
        return AuditReport(
            audited_functions=len(audit_specs),
            audited_bodies=audited_bodies,
            audited_lines=audited_lines,
            violations=tuple(violations),
        )

    bridge_source = cache.get(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectAudioBridge.cpp",
        "",
    )
    if "State.StereoWet.Reserve(MaxFramesPerCallback);" not in bridge_source:
        violations.append(
            AuditViolation(
                "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectAudioBridge.cpp",
                "FUERayTracingAudioIndirectAudioBridge::Initialize",
                "missing-preallocation",
                "StereoWet.Reserve(MaxFramesPerCallback)",
            )
        )

    renderer_source = cache.get(
        "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
        "",
    )
    legacy_owning_symbols = (
        "TestBakedLeftConvolver",
        "TestBakedRightConvolver",
        "TestRealtimeLeftConvolver",
        "TestRealtimeRightConvolver",
        "bUseControlThreadTestPath",
    )
    for symbol in legacy_owning_symbols:
        if symbol in renderer_source:
            violations.append(
                AuditViolation(
                    "Source/UERayTracingAudio/Private/Audio/UERayTracingAudioIndirectRenderer.cpp",
                    "legacy-owning-renderer-path",
                    "shared-ownership",
                    symbol,
                )
            )

    rhi_relative_path = (
        "Source/UERayTracingAudioSDK/Private/RayTracing/"
        "UERayTracingAudioRayTracingDevice.cpp"
    )
    rhi_path = repo_root / rhi_relative_path
    rhi_source = (
        rhi_path.read_text(encoding="utf-8")
        if rhi_path.is_file()
        else ""
    )
    weak_query_backrefs = (
        "TWeakPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> Query;",
        "TWeakPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe> Query;",
    )
    for invariant in weak_query_backrefs:
        if invariant not in rhi_source:
            violations.append(
                AuditViolation(
                    rhi_relative_path,
                    "async-readback-state",
                    "strong-cycle-risk",
                    invariant,
                )
            )
    forbidden_query_backrefs = (
        "TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> Query;",
        "TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe> Query;",
    )
    for invariant in forbidden_query_backrefs:
        if invariant in rhi_source:
            violations.append(
                AuditViolation(
                    rhi_relative_path,
                    "async-readback-state",
                    "strong-cycle-risk",
                    invariant,
                )
            )
    if "ReadbackState.Reset" in rhi_source:
        violations.append(
            AuditViolation(
                rhi_relative_path,
                "async-readback-state",
                "cross-thread-reset",
                "ReadbackState.Reset",
            )
        )
    for qualified_name in (
        "FUERayTracingAudioAsyncRayQuery::IsComplete",
        "FUERayTracingAudioAsyncEnergyFieldQuery::IsComplete",
    ):
        bodies = extract_function_bodies(rhi_source, qualified_name)
        if len(bodies) != 1:
            violations.append(
                AuditViolation(
                    rhi_relative_path,
                    qualified_name,
                    "missing-function",
                    f"found={len(bodies)} expected=1",
                )
            )
            continue
        publish_observation = bodies[0].find("bReadbackSubmitted.Load()")
        state_read = bodies[0].find("GetPublishedReadbackState()")
        if (
            publish_observation < 0
            or state_read < 0
            or publish_observation > state_read
        ):
            violations.append(
                AuditViolation(
                    rhi_relative_path,
                    qualified_name,
                    "publication-order",
                    "observe bReadbackSubmitted before ReadbackState",
                )
            )

    return AuditReport(
        audited_functions=len(audit_specs),
        audited_bodies=audited_bodies,
        audited_lines=audited_lines,
        violations=tuple(violations),
    )


def validate_repo(repo_root: Path) -> AuditReport:
    report = audit_repo(repo_root)
    if report.violations:
        details = "\n".join(
            f"- {item.category}: {item.relative_path} "
            f"{item.qualified_name}: {item.token}"
            for item in report.violations
        )
        raise RuntimeError(
            "Audio hard-real-time source audit failed:\n"
            + details
        )
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    report = validate_repo(args.repo_root)
    print(
        "Audio hard-real-time source audit: "
        f"passed=1 functions={report.audited_functions} "
        f"bodies={report.audited_bodies} "
        f"lines={report.audited_lines} "
        "lock_ops=0 heap_ops=0 shared_ownership_ops=0 "
        "blocking_ops=0 uobject_ops=0."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
