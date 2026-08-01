# R3 32-Bounce Reflection Environment Matrix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and prove a hardware-ray-traced, 32-reflection-bounce OpenSpace/NearWall/Enclosed acceptance matrix that emits same-origin MarchingBand Reference/Direct/Wet/Full artifacts, visible screenshots, strict manifests, and environment-correct indirect-audio evidence.

**Architecture:** A single validated Editor command-line value configures both the realtime validation Source and the hardware Bake. Existing tagged fixtures provide 0/1/7 acoustic Geometry; a visible-Editor helper returns strict JSON evidence; a focused Python validation module checks per-environment physics, hardware/CPU agreement, WAV safety, and cross-environment growth without weakening the generic audible-Wet gate.

**Tech Stack:** Unreal Engine 5.7 C++, UE Automation, DX12 Ray Tracing RHI, existing Editor Bake/Offline Renderer, Python 3 standard library (`argparse`, `dataclasses`, `hashlib`, `json`, `math`, `wave`, `unittest`), PowerShell, `uv`.

## Global Constraints

- Modify only `D:\Labs\2602-unreal\ue-audio-plugin`; never edit the synchronized `TestProject/UeVersion1/Plugins/UERayTracingAudio` copy directly.
- Work in `.worktrees/configurable-direct-audio-validation` and preserve unrelated `AGENTS.md`, `.claude/`, `CLAUDE.md`, and `MYPROMPT.md` changes.
- Use `apply_patch` for source, test, plan, and document edits.
- Follow RED → GREEN → REFACTOR; record the intended RED failure before each behavior implementation.
- Use exactly 4096 reflection rays and at most 32 reflected segments. The Direct segment is not a bounce.
- Apply the same effective 32-bounce value to the Editor validation Source and the hardware Bake; retain the legal clamp range `[1, 64]`.
- The only validation input is `/Game/FirstPerson/Audio/MarchingBand.MarchingBand`; do not restore Ravel files or accept generated validation SoundWaves as input.
- OpenSpace must produce physical zero Wet. Never inject a gain floor, synthetic reflection, fixed tail, or test-only reverb.
- Keep the existing generic `automatic_checks_passed` semantics unchanged: false is required for correct OpenSpace zero Wet, while NearWall and Enclosed must remain true.
- Do not modify product DSP merely to satisfy validation. If real hardware evidence exposes a DSP defect, first add a focused regression that reproduces the defect.
- Keep temporary R3 runs in separate Editor processes; stop only validation-owned Editors, never unrelated Unreal processes.
- A screenshot/fixed camera proves visibility only. It does not replace interactive PIE or target-device Human Pass.
- Before every UE build, run the prescribed `uv run script\build_and_validate.py`; do not call UnrealBuildTool directly.
- After the major feature, run `uv run script\launch_runtime_validation.py` and leave its final Editor open for the user.
- If any build, Game, Editor, or validation process crashes, stop normal work and follow `workflow/crash-debugging.md` immediately.
- Update `TODO.md`, `USAGE.md`, `IMPLEMENTATION_STATUS.md`, and `progress_log.md` with actual evidence; do not mark Human Pass from automation.

## Test Command Convention

Run all Python tests with:

```powershell
uv run python -m unittest discover -s script\tests -v
```

After a successful prescribed build, run focused Editor Automation with:

```powershell
$EngineRoot = (uv run python -c "from script.validation_environment import resolve_engine_root; print(resolve_engine_root(None))").Trim()
$Project = (uv run python -c "from pathlib import Path; from script.validation_environment import resolve_project_path; print(resolve_project_path(None, Path.cwd(), 'UERayTracingAudio'))").Trim()
$Log = Join-Path (Split-Path $Project) "Saved\Logs\R3-EditorFixture.log"
& "$EngineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  $Project `
  -unattended -nop4 -NullRHI `
  "-ExecCmds=Automation RunTests UERayTracingAudio.Editor.ValidationFixtureControls;Quit" `
  "-TestExit=Automation Test Queue Empty" `
  "-abslog=$Log"
```

The focused log must contain one successful test completion, zero failed tests, and no `Fatal error`, `Unhandled Exception`, or `Assertion failed`. Run the full `UERayTracingAudio.Editor` and `UERayTracingAudio` groups at the final gate.

## File Map

- Modify `script/launch_runtime_validation.py`: strict environment/bounce command construction and marker validation.
- Modify `script/validate_visible_editor_ab_scene.py`: environment option, strict case validation, and atomic result JSON output.
- Modify `script/tests/test_validation_scripts.py`: Python RED/GREEN coverage for command, scene marker, artifact marker, and result JSON.
- Modify `Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.h`: expose effective fixture bounce provenance.
- Modify `Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.cpp`: clamp and apply the requested bounce to the tagged Source.
- Modify `Source/UERayTracingAudioEditor/Private/UERayTracingAudioEditorModule.cpp`: parse one effective bounce value before scene creation and use it for both Source and Bake.
- Modify `Source/UERayTracingAudioEditor/Private/Tests/UERayTracingAudioOfflineComparisonTests.cpp`: 32-bounce, clamp, and 0/1/7 Geometry Automation coverage.
- Create `script/reflection_environment_matrix.py`: pure manifest/evidence model and R3 physics validation; no process launching.
- Create `script/validate_reflection_environment_matrix.py`: three-Editor orchestration, manifest-only recheck mode, summary output, and strict terminal marker.
- Create `script/tests/test_reflection_environment_matrix.py`: pure per-case, cross-case, artifact, and orchestration tests.
- Modify `TODO.md`, `USAGE.md`, `IMPLEMENTATION_STATUS.md`, and `progress_log.md`: usage, status, and evidence.

---

### Task 1: Strict Python Editor Configuration and Evidence Contract

**Files:**
- Modify: `script/launch_runtime_validation.py:158-214,319-467,637-690,1494-1600`
- Modify: `script/validate_visible_editor_ab_scene.py:13-176`
- Modify: `script/tests/test_validation_scripts.py:129-160,283-516,773-840`

**Interfaces:**
- Produces:

```text
EDITOR_REFLECTION_ENVIRONMENTS = ("enclosed", "open_space", "near_wall")
EDITOR_EXPECTED_GEOMETRY = {"open_space": 0, "near_wall": 1, "enclosed": 7}

def build_editor_command(
    editor_exe: Path,
    project_path: Path,
    log_path: Path,
    source_count: int = 4,
    bake_repeatability: bool = False,
    editor_ab_artifacts: bool = False,
    editor_direct_preset: str = "clear",
    editor_distance_cm: int = 200,
    editor_air_absorption_profile: str = "default",
    editor_reflection_environment: str = "enclosed",
    editor_reflection_bounces: int = 8,
    interactive_runtime: bool = False,
) -> list[str]

def validate_editor_scene_ready(
    log_text: str,
    *,
    expected_direct_preset: str,
    expected_reflection_environment: str,
    expected_distance_cm: int,
    expected_air_absorption_profile: str,
    expected_reflection_bounces: int,
) -> dict[str, object]

def validate_editor_ab_artifacts_marker(
    log_text: str,
    *,
    expected_direct_preset: str,
    expected_reflection_environment: str,
    expected_reflection_bounces: int,
) -> dict[str, object]
```

- `validate_visible_editor_ab_scene.py` consumes these functions and optionally writes one result JSON with `scene`, `artifacts`, `image_metrics`, `screenshot`, and `log`.
- Tasks 3–5 consume that JSON and never scrape non-strict free-form logs.

- [ ] **Step 1: Write failing command and marker tests**

Extend the synthetic scene marker so bounce provenance appears between environment and distance:

```python
marker = (
    "UERayTracingAudioEditor validation scene ready: "
    "source=1 listener=1 geometry=1 lighting=1 bake_ui=1 "
    "direct_preset=clear reflection_environment=near_wall "
    "reflection_bounces=32 source_listener_distance_cm=200.00 "
    "air_absorption_profile=default "
    "air_absorption_per_meter=(0.000200,0.000600,0.001200)."
)
values = launch_runtime_validation.validate_editor_scene_ready(
    marker,
    expected_direct_preset="clear",
    expected_reflection_environment="near_wall",
    expected_distance_cm=200,
    expected_air_absorption_profile="default",
    expected_reflection_bounces=32,
)
self.assertEqual(values["geometry"], 1)
self.assertEqual(values["reflection_bounces"], 32)
```

Add assertions that `build_editor_command(Path("UnrealEditor.exe"), Path("Test.uproject"), Path("Editor.log"), editor_reflection_environment="near_wall", editor_reflection_bounces=32)` includes both exact flags, rejects `warehouse`, clamps `0` to `1`, and clamps `65` to `64`. Add malformed-provenance tests for wrong Geometry count, missing bounce, wrong bounce, and duplicate scene markers. Update `make_editor_ab_artifact_marker` to expose `reflection_rays=4096 reflection_bounces=32` and assert both parsed values.

- [ ] **Step 2: Run the focused Python test and confirm RED**

Run:

```powershell
uv run python script\tests\test_validation_scripts.py
```

Expected: failures because `build_editor_command` has no environment argument, strict patterns have no bounce group, and validators do not enforce environment Geometry counts.

- [ ] **Step 3: Implement command and strict marker parsing**

Add the exact scene pattern fields and validation:

```python
EDITOR_EXPECTED_GEOMETRY = {
    "open_space": 0,
    "near_wall": 1,
    "enclosed": 7,
}

actual_geometry = int(match.group("geometry"))
actual_bounces = int(match.group("reflection_bounces"))
if actual_geometry != EDITOR_EXPECTED_GEOMETRY[expected_reflection_environment]:
    failures.append(
        "fixture geometry count "
        f"({actual_geometry} != "
        f"{EDITOR_EXPECTED_GEOMETRY[expected_reflection_environment]})"
    )
if actual_bounces != expected_reflection_bounces:
    failures.append(
        f"reflection bounces ({actual_bounces} != {expected_reflection_bounces})"
    )
```

Validate `editor_reflection_environment` before building the command and append:

```python
f"-UERayTracingAudioValidationReflectionEnvironment={editor_reflection_environment}"
```

Make the artifact regex explicitly parse `reflection_rays` and `reflection_bounces`; convert both to integers and compare the latter with `expected_reflection_bounces`. Update all existing call sites with the actual expected value (`8` in the fixed default flow).

- [ ] **Step 4: Add visible-helper options and atomic result JSON**

Add:

```python
parser.add_argument(
    "--reflection-environment",
    choices=launch_runtime_validation.EDITOR_REFLECTION_ENVIRONMENTS,
    default="enclosed",
)
parser.add_argument("--result-json", type=Path)
```

Pass the environment to `build_editor_command`. Before accepting the screenshot, call both strict validators with the requested environment and bounce. After the luma gates pass, atomically write:

```python
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
    temporary_path = result_json_path.with_suffix(result_json_path.suffix + ".tmp")
    temporary_path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary_path.replace(result_json_path)
```

Print both raw strict marker lines once so a caller can independently parse stdout. Do not write the result JSON on timeout, black frame, malformed marker, or artifact failure.

- [ ] **Step 5: Run focused and full Python GREEN**

Run:

```powershell
uv run python script\tests\test_validation_scripts.py
uv run python -m unittest discover -s script\tests -v
```

Expected: all existing and new tests pass; default Editor commands still request Enclosed with 8 bounces.

- [ ] **Step 6: Commit the Python contract**

```powershell
git add script/launch_runtime_validation.py script/validate_visible_editor_ab_scene.py script/tests/test_validation_scripts.py
git commit -m "test: define strict R3 editor evidence"
```

---

### Task 2: Apply One Effective Bounce Value to the C++ Fixture and Bake

**Files:**
- Modify: `Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.h:39-80`
- Modify: `Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.cpp:644-830`
- Modify: `Source/UERayTracingAudioEditor/Private/UERayTracingAudioEditorModule.cpp:1018-1110,1976-2020,2038-2145`
- Modify: `Source/UERayTracingAudioEditor/Private/Tests/UERayTracingAudioOfflineComparisonTests.cpp:212-468`

**Interfaces:**
- Produces:

```cpp
struct FUERayTracingAudioEditorValidationSceneResult
{
    bool bSucceeded = false;
    int32 ReflectionBounces = 8;
};

static FUERayTracingAudioEditorValidationSceneResult EnsureScene(
    UWorld& World,
    EUERayTracingAudioEditorValidationSceneMode Mode,
    EUERayTracingAudioEditorDirectPreset DirectPreset =
        EUERayTracingAudioEditorDirectPreset::Clear,
    EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment =
        EUERayTracingAudioEditorReflectionEnvironment::Enclosed,
    float DistanceCmOverride = -1.0f,
    EUERayTracingAudioEditorAirAbsorptionProfile AirProfile =
        EUERayTracingAudioEditorAirAbsorptionProfile::Default,
    int32 ReflectionBounces = 8);
```

- The Editor module consumes `SceneResult.ReflectionBounces` for `FUERayTracingAudioEditorArtifactRunner::Start`.
- Task 1's strict marker parser consumes `reflection_bounces=32` from the final R3 scene-ready marker and accepts 1/64 in clamp-focused tests.

- [ ] **Step 1: Write the failing 32-bounce and Geometry Automation assertions**

Extend `FUERayTracingAudioEditorValidationFixtureControlsTest` with calls that pass the final argument and assert exact Source/result values:

```cpp
const FUERayTracingAudioEditorValidationSceneResult NearWall32 =
    FUERayTracingAudioEditorValidationScene::EnsureScene(
        *World,
        EUERayTracingAudioEditorValidationSceneMode::Transient,
        EUERayTracingAudioEditorDirectPreset::Clear,
        EUERayTracingAudioEditorReflectionEnvironment::NearWall,
        200.0f,
        EUERayTracingAudioEditorAirAbsorptionProfile::Default,
        32);
TestTrue(TEXT("Near-wall 32-bounce fixture succeeds"), NearWall32.bSucceeded);
TestEqual(TEXT("Near-wall owns one Geometry"), CountTaggedGeometry(), 1);
TestEqual(TEXT("Result records 32 bounces"), NearWall32.ReflectionBounces, 32);
TestEqual(
    TEXT("Source executes 32 realtime bounces"),
    NearWall32.Source.IsValid()
        ? NearWall32.Source->GetMaxReflectionBounces()
        : -1,
    32);
```

Add low/high calls with `0` and `65`, expecting effective values `1` and `64`. Finish with Enclosed 32 and assert seven Geometry actors. Preserve the existing untagged-actor and stale-component lifetime assertions.

- [ ] **Step 2: Run the prescribed build and confirm RED**

Run:

```powershell
uv run script\build_and_validate.py
```

Expected: compile failure because the result has no `ReflectionBounces` and `EnsureScene` does not accept the seventh argument. Record the exact compiler diagnostic in `progress_log.md` only when Task 7 writes final evidence.

- [ ] **Step 3: Implement fixture clamping and Source assignment**

In `EnsureScene`:

```cpp
Result.ReflectionBounces = FMath::Clamp(ReflectionBounces, 1, 64);
```

Replace both hard-coded Source comparisons/assignments:

```cpp
|| Source->MaxReflectionBounces != Result.ReflectionBounces
```

and:

```cpp
Source->MaxReflectionBounces = Result.ReflectionBounces;
```

No ordinary Source default changes. Keep 4096 rays, 2.0-second realtime indirect duration, HybridReverb, Realtime data source, and validation Wet mix 0.8 unchanged.

- [ ] **Step 4: Parse bounce before scene creation and reuse the result for Bake**

Add one helper in the Editor module's private namespace:

```cpp
int32 GetValidationReflectionBounces()
{
    int32 ReflectionBounces = 8;
    FParse::Value(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioValidationReflectionBounces="),
        ReflectionBounces);
    return FMath::Clamp(ReflectionBounces, 1, 64);
}
```

Pass this value to automatic and persistent `EnsureScene` calls. Change the scene-ready marker to the exact order required by Task 1:

```cpp
TEXT("UERayTracingAudioEditor validation scene ready: source=1 listener=1 geometry=%d lighting=1 bake_ui=1 direct_preset=%s reflection_environment=%s reflection_bounces=%d source_listener_distance_cm=%.2f air_absorption_profile=%s air_absorption_per_meter=(%.6f,%.6f,%.6f).")
```

Pass `SceneResult.ReflectionBounces` to `ValidationArtifactRunner->Start`; delete the second independent parse inside the Editor-Bake branch. The existing artifact marker already emits `Offline.ReflectionBounceCount`; do not add a competing requested-value field.

- [ ] **Step 5: Re-run the prescribed build for GREEN**

Run:

```powershell
uv run script\build_and_validate.py
```

Expected: project and standalone plugin build both exit 0; realtime safety audit still reports zero forbidden callback operations.

- [ ] **Step 6: Run focused fixture Automation**

Run the `UERayTracingAudio.Editor.ValidationFixtureControls` command from the Test Command Convention. Expected: one passing focused test, with exact 0/1/7 Geometry and 1/32/64 bounce assertions.

- [ ] **Step 7: Re-run strict Python marker tests against producer order**

```powershell
uv run python script\tests\test_validation_scripts.py
```

Expected: PASS; synthetic patterns and the C++ format string have the same field order.

- [ ] **Step 8: Commit the C++ fixture contract**

```powershell
git add Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.h Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.cpp Source/UERayTracingAudioEditor/Private/UERayTracingAudioEditorModule.cpp Source/UERayTracingAudioEditor/Private/Tests/UERayTracingAudioOfflineComparisonTests.cpp
git commit -m "feat: configure 32-bounce R3 fixtures"
```

---

### Task 3: Pure Per-Environment Physics Validator

**Files:**
- Create: `script/reflection_environment_matrix.py`
- Create: `script/tests/test_reflection_environment_matrix.py`

**Interfaces:**
- Produces:

```text
ENVIRONMENTS = ("open_space", "near_wall", "enclosed")
EXPECTED_GEOMETRY = {"open_space": 0, "near_wall": 1, "enclosed": 7}
REFLECTION_RAYS = 4096
REFLECTION_BOUNCES = 32
ZERO_TOLERANCE = 1.0e-9
MAX_CPU_RELATIVE_DELTA = 0.05
MIN_DIRECTION_DOT = 0.99
MIN_DIRECTIONAL_ENERGY_RATIO = 0.05
MIN_WET_TO_REFERENCE_RATIO = 0.05
MIN_WET_STEREO_DIFFERENCE = 0.01

@dataclass(frozen=True)
class CaseManifest:
    environment: str
    path: Path
    payload: Mapping[str, Any]

load_case_manifest(environment: str, path: Path) -> CaseManifest

validate_case_manifest(case: CaseManifest) -> dict[str, float | int | str]
```

- Task 4 consumes three validated `CaseManifest` values for cross-environment and file checks.
- Task 5 uses `load_case_manifest` for both end-to-end and manifest-only modes.

- [ ] **Step 1: Create complete passing synthetic manifests**

In the new test file, use one factory with exact R3 values. The OpenSpace branch must set all hardware/CPU indirect values and Wet RMS to zero and all distinction booleans false. NearWall must use positive early reflection, zero late gain, audible Wet, directionality, and matching CPU values. Enclosed must use values at least 20% above NearWall for paths, energy, late tail, directional bins, and Wet ratio.

The common payload must include every field read by the validator:

```python
{
    "input_asset": "/Game/FirstPerson/Audio/MarchingBand.MarchingBand",
    "direct_preset": "clear",
    "reflection_environment": environment,
    "direct_distance_cm": 200.0,
    "reflection_ray_count": 4096,
    "reflection_bounce_count": 32,
    "hardware_ray_tracing": True,
    "has_cpu_reference": True,
    "sample_rate": 16000,
    "channels": 2,
    "impulse_response_channels": 2,
    "frames": 160000,
    "samples_finite": True,
    "audio_safety_checks_passed": True,
    "direct_semantics_passed": True,
    "clipped_sample_count": 0,
    "post_scale_peak": 0.8,
    "direct_dropout_window_count": 0,
    "common_output_scale": 1.0,
}
```

Use these deterministic environment values in the factory and copy every hardware value to the matching `cpu_reference_*` field:

```python
ENVIRONMENT_VALUES = {
    "open_space": {
        "paths": 0,
        "indirect_gain": 0.0,
        "early_gain": 0.0,
        "late_gain": 0.0,
        "ir_energy": 0.0,
        "directional_ratio": 0.0,
        "directional_bins": 0,
        "wet_rms": 0.0,
        "wet_ratio": 0.0,
        "automatic": False,
        "distinct": False,
        "earliest_arrival": 0.0,
        "average_delay": 0.0,
        "reverb_times": (0.0, 0.0, 0.0),
        "direction": (0.0, 0.0, 0.0),
    },
    "near_wall": {
        "paths": 1000,
        "indirect_gain": 0.08,
        "early_gain": 0.08,
        "late_gain": 0.0,
        "ir_energy": 0.08,
        "directional_ratio": 0.60,
        "directional_bins": 100,
        "wet_rms": 0.10,
        "wet_ratio": 0.10,
        "automatic": True,
        "distinct": True,
        "earliest_arrival": 0.005,
        "average_delay": 0.009,
        "reverb_times": (0.0, 0.0, 0.0),
        "direction": (0.0, 1.0, 0.0),
    },
    "enclosed": {
        "paths": 1600,
        "indirect_gain": 0.12,
        "early_gain": 0.10,
        "late_gain": 0.02,
        "ir_energy": 0.12,
        "directional_ratio": 0.55,
        "directional_bins": 180,
        "wet_rms": 0.14,
        "wet_ratio": 0.14,
        "automatic": True,
        "distinct": True,
        "earliest_arrival": 0.004,
        "average_delay": 0.020,
        "reverb_times": (0.7, 0.6, 0.5),
        "direction": (0.2, 0.97, 0.1),
    },
}
```

The factory also sets the same normalized `direction` on hardware and CPU fields and uses these complete signal metrics:

```python
payload.update(
    {
        "reference_rms": 0.20,
        "direct_rms": 0.10,
        "wet_rms": values["wet_rms"],
        "full_rms": 0.10 + values["wet_rms"],
        "direct_to_reference_rms_ratio": 0.50,
        "wet_to_reference_rms_ratio": values["wet_ratio"],
        "full_to_reference_rms_ratio": (0.10 + values["wet_rms"]) / 0.20,
        "direct_dry_correlation": 1.0,
        "full_dry_correlation": 1.0 if environment == "open_space" else 0.70,
        "wet_dry_correlation": 0.0,
        "direct_wet_normalized_difference": 0.0 if environment == "open_space" else 0.80,
        "wet_stereo_normalized_difference": 0.0 if environment == "open_space" else 0.30,
        "modes_are_distinct": values["distinct"],
        "directional_wet_is_distinct": values["distinct"],
        "automatic_checks_passed": values["automatic"],
    }
)
```

Add passing tests for each environment and rejection tests for wrong input, wrong environment, non-32 bounce, wrong rays, CPU-only, missing CPU reference, non-finite numeric data, clipping, Direct dropout, and failed Direct semantics.

- [ ] **Step 2: Run the new tests and confirm RED**

```powershell
uv run python script\tests\test_reflection_environment_matrix.py
```

Expected: import failure because `script/reflection_environment_matrix.py` does not exist.

- [ ] **Step 3: Implement common provenance and finite/safety checks**

Use exact MarchingBand provenance:

```python
if payload.get("input_asset") != "/Game/FirstPerson/Audio/MarchingBand.MarchingBand":
    failures.append("exact MarchingBand input asset")
if "ravel" in str(payload.get("input_asset", "")).lower():
    failures.append("no Ravel input")
```

Require exact Direct preset, environment, distance, rays, bounces, sample rate, directional-stereo IR, all common booleans, zero clipped samples, zero Direct dropouts, and peak `<= 0.99001`. Convert each of these fields through one finite-number helper that rejects booleans, missing values, NaN, and infinity:

```python
FINITE_FIELDS = (
    "direct_distance_cm",
    "reflection_ray_count",
    "reflection_bounce_count",
    "sample_rate",
    "channels",
    "impulse_response_channels",
    "frames",
    "post_scale_peak",
    "clipped_sample_count",
    "direct_dropout_window_count",
    "common_output_scale",
    "hardware_indirect_valid_paths",
    "cpu_reference_indirect_valid_paths",
    "hardware_indirect_gain",
    "cpu_reference_indirect_gain",
    "hardware_early_reflection_gain",
    "cpu_reference_early_reflection_gain",
    "hardware_late_reverb_gain",
    "cpu_reference_late_reverb_gain",
    "hardware_impulse_response_energy",
    "cpu_reference_impulse_response_energy",
    "hardware_directional_energy_ratio",
    "cpu_reference_directional_energy_ratio",
    "hardware_directional_bin_count",
    "cpu_reference_directional_bin_count",
    "hardware_dominant_arrival_direction_x",
    "hardware_dominant_arrival_direction_y",
    "hardware_dominant_arrival_direction_z",
    "cpu_reference_dominant_arrival_direction_x",
    "cpu_reference_dominant_arrival_direction_y",
    "cpu_reference_dominant_arrival_direction_z",
    "hardware_earliest_arrival_seconds",
    "hardware_average_delay_seconds",
    "hardware_reverb_time_low_seconds",
    "hardware_reverb_time_mid_seconds",
    "hardware_reverb_time_high_seconds",
    "reference_rms",
    "direct_rms",
    "wet_rms",
    "full_rms",
    "direct_to_reference_rms_ratio",
    "wet_to_reference_rms_ratio",
    "full_to_reference_rms_ratio",
    "direct_dry_correlation",
    "full_dry_correlation",
    "wet_dry_correlation",
    "direct_wet_normalized_difference",
    "wet_stereo_normalized_difference",
)
```

- [ ] **Step 4: Implement hardware/CPU agreement and environment-specific gates**

Use:

```python
def relative_delta(first: float, second: float) -> float:
    return abs(first - second) / max(abs(first), abs(second), 1.0e-12)
```

Compare these exact hardware/CPU pairs: valid paths, indirect gain, early gain, late gain, IR energy, directional energy ratio, and directional bin count. For nonzero expected metrics require both sides positive and delta `<= 0.05`. For OpenSpace require both sides `<= 1e-9`. For NearWall/Enclosed normalize the two dominant direction vectors and require dot product `>= 0.99`.

Enforce:

```python
if environment == "open_space":
    for field in (
        "modes_are_distinct",
        "directional_wet_is_distinct",
        "automatic_checks_passed",
    ):
        if payload.get(field) is not False:
            failures.append(f"open_space {field}=false")
    for field in (
        "wet_rms",
        "wet_to_reference_rms_ratio",
        "hardware_indirect_gain",
        "hardware_early_reflection_gain",
        "hardware_late_reverb_gain",
        "hardware_impulse_response_energy",
        "cpu_reference_indirect_gain",
        "cpu_reference_early_reflection_gain",
        "cpu_reference_late_reverb_gain",
        "cpu_reference_impulse_response_energy",
    ):
        if abs(float(payload[field])) > ZERO_TOLERANCE:
            failures.append(f"open_space zero {field}")
else:
    for field in (
        "modes_are_distinct",
        "directional_wet_is_distinct",
        "automatic_checks_passed",
    ):
        if payload.get(field) is not True:
            failures.append(f"{environment} {field}=true")
    if float(payload["wet_to_reference_rms_ratio"]) < MIN_WET_TO_REFERENCE_RATIO:
        failures.append(f"{environment} audible Wet")
    if float(payload["wet_stereo_normalized_difference"]) < MIN_WET_STEREO_DIFFERENCE:
        failures.append(f"{environment} stereo Wet distinction")
    if float(payload["hardware_directional_energy_ratio"]) < MIN_DIRECTIONAL_ENERGY_RATIO:
        failures.append(f"{environment} directional energy")
```

NearWall requires positive paths, IR energy, indirect gain, and early gain but permits zero late gain. Enclosed requires the same plus positive late gain.

- [ ] **Step 5: Run per-case GREEN and negative coverage**

```powershell
uv run python script\tests\test_reflection_environment_matrix.py -v
```

Expected: every per-case pass/reject test succeeds, including OpenSpace with `automatic_checks_passed=false` and NearWall/Enclosed with it true.

- [ ] **Step 6: Commit the pure per-case validator**

```powershell
git add script/reflection_environment_matrix.py script/tests/test_reflection_environment_matrix.py
git commit -m "test: validate R3 environment physics"
```

---

### Task 4: Cross-Environment WAV, Growth, and End-to-End Evidence Gates

**Files:**
- Modify: `script/reflection_environment_matrix.py`
- Modify: `script/tests/test_reflection_environment_matrix.py`

**Interfaces:**
- Produces:

```text
@dataclass(frozen=True)
class CaseEvidence:
    case: CaseManifest
    result_path: Path
    scene: Mapping[str, Any]
    artifacts: Mapping[str, Any]
    image_metrics: Mapping[str, Any]
    screenshot_path: Path
    log_path: Path

load_case_evidence(environment: str, result_path: Path) -> CaseEvidence

validate_matrix_manifests(
    cases: Mapping[str, CaseManifest],
) -> dict[str, object]

validate_end_to_end_evidence(
    cases: Mapping[str, CaseEvidence],
    project_root: Path,
) -> None
```

- Task 5 consumes the returned summary payload and evidence validation.
- `validate_matrix_manifests` returns keys `thresholds`, `cases`, and `comparisons`; `thresholds` is `dict[str, float]`, `cases` is `dict[str, dict[str, object]]`, and `comparisons` is `dict[str, float]`. Each case contains manifest/WAV paths, SHA-256 values, and validated physical metrics, while comparisons contains NearWall→Enclosed ratios.

- [ ] **Step 1: Add failing real-file and cross-environment tests**

Use `tempfile.TemporaryDirectory` and Python `wave` to write equal-length PCM16 files. The helper must write zero Wet for OpenSpace, `Full == Direct` for OpenSpace, identical Reference and Direct content across all cases, and distinct Wet/Full content for NearWall and Enclosed.

Add tests that reject:

- one nonzero OpenSpace Wet PCM sample;
- different OpenSpace Direct and Full hashes;
- different Reference hashes;
- different Direct hashes;
- unequal `common_output_scale` beyond `1e-6`;
- missing WAV, screenshot, or log;
- black screenshot metrics (`non_black_ratio < 0.10` or `luma_stddev < 8.0`);
- wrong 0/1/7 Geometry evidence;
- artifact marker/manifest path mismatch;
- missing IR `.uasset` or imported asset count other than 4;
- Enclosed path count or directional bins not greater than NearWall;
- Enclosed IR energy or Wet ratio below `1.10 * NearWall`;
- Enclosed late gain not positive, or below `1.10 * NearWall` when NearWall late gain exceeds `1e-9`.

Use direct mutations so each rejection has one cause:

```python
def test_rejects_nonzero_open_space_wet_pcm(self) -> None:
    cases, evidence, project_root = make_complete_matrix(self.temporary_root)
    write_pcm16(Path(cases["open_space"].payload["wet_wav"]), [1, 0, 0, 0])
    with self.assertRaisesRegex(RuntimeError, "OpenSpace zero Wet PCM"):
        reflection_environment_matrix.validate_matrix_manifests(cases)

def test_rejects_wrong_near_wall_geometry(self) -> None:
    cases, evidence, project_root = make_complete_matrix(self.temporary_root)
    wrong = dataclasses.replace(
        evidence["near_wall"],
        scene={**evidence["near_wall"].scene, "geometry": 7},
    )
    evidence = {**evidence, "near_wall": wrong}
    with self.assertRaisesRegex(RuntimeError, "fixture geometry count"):
        reflection_environment_matrix.validate_end_to_end_evidence(
            evidence,
            project_root,
        )

def test_rejects_insufficient_enclosed_energy_growth(self) -> None:
    cases, evidence, project_root = make_complete_matrix(self.temporary_root)
    enclosed_payload = dict(cases["enclosed"].payload)
    enclosed_payload["hardware_impulse_response_energy"] = 0.087
    enclosed_payload["cpu_reference_impulse_response_energy"] = 0.087
    cases = {
        **cases,
        "enclosed": CaseManifest(
            "enclosed",
            cases["enclosed"].path,
            enclosed_payload,
        ),
    }
    with self.assertRaisesRegex(RuntimeError, "Enclosed IR energy growth"):
        reflection_environment_matrix.validate_matrix_manifests(cases)
```

- [ ] **Step 2: Run the focused tests and confirm RED**

```powershell
uv run python script\tests\test_reflection_environment_matrix.py -v
```

Expected: missing `CaseEvidence`, `validate_matrix_manifests`, and `validate_end_to_end_evidence` failures.

- [ ] **Step 3: Implement SHA-256, PCM-zero, and cross-case invariants**

Use standard-library helpers:

```python
def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

def pcm16_is_zero(path: Path) -> bool:
    with wave.open(str(path), "rb") as stream:
        if stream.getsampwidth() != 2:
            return False
        return all(byte == 0 for byte in stream.readframes(stream.getnframes()))
```

Require identical common provenance, frames/channels, common scale, Reference hash, and Direct hash. Require OpenSpace Direct/Full hash equality and zero Wet PCM. Require NearWall and Enclosed Wet/Full hashes to differ from OpenSpace.

- [ ] **Step 4: Implement growth and strict evidence validation**

Use strict `>` for Enclosed paths and directional bins. Use `>= near * 1.10` for hardware IR energy and Wet/Reference RMS. For late gain:

```python
minimum_enclosed_late = (
    near_late * 1.10 if near_late > ZERO_TOLERANCE else ZERO_TOLERANCE
)
if enclosed_late <= minimum_enclosed_late:
    failures.append("Enclosed late-reverb growth")
```

Validate result JSON schema/version/pass, exact scene environment/geometry/bounces/distance/air profile, artifact hardware flag/rays/bounces/import count, screenshot/log existence, luma thresholds, marker WAV/manifest path agreement, and IR object-path `.uasset` under `project_root / "Content"`. Scan retained Editor logs for the project failure patterns.

- [ ] **Step 5: Run focused and full Python GREEN**

```powershell
uv run python script\tests\test_reflection_environment_matrix.py -v
uv run python -m unittest discover -s script\tests -v
```

Expected: all new failure modes are caught and all prior validation tests remain green.

- [ ] **Step 6: Commit cross-environment validation**

```powershell
git add script/reflection_environment_matrix.py script/tests/test_reflection_environment_matrix.py
git commit -m "feat: gate R3 cross-environment evidence"
```

---

### Task 5: Three-Editor Matrix Orchestration and Summary Manifest

**Files:**
- Create: `script/validate_reflection_environment_matrix.py`
- Modify: `script/tests/test_reflection_environment_matrix.py`

**Interfaces:**
- Produces:

```text
build_case_command(
    repo_root: Path,
    engine_root: Path,
    project_path: Path,
    environment: str,
    timeout_seconds: float,
    screenshot_path: Path,
    result_path: Path,
) -> list[str]

run_editor_case(
    repo_root: Path,
    engine_root: Path,
    project_path: Path,
    environment: str,
    timeout_seconds: float,
    output_root: Path,
) -> CaseEvidence
```

- Default CLI output is `project_path.parent / "Saved/UERayTracingAudio/ListeningAcceptance/ReflectionEnvironmentMatrix" / time.strftime("%Y%m%d-%H%M%S") / "ReflectionEnvironmentMatrix_Manifest.json"`.
- Full mode prints `R3_REFLECTION_MATRIX_PASS`; manifest-only mode prints `R3_REFLECTION_MATRIX_RECHECK_PASS` and records `end_to_end=false`.

- [ ] **Step 1: Write failing command, mode, and summary tests**

Call the function and verify every exact option/value pair:

```python
command = validate_reflection_environment_matrix.build_case_command(
    repo_root,
    engine_root,
    project_path,
    "near_wall",
    180.0,
    output_root / "NearWall.png",
    output_root / "NearWall_Result.json",
)
self.assertIn("--artifacts", command)
for option, value in (
    ("--direct-preset", "clear"),
    ("--reflection-environment", "near_wall"),
    ("--reflection-bounces", "32"),
    ("--screenshot", str(output_root / "NearWall.png")),
    ("--result-json", str(output_root / "NearWall_Result.json")),
):
    index = command.index(option)
    self.assertEqual(command[index + 1], value)
```

Add all-or-none tests for `--open-space-manifest`, `--near-wall-manifest`, and `--enclosed-manifest`. Mock `subprocess.run` to prove default mode executes in the order OpenSpace → NearWall → Enclosed and stops on the first nonzero exit. Assert full summaries contain `passed=true`, `end_to_end=true`, fixed config, thresholds, case paths/hashes/metrics, and one strict pass marker. Assert manifest-only mode cannot emit the full pass marker.

- [ ] **Step 2: Run the focused tests and confirm RED**

```powershell
uv run python script\tests\test_reflection_environment_matrix.py -v
```

Expected: import failure because `validate_reflection_environment_matrix.py` does not exist.

- [ ] **Step 3: Implement argument parsing and case command construction**

Use these arguments:

```python
parser.add_argument("--engine-root", type=Path)
parser.add_argument("--project", type=Path)
parser.add_argument("--timeout", type=float, default=180.0)
parser.add_argument("--output", type=Path)
parser.add_argument("--open-space-manifest", type=Path)
parser.add_argument("--near-wall-manifest", type=Path)
parser.add_argument("--enclosed-manifest", type=Path)
```

Build each full-case command with `sys.executable` and `script/validate_visible_editor_ab_scene.py`; use separate screenshot and result JSON paths in the matrix output directory. Run sequentially with `capture_output=True`, `text=True`, `check=False`, and `timeout=timeout_seconds + 60.0`. Forward captured stdout/stderr and raise on nonzero exit or missing result JSON.

- [ ] **Step 4: Implement full and manifest-only flows**

Full flow loads three `CaseEvidence` objects, calls `validate_matrix_manifests(case_manifests)` and `validate_end_to_end_evidence(case_evidence, project_path.parent)`, then writes an atomic summary. Manifest-only flow loads the three manifests and runs only semantic/cross-file gates. It must write:

```python
matrix_validation = validate_matrix_manifests(case_manifests)
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
```

Wrap `main` so every exception prints `f"R3_REFLECTION_MATRIX_FAIL {exc}"` to stderr and exits 1. A full success prints `f'R3_REFLECTION_MATRIX_PASS bounces=32 manifest="{output_path}"'`; a recheck prints the distinct recheck marker.

- [ ] **Step 5: Run focused and full Python GREEN**

```powershell
uv run python script\tests\test_reflection_environment_matrix.py -v
uv run python -m unittest discover -s script\tests -v
```

Expected: orchestration, mode distinction, semantic gates, and all existing Python tests pass.

- [ ] **Step 6: Commit the matrix CLI**

```powershell
git add script/validate_reflection_environment_matrix.py script/tests/test_reflection_environment_matrix.py
git commit -m "feat: orchestrate 32-bounce R3 matrix"
```

---

### Task 6: Build, Automation, Hardware Matrix, and Fixed Runtime Validation

**Files:**
- Verify only; modify implementation/tests only when a reproduced RED proves a defect.

**Interfaces:**
- Consumes all Tasks 1–5.
- Produces real hardware manifests, WAVs, screenshots, logs, a matrix summary, and a final interactive Editor.

- [ ] **Step 1: Stop only the previously launched validation Editor**

Identify the PID from the prior fixed-launch log/output, confirm its executable is `UnrealEditor.exe` and project command line points at the validation project, then stop that one process. Do not stop unrelated Unreal Editors.

```powershell
$Project = (uv run python -c "from pathlib import Path; from script.validation_environment import resolve_project_path; print(resolve_project_path(None, Path.cwd(), 'UERayTracingAudio'))").Trim()
$ValidationEditors = @(
  Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq "UnrealEditor.exe" -and
    $_.CommandLine -like "*$Project*" -and
    $_.CommandLine -like "*-UERayTracingAudioValidationScenario*"
  }
)
if ($ValidationEditors.Count -gt 1) { throw "More than one validation-owned Editor is running." }
if ($ValidationEditors.Count -eq 1) {
  Stop-Process -Id $ValidationEditors[0].ProcessId -ErrorAction Stop
}
```

- [ ] **Step 2: Run the full Python and realtime-safety gates**

```powershell
uv run python -m unittest discover -s script\tests -v
uv run script\validate_audio_realtime_safety.py
git diff --check
```

Expected: all tests pass, every forbidden callback-operation count is zero, and diff check exits 0.

- [ ] **Step 3: Run the prescribed Development build**

```powershell
uv run script\build_and_validate.py
```

Expected: sync, UE 5.7 project build, and standalone plugin build exit 0. A crash enters `workflow/crash-debugging.md`; a non-crash failure blocks the next step until fixed with a focused RED.

- [ ] **Step 4: Run focused and full UE Automation**

Run the focused fixture command, then repeat it with:

```text
Automation RunTests UERayTracingAudio.Editor;Quit
Automation RunTests UERayTracingAudio;Quit
```

Use unique `-abslog` files. Require zero failed tests and no fatal/assertion markers in every log.

- [ ] **Step 5: Run the real 32-bounce hardware matrix**

```powershell
uv run script\validate_reflection_environment_matrix.py
```

Expected: three visible DX12 Editor runs complete sequentially and print one final `R3_REFLECTION_MATRIX_PASS bounces=32` marker. The summary must report OpenSpace/NearWall/Enclosed Geometry `0/1/7`, hardware and CPU provenance, 32 bounces, four WAVs and one screenshot/log/IR per case.

- [ ] **Step 6: Audit the generated summary and artifacts**

Run a Python assertion against the emitted summary path:

```powershell
$Project = (uv run python -c "from pathlib import Path; from script.validation_environment import resolve_project_path; print(resolve_project_path(None, Path.cwd(), 'UERayTracingAudio'))").Trim()
$SummaryRoot = Join-Path (Split-Path $Project) "Saved\UERayTracingAudio\ListeningAcceptance\ReflectionEnvironmentMatrix"
$Summary = Get-ChildItem -LiteralPath $SummaryRoot -Recurse -Filter ReflectionEnvironmentMatrix_Manifest.json | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $Summary) { throw "No R3 matrix summary was generated." }
uv run python -c "import json,sys; from pathlib import Path; p=Path(sys.argv[1]); d=json.loads(p.read_text(encoding='utf-8-sig')); assert d['passed'] and d['end_to_end']; assert d['fixed_config']['reflection_bounces']==32; assert set(d['cases'])=={'open_space','near_wall','enclosed'}; print(p)" $Summary.FullName
```

Confirm `$Summary.FullName` is the path printed by Step 5. Manually verify every referenced file exists and all three logs contain no crash markers. Do not substitute an older manifest.

- [ ] **Step 7: Run the fixed runtime launcher and leave Editor open**

```powershell
uv run script\launch_runtime_validation.py
```

Expected: the fixed `-game` phase exits cleanly with hardware Direct/Indirect, data-source, Direct-sweep, and hard-realtime gates; the script then opens a responding Editor and leaves it ready for interactive F3/F1/F2/F5 validation.

- [ ] **Step 8: Record any non-crash failure with a focused RED before repair**

If Steps 3–7 expose a numerical or provenance failure, add the smallest reproducer to `test_reflection_environment_matrix.py`, `test_validation_scripts.py`, or `UERayTracingAudioOfflineComparisonTests.cpp`, run it to RED, make the minimal in-scope repair, and repeat Steps 2–7. Never lower the 32-bounce, 5%, zero-Wet, 10%-growth, luma, safety, or hardware thresholds to convert a failure into a pass.

---

### Task 7: Usage, Status, Progress, and Evidence Commit

**Files:**
- Modify: `TODO.md`
- Modify: `USAGE.md`
- Modify: `IMPLEMENTATION_STATUS.md`
- Modify: `progress_log.md`

**Interfaces:**
- Consumes exact command outputs and artifact paths from Task 6.
- Produces the user-facing R3 operating procedure and an honest completion ledger.

- [ ] **Step 1: Update the task ledger without claiming Human Pass**

Mark the automated OpenSpace/NearWall/Enclosed R3 matrix complete only if Task 6 passed. Keep target-device Human A/B, click/pop judgment, true multi-PIE, and any other existing open items unchecked.

- [ ] **Step 2: Add the 32-bounce R3 usage flow**

Document:

```powershell
uv run script\validate_reflection_environment_matrix.py
```

Explain the output directory, four-track meanings, 0/1/7 Geometry, 32 reflected segments, OpenSpace zero Wet, NearWall directional early reflection, Enclosed early + late reverb, and the fact that `automatic_checks_passed=false` is correct only for OpenSpace. Include the exact user listening sequence and state that only the user may record Human Pass.

- [ ] **Step 3: Record implementation status and concise progress evidence**

Add actual Python counts, realtime audit totals, build result, Automation counts/logs, three manifest/screenshot/log paths, hardware/CPU deltas, cross-environment ratios, fixed-launch logs, and final Editor PID. `progress_log.md` records only important action/result pairs; it does not copy the full status narrative.

- [ ] **Step 4: Verify documentation integrity**

```powershell
git diff --check
rg --files TestProject | rg -i "ravel"
```

Expected: diff check exits 0 and the asset search returns no Ravel file. References in historical documentation may say Ravel was removed; they are not project audio assets.

- [ ] **Step 5: Commit documentation and evidence**

```powershell
git add TODO.md USAGE.md IMPLEMENTATION_STATUS.md progress_log.md
git commit -m "docs: record 32-bounce R3 matrix evidence"
```

---

### Task 8: R3 Completion Audit and Handoff to the Remaining Plugin Goal

**Files:**
- Verify: `docs/superpowers/specs/2026-08-02-r3-reflection-environment-matrix-design.md`
- Verify: matrix summary and every referenced manifest/WAV/screenshot/log/IR asset
- Verify: `TODO.md`, `USAGE.md`, `IMPLEMENTATION_STATUS.md`, `progress_log.md`

**Interfaces:**
- Consumes Tasks 1–7.
- Produces an evidence-backed R3 status and the exact manual listening request.

- [ ] **Step 1: Audit every design requirement against authoritative evidence**

Create a private checklist mapping each design requirement to current source, test, command output, and artifact path. Treat missing, stale, indirect, or mismatched evidence as incomplete and rerun the narrowest authoritative gate.

- [ ] **Step 2: Confirm worktree and commit hygiene**

```powershell
git status --short
git log --oneline --decorate -8
git diff --check HEAD~1 HEAD
```

Expected: only pre-existing unrelated user changes remain; R3 source/tests/docs are committed in focused commits; the final commit has no whitespace errors.

- [ ] **Step 3: Hand off the Human Pass procedure**

Give the user the exact matrix summary path, three screenshot paths, twelve WAV paths, fixed-launch Game/Editor logs, and open Editor PID. Ask them to listen for silence in OpenSpace Wet, a directional first reflection at NearWall, a longer late tail in Enclosed, stable MarchingBand playback, and no click/pop.

- [ ] **Step 4: Keep the overall goal active unless all remaining requirements are independently proven**

R3 automated completion does not prove target-device Human Pass, true multi-PIE hardware isolation, remaining Realtime/Baked/Hybrid closure, or any other open ledger item. Continue the persistent plugin goal after the R3 handoff; do not call the overall goal complete from this feature alone.
