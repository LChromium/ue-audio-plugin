# Configurable Direct Audio and Isolated Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver configurable, frequency-dependent Direct rendering for arbitrary UE 5.7 worlds and an opt-in hardware Direct sweep fixture that proves continuous Clear → Soft Occluded → Clear behavior without becoming a product dependency.

**Architecture:** The Runtime module reads project physics settings and passes plain values into the UObject-free SDK context. The manager owns Listener and acoustic-scene state per `UWorld`; the audio callback applies a preallocated complementary three-band Direct filter; public Source methods drive Realtime/Baked/Hybrid changes. Non-Shipping validation code consumes those same APIs, moves only tagged validation actors, emits a strict Direct-sweep marker, and exposes F6 plus Editor fixture controls.

**Tech Stack:** Unreal Engine 5.7 C++, AudioExtensions/Occlusion plugin API, UE Ray Tracing RHI, `UDeveloperSettings`, Slate, Unreal Automation, Python `unittest`, PowerShell, `uv`.

## Global Constraints

- Modify only `D:\Labs\2602-unreal\ue-audio-plugin`; never edit the synchronized `TestProject/UeVersion1/Plugins/UERayTracingAudio` copy directly.
- Preserve unrelated dirty-worktree changes and commit only files belonging to the current task.
- Use `apply_patch` for source and document edits.
- Follow RED → GREEN → REFACTOR. Every behavior change starts with a failing test and the failure reason must be recorded.
- Audio callbacks may not lock, allocate, block, log, access UObject, or create/destroy shared ownership.
- The SDK module remains UObject-free; only the Runtime module reads project settings.
- The 200 cm arc, F6, MarchingBand, blue/orange actors, test air profile, and validation HUD never affect a normal project launch.
- Validation runtime entry points compile only for non-Shipping targets and run only with `-UERayTracingAudioValidationScenario`.
- A fixed-camera gate is evidence, not a substitute for interactive PIE and Human Pass.
- Before every build, use the prescribed `uv run script\build_and_validate.py`; do not invoke UnrealBuildTool directly.
- After each major feature, use the prescribed `uv run script\launch_runtime_validation.py`.
- If a build, Game, Editor, or runtime validation crashes, immediately follow `workflow/crash-debugging.md`.
- After each major feature update `TODO.md`, `USAGE.md`, `IMPLEMENTATION_STATUS.md`, `plan.md`, and `progress_log.md`.

## Test Command Convention

After a successful prescribed build, run a focused UE Automation group with:

```powershell
$EngineRoot = (uv run python -c "from script.validation_environment import resolve_engine_root; print(resolve_engine_root(None))").Trim()
& "$EngineRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "TestProject\UeVersion1\UeVersion1.uproject" `
  -unattended -nop4 -NullRHI `
  "-ExecCmds=Automation RunTests UERayTracingAudio.Audio.ConfigurableDirect;Quit" `
  "-TestExit=Automation Test Queue Empty" `
  -log
```

The focused log must contain no failed `UERayTracingAudio.Audio.ConfigurableDirect` test. Run the existing `UERayTracingAudio.Audio` and full `UERayTracingAudio` groups at the final gate.

## File Map

- Create `Source/UERayTracingAudio/Public/Settings/UERayTracingAudioProjectSettings.h`: project-visible physical settings and validated plain-value conversion.
- Create `Source/UERayTracingAudio/Private/Settings/UERayTracingAudioProjectSettings.cpp`: defaults, clamping, and settings metadata.
- Modify `Source/UERayTracingAudioSDK/Public/API/UERayTracingAudioContext.h` and its `.cpp`: plain configurable SDK context.
- Modify `Source/UERayTracingAudio/Public/Managers/UERayTracingAudioManager.h` and its `.cpp`: per-World Listener and acoustic-scene state.
- Modify `Source/UERayTracingAudio/Public/Components/UERayTracingAudioListenerComponent.h` and its `.cpp`: remove process-global Listener singleton.
- Create `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.h` and `.cpp`: allocation-free per-sample Direct filter after initialization.
- Modify `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.h` and `.cpp`: integrate per-channel three-band Direct rendering.
- Modify `Source/UERayTracingAudio/Public/Audio/UERayTracingAudioAudioDiagnostics.h` and its `.cpp`: Direct continuity statistics.
- Modify `Source/UERayTracingAudio/Public/Components/UERayTracingAudioSourceComponent.h` and its `.cpp`: explicit Blueprint runtime setters and same-World Listener lookup.
- Create `Source/UERayTracingAudio/Private/Validation/UERayTracingAudioDirectSweep.h` and `.cpp`: pure trajectory/metrics logic.
- Modify `Source/UERayTracingAudio/Private/Validation/UERayTracingAudioRuntimeValidation.h` and `.cpp`: automatic/F6 state machine and HUD.
- Modify `Source/UERayTracingAudio/Private/UERayTracingAudioModule.cpp`, its public header, and `UERayTracingAudio.Build.cs`: validated settings injection and non-Shipping validation isolation.
- Modify `Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.h` and `.cpp`: tagged fixture distance/air profile and stale-geometry cleanup.
- Modify `Source/UERayTracingAudioEditor/Private/UERayTracingAudioEditorModule.cpp`: Direct fixture UI.
- Create `Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp`: Runtime/SDK/DSP/World/sweep Automation.
- Modify `Source/UERayTracingAudioEditor/Private/Tests/UERayTracingAudioOfflineComparisonTests.cpp`: Editor fixture tests.
- Modify `script/launch_runtime_validation.py` and `script/tests/test_validation_scripts.py`: command, parser, and strict Direct-sweep gate.
- Modify `script/validate_audio_realtime_safety.py`: audit new DSP callback helpers.

---

### Task 1: Project Physics Settings and Plain SDK Context

**Files:**
- Create: `Source/UERayTracingAudio/Public/Settings/UERayTracingAudioProjectSettings.h`
- Create: `Source/UERayTracingAudio/Private/Settings/UERayTracingAudioProjectSettings.cpp`
- Modify: `Source/UERayTracingAudioSDK/Public/API/UERayTracingAudioContext.h`
- Modify: `Source/UERayTracingAudioSDK/Private/API/UERayTracingAudioContext.cpp`
- Modify: `Source/UERayTracingAudio/Public/Managers/UERayTracingAudioManager.h`
- Modify: `Source/UERayTracingAudio/Private/Managers/UERayTracingAudioManager.cpp`
- Modify: `Source/UERayTracingAudio/Private/UERayTracingAudioModule.cpp`
- Modify: `Source/UERayTracingAudio/UERayTracingAudio.Build.cs`
- Create: `Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp`

**Interfaces:**
- Produces:

```cpp
struct FUERayTracingAudioContextSettings
{
    float ReferenceDistanceCm = 100.0f;
    float MaxDistanceCm = 5000.0f;
    float SpeedOfSoundCmPerSecond = 34300.0f;
};

class FUERayTracingAudioContext
{
public:
    explicit FUERayTracingAudioContext(
        const FUERayTracingAudioContextSettings& Settings = {});
    void Configure(const FUERayTracingAudioContextSettings& Settings);
};

UCLASS(Config=Engine, DefaultConfig, meta=(DisplayName="UE Ray Tracing Audio"))
class UUERayTracingAudioProjectSettings : public UDeveloperSettings
{
public:
    FUERayTracingAudioContextSettings GetValidatedContextSettings() const;
    FVector2f GetValidatedAirAbsorptionCrossoversHz(float SampleRate) const;
};
```

- `FUERayTracingAudioManager` consumes validated context settings in its constructor.
- Task 3 consumes the validated crossover pair without reading UObject on the audio thread.

- [ ] **Step 1: Write a failing reflection/default/clamp Automation test**

Add `FUERayTracingAudioProjectSettingsTest` to the new test file. The RED test must compile against current code by finding the class through reflection:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioProjectSettingsTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.ProjectSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioProjectSettingsTest::RunTest(const FString&)
{
    UClass* SettingsClass = FindObject<UClass>(
        nullptr,
        TEXT("/Script/UERayTracingAudio.UERayTracingAudioProjectSettings"));
    TestNotNull(TEXT("project settings class is registered"), SettingsClass);
    return true;
}
```

- [ ] **Step 2: Build and run the focused test to verify RED**

Run:

```powershell
uv run script\build_and_validate.py
```

Then run the focused Automation command from “Test Command Convention”.

Expected: build succeeds, but `ProjectSettings` fails because the reflected class is absent.

- [ ] **Step 3: Implement validated settings and SDK context injection**

Implement the UPROPERTY fields with the exact defaults from the design. `GetValidatedContextSettings()` must enforce:

```cpp
Result.ReferenceDistanceCm = FMath::Max(ReferenceDistanceCm, 1.0f);
Result.MaxDistanceCm = FMath::Max(MaxDistanceCm, Result.ReferenceDistanceCm);
Result.SpeedOfSoundCmPerSecond = FMath::Max(SpeedOfSoundCmPerSecond, 1.0f);
```

For crossovers:

```cpp
const float Nyquist = FMath::Max(SampleRate * 0.5f, 40.0f);
const float LowMid = FMath::Clamp(AirAbsorptionLowMidCrossoverHz, 20.0f, Nyquist);
const float MidHigh = FMath::Clamp(AirAbsorptionMidHighCrossoverHz, LowMid, Nyquist);
return FVector2f(LowMid, MidHigh);
```

Add `"DeveloperSettings"` to `PublicDependencyModuleNames` because the public settings header derives from `UDeveloperSettings`.

Construct the manager with `GetDefault<UUERayTracingAudioProjectSettings>()->GetValidatedContextSettings()` in `StartupModule()`. Log one Game Thread line containing effective reference distance, maximum distance, speed, and crossovers.

- [ ] **Step 4: Extend the test to prove defaults and invalid-value clamping**

After the class exists, replace the reflection-only assertion with:

```cpp
UUERayTracingAudioProjectSettings* Settings =
    NewObject<UUERayTracingAudioProjectSettings>();
TestEqual(TEXT("default reference"), Settings->ReferenceDistanceCm, 100.0f);
TestEqual(TEXT("default maximum"), Settings->MaxDistanceCm, 5000.0f);
TestEqual(TEXT("default speed"), Settings->SpeedOfSoundCmPerSecond, 34300.0f);

Settings->ReferenceDistanceCm = -10.0f;
Settings->MaxDistanceCm = 0.25f;
Settings->SpeedOfSoundCmPerSecond = 0.0f;
const FUERayTracingAudioContextSettings Valid =
    Settings->GetValidatedContextSettings();
TestEqual(TEXT("reference clamped"), Valid.ReferenceDistanceCm, 1.0f);
TestEqual(TEXT("maximum follows reference"), Valid.MaxDistanceCm, 1.0f);
TestEqual(TEXT("positive speed"), Valid.SpeedOfSoundCmPerSecond, 1.0f);
```

Also construct `FUERayTracingAudioContext Context(Valid)` and assert its getters return the validated values.

- [ ] **Step 5: Rebuild and verify GREEN**

Run the prescribed build and focused Automation command. Expected: `ProjectSettings` passes and all pre-existing audio tests remain green.

- [ ] **Step 6: Commit only Task 1 files**

```powershell
git add Source/UERayTracingAudio/Public/Settings/UERayTracingAudioProjectSettings.h `
  Source/UERayTracingAudio/Private/Settings/UERayTracingAudioProjectSettings.cpp `
  Source/UERayTracingAudioSDK/Public/API/UERayTracingAudioContext.h `
  Source/UERayTracingAudioSDK/Private/API/UERayTracingAudioContext.cpp `
  Source/UERayTracingAudio/Public/Managers/UERayTracingAudioManager.h `
  Source/UERayTracingAudio/Private/Managers/UERayTracingAudioManager.cpp `
  Source/UERayTracingAudio/Private/UERayTracingAudioModule.cpp `
  Source/UERayTracingAudio/UERayTracingAudio.Build.cs `
  Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp
git commit -m "feat: configure acoustic physics from project settings"
```

---

### Task 2: World-Scoped Listener and Acoustic Scene State

**Files:**
- Modify: `Source/UERayTracingAudio/Public/Managers/UERayTracingAudioManager.h`
- Modify: `Source/UERayTracingAudio/Private/Managers/UERayTracingAudioManager.cpp`
- Modify: `Source/UERayTracingAudio/Public/Components/UERayTracingAudioListenerComponent.h`
- Modify: `Source/UERayTracingAudio/Private/Components/UERayTracingAudioListenerComponent.cpp`
- Modify: `Source/UERayTracingAudio/Private/Components/UERayTracingAudioSourceComponent.cpp`
- Modify: `Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp`

**Interfaces:**
- Produces:

```cpp
UUERayTracingAudioListenerComponent* GetCurrentListener(
    const UWorld* World) const;

struct FWorldAcousticState
{
    FUERayTracingAudioScene Scene;
    FString SceneSignature;
    bool bSceneDirty = true;
};
```

- Store world states as `TMap<TWeakObjectPtr<UWorld>, TUniquePtr<FWorldAcousticState>>` so in-flight simulation inputs keep stable `Scene` addresses.
- `BuildDirectSimulationInput`, `BuildIndirectSimulationInput`, Bake, stale-asset checks, and directional IR reconstruction consume the Source/Bake World explicitly.

- [ ] **Step 1: Write the wrong-World Listener RED test**

Create two transient Game Worlds, place a Source in World A, register only a Listener from World B with a local manager, then call `SimulateDirectSource(SourceA)`.

```cpp
FUERayTracingAudioDirectSimulationResult WrongWorld =
    Manager.SimulateDirectSource(SourceA);
TestFalse(
    TEXT("a source cannot consume another world's listener"),
    WrongWorld.bHasListener);
```

Current code selects any valid Listener and must fail this assertion.

- [ ] **Step 2: Write the cross-World geometry RED test**

Register acoustic geometry only in World B, request the scene signature for World A, then for World B. Assert World A remains the empty-scene signature and World B differs. Current global scene aggregation must fail.

- [ ] **Step 3: Run RED**

Run the prescribed build and focused Automation command. Expected: the wrong-World Listener and/or geometry isolation assertions fail for the current global state.

- [ ] **Step 4: Replace global Listener/scene state**

Implement:

```cpp
TMap<TWeakObjectPtr<UWorld>, TWeakObjectPtr<UUERayTracingAudioListenerComponent>>
    ListenersByWorld;
TMap<TWeakObjectPtr<UWorld>, TUniquePtr<FWorldAcousticState>>
    AcousticStatesByWorld;
```

Rules:

- `AddListener` keeps the first valid Listener for a World and logs a warning for a second.
- `RemoveListener` removes only if the map value equals the removed component.
- `AddGeometry`, `RemoveGeometry`, and `MarkSceneDirty` dirty the owning World only.
- Scene rebuild filters `GeometryComponents` by `Geometry->GetWorld() == World`.
- Remove dead World keys during the manager ticker on the Game Thread.
- Delete `UUERayTracingAudioListenerComponent::CurrentListener` and its no-argument getter.
- Replace every manager/listener call with the appropriate Source or Bake World.
- Replace `GetScene()` with `GetScene(UWorld*)` or an internal stable world-state accessor; do not return a reference to a temporary.

- [ ] **Step 5: Add the positive two-World assertions**

Register Listener A and Listener B. Assert:

```cpp
TestTrue(TEXT("World A listener"),
    Manager.GetCurrentListener(WorldA) == ListenerA);
TestTrue(TEXT("World B listener"),
    Manager.GetCurrentListener(WorldB) == ListenerB);
```

Also assert removing Listener A does not affect World B.

- [ ] **Step 6: Rebuild and verify GREEN**

Run the prescribed build and focused Automation group. Then run `UERayTracingAudio.Audio` to catch Bake/stale-asset regressions.

- [ ] **Step 7: Commit Task 2**

```powershell
git add Source/UERayTracingAudio/Public/Managers/UERayTracingAudioManager.h `
  Source/UERayTracingAudio/Private/Managers/UERayTracingAudioManager.cpp `
  Source/UERayTracingAudio/Public/Components/UERayTracingAudioListenerComponent.h `
  Source/UERayTracingAudio/Private/Components/UERayTracingAudioListenerComponent.cpp `
  Source/UERayTracingAudio/Private/Components/UERayTracingAudioSourceComponent.cpp `
  Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp
git commit -m "fix: scope listeners and acoustic scenes by world"
```

---

### Task 3: Frequency-Dependent, Real-Time-Safe Direct DSP

**Files:**
- Create: `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.h`
- Create: `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.cpp`
- Modify: `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.h`
- Modify: `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp`
- Modify: `Source/UERayTracingAudio/Private/UERayTracingAudioModule.cpp`
- Modify: `Source/UERayTracingAudio/Public/UERayTracingAudioModule.h`
- Modify: `Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp`
- Modify: `script/validate_audio_realtime_safety.py`

**Interfaces:**
- Produces:

```cpp
class FUERayTracingAudioThreeBandAirAbsorption
{
public:
    void Initialize(
        int32 SampleRate,
        int32 NumChannels,
        float LowMidCrossoverHz,
        float MidHighCrossoverHz);
    void Reset();
    bool CanProcess(int32 NumChannels) const;
    float ProcessSample(
        float Input,
        int32 ChannelIndex,
        const FVector& BandGains);
};

class FUERayTracingAudioOcclusionPluginFactory
{
public:
    explicit FUERayTracingAudioOcclusionPluginFactory(
        const FVector2f& InCrossoversHz);
};

class FUERayTracingAudioOcclusionPlugin
{
public:
    FUERayTracingAudioOcclusionPlugin(
        TSharedRef<FUERayTracingAudioSimulationSnapshotRegistry,
            ESPMode::ThreadSafe> InSnapshotRegistry,
        TSharedRef<FUERayTracingAudioIndirectAudioBridge,
            ESPMode::ThreadSafe> InIndirectAudioBridge,
        const FVector2f& InCrossoversHz,
        FAudioDevice* InOwningDevice = nullptr);
};
```

- `FUERayTracingAudioOcclusionSource` owns one processor and `FVector PreviousBandGains`.
- The factory receives a plain `FVector2f` crossover pair cached at module startup.

- [ ] **Step 1: Write an integration RED test against the current Occlusion plugin**

Reuse the existing real snapshot/occlusion-plugin harness. Render equal-RMS 100 Hz and 10 kHz buffers with:

```cpp
Snapshot.DirectResult.DistanceAttenuation = 1.0f;
Snapshot.DirectResult.Occlusion = 1.0f;
Snapshot.DirectResult.AirAbsorption = FVector(1.0f, 1.0f, 0.1f);
```

Assert the low-frequency output RMS is at least twice the high-frequency output RMS. Current average-air implementation applies the same scalar to both and must fail.

- [ ] **Step 2: Run RED**

Build and run the focused group. Expected: `FrequencyDependentAirAbsorption` fails because low/high output ratios are approximately equal.

- [ ] **Step 3: Implement the complementary filter**

For each channel, keep `LowMidState` and `MidHighState`. Compute:

```cpp
State.LowMid += LowMidCoefficient * (Input - State.LowMid);
State.MidHigh += MidHighCoefficient * (Input - State.MidHigh);
const float Low = State.LowMid;
const float Mid = State.MidHigh - State.LowMid;
const float High = Input - State.MidHigh;
return Low * BandGains.X + Mid * BandGains.Y + High * BandGains.Z;
```

Use:

```cpp
Coefficient = 1.0f - FMath::Exp(-2.0f * PI * CutoffHz / SampleRate);
```

Allocate channel states only in `Initialize`. `ProcessSample` checks the preallocated index and never resizes.

- [ ] **Step 4: Integrate the DSP into the real callback**

Replace scalar `PreviousGain` with `PreviousBandGains`. For a valid snapshot:

```cpp
const float Broadband =
    (bApplyDistanceAttenuation ? Result.DistanceAttenuation : 1.0f)
    * (bApplyOcclusion ? Result.Occlusion : 1.0f);
const FVector Air =
    bApplyAirAbsorption ? Result.AirAbsorption : FVector::OneVector;
const FVector TargetBandGains = Broadband * Air;
```

Interpolate the `FVector` once per frame, process each channel independently, and add Wet exactly as before. On capacity mismatch record `RecordHardRealtimeCapacityMiss()` and use a non-allocating scalar broadband fallback for that buffer.

- [ ] **Step 5: Add unity, spectrum, stereo, and setting-disable tests**

Add focused tests:

- `UnityReconstruction`: random deterministic input with `(1,1,1)`, absolute error `< 1e-6`.
- `FrequencyDependentAirAbsorption`: low RMS/high RMS `>= 2`.
- `StereoIsolation`: left impulse does not alter right filter state.
- `DisabledAirAbsorption`: output equals distance×occlusion broadband behavior.
- `PreparedCapacity`: unsupported channel count records a capacity miss without resizing.

- [ ] **Step 6: Extend the hard-real-time source audit**

Add `FUERayTracingAudioThreeBandAirAbsorption::ProcessSample` and any callback helper to `AUDITED_FUNCTIONS`. Run:

```powershell
uv run script\validate_audio_realtime_safety.py
uv run python -m unittest discover -s script\tests -v
```

Expected: zero lock, heap, shared-ownership, blocking, and UObject operations.

- [ ] **Step 7: Rebuild and verify GREEN**

Run the prescribed build, focused Automation, and the whole `UERayTracingAudio.Audio` group.

- [ ] **Step 8: Commit Task 3**

```powershell
git add Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.h `
  Source/UERayTracingAudio/Private/Audio/UERayTracingAudioThreeBandAirAbsorption.cpp `
  Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.h `
  Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp `
  Source/UERayTracingAudio/Private/UERayTracingAudioModule.cpp `
  Source/UERayTracingAudio/Public/UERayTracingAudioModule.h `
  Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp `
  script/validate_audio_realtime_safety.py
git commit -m "feat: render direct air absorption in three bands"
```

---

### Task 4: Direct Continuity Diagnostics and Public Runtime Setters

**Files:**
- Modify: `Source/UERayTracingAudio/Public/Audio/UERayTracingAudioAudioDiagnostics.h`
- Modify: `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp`
- Modify: `Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp`
- Modify: `Source/UERayTracingAudio/Public/Components/UERayTracingAudioSourceComponent.h`
- Modify: `Source/UERayTracingAudio/Private/Components/UERayTracingAudioSourceComponent.cpp`
- Modify: `Source/UERayTracingAudio/Private/Validation/UERayTracingAudioRuntimeValidation.cpp`
- Modify: `Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp`

**Interfaces:**
- Produces:

```cpp
struct FUERayTracingAudioDirectAudioStats
{
    uint64 BufferCount = 0;
    uint64 NonSilentInputBufferCount = 0;
    uint64 DirectPresentInputBufferCount = 0;
    uint64 MaxConsecutiveSilentDirectBufferCount = 0;
    uint64 NonFiniteDirectSampleCount = 0;
    uint64 OverUnitDirectSampleCount = 0;
    float MaxBandGainStep = 0.0f;
};

static void ResetDirect();
static void RecordDirectBuffer(
    uint64 AudioComponentId,
    int32 NumFrames,
    float PeakAbsoluteInput,
    float DirectRms,
    float MaxBandGainStep,
    uint64 NonFiniteDirectSampleCount,
    uint64 OverUnitDirectSampleCount);
static FUERayTracingAudioDirectAudioStats ReadDirect();
```

- Produces Blueprint setters exactly as approved:

```cpp
void SetIndirectDataSource(EUERayTracingAudioIndirectDataSource NewDataSource);
void SetBakedImpulseResponseAsset(
    UUERayTracingAudioImpulseResponseAsset* NewAsset);
```

- [ ] **Step 1: Write RED tests**

Add:

1. A reflection test requiring `SetIndirectDataSource` and `SetBakedImpulseResponseAsset` UFUNCTIONs.
2. A diagnostics epoch test that resets, records two Direct buffers, and expects one silent-run count plus a nonzero maximum band-gain step.

The first test fails because the UFUNCTIONs are absent; the second cannot pass until Direct diagnostics are exposed.

- [ ] **Step 2: Run RED**

Run prescribed build and focused Automation. Record the missing-function/diagnostics failures.

- [ ] **Step 3: Implement lock-free Direct diagnostics**

Follow the existing single-writer sequence/epoch design used by `FAtomicDataSourceStats`. Quantize the maximum gain step to the existing fixed-point scale. `RecordDirectBuffer` must take only plain values and must not wait if the diagnostic writer is busy.

Record Direct-only RMS/peak/non-finite values before adding Wet in `ProcessAudio`.

- [ ] **Step 4: Implement public setters**

`SetIndirectDataSource` must update the property only when it changes and ensure the next component tick publishes the new data-source snapshot. `SetBakedImpulseResponseAsset` must clear cached baked identities/revisions before the next refresh so assigning a new asset cannot reuse stale prepared convolution state.

Change validation F1/F2/F5 and internal validation mode changes to call `SetIndirectDataSource()`.

- [ ] **Step 5: Verify GREEN**

Run the prescribed build, focused Automation, full audio Automation, static callback audit, and Python tests.

- [ ] **Step 6: Commit Task 4**

```powershell
git add Source/UERayTracingAudio/Public/Audio/UERayTracingAudioAudioDiagnostics.h `
  Source/UERayTracingAudio/Private/Audio/UERayTracingAudioAudioDiagnostics.cpp `
  Source/UERayTracingAudio/Private/Audio/UERayTracingAudioOcclusion.cpp `
  Source/UERayTracingAudio/Public/Components/UERayTracingAudioSourceComponent.h `
  Source/UERayTracingAudio/Private/Components/UERayTracingAudioSourceComponent.cpp `
  Source/UERayTracingAudio/Private/Validation/UERayTracingAudioRuntimeValidation.cpp `
  Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp
git commit -m "feat: expose runtime modes and direct continuity diagnostics"
```

---

### Task 5: Pure Direct Sweep Trajectory and Runtime State Machine

**Files:**
- Create: `Source/UERayTracingAudio/Private/Validation/UERayTracingAudioDirectSweep.h`
- Create: `Source/UERayTracingAudio/Private/Validation/UERayTracingAudioDirectSweep.cpp`
- Modify: `Source/UERayTracingAudio/Private/Validation/UERayTracingAudioRuntimeValidation.h`
- Modify: `Source/UERayTracingAudio/Private/Validation/UERayTracingAudioRuntimeValidation.cpp`
- Modify: `Source/UERayTracingAudio/Private/UERayTracingAudioModule.cpp`
- Modify: `Source/UERayTracingAudio/Public/UERayTracingAudioModule.h`
- Modify: `Source/UERayTracingAudio/UERayTracingAudio.Build.cs`
- Modify: `Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp`

**Interfaces:**
- Produces:

```cpp
enum class EUERayTracingAudioDirectSweepPhase : uint8
{
    Idle, ClearHold, EnteringWall, OccludedHold,
    Returning, Restoring, Complete, Failed
};

struct FUERayTracingAudioDirectSweepMetrics
{
    void Reset();
    void Observe(
        uint64 DirectGeneration,
        const FUERayTracingAudioDirectSimulationResult& Result);
    bool Passes(
        const FUERayTracingAudioDirectAudioStats& AudioStats,
        bool bHardwareObserved,
        bool bRestored) const;
};

class FUERayTracingAudioDirectSweepTrajectory
{
public:
    static FVector Evaluate(
        const FVector& ListenerLocation,
        float NormalizedProgress);
};
```

- Runtime validation owns weak UObject references and the saved Source configuration; the pure helper owns no UObject.

- [ ] **Step 1: Establish a behavioral RED against the current runtime**

Read the newest fixed Game validation log and require the approved terminal marker:

```powershell
$LatestGameLog = Get-ChildItem `
  TestProject\UeVersion1\Saved\Logs\UERayTracingAudioValidation-Game-*.log |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1
if (-not (Select-String -Path $LatestGameLog.FullName -Pattern "UERayTracingAudio direct sweep: passed=1")) {
    throw "RED: current runtime has no passing hardware Direct sweep"
}
```

Expected: the command fails with the explicit RED message. This is the
behavioral failure for the feature; a missing C++ header is not accepted as
the RED evidence.

- [ ] **Step 2: Write trajectory/metrics tests before implementation**

The test includes the new header and asserts:

```cpp
for (int32 Index = 0; Index <= 100; ++Index)
{
    const FVector P = FUERayTracingAudioDirectSweepTrajectory::Evaluate(
        FVector(-100.0, 0.0, 180.0),
        static_cast<float>(Index) / 100.0f);
    TestTrue(TEXT("constant 2 m radius"),
        FMath::IsNearlyEqual(
            FVector::Distance(P, FVector(-100.0, 0.0, 180.0)),
            200.0f,
            0.01f));
}
```

Assert endpoints are the existing clear/occluded fixture positions and that the path crosses the wall plane in both directions. Add metric cases that reject too few generations, distance drift, visibility not crossing, zero soft gain, dropout, gain step `> 0.01`, CPU-only, and failed restoration.

- [ ] **Step 3: Implement the pure helper**

Use a quarter-circle outbound arc and the same reverse arc:

```cpp
const float Angle = FMath::Lerp(PI * 0.5f, 0.0f, Alpha);
return Listener + FVector(
    200.0f * FMath::Cos(Angle),
    200.0f * FMath::Sin(Angle),
    0.0f);
```

The runtime validation fixture listener is at `(-100,0,Z)`, so this yields clear `(-100,200,Z)` and occluded `(100,0,Z)`.

- [ ] **Step 4: Implement saved-state and restoration**

Before motion save:

```cpp
FTransform SourceTransform;
bool bHardOcclusion;
float OccludedGain;
float IndirectMix;
EUERayTracingAudioIndirectDataSource DataSource;
```

Temporarily use Soft Occlusion and Realtime. On success, timeout, Actor destruction, World shutdown, or validation stop, restore every saved field exactly once. Wait for a post-restore Direct generation at the restored location before marking `restored=1`.

- [ ] **Step 5: Integrate automatic sweep and F6**

- Automatic Game validation starts the sweep after the first hardware Direct result and before Bake/data-source validation.
- F6 in interactive validation returns the Listener to the origin, then starts the same state machine.
- Reject reentrant F6.
- Add HUD phase, distance, visibility, overall Direct gain, and three air-band values.
- Add F6 to interactive-ready/control log text.

In `UERayTracingAudio.Build.cs` define:

```csharp
PublicDefinitions.Add(
    $"WITH_UERAYTRACINGAUDIO_VALIDATION={(Target.Configuration != UnrealTargetConfiguration.Shipping ? 1 : 0)}");
```

Wrap the validation class declarations/definitions, module include/member,
construction, startup, and shutdown with:

```cpp
#if WITH_UERAYTRACINGAUDIO_VALIDATION
// validation-only declaration or code
#endif
```

Normal startup without the validation command remains inert.

- [ ] **Step 6: Emit the strict summary**

Emit exactly one terminal line per sweep:

```text
UERayTracingAudio direct sweep: passed=%d generations=%d distance_min_cm=%.3f distance_max_cm=%.3f visibility_min=%.6f visibility_max=%.6f gain_min=%.6f gain_max=%.6f max_gain_step=%.8f direct_dropouts=%llu restored=%d hardware=%d
```

- [ ] **Step 7: Rebuild and verify GREEN**

Run prescribed build and focused Automation. Do not run the full runtime gate until Task 6 can parse the new required marker.

- [ ] **Step 8: Commit Task 5**

```powershell
git add Source/UERayTracingAudio/Private/Validation/UERayTracingAudioDirectSweep.h `
  Source/UERayTracingAudio/Private/Validation/UERayTracingAudioDirectSweep.cpp `
  Source/UERayTracingAudio/Private/Validation/UERayTracingAudioRuntimeValidation.h `
  Source/UERayTracingAudio/Private/Validation/UERayTracingAudioRuntimeValidation.cpp `
  Source/UERayTracingAudio/Private/UERayTracingAudioModule.cpp `
  Source/UERayTracingAudio/Public/UERayTracingAudioModule.h `
  Source/UERayTracingAudio/UERayTracingAudio.Build.cs `
  Source/UERayTracingAudio/Private/Tests/UERayTracingAudioConfigurableDirectTests.cpp
git commit -m "feat: add isolated hardware direct sweep fixture"
```

---

### Task 6: Direct Sweep Launcher and Parser Gate

**Files:**
- Modify: `script/launch_runtime_validation.py`
- Modify: `script/tests/test_validation_scripts.py`

**Interfaces:**
- Produces:

```python
DIRECT_SWEEP_MARKER = "UERayTracingAudio direct sweep:"
DIRECT_SWEEP_PATTERN = re.compile(
    r"UERayTracingAudio direct sweep: passed=(?P<passed>[01]) "
    r"generations=(?P<generations>[0-9]+) "
    r"distance_min_cm=(?P<distance_min>[0-9.eE+-]+) "
    r"distance_max_cm=(?P<distance_max>[0-9.eE+-]+) "
    r"visibility_min=(?P<visibility_min>[0-9.eE+-]+) "
    r"visibility_max=(?P<visibility_max>[0-9.eE+-]+) "
    r"gain_min=(?P<gain_min>[0-9.eE+-]+) "
    r"gain_max=(?P<gain_max>[0-9.eE+-]+) "
    r"max_gain_step=(?P<max_gain_step>[0-9.eE+-]+) "
    r"direct_dropouts=(?P<direct_dropouts>[0-9]+) "
    r"restored=(?P<restored>[01]) hardware=(?P<hardware>[01])"
)

def validate_direct_sweep(log_text: str) -> dict[str, float | int]:
    match = DIRECT_SWEEP_PATTERN.search(log_text)
    if match is None:
        raise RuntimeError("missing parseable hardware Direct sweep")
    values = {
        key: float(value) if key not in {
            "passed", "generations", "direct_dropouts", "restored", "hardware"
        } else int(value)
        for key, value in match.groupdict().items()
    }
    failures: list[str] = []
    if values["passed"] != 1:
        failures.append("passing Direct sweep")
    if values["generations"] < 8:
        failures.append("at least eight Direct generations")
    if values["distance_min"] < 198.0 or values["distance_max"] > 202.0:
        failures.append("constant two-metre distance")
    if values["visibility_min"] > 0.10 or values["visibility_max"] < 0.90:
        failures.append("Clear and Occluded visibility endpoints")
    if values["gain_min"] <= 0.0:
        failures.append("nonzero Soft Occlusion gain")
    if values["max_gain_step"] > 0.01:
        failures.append("bounded per-sample gain step")
    if values["direct_dropouts"] != 0:
        failures.append("zero Direct dropouts")
    if values["restored"] != 1:
        failures.append("restored Source state")
    if values["hardware"] != 1:
        failures.append("hardware provenance")
    if failures:
        raise RuntimeError("Direct sweep failed: " + ", ".join(failures))
    return values
```

- [ ] **Step 1: Write failing Python tests**

Add tests requiring:

```python
command = build_game_command(
    Path("UnrealEditor.exe"),
    Path("Test.uproject"),
    Path("Game.log"),
)
self.assertIn("-UERayTracingAudioValidationDirectSweep", command)
```

Create one passing synthetic marker and rejection cases for:

- missing marker;
- `passed=0`;
- fewer than 8 generations;
- distance outside 198–202 cm;
- visibility range not covering `>= 0.90` and `<= 0.10`;
- `gain_min <= 0`;
- `max_gain_step > 0.01`;
- nonzero Direct dropout;
- `restored=0`;
- `hardware=0`.

- [ ] **Step 2: Run RED**

```powershell
uv run python -m unittest script.tests.test_validation_scripts.RuntimeValidationTests -v
```

Expected: failures for missing command flag/function/pattern.

- [ ] **Step 3: Implement parser and gate**

Add `-UERayTracingAudioValidationDirectSweep` to every fixed Game validation command. Parse the final marker and append explicit failure reasons. Require it in runtime evidence before accepting data-source validation.

Do not require an automatic sweep in the Editor command; Editor exposes F6 for the user.

- [ ] **Step 4: Run GREEN**

```powershell
uv run python -m unittest discover -s script\tests -v
uv run script\validate_audio_realtime_safety.py
```

- [ ] **Step 5: Run the prescribed runtime validation**

```powershell
uv run script\launch_runtime_validation.py
```

Expected: Game log contains `direct sweep: passed=1`, then existing Baked/Realtime/Hybrid and hard-real-time markers pass, and Editor is left initialized for interactive testing.

If the process crashes or the new marker never appears, enter `workflow/crash-debugging.md` immediately.

- [ ] **Step 6: Commit Task 6**

```powershell
git add script/launch_runtime_validation.py script/tests/test_validation_scripts.py
git commit -m "test: gate runtime validation on direct sweep"
```

---

### Task 7: Editor Direct Fixture Controls

**Files:**
- Modify: `Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.h`
- Modify: `Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.cpp`
- Modify: `Source/UERayTracingAudioEditor/Private/UERayTracingAudioEditorModule.cpp`
- Modify: `Source/UERayTracingAudioEditor/Private/Tests/UERayTracingAudioOfflineComparisonTests.cpp`
- Modify: `script/launch_runtime_validation.py`
- Modify: `script/tests/test_validation_scripts.py`

**Interfaces:**
- Produces:

```cpp
enum class EUERayTracingAudioEditorAirAbsorptionProfile : uint8
{
    Off, Default, Stress
};

static FUERayTracingAudioEditorValidationSceneResult EnsureScene(
    UWorld& World,
    EUERayTracingAudioEditorValidationSceneMode Mode,
    EUERayTracingAudioEditorDirectPreset DirectPreset,
    EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment,
    float DistanceCmOverride,
    EUERayTracingAudioEditorAirAbsorptionProfile AirProfile);
```

- Only actors tagged `VRTA_EditorValidationScene` may be mutated by these controls.

- [ ] **Step 1: Write failing Editor Automation tests**

In a transient Editor World:

1. Create Clear at 100 cm and assert result distance `100 ± 0.1`.
2. Reuse at 400 cm and assert the existing tagged Source moved to `400 ± 0.1`.
3. Apply Off/Default/Stress and assert exact Source vectors.
4. Switch Enclosed → OpenSpace and assert old tagged acoustic Geometry actors are removed, not merely omitted from `GeometryCount`.
5. Create an untagged Source and assert fixture controls never change it.

Current code has no air profile and retains old environment geometry, so RED is expected.

- [ ] **Step 2: Run RED**

Run prescribed build and `UERayTracingAudio` Editor Automation. Record the profile/cleanup failures.

- [ ] **Step 3: Implement tagged fixture mutation**

Before creating the selected reflection environment, enumerate actors with `VRTA_EditorValidationScene` and destroy only tagged geometry roles not present in the selected definition set. Reuse and move the tagged Source; never search for or mutate a generic first Source.

Map profiles exactly:

```cpp
Off     -> FVector::ZeroVector
Default -> FVector(0.0002f, 0.0006f, 0.0012f)
Stress  -> FVector(0.01f, 0.04f, 0.12f)
```

- [ ] **Step 4: Add Slate controls**

Add two clearly labeled rows:

```text
Validation Distance: Clear 1 m | Clear 2 m | Clear 4 m
Validation Air Absorption: Off | Default | Stress
```

Display current distance/vector plus `Validation fixture only`. Disable controls while Bake/offline render is active.

- [ ] **Step 5: Add CLI plumbing tests and implementation**

Add launcher options:

```text
--editor-distance-cm {100,200,400}
--editor-air-absorption-profile {off,default,stress}
```

Pass:

```python
command.extend(
    (
        f"-UERayTracingAudioValidationDistanceCm={editor_distance_cm:g}",
        "-UERayTracingAudioValidationAirAbsorptionProfile="
        f"{editor_air_absorption_profile}",
    )
)
```

Update the Editor scene-ready marker to include the profile and vector, and test the command without claiming an R3 reflection-environment Human Pass.

- [ ] **Step 6: Rebuild and verify GREEN**

Run prescribed build, full plugin Automation, and Python tests.

- [ ] **Step 7: Run prescribed Editor/runtime validation**

```powershell
uv run script\launch_runtime_validation.py
```

Confirm the script leaves the Editor initialized. The automated log proves the controls exist and the scene is ready; it does not prove audibility.

- [ ] **Step 8: Commit Task 7**

```powershell
git add Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.h `
  Source/UERayTracingAudioEditor/Private/Validation/UERayTracingAudioEditorValidationScene.cpp `
  Source/UERayTracingAudioEditor/Private/UERayTracingAudioEditorModule.cpp `
  Source/UERayTracingAudioEditor/Private/Tests/UERayTracingAudioOfflineComparisonTests.cpp `
  script/launch_runtime_validation.py `
  script/tests/test_validation_scripts.py
git commit -m "feat: add editor direct validation controls"
```

---

### Task 8: Documentation, Shipping Isolation, and Final Verification

**Files:**
- Modify: `TODO.md`
- Modify: `USAGE.md`
- Modify: `IMPLEMENTATION_STATUS.md`
- Modify: `plan.md`
- Modify: `progress_log.md`
- Modify if required by verification: only files from Tasks 1–7

**Interfaces:**
- Documentation must distinguish general product configuration from the opt-in validation fixture.
- This task produces final evidence but does not mark Human Pass or R3 environment work complete.

- [ ] **Step 1: Update project documentation**

Document:

- Project Settings physical parameters and restart behavior.
- Per-Source air absorption and Blueprint setters.
- Per-World Listener behavior.
- Unity reconstruction and frequency-dependent Direct behavior.
- `-UERayTracingAudioValidationScenario` isolation.
- F6 sweep phases and parsed marker.
- Editor 1/2/4 m and Off/Default/Stress controls.
- Remaining Human Pass, moving-player listening, open-space, and near-wall work.

Mark only the implemented automatic/technical R2 items complete.

- [ ] **Step 2: Run static and Python verification**

```powershell
uv run script\validate_audio_realtime_safety.py
uv run python -m unittest discover -s script\tests -v
git diff --check
```

Expected: zero forbidden callback operations, all Python tests pass, and no whitespace errors.

- [ ] **Step 3: Run the prescribed Development build**

```powershell
uv run script\build_and_validate.py
```

Expected: sync, UE 5.7 test project build, and standalone plugin build all exit 0.

- [ ] **Step 4: Run focused, audio, and full Automation**

Use UnrealEditor-Cmd three times for:

```text
UERayTracingAudio.Audio.ConfigurableDirect
UERayTracingAudio.Audio
UERayTracingAudio
```

Record exact pass counts and log paths in `IMPLEMENTATION_STATUS.md` and `progress_log.md`.

- [ ] **Step 5: Verify Shipping excludes validation runtime**

Run:

```powershell
uv run script\build_and_validate.py --target UeVersion1 --configuration Shipping
```

Inspect the Shipping log/binary strings or compile output to confirm validation entry points are excluded while normal Direct/Indirect modules build.

- [ ] **Step 6: Run the prescribed fixed Game and Editor flow**

```powershell
uv run script\launch_runtime_validation.py
```

Require:

- hardware Direct and Indirect RHI markers;
- Direct sweep `passed=1`;
- `distance_min/max` within 198–202 cm;
- visibility covers Clear and Occluded;
- positive soft Direct gain;
- zero Direct dropout;
- maximum band-gain step `<= 0.01`;
- restored Source;
- Baked/Realtime/Hybrid continuous Wet;
- hard-real-time capacity misses and prepare drops remain zero;
- Editor scene/UI readiness.

- [ ] **Step 7: Leave the Editor for Human verification**

In PIE:

1. Wait for the hardware/data-source gates.
2. Press F3 to compare Original and Rendered.
3. Press F6 and listen across Clear → wall → Soft Occluded → Clear.
4. Confirm no restart, click/pop, unexpected silence, or timing jump.
5. In Bake panel compare Clear 1/2/4 m and Default/Stress air absorption.

Record Human Pass only if the user performs and confirms this step.

- [ ] **Step 8: Commit documentation and any verified corrections**

```powershell
git add TODO.md USAGE.md IMPLEMENTATION_STATUS.md plan.md progress_log.md
git commit -m "docs: document configurable direct audio validation"
```

Do not declare the overall plugin goal complete while Human Pass or the separate R3 open-space/near-wall matrix remains unverified.
