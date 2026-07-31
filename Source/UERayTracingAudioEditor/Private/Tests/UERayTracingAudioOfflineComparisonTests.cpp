#include "Bake/UERayTracingAudioOfflineRenderer.h"

#include "Components/SceneComponent.h"
#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Validation/UERayTracingAudioEditorValidationScene.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioEditorValidationFixtureControlsTest,
    "UERayTracingAudio.Editor.ValidationFixtureControls",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioEditorValidationFixtureControlsTest::RunTest(
    const FString& Parameters)
{
    static_cast<void>(Parameters);
    UWorld* World = UWorld::CreateWorld(
        EWorldType::Editor,
        false,
        TEXT("UERayTracingAudioEditorValidationFixtureControls"));
    TestNotNull(TEXT("Transient Editor World"), World);
    if (!World)
    {
        return false;
    }

    AActor* UntaggedActor = World->SpawnActor<AActor>();
    TestNotNull(TEXT("Untagged Source actor"), UntaggedActor);
    if (!UntaggedActor)
    {
        World->DestroyWorld(false);
        return false;
    }

    USceneComponent* UntaggedRoot =
        NewObject<USceneComponent>(UntaggedActor, TEXT("UntaggedRoot"));
    UntaggedActor->AddInstanceComponent(UntaggedRoot);
    UntaggedActor->SetRootComponent(UntaggedRoot);
    UntaggedRoot->RegisterComponent();
    const FVector UntaggedLocation(123.0f, 456.0f, 789.0f);
    UntaggedActor->SetActorLocation(UntaggedLocation);

    UUERayTracingAudioSourceComponent* UntaggedSource =
        NewObject<UUERayTracingAudioSourceComponent>(
            UntaggedActor,
            TEXT("UntaggedSource"));
    UntaggedActor->AddInstanceComponent(UntaggedSource);
    UntaggedSource->AirAbsorptionPerMeter = FVector(9.0f, 8.0f, 7.0f);
    UntaggedSource->RegisterComponent();

    UUERayTracingAudioGeometryComponent* UntaggedGeometry =
        NewObject<UUERayTracingAudioGeometryComponent>(
            UntaggedActor,
            TEXT("UntaggedGeometry"));
    UntaggedActor->AddInstanceComponent(UntaggedGeometry);
    UntaggedGeometry->RegisterComponent();

    const auto CountTaggedGeometry = [World]()
    {
        int32 Count = 0;
        for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
        {
            if (ActorIt->ActorHasTag(
                    FName(TEXT("VRTA_EditorValidationScene")))
                && ActorIt->FindComponentByClass<
                    UUERayTracingAudioGeometryComponent>())
            {
                ++Count;
            }
        }
        return Count;
    };

    const FUERayTracingAudioEditorValidationSceneResult OneMeterOff =
        FUERayTracingAudioEditorValidationScene::EnsureScene(
            *World,
            EUERayTracingAudioEditorValidationSceneMode::Transient,
            EUERayTracingAudioEditorDirectPreset::Clear,
            EUERayTracingAudioEditorReflectionEnvironment::Enclosed,
            100.0f,
            EUERayTracingAudioEditorAirAbsorptionProfile::Off);
    TestTrue(TEXT("One-meter Off fixture succeeds"), OneMeterOff.bSucceeded);
    TestTrue(
        TEXT("Clear fixture distance is exactly 100 cm"),
        FMath::IsNearlyEqual(
            OneMeterOff.SourceListenerDistanceCm,
            100.0f,
            0.1f));
    TestEqual(TEXT("Enclosed fixture owns seven geometry actors"), CountTaggedGeometry(), 7);
    TestEqual(
        TEXT("Off profile is exact"),
        OneMeterOff.Source.IsValid()
            ? OneMeterOff.Source->GetAirAbsorptionPerMeter()
            : FVector(-1.0f),
        FVector::ZeroVector);

    AActor* TaggedSourceActor = OneMeterOff.Source.IsValid()
        ? OneMeterOff.Source->GetOwner()
        : nullptr;
    AActor* TaggedListenerActor = OneMeterOff.Listener.IsValid()
        ? OneMeterOff.Listener->GetOwner()
        : nullptr;

    const FUERayTracingAudioEditorValidationSceneResult TwoMeterDefault =
        FUERayTracingAudioEditorValidationScene::EnsureScene(
            *World,
            EUERayTracingAudioEditorValidationSceneMode::Transient,
            EUERayTracingAudioEditorDirectPreset::Clear,
            EUERayTracingAudioEditorReflectionEnvironment::Enclosed,
            200.0f,
            EUERayTracingAudioEditorAirAbsorptionProfile::Default);
    TestTrue(
        TEXT("Clear fixture distance is exactly 200 cm"),
        FMath::IsNearlyEqual(
            TwoMeterDefault.SourceListenerDistanceCm,
            200.0f,
            0.1f));
    TestTrue(
        TEXT("Tagged validation Source is reused at 200 cm"),
        TwoMeterDefault.Source.IsValid()
            && TwoMeterDefault.Source->GetOwner() == TaggedSourceActor);
    TestEqual(
        TEXT("Default profile is exact"),
        TwoMeterDefault.Source.IsValid()
            ? TwoMeterDefault.Source->GetAirAbsorptionPerMeter()
            : FVector(-1.0f),
        FVector(0.0002f, 0.0006f, 0.0012f));

    const FUERayTracingAudioEditorValidationSceneResult FourMeterStress =
        FUERayTracingAudioEditorValidationScene::EnsureScene(
            *World,
            EUERayTracingAudioEditorValidationSceneMode::Transient,
            EUERayTracingAudioEditorDirectPreset::Clear,
            EUERayTracingAudioEditorReflectionEnvironment::Enclosed,
            400.0f,
            EUERayTracingAudioEditorAirAbsorptionProfile::Stress);
    TestTrue(
        TEXT("Clear fixture distance is exactly 400 cm"),
        FMath::IsNearlyEqual(
            FourMeterStress.SourceListenerDistanceCm,
            400.0f,
            0.1f));
    TestTrue(
        TEXT("Tagged validation Source is moved rather than duplicated"),
        FourMeterStress.Source.IsValid()
            && FourMeterStress.Source->GetOwner() == TaggedSourceActor);
    TestEqual(
        TEXT("Stress profile is exact"),
        FourMeterStress.Source.IsValid()
            ? FourMeterStress.Source->GetAirAbsorptionPerMeter()
            : FVector(-1.0f),
        FVector(0.01f, 0.04f, 0.12f));

    const FUERayTracingAudioEditorValidationSceneResult OpenSpace =
        FUERayTracingAudioEditorValidationScene::EnsureScene(
            *World,
            EUERayTracingAudioEditorValidationSceneMode::Transient,
            EUERayTracingAudioEditorDirectPreset::Clear,
            EUERayTracingAudioEditorReflectionEnvironment::OpenSpace,
            400.0f,
            EUERayTracingAudioEditorAirAbsorptionProfile::Stress);
    TestTrue(TEXT("Open-space fixture succeeds"), OpenSpace.bSucceeded);
    TestEqual(
        TEXT("Switching to OpenSpace actually destroys stale tagged geometry actors"),
        CountTaggedGeometry(),
        0);
    TestTrue(
        TEXT("Tagged non-geometry Source survives environment cleanup"),
        OpenSpace.Source.IsValid()
            && OpenSpace.Source->GetOwner() == TaggedSourceActor);
    TestTrue(
        TEXT("Tagged non-geometry Listener survives environment cleanup"),
        OpenSpace.Listener.IsValid()
            && OpenSpace.Listener->GetOwner() == TaggedListenerActor);
    TestTrue(
        TEXT("Untagged geometry actor survives fixture cleanup"),
        IsValid(UntaggedActor)
            && IsValid(UntaggedGeometry)
            && UntaggedActor->FindComponentByClass<
                UUERayTracingAudioGeometryComponent>() == UntaggedGeometry);
    TestEqual(
        TEXT("Untagged Source location is never mutated"),
        UntaggedActor->GetActorLocation(),
        UntaggedLocation);
    TestEqual(
        TEXT("Untagged Source air absorption is never mutated"),
        UntaggedSource->GetAirAbsorptionPerMeter(),
        FVector(9.0f, 8.0f, 7.0f));

    World->DestroyWorld(false);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOfflineComparisonContractTest,
    "UERayTracingAudio.Editor.OfflineComparisonContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOfflineComparisonContractTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioOfflineRenderRequest Request;
    Request.InputSampleRate = 8000;
    Request.OutputSampleRate = 8000;
    Request.NumChannels = 1;
    Request.DirectGain = 0.6f;
    Request.WetMix = 0.25f;
    Request.ImpulseResponse = { 0.0f, 0.5f, 0.25f };
    Request.OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("UERayTracingAudio"),
        TEXT("Automation"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Request.OutputFilenamePrefix = TEXT("AlignedComparison");
    Request.InputAssetPath = TEXT("/Game/Automation/TestInput.TestInput");
    Request.SourceActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Source");
    Request.ListenerActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Listener");
    Request.SceneSignature = TEXT("automation-scene");
    Request.bUsedHardwareRayTracing = true;

    constexpr int32 NumInputFrames = 256;
    Request.InputInterleavedPcm.Reserve(NumInputFrames);
    for (int32 FrameIndex = 0; FrameIndex < NumInputFrames; ++FrameIndex)
    {
        const float Phase = 2.0f * PI * 440.0f * static_cast<float>(FrameIndex) / 8000.0f;
        Request.InputInterleavedPcm.Add(
            static_cast<int16>(FMath::RoundToInt(FMath::Sin(Phase) * 12000.0f)));
    }

    const FUERayTracingAudioOfflineRenderResult Result =
        FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(MoveTemp(Request));

    TestTrue(TEXT("Comparison render succeeds"), Result.bSucceeded);
    TestEqual(TEXT("All modes share the exact output frame count"), Result.NumFrames, NumInputFrames + 2);
    TestTrue(TEXT("Direct retains the dry waveform"), Result.DirectDryCorrelation >= 0.999f);
    TestTrue(TEXT("Full retains recognizable dry content"), Result.FullDryCorrelation >= 0.35f);
    TestTrue(TEXT("Direct retains meaningful dry level"), Result.DirectToReferenceRmsRatio >= 0.05f);
    TestTrue(TEXT("Wet has audible spatial energy"), Result.WetToReferenceRmsRatio >= 0.05f);
    TestTrue(TEXT("Direct and wet are measurably distinct"), Result.bModesAreDistinct);
    TestTrue(TEXT("Direct/wet normalized difference is meaningful"), Result.DirectWetNormalizedDifference >= 0.05f);
    TestTrue(TEXT("Automatic dry/alignment checks pass"), Result.bAutomaticChecksPassed);
    TestTrue(TEXT("Direct preset semantics pass"), Result.bDirectSemanticsPassed);
    TestTrue(TEXT("Audio safety checks pass"), Result.bAudioSafetyChecksPassed);
    TestTrue(TEXT("All generated samples are finite"), Result.bSamplesFinite);
    TestEqual(TEXT("No output sample clips"), Result.ClippedSampleCount, static_cast<int64>(0));
    TestEqual(TEXT("No active direct window drops out"), Result.DirectDropoutWindowCount, 0);
    TestTrue(TEXT("Direct dropout analysis saw active input"), Result.DirectActiveWindowCount > 0);
    TestTrue(TEXT("Direct signal remains sample-aligned"), Result.DirectModelResidualRms <= 1.0e-6f);
    TestTrue(TEXT("Full is the exact direct plus wet mix"), Result.FullMixResidualRms <= 1.0e-6f);
    TestTrue(TEXT("Direct introduces no discontinuity"), Result.MaxDirectDiscontinuityResidual <= 1.0e-5f);
    TestTrue(TEXT("One common non-amplifying scale is recorded"), Result.CommonOutputScale > 0.0f && Result.CommonOutputScale <= 1.0f);
    TestTrue(TEXT("Hardware provenance is retained"), Result.bUsedHardwareRayTracing);

    const int64 ReferenceSize = IFileManager::Get().FileSize(*Result.ReferenceWaveFilename);
    const int64 DirectSize = IFileManager::Get().FileSize(*Result.DirectWaveFilename);
    const int64 WetSize = IFileManager::Get().FileSize(*Result.WetWaveFilename);
    const int64 FullSize = IFileManager::Get().FileSize(*Result.FullWaveFilename);
    TestTrue(TEXT("Reference WAV exists"), ReferenceSize > 44);
    TestEqual(TEXT("Direct WAV is sample-aligned"), DirectSize, ReferenceSize);
    TestEqual(TEXT("Wet WAV is sample-aligned"), WetSize, ReferenceSize);
    TestEqual(TEXT("Full WAV is sample-aligned"), FullSize, ReferenceSize);
    TestTrue(TEXT("Comparison manifest exists"), IFileManager::Get().FileSize(*Result.ManifestFilename) > 0);

    IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Result.ManifestFilename), false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOfflineDirectionalStereoTest,
    "UERayTracingAudio.Editor.OfflineDirectionalStereo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOfflineDirectionalStereoTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioOfflineRenderRequest Request;
    Request.InputSampleRate = 8000;
    Request.OutputSampleRate = 8000;
    Request.NumChannels = 1;
    Request.DirectGain = 0.6f;
    Request.WetMix = 0.25f;
    Request.ImpulseResponseNumChannels = 2;
    Request.ImpulseResponse = {
        0.0f, 0.0f,
        0.7f, 0.1f,
        0.2f, 0.5f,
    };
    Request.OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("UERayTracingAudio"),
        TEXT("Automation"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Request.OutputFilenamePrefix = TEXT("DirectionalStereo");
    Request.InputAssetPath = TEXT("/Game/Automation/TestInput.TestInput");
    Request.SourceActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Source");
    Request.ListenerActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Listener");
    Request.SceneSignature = TEXT("automation-directional-scene");
    Request.bUsedHardwareRayTracing = true;

    constexpr int32 NumInputFrames = 256;
    Request.InputInterleavedPcm.Reserve(NumInputFrames);
    for (int32 FrameIndex = 0; FrameIndex < NumInputFrames; ++FrameIndex)
    {
        const float Phase = 2.0f * PI * 440.0f * static_cast<float>(FrameIndex) / 8000.0f;
        Request.InputInterleavedPcm.Add(
            static_cast<int16>(FMath::RoundToInt(FMath::Sin(Phase) * 12000.0f)));
    }

    const FUERayTracingAudioOfflineRenderResult Result =
        FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(MoveTemp(Request));

    TestTrue(TEXT("Directional stereo comparison succeeds"), Result.bSucceeded);
    TestEqual(TEXT("Directional IR layout is retained"), Result.ImpulseResponseNumChannels, 2);
    TestEqual(TEXT("Mono input is rendered to stereo"), Result.NumChannels, 2);
    TestTrue(TEXT("Wet left and right channels are measurably different"), Result.WetStereoNormalizedDifference >= 0.01f);
    TestTrue(TEXT("Directional wet output is explicitly accepted"), Result.bDirectionalWetIsDistinct);
    TestTrue(TEXT("Stereo render keeps direct and wet modes distinct"), Result.bModesAreDistinct);
    TestTrue(TEXT("Stereo render passes automatic checks"), Result.bAutomaticChecksPassed);
    TestTrue(TEXT("Stereo render passes audio safety checks"), Result.bAudioSafetyChecksPassed);

    const int64 ReferenceSize = IFileManager::Get().FileSize(*Result.ReferenceWaveFilename);
    TestTrue(TEXT("Directional reference WAV exists"), ReferenceSize > 44);
    TestEqual(TEXT("Directional wet WAV remains sample-aligned"), IFileManager::Get().FileSize(*Result.WetWaveFilename), ReferenceSize);
    TestEqual(TEXT("Directional full WAV remains sample-aligned"), IFileManager::Get().FileSize(*Result.FullWaveFilename), ReferenceSize);

    IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Result.ManifestFilename), false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOfflineComparisonRejectsInaudibleDirectTest,
    "UERayTracingAudio.Editor.OfflineComparisonRejectsInaudibleDirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOfflineComparisonRejectsInaudibleDirectTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioOfflineRenderRequest Request;
    Request.InputSampleRate = 8000;
    Request.OutputSampleRate = 8000;
    Request.NumChannels = 1;
    Request.DirectGain = 0.001f;
    Request.WetMix = 0.25f;
    Request.ImpulseResponse = { 0.0f, 0.5f, 0.25f };
    Request.OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("UERayTracingAudio"),
        TEXT("Automation"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Request.OutputFilenamePrefix = TEXT("InaudibleDirect");
    Request.InputAssetPath = TEXT("/Game/Automation/TestInput.TestInput");
    Request.SourceActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Source");
    Request.ListenerActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Listener");
    Request.SceneSignature = TEXT("automation-scene");
    Request.bUsedHardwareRayTracing = true;

    constexpr int32 NumInputFrames = 256;
    Request.InputInterleavedPcm.Reserve(NumInputFrames);
    for (int32 FrameIndex = 0; FrameIndex < NumInputFrames; ++FrameIndex)
    {
        const float Phase = 2.0f * PI * 440.0f * static_cast<float>(FrameIndex) / 8000.0f;
        Request.InputInterleavedPcm.Add(
            static_cast<int16>(FMath::RoundToInt(FMath::Sin(Phase) * 12000.0f)));
    }

    const FUERayTracingAudioOfflineRenderResult Result =
        FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(MoveTemp(Request));
    TestTrue(TEXT("Comparison render still succeeds"), Result.bSucceeded);
    TestTrue(TEXT("Correlation alone remains deceptively high"), Result.DirectDryCorrelation >= 0.999f);
    TestTrue(TEXT("Direct level ratio exposes the inaudible result"), Result.DirectToReferenceRmsRatio < 0.05f);
    TestFalse(TEXT("Automatic checks reject inaudible direct"), Result.bAutomaticChecksPassed);

    IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Result.ManifestFilename), false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOfflineComparisonRejectsInaudibleWetTest,
    "UERayTracingAudio.Editor.OfflineComparisonRejectsInaudibleWet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOfflineComparisonRejectsInaudibleWetTest::RunTest(
    const FString& Parameters)
{
    FUERayTracingAudioOfflineRenderRequest Request;
    Request.InputSampleRate = 8000;
    Request.OutputSampleRate = 8000;
    Request.NumChannels = 1;
    Request.DirectGain = 0.6f;
    Request.WetMix = 0.001f;
    Request.ImpulseResponse = { 0.0f, 0.5f, 0.25f };
    Request.OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("UERayTracingAudio"),
        TEXT("Automation"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Request.OutputFilenamePrefix = TEXT("InaudibleWet");
    Request.InputAssetPath = TEXT("/Game/Automation/TestInput.TestInput");
    Request.SourceActorPath =
        TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Source");
    Request.ListenerActorPath =
        TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Listener");
    Request.SceneSignature = TEXT("automation-scene");
    Request.bUsedHardwareRayTracing = true;

    constexpr int32 NumInputFrames = 256;
    Request.InputInterleavedPcm.Reserve(NumInputFrames);
    for (int32 FrameIndex = 0; FrameIndex < NumInputFrames; ++FrameIndex)
    {
        const float Phase =
            2.0f
            * PI
            * 440.0f
            * static_cast<float>(FrameIndex)
            / 8000.0f;
        Request.InputInterleavedPcm.Add(static_cast<int16>(
            FMath::RoundToInt(FMath::Sin(Phase) * 12000.0f)));
    }

    const FUERayTracingAudioOfflineRenderResult Result =
        FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(
            MoveTemp(Request));
    TestTrue(TEXT("Comparison render still succeeds"), Result.bSucceeded);
    TestTrue(
        TEXT("Wet level ratio exposes the inaudible result"),
        Result.WetToReferenceRmsRatio < 0.05f);
    TestFalse(
        TEXT("Mode distinction rejects inaudible wet"),
        Result.bModesAreDistinct);
    TestFalse(
        TEXT("Automatic checks reject inaudible wet"),
        Result.bAutomaticChecksPassed);

    IFileManager::Get().DeleteDirectory(
        *FPaths::GetPath(Result.ManifestFilename),
        false,
        true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOfflineComparisonHardOcclusionTest,
    "UERayTracingAudio.Editor.OfflineComparisonHardOcclusion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOfflineComparisonHardOcclusionTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioOfflineRenderRequest Request;
    Request.InputSampleRate = 8000;
    Request.OutputSampleRate = 8000;
    Request.NumChannels = 1;
    Request.DirectGain = 0.0f;
    Request.WetMix = 0.25f;
    Request.DirectPreset = TEXT("hard_occluded");
    Request.DirectVisibility = 0.0f;
    Request.DirectOcclusion = 0.0f;
    Request.ImpulseResponse = { 0.0f, 0.5f, 0.25f };
    Request.OutputDirectory = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("UERayTracingAudio"),
        TEXT("Automation"),
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Request.OutputFilenamePrefix = TEXT("HardOccludedComparison");
    Request.InputAssetPath = TEXT("/Game/Automation/TestInput.TestInput");
    Request.SourceActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Source");
    Request.ListenerActorPath = TEXT("/Game/Automation/TestMap.TestMap:PersistentLevel.Listener");
    Request.SceneSignature = TEXT("automation-scene");
    Request.bUsedHardwareRayTracing = true;

    constexpr int32 NumInputFrames = 256;
    Request.InputInterleavedPcm.Reserve(NumInputFrames);
    for (int32 FrameIndex = 0; FrameIndex < NumInputFrames; ++FrameIndex)
    {
        const float Phase = 2.0f * PI * 440.0f * static_cast<float>(FrameIndex) / 8000.0f;
        Request.InputInterleavedPcm.Add(
            static_cast<int16>(FMath::RoundToInt(FMath::Sin(Phase) * 12000.0f)));
    }

    const FUERayTracingAudioOfflineRenderResult Result =
        FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(MoveTemp(Request));

    TestTrue(TEXT("Hard-occluded render succeeds"), Result.bSucceeded);
    TestTrue(TEXT("Hard occlusion intentionally silences direct"), Result.DirectToReferenceRmsRatio <= 1.0e-4f);
    TestTrue(TEXT("Hard occlusion semantics pass"), Result.bDirectSemanticsPassed);
    TestFalse(TEXT("Expected hard silence is not analyzed as a dropout"), Result.bDirectDropoutCheckApplicable);
    TestEqual(TEXT("Expected hard silence has no reported dropout"), Result.DirectDropoutWindowCount, 0);
    TestTrue(TEXT("Reflected full output remains non-silent"), Result.FullToReferenceRmsRatio >= 0.001f);
    TestTrue(TEXT("Hard-occluded modes remain distinct"), Result.bModesAreDistinct);
    TestTrue(TEXT("Hard-occluded audio safety checks pass"), Result.bAudioSafetyChecksPassed);
    TestTrue(TEXT("Hard-occluded automatic checks pass"), Result.bAutomaticChecksPassed);

    IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Result.ManifestFilename), false, true);
    return true;
}

#endif
