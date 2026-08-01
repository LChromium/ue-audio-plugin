#include "Assets/UERayTracingAudioImpulseResponseAsset.h"
#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioConvolution.h"
#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Audio/UERayTracingAudioIndirectRenderer.h"
#include "Audio/UERayTracingAudioOcclusion.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "Audio/UERayTracingAudioSpatialization.h"
#include "Misc/AutomationTest.h"
#include "Sound/SoundGenerator.h"
#include "Tests/UERayTracingAudioPreparedRendererTestHarness.h"
#include "UObject/Package.h"
#include "UERayTracingAudioModule.h"
#include "Validation/UERayTracingAudioValidationSoundWave.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    float CalculateEnergy(const TArray<float>& Samples)
    {
        float Energy = 0.0f;
        for (const float Sample : Samples)
        {
            Energy += Sample * Sample;
        }
        return Energy;
    }

    float CalculateEnergyRange(const TArray<float>& Samples, const int32 BeginIndex, const int32 EndIndex)
    {
        float Energy = 0.0f;
        for (int32 SampleIndex = FMath::Max(BeginIndex, 0);
            SampleIndex < FMath::Min(EndIndex, Samples.Num());
            ++SampleIndex)
        {
            Energy += Samples[SampleIndex] * Samples[SampleIndex];
        }
        return Energy;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioValidationSoundWaveGeneratorTest,
    "UERayTracingAudio.Audio.ValidationSoundWaveGenerator",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioValidationSoundWaveGeneratorTest::RunTest(const FString& Parameters)
{
    constexpr int32 FramesPerBlock = 1024;
    constexpr int32 NumChannels = 2;
    constexpr int32 SamplesPerBlock = FramesPerBlock * NumChannels;
    TArray<float> Samples;
    Samples.SetNumUninitialized(SamplesPerBlock * 2);
    for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
    {
        Samples[SampleIndex] = static_cast<float>(SampleIndex + 1)
            / static_cast<float>(Samples.Num());
    }
    const TArray<float> ExpectedSamples = Samples;

    UUERayTracingAudioValidationSoundWave* SoundWave =
        NewObject<UUERayTracingAudioValidationSoundWave>(GetTransientPackage());
    SoundWave->NumChannels = NumChannels;
    SoundWave->SetSampleRate(48000);
    SoundWave->InitializeSamples(MoveTemp(Samples), 0);

    FSoundGeneratorInitParams InitParams;
    InitParams.SampleRate = 48000.0f;
    InitParams.NumChannels = NumChannels;
    InitParams.NumFramesPerCallback = FramesPerBlock;
    InitParams.AudioMixerNumOutputFrames = FramesPerBlock;
    ISoundGeneratorPtr Generator = SoundWave->CreateSoundGenerator(InitParams);
    if (!TestTrue(TEXT("The validation SoundWave creates the UE 5.7 generator path"), Generator.IsValid()))
    {
        return false;
    }
    TestEqual(
        TEXT("The generator requests one complete interleaved stereo block"),
        Generator->GetDesiredNumSamplesToRenderPerCallback(),
        SamplesPerBlock);

    TArray<float> OutputSamples;
    OutputSamples.SetNumZeroed(SamplesPerBlock);
    const int32 FirstBlockSamples = Generator->OnGenerateAudio(
        OutputSamples.GetData(),
        SamplesPerBlock);
    TestEqual(
        TEXT("The first generator callback is completely filled"),
        FirstBlockSamples,
        SamplesPerBlock);
    for (int32 SampleIndex = 0; SampleIndex < SamplesPerBlock; ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(TEXT("First block sample %d preserves the source"), SampleIndex),
            FMath::IsNearlyEqual(
                OutputSamples[SampleIndex],
                ExpectedSamples[SampleIndex],
                1.0e-7f));
    }

    OutputSamples.SetNumZeroed(SamplesPerBlock);
    const int32 SecondBlockSamples = Generator->OnGenerateAudio(
        OutputSamples.GetData(),
        SamplesPerBlock);
    TestEqual(
        TEXT("The second generator callback is completely filled"),
        SecondBlockSamples,
        SamplesPerBlock);
    for (int32 SampleIndex = 0; SampleIndex < SamplesPerBlock; ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(TEXT("Second block sample %d is seamless"), SampleIndex),
            FMath::IsNearlyEqual(
                OutputSamples[SampleIndex],
                ExpectedSamples[SamplesPerBlock + SampleIndex],
                1.0e-7f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOcclusionInPlaceBufferTest,
    "UERayTracingAudio.Audio.OcclusionInPlaceBuffer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOcclusionInPlaceBufferTest::RunTest(const FString& Parameters)
{
    const TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry =
            MakeShared<
                FUERayTracingAudioSimulationSnapshotRegistry,
                ESPMode::ThreadSafe>();
    const TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> Bridge =
        MakeShared<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>();
    FUERayTracingAudioOcclusionPlugin Plugin(SnapshotRegistry, Bridge);

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 2;
    InitializationParams.SampleRate = 48000;
    InitializationParams.BufferLength = 4;
    Plugin.Initialize(InitializationParams);
    Plugin.OnInitSource(0, NAME_None, 2, nullptr);

    FAudioPluginSourceOutputData OutputData;
    OutputData.AudioBuffer = { 0.25f, -0.5f, 0.75f, -1.0f };
    const Audio::FAlignedFloatBuffer Expected = OutputData.AudioBuffer;
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = 1;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    InputData.NumChannels = 2;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;

    Plugin.ProcessAudio(InputData, OutputData);
    TestEqual(TEXT("In-place output preserves the sample count"), OutputData.AudioBuffer.Num(), Expected.Num());
    for (int32 SampleIndex = 0; SampleIndex < Expected.Num(); ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(TEXT("In-place sample %d preserves non-zero input"), SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                Expected[SampleIndex],
                1.0e-7f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOcclusionStereoFinalOutputDiagnosticsTest,
    "UERayTracingAudio.Audio.OcclusionStereoFinalOutputDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOcclusionStereoFinalOutputDiagnosticsTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 AudioComponentId = 0x57E2E0ULL;
    constexpr EUERayTracingAudioRuntimeDataSource DataSource =
        EUERayTracingAudioRuntimeDataSource::Baked;
    const TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry =
            MakeShared<
                FUERayTracingAudioSimulationSnapshotRegistry,
                ESPMode::ThreadSafe>();
    const TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> Bridge =
        MakeShared<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>();
    FUERayTracingAudioOcclusionPlugin Plugin(SnapshotRegistry, Bridge);

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 2;
    InitializationParams.SampleRate = 48000;
    InitializationParams.BufferLength = 2;
    Plugin.Initialize(InitializationParams);
    Plugin.OnInitSource(0, NAME_None, 2, nullptr);

    FUERayTracingAudioSimulationSnapshot Snapshot;
    Snapshot.DataSource = DataSource;
    Snapshot.DirectResult.bHasListener = true;
    Snapshot.DirectResult.DistanceAttenuation = 1.0f;
    Snapshot.DirectResult.AirAbsorption = FVector::OneVector;
    Snapshot.DirectResult.Occlusion = 1.0f;
    Snapshot.DirectResult.OverallGain = 1.0f;
    Snapshot.IndirectMix = 0.0f;
    SnapshotRegistry->Publish(AudioComponentId, MoveTemp(Snapshot));

    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
        AudioComponentId);
    FUERayTracingAudioAudioDiagnostics::Reset(DataSource);

    FAudioPluginSourceOutputData OutputData;
    OutputData.AudioBuffer = { 0.25f, -0.5f, 0.75f, -1.0f };
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = AudioComponentId;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    InputData.NumChannels = 2;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;

    Plugin.ProcessAudio(InputData, OutputData);
    const FUERayTracingAudioDataSourceAudioStats Stats =
        FUERayTracingAudioAudioDiagnostics::Read(DataSource);
    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(0);

    TestTrue(
        TEXT("Stereo sources bypassing Spatialization retain the Occlusion Full peak"),
        FMath::IsNearlyEqual(Stats.MaxOutputPeak, 1.0f, 1.0e-6f));
    TestEqual(
        TEXT("Unity stereo fallback contains no over-unit samples"),
        Stats.OverUnitOutputSampleCount,
        0ULL);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOcclusionPreparedConvolutionEndToEndTest,
    "UERayTracingAudio.Audio.OcclusionPreparedConvolutionEndToEnd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOcclusionPreparedConvolutionEndToEndTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 AudioComponentId = 0xC011B4C4ULL;
    constexpr int32 SampleRate = 8000;
    constexpr int32 BlockSize = 8;
    constexpr int32 CrossfadeSamples = 2048;
    FUERayTracingAudioModule& Module =
        FUERayTracingAudioModule::Get();
    const TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry =
            MakeShared<
                FUERayTracingAudioSimulationSnapshotRegistry,
                ESPMode::ThreadSafe>();
    const TSharedRef<
        FUERayTracingAudioIndirectAudioBridge,
        ESPMode::ThreadSafe> Bridge =
            Module.GetOrCreateIndirectAudioBridge(nullptr);
    FUERayTracingAudioOcclusionPlugin Plugin(
        SnapshotRegistry,
        Bridge);

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 1;
    InitializationParams.SampleRate = SampleRate;
    InitializationParams.BufferLength = 4096;
    Plugin.Initialize(InitializationParams);
    Plugin.OnInitSource(0, NAME_None, 1, nullptr);

    const FUERayTracingAudioConvolutionKernel::FKernelPtr LeftKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.75f },
            SampleRate,
            BlockSize);
    const FUERayTracingAudioConvolutionKernel::FKernelPtr RightKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.25f },
            SampleRate,
            BlockSize);
    TestTrue(TEXT("Production left IR kernel is valid"), LeftKernel.IsValid());
    TestTrue(TEXT("Production right IR kernel is valid"), RightKernel.IsValid());
    if (!LeftKernel || !RightKernel)
    {
        Plugin.Shutdown();
        Module.UnregisterAudioDevice(nullptr);
        return false;
    }

    FUERayTracingAudioConvolutionRevisions Revisions;
    Revisions.RealtimeLeft = 1;
    Revisions.RealtimeRight = 1;
    FUERayTracingAudioSimulationSnapshot Snapshot;
    Snapshot.RealtimeConvolutionKernelLeft = LeftKernel;
    Snapshot.RealtimeConvolutionKernelRight = RightKernel;
    Snapshot.ConvolutionRevisions = Revisions;
    Snapshot.DataSource =
        EUERayTracingAudioRuntimeDataSource::Realtime;
    Snapshot.IndirectMix = 1.0f;
    Snapshot.IndirectDurationSeconds = 1.0f;
    TestTrue(
        TEXT("Production snapshot publishes to the audio registry"),
        SnapshotRegistry->Publish(
            AudioComponentId,
            MoveTemp(Snapshot)));
    Module.PublishConvolutionTargets(
        nullptr,
        AudioComponentId,
        Revisions,
        nullptr,
        nullptr,
        LeftKernel,
        RightKernel);

    FAudioPluginSourceOutputData OutputData;
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = AudioComponentId;
    InputData.NumChannels = 1;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;
    auto ProcessZeroFrames = [
        &Plugin,
        &InputData,
        &OutputData](
        const int32 NumFrames)
    {
        OutputData.AudioBuffer.Init(0.0f, NumFrames);
        InputData.AudioBuffer = &OutputData.AudioBuffer;
        Plugin.ProcessAudio(InputData, OutputData);
    };

    // The first production callback may only post requests. Kernel ownership
    // and all workspace preparation stay on the game/control thread.
    ProcessZeroFrames(BlockSize);
    Module.ServiceIndirectAudioBridges();
    TestEqual(
        TEXT("GT service prepares independent realtime L/R states"),
        Bridge->GetPreparedStateCountForTesting(),
        2);

    // The next callback adopts both raw leases. Advance exactly an integral
    // number of convolution blocks through warmup and the 2048-sample fade.
    ProcessZeroFrames(BlockSize);
    ProcessZeroFrames(CrossfadeSamples);

    OutputData.AudioBuffer.SetNumZeroed(BlockSize);
    OutputData.AudioBuffer[0] = 1.0f;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    Plugin.ProcessAudio(InputData, OutputData);
    ProcessZeroFrames(BlockSize);

    TArrayView<const FVector2f> StereoWet;
    TestTrue(
        TEXT("Production Occlusion publishes convolution wet frames"),
        Bridge->Consume(
            0,
            AudioComponentId,
            StereoWet));
    TestEqual(
        TEXT("Production wet callback has one convolution block"),
        StereoWet.Num(),
        BlockSize);
    if (StereoWet.Num() == BlockSize)
    {
        TestTrue(
            TEXT("Production left wet channel follows the left IR"),
            FMath::IsNearlyEqual(
                StereoWet[0].X,
                0.75f,
                1.0e-4f));
        TestTrue(
            TEXT("Production right wet channel follows the right IR"),
            FMath::IsNearlyEqual(
                StereoWet[0].Y,
                0.25f,
                1.0e-4f));
    }
    TestTrue(
        TEXT("Production mono Occlusion output contains averaged wet audio"),
        OutputData.AudioBuffer.Num() == BlockSize
        && FMath::IsNearlyEqual(
            OutputData.AudioBuffer[0],
            0.5f,
            1.0e-4f));

    Plugin.Shutdown();
    SnapshotRegistry->Remove(AudioComponentId);
    Module.RemoveConvolutionTargets(AudioComponentId);
    Module.ServiceIndirectAudioBridges();
    TestEqual(
        TEXT("Production Occlusion shutdown reclaims prepared workspace"),
        Bridge->GetPreparedWorkspaceBytes(),
        static_cast<uint64>(0));
    TestEqual(
        TEXT("Production Occlusion used no overflowing return path"),
        Bridge->GetConvolutionReturnOverflowCount(),
        static_cast<uint64>(0));
    Module.UnregisterAudioDevice(nullptr);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioFirstSnapshotDirectGainTest,
    "UERayTracingAudio.Audio.FirstSnapshotDirectGain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioFirstSnapshotDirectGainTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 AudioComponentId = 0xD1EC7ULL;
    const TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry =
            MakeShared<
                FUERayTracingAudioSimulationSnapshotRegistry,
                ESPMode::ThreadSafe>();
    const TSharedRef<
        FUERayTracingAudioIndirectAudioBridge,
        ESPMode::ThreadSafe> Bridge =
            MakeShared<
                FUERayTracingAudioIndirectAudioBridge,
                ESPMode::ThreadSafe>();
    FUERayTracingAudioOcclusionPlugin Plugin(SnapshotRegistry, Bridge);

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 2;
    InitializationParams.SampleRate = 48000;
    InitializationParams.BufferLength = 4;
    Plugin.Initialize(InitializationParams);
    Plugin.OnInitSource(0, NAME_None, 2, nullptr);

    FUERayTracingAudioSimulationSnapshot Snapshot;
    Snapshot.DirectResult.bHasListener = true;
    Snapshot.DirectResult.DistanceAttenuation = 0.5f;
    Snapshot.DirectResult.AirAbsorption = FVector::OneVector;
    Snapshot.DirectResult.Occlusion = 0.5f;
    Snapshot.DirectResult.OverallGain = 0.25f;
    Snapshot.IndirectMix = 0.0f;
    SnapshotRegistry->Publish(AudioComponentId, MoveTemp(Snapshot));

    FAudioPluginSourceOutputData OutputData;
    OutputData.AudioBuffer = { 1.0f, -1.0f, 1.0f, -1.0f };
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = AudioComponentId;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    InputData.NumChannels = 2;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;

    Plugin.ProcessAudio(InputData, OutputData);
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        const float Expected = (SampleIndex & 1) == 0 ? 0.25f : -0.25f;
        TestTrue(
            *FString::Printf(
                TEXT("First snapshot sample %d starts at physical Direct gain"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                Expected,
                1.0e-6f));
    }

    SnapshotRegistry->Remove(AudioComponentId);
    OutputData.AudioBuffer = { 1.0f, -1.0f, 1.0f, -1.0f };
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    Plugin.ProcessAudio(InputData, OutputData);
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        const float ExpectedMagnitude = SampleIndex < 2 ? 0.625f : 1.0f;
        const float Expected = (SampleIndex & 1) == 0
            ? ExpectedMagnitude
            : -ExpectedMagnitude;
        TestTrue(
            *FString::Printf(
                TEXT("Removed snapshot sample %d ramps back to unity"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                Expected,
                1.0e-6f));
    }

    constexpr uint64 DelayedAudioComponentId = AudioComponentId + 1;
    Plugin.OnReleaseSource(0);
    Plugin.OnInitSource(0, NAME_None, 2, nullptr);

    FUERayTracingAudioSimulationSnapshot DefaultSnapshot;
    DefaultSnapshot.IndirectMix = 0.0f;
    SnapshotRegistry->Publish(
        DelayedAudioComponentId,
        MoveTemp(DefaultSnapshot));
    OutputData.AudioBuffer = { 1.0f, -1.0f, 1.0f, -1.0f };
    InputData.AudioComponentId = DelayedAudioComponentId;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    Plugin.ProcessAudio(InputData, OutputData);
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        const float Expected = (SampleIndex & 1) == 0 ? 1.0f : -1.0f;
        TestTrue(
            *FString::Printf(
                TEXT("Default snapshot sample %d remains unity"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                Expected,
                1.0e-6f));
    }

    FUERayTracingAudioSimulationSnapshot DelayedSnapshot;
    DelayedSnapshot.DirectResult.bHasListener = true;
    DelayedSnapshot.DirectResult.DistanceAttenuation = 0.5f;
    DelayedSnapshot.DirectResult.AirAbsorption = FVector::OneVector;
    DelayedSnapshot.DirectResult.Occlusion = 0.5f;
    DelayedSnapshot.DirectResult.OverallGain = 0.25f;
    DelayedSnapshot.IndirectMix = 0.0f;
    SnapshotRegistry->Publish(
        DelayedAudioComponentId,
        MoveTemp(DelayedSnapshot));
    OutputData.AudioBuffer = { 1.0f, -1.0f, 1.0f, -1.0f };
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    Plugin.ProcessAudio(InputData, OutputData);
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        const float ExpectedMagnitude = SampleIndex < 2 ? 0.625f : 0.25f;
        const float Expected = (SampleIndex & 1) == 0
            ? ExpectedMagnitude
            : -ExpectedMagnitude;
        TestTrue(
            *FString::Printf(
                TEXT("Delayed valid snapshot sample %d ramps without a hard jump"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                Expected,
                1.0e-6f));
    }

    SnapshotRegistry->Remove(DelayedAudioComponentId);
    OutputData.AudioBuffer.Reset();
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    Plugin.ProcessAudio(InputData, OutputData);
    OutputData.AudioBuffer = { 1.0f, -1.0f, 1.0f, -1.0f };
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    Plugin.ProcessAudio(InputData, OutputData);
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        const float ExpectedMagnitude = SampleIndex < 2 ? 0.625f : 1.0f;
        const float Expected = (SampleIndex & 1) == 0
            ? ExpectedMagnitude
            : -ExpectedMagnitude;
        TestTrue(
            *FString::Printf(
                TEXT("Zero-frame callback does not consume removal ramp sample %d"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                Expected,
                1.0e-6f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioSpatializationInPlaceBufferTest,
    "UERayTracingAudio.Audio.SpatializationInPlaceBuffer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioSpatializationInPlaceBufferTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> Bridge =
        MakeShared<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>();
    FUERayTracingAudioSpatializationPlugin Plugin(Bridge);

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 2;
    InitializationParams.SampleRate = 48000;
    InitializationParams.BufferLength = 4;
    Plugin.Initialize(InitializationParams);
    Plugin.OnInitSource(0, NAME_None, 2, nullptr);

    FAudioPluginSourceOutputData OutputData;
    OutputData.AudioBuffer = { 0.25f, -0.5f, 0.75f, -1.0f };
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = 1;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    InputData.NumChannels = 2;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;

    Plugin.ProcessAudio(InputData, OutputData);
    const float ExpectedSample = -0.125f * UE_INV_SQRT_2;
    for (int32 SampleIndex = 0; SampleIndex < OutputData.AudioBuffer.Num(); ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(TEXT("In-place spatialization sample %d remains audible"), SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                ExpectedSample,
                1.0e-6f));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioSpatializationFinalOutputDiagnosticsTest,
    "UERayTracingAudio.Audio.SpatializationFinalOutputDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioSpatializationFinalOutputDiagnosticsTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 AudioComponentId = 0xF0110ULL;
    constexpr EUERayTracingAudioRuntimeDataSource DataSource =
        EUERayTracingAudioRuntimeDataSource::Realtime;
    const TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> Bridge =
        MakeShared<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>();
    FUERayTracingAudioSpatializationPlugin Plugin(Bridge);

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 2;
    InitializationParams.SampleRate = 48000;
    InitializationParams.BufferLength = 1;
    Plugin.Initialize(InitializationParams);
    Plugin.OnInitSource(0, NAME_None, 2, nullptr);

    TArrayView<FVector2f> StereoWet =
        Bridge->BeginWrite(0, AudioComponentId, 1, DataSource);
    StereoWet[0] = FVector2f(0.7f, -0.7f);
    Bridge->EndWrite(0, AudioComponentId);

    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
        AudioComponentId);
    FUERayTracingAudioAudioDiagnostics::Reset(DataSource);
    // This is the pre-spatialization Full observed by the occlusion stage:
    // Direct=(0, 1), Wet=(0.7, -0.7), hence (0.7, 0.3).
    FUERayTracingAudioAudioDiagnostics::RecordBuffer(
        DataSource,
        AudioComponentId,
        1,
        1.0f,
        0.7f,
        UE_INV_SQRT_2,
        0.7f,
        0);

    FAudioPluginSourceOutputData OutputData;
    OutputData.AudioBuffer = { 0.7f, 0.3f };
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = AudioComponentId;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    InputData.NumChannels = 2;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;

    Plugin.ProcessAudio(InputData, OutputData);
    const FUERayTracingAudioDataSourceAudioStats Stats =
        FUERayTracingAudioAudioDiagnostics::Read(DataSource);
    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(0);

    const float ExpectedFinalPeak = 0.7f + (0.5f * UE_INV_SQRT_2);
    TestTrue(
        TEXT("Final post-spatialization Full peak is measured"),
        FMath::IsNearlyEqual(
            Stats.MaxOutputPeak,
            ExpectedFinalPeak,
            1.0e-6f));
    TestEqual(
        TEXT("Final post-spatialization over-unit samples are counted"),
        Stats.OverUnitOutputSampleCount,
        1ULL);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioIntegratedWetDiagnosticsTest,
    "UERayTracingAudio.Audio.IntegratedWetDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioIntegratedWetDiagnosticsTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 AudioComponentId = 0xA11D10ULL;
    constexpr int32 ShortWetFrames = 100;
    constexpr int32 LongWetFrames = 300;
    constexpr int32 SilentWetFrames = 200;
    constexpr EUERayTracingAudioRuntimeDataSource DataSource =
        EUERayTracingAudioRuntimeDataSource::Realtime;
    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
        AudioComponentId);
    FUERayTracingAudioAudioDiagnostics::Reset(DataSource);

    // Publish stale, audible data in one epoch, then verify Reset makes it
    // immediately unreadable without racing individual counter stores.
    FUERayTracingAudioAudioDiagnostics::RecordBuffer(
        DataSource,
        AudioComponentId,
        50,
        0.2f,
        0.2f,
        0.1f,
        0.1f,
        0);
    FUERayTracingAudioAudioDiagnostics::Reset(DataSource);
    const FUERayTracingAudioDataSourceAudioStats ResetStats =
        FUERayTracingAudioAudioDiagnostics::Read(DataSource);
    TestEqual(
        TEXT("A reset epoch hides all previously published buffers"),
        ResetStats.BufferCount,
        0ULL);

    // Both non-zero wet buffers are present but remain below the 5% audible
    // wet/input ratio. Their unequal frame counts make the energy assertion
    // sensitive to NumFrames weighting.
    FUERayTracingAudioAudioDiagnostics::RecordBuffer(
        DataSource,
        AudioComponentId,
        ShortWetFrames,
        0.2f,
        0.002f,
        0.1f,
        0.001f,
        0);
    FUERayTracingAudioAudioDiagnostics::RecordBuffer(
        DataSource,
        AudioComponentId,
        LongWetFrames,
        0.2f,
        0.004f,
        0.1f,
        0.002f,
        0);
    FUERayTracingAudioAudioDiagnostics::RecordBuffer(
        DataSource,
        AudioComponentId,
        SilentWetFrames,
        0.2f,
        0.0f,
        0.1f,
        0.0f,
        0);
    FUERayTracingAudioAudioDiagnostics::RecordFinalOutput(
        DataSource,
        AudioComponentId,
        1.1f,
        1,
        0);

    const FUERayTracingAudioDataSourceAudioStats Stats =
        FUERayTracingAudioAudioDiagnostics::Read(DataSource);
    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(0);

    TestEqual(
        TEXT("Only post-reset buffers are visible"),
        Stats.BufferCount,
        3ULL);
    TestEqual(
        TEXT("Frame count accumulates unequal post-reset buffers"),
        Stats.FrameCount,
        600ULL);
    TestEqual(TEXT("All input-bearing buffers are measured"), Stats.RmsMeasuredBufferCount, 3ULL);
    TestEqual(TEXT("Wet presence distinguishes true silence from a low ratio"), Stats.WetPresentInputBufferCount, 2ULL);
    TestEqual(
        TEXT("Sub-threshold wet buffers are present but not audible"),
        Stats.AudibleWetBufferCount,
        0ULL);
    TestEqual(TEXT("A single truly silent wet buffer is recorded as one run"), Stats.MaxConsecutiveSilentWetBufferCount, 1ULL);
    TestTrue(
        TEXT("Full-output peak is retained in the same diagnostics epoch"),
        FMath::IsNearlyEqual(Stats.MaxOutputPeak, 1.1f, 1.0e-6f));
    TestEqual(
        TEXT("Over-unit Full samples are counted"),
        Stats.OverUnitOutputSampleCount,
        1ULL);
    TestTrue(
        TEXT("Integrated RMS uses frame-weighted energy across unequal buffers"),
        FMath::IsNearlyEqual(
            Stats.IntegratedWetToInputRmsRatio,
            FMath::Sqrt(13.0f / 60000.0f),
            1.0e-5f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioHardRealtimeDiagnosticsTest,
    "UERayTracingAudio.Audio.HardRealtimeDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioHardRealtimeDiagnosticsTest::RunTest(
    const FString& Parameters)
{
    FUERayTracingAudioAudioDiagnostics::ResetHardRealtime();
    TestEqual(
        TEXT("A hard-realtime diagnostics epoch starts empty"),
        FUERayTracingAudioAudioDiagnostics::ReadHardRealtime().
            AudioCallbackCount,
        0ULL);

    FUERayTracingAudioAudioDiagnostics::
        RecordHardRealtimeCallback();
    FUERayTracingAudioAudioDiagnostics::
        RecordHardRealtimeCallback();
    FUERayTracingAudioAudioDiagnostics::
        RecordHardRealtimeCapacityMiss();
    FUERayTracingAudioAudioDiagnostics::
        RecordConvolutionPrepareCapacityDrop();
    const FUERayTracingAudioHardRealtimeStats Stats =
        FUERayTracingAudioAudioDiagnostics::
            ReadHardRealtime();
    TestEqual(
        TEXT("Audio callbacks are observable"),
        Stats.AudioCallbackCount,
        2ULL);
    TestEqual(
        TEXT("Callback capacity misses are observable"),
        Stats.CallbackCapacityMissCount,
        1ULL);
    TestEqual(
        TEXT("Control-thread convolution admission drops are observable"),
        Stats.ConvolutionPrepareCapacityDropCount,
        1ULL);

    FUERayTracingAudioAudioDiagnostics::ResetHardRealtime();
    const FUERayTracingAudioHardRealtimeStats ResetStats =
        FUERayTracingAudioAudioDiagnostics::
            ReadHardRealtime();
    TestEqual(
        TEXT("Reset clears callback observations"),
        ResetStats.AudioCallbackCount,
        0ULL);
    TestEqual(
        TEXT("Reset clears callback capacity misses"),
        ResetStats.CallbackCapacityMissCount,
        0ULL);
    TestEqual(
        TEXT("Reset clears control prepare drops"),
        ResetStats.ConvolutionPrepareCapacityDropCount,
        0ULL);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioConvolutionImpulseResponseTest,
    "UERayTracingAudio.Audio.ConvolutionImpulseResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioConvolutionImpulseResponseTest::RunTest(const FString& Parameters)
{
    constexpr int32 BlockSize = 8;
    const TArray<float> ImpulseResponse{ 1.0f, 0.5f, -0.25f };
    const FUERayTracingAudioConvolutionKernel::FKernelPtr Kernel =
        FUERayTracingAudioConvolutionKernel::Build(ImpulseResponse, 48000, BlockSize);

    TestTrue(TEXT("A finite impulse response creates a convolution kernel"), Kernel.IsValid());
    if (!Kernel.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("Short impulse uses one partition"), Kernel->GetNumPartitions(), 1);
    TestTrue(
        TEXT("Kernel duration follows impulse length"),
        FMath::IsNearlyEqual(Kernel->GetDurationSeconds(), 3.0f / 48000.0f, 1.0e-7f));

    FUERayTracingAudioPartitionedConvolver Convolver;
    Convolver.SetKernel(Kernel);
    TArray<float> Output;
    Output.Reserve(BlockSize * 3);
    for (int32 SampleIndex = 0; SampleIndex < BlockSize * 3; ++SampleIndex)
    {
        Output.Add(Convolver.ProcessSample(SampleIndex == 0 ? 1.0f : 0.0f));
    }

    for (int32 SampleIndex = 0; SampleIndex < BlockSize; ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(TEXT("Partition warm-up sample %d is silent"), SampleIndex),
            FMath::IsNearlyZero(Output[SampleIndex], 1.0e-5f));
    }
    TestTrue(TEXT("Impulse peak is reconstructed"), FMath::IsNearlyEqual(Output[BlockSize], 1.0f, 1.0e-4f));
    TestTrue(TEXT("Second tap is reconstructed"), FMath::IsNearlyEqual(Output[BlockSize + 1], 0.5f, 1.0e-4f));
    TestTrue(TEXT("Third tap is reconstructed"), FMath::IsNearlyEqual(Output[BlockSize + 2], -0.25f, 1.0e-4f));

    for (const float Sample : Output)
    {
        TestTrue(TEXT("Convolution output remains finite"), FMath::IsFinite(Sample));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioRuntimeConvolutionBudgetTest,
    "UERayTracingAudio.Audio.RuntimeConvolutionBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioRuntimeConvolutionBudgetTest::RunTest(
    const FString& Parameters)
{
    constexpr int32 BlockSize = 8;
    constexpr int32 SampleRate = 48000;
    constexpr int32 ExtraTailSamples = 11;
    const int32 RuntimeSampleLimit =
        BlockSize
        * UERayTracingAudioConvolutionLimits::
            MaxRuntimePartitionsPerLane;
    TArray<float> LongImpulse;
    LongImpulse.SetNumZeroed(
        RuntimeSampleLimit + ExtraTailSamples);
    LongImpulse[0] = 1.0f;
    LongImpulse.Last() = 0.5f;

    const FUERayTracingAudioConvolutionKernel::FKernelPtr Kernel =
        FUERayTracingAudioConvolutionKernel::Build(
            LongImpulse,
            SampleRate,
            BlockSize);
    TestTrue(
        TEXT("A long finite impulse creates a runtime kernel"),
        Kernel.IsValid());
    if (!Kernel)
    {
        return false;
    }

    TestEqual(
        TEXT("Runtime convolution has a fixed partition budget"),
        Kernel->GetNumPartitions(),
        UERayTracingAudioConvolutionLimits::
            MaxRuntimePartitionsPerLane);
    TestTrue(
        TEXT("The kernel reports that its long tail was truncated"),
        Kernel->WasRuntimeTailTruncated());
    TestTrue(
        TEXT("Runtime duration is capped at the admitted sample count"),
        FMath::IsNearlyEqual(
            Kernel->GetDurationSeconds(),
            static_cast<float>(RuntimeSampleLimit)
                / static_cast<float>(SampleRate),
            1.0e-7f));
    TestTrue(
        TEXT("Original duration remains available for tail handoff"),
        FMath::IsNearlyEqual(
            Kernel->GetOriginalDurationSeconds(),
            static_cast<float>(LongImpulse.Num())
                / static_cast<float>(SampleRate),
            1.0e-7f));

    FUERayTracingAudioPartitionedConvolver Convolver;
    Convolver.SetKernel(Kernel);
    for (int32 SampleIndex = 0;
        SampleIndex < RuntimeSampleLimit + (BlockSize * 2);
        ++SampleIndex)
    {
        TestTrue(
            TEXT("Budgeted convolution output remains finite"),
            FMath::IsFinite(
                Convolver.ProcessSample(
                    SampleIndex == 0 ? 1.0f : 0.0f)));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioPreparedConvolverOwnershipTest,
    "UERayTracingAudio.Audio.PreparedConvolverOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioPreparedConvolverOwnershipTest::RunTest(
    const FString& Parameters)
{
    constexpr int32 BlockSize = 8;
    constexpr int32 CrossfadeSamples = 8;
    FUERayTracingAudioConvolutionKernel::FKernelPtr FirstKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.75f },
            48000,
            BlockSize);
    FUERayTracingAudioConvolutionKernel::FKernelPtr SecondKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.25f },
            48000,
            BlockSize);
    TestTrue(TEXT("First prepared kernel is valid"), FirstKernel.IsValid());
    TestTrue(TEXT("Second prepared kernel is valid"), SecondKernel.IsValid());
    if (!FirstKernel || !SecondKernel)
    {
        return false;
    }

    TWeakPtr<
        const FUERayTracingAudioConvolutionKernel,
        ESPMode::ThreadSafe> FirstKernelLifetime = FirstKernel;
    TWeakPtr<
        const FUERayTracingAudioConvolutionKernel,
        ESPMode::ThreadSafe> SecondKernelLifetime = SecondKernel;
    FUERayTracingAudioPreparedConvolverState FirstState;
    FUERayTracingAudioPreparedConvolverState SecondState;
    FirstState.Prepare(MoveTemp(FirstKernel), 11);
    SecondState.Prepare(MoveTemp(SecondKernel), 12);
    const uint64 FirstStorageBefore =
        FirstState.GetStorageFingerprintForTesting();
    const uint64 SecondStorageBefore =
        SecondState.GetStorageFingerprintForTesting();

    FUERayTracingAudioPreparedCrossfadingConvolver Convolver;
    TestTrue(
        TEXT("A prepared state can be adopted without transferring ownership"),
        Convolver.AdoptPreparedState(
            &FirstState,
            CrossfadeSamples));
    for (int32 SampleIndex = 0;
        SampleIndex < BlockSize + CrossfadeSamples + 2;
        ++SampleIndex)
    {
        TestTrue(
            TEXT("Prepared convolution output stays finite"),
            FMath::IsFinite(Convolver.ProcessSample(1.0f)));
    }
    TestTrue(
        TEXT("A second prepared state can crossfade after the first settles"),
        Convolver.AdoptPreparedState(
            &SecondState,
            CrossfadeSamples));
    for (int32 SampleIndex = 0;
        SampleIndex < BlockSize + CrossfadeSamples + 2;
        ++SampleIndex)
    {
        TestTrue(
            TEXT("Prepared crossfade output stays finite"),
            FMath::IsFinite(Convolver.ProcessSample(1.0f)));
    }

    FUERayTracingAudioPreparedConvolverState* RetiredState =
        Convolver.TakeRetiredState();
    TestTrue(
        TEXT("Crossfade completion returns the old lease instead of destroying it"),
        RetiredState == &FirstState);
    FUERayTracingAudioPreparedConvolverState* DetachedStates[3] =
    {
        nullptr,
        nullptr,
        nullptr
    };
    const int32 NumDetachedStates = Convolver.DetachAllStates(
        DetachedStates,
        UE_ARRAY_COUNT(DetachedStates));
    TestEqual(
        TEXT("Audio-thread reset detaches the current lease"),
        NumDetachedStates,
        1);
    TestTrue(
        TEXT("The current prepared lease is returned to its owner"),
        DetachedStates[0] == &SecondState);

    TestEqual(
        TEXT("Processing and detaching preserve first-state storage"),
        FirstState.GetStorageFingerprintForTesting(),
        FirstStorageBefore);
    TestEqual(
        TEXT("Processing and detaching preserve second-state storage"),
        SecondState.GetStorageFingerprintForTesting(),
        SecondStorageBefore);
    TestTrue(
        TEXT("The old kernel remains alive until control-thread reclamation"),
        FirstKernelLifetime.IsValid());
    TestTrue(
        TEXT("The current kernel remains alive until control-thread reclamation"),
        SecondKernelLifetime.IsValid());

    FirstState.Prepare(nullptr, 0);
    SecondState.Prepare(nullptr, 0);
    TestFalse(
        TEXT("Control-thread reclamation releases the old kernel"),
        FirstKernelLifetime.IsValid());
    TestFalse(
        TEXT("Control-thread reclamation releases the current kernel"),
        SecondKernelLifetime.IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioConvolutionKernelChurnTest,
    "UERayTracingAudio.Audio.ConvolutionKernelChurn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioConvolutionKernelChurnTest::RunTest(
    const FString& Parameters)
{
    constexpr int32 BlockSize = 8;
    constexpr int32 CrossfadeSamples = 16;
    constexpr int32 KernelUpdateIntervalSamples = BlockSize / 2;
    constexpr int32 ChurnSamples = 128;

    TArray<FUERayTracingAudioConvolutionKernel::FKernelPtr> Kernels;
    Kernels.Reserve((ChurnSamples / KernelUpdateIntervalSamples) + 1);
    for (int32 KernelIndex = 0;
        KernelIndex <= ChurnSamples / KernelUpdateIntervalSamples;
        ++KernelIndex)
    {
        const float Gain = (KernelIndex & 1) == 0 ? 0.75f : 0.9f;
        Kernels.Add(FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ Gain },
            48000,
            BlockSize));
        if (!TestTrue(
                *FString::Printf(
                    TEXT("Churn kernel %d is valid"),
                    KernelIndex),
                Kernels.Last().IsValid()))
        {
            return false;
        }
    }

    FUERayTracingAudioCrossfadingConvolver Convolver;
    Convolver.SetKernel(Kernels[0], CrossfadeSamples);

    int32 FirstAudibleSample = INDEX_NONE;
    int32 SilentSamplesAfterInitialTransition = 0;
    int32 SubmittedKernelIndex = 0;
    for (int32 SampleIndex = 0;
        SampleIndex < ChurnSamples;
        ++SampleIndex)
    {
        if (SampleIndex > 0
            && SampleIndex % KernelUpdateIntervalSamples == 0)
        {
            ++SubmittedKernelIndex;
            Convolver.SetKernel(
                Kernels[SubmittedKernelIndex],
                CrossfadeSamples);
        }

        const float Output = Convolver.ProcessSample(1.0f);
        TestTrue(
            TEXT("Rapid kernel updates keep convolution finite"),
            FMath::IsFinite(Output));
        if (FirstAudibleSample == INDEX_NONE
            && FMath::Abs(Output) > 0.01f)
        {
            FirstAudibleSample = SampleIndex;
        }
        if (SampleIndex >= BlockSize + CrossfadeSamples
            && FMath::Abs(Output) < 0.25f)
        {
            ++SilentSamplesAfterInitialTransition;
        }
    }

    TestTrue(
        TEXT("The first kernel starts without waiting for the update stream to settle"),
        FirstAudibleSample != INDEX_NONE
            && FirstAudibleSample <= BlockSize + 1);
    TestEqual(
        TEXT("Sub-block kernel churn does not restart warm-up into silence"),
        SilentSamplesAfterInitialTransition,
        0);

    // Stop publishing and allow the in-flight transition plus the single
    // last-wins pending transition to complete.
    for (int32 SampleIndex = 0;
        SampleIndex < (BlockSize + CrossfadeSamples) * 3;
        ++SampleIndex)
    {
        Convolver.ProcessSample(1.0f);
    }
    const float SettledOutput = Convolver.ProcessSample(1.0f);
    const float ExpectedLatestGain =
        (SubmittedKernelIndex & 1) == 0 ? 0.75f : 0.9f;
    TestTrue(
        TEXT("The newest queued kernel becomes the stable convolution state"),
        FMath::IsNearlyEqual(
            SettledOutput,
            ExpectedLatestGain,
            1.0e-4f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioIndirectRendererQuantitativeTest,
    "UERayTracingAudio.Audio.IndirectRendererQuantitative",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioIndirectRendererQuantitativeTest::RunTest(const FString& Parameters)
{
    constexpr int32 TestSampleRate = 8000;
    constexpr int32 ConvolutionBlockSize = 8;
    constexpr float IndirectMix = 0.5f;

    auto RenderDirectionalImpulse = [=](
        const float LeftTap,
        const float RightTap,
        const float WetSend)
    {
        FUERayTracingAudioSimulationSnapshot Snapshot;
        Snapshot.IndirectMix = WetSend;
        Snapshot.IndirectDurationSeconds = 0.25f;
        Snapshot.IndirectResult.bHasValidPaths = true;
        Snapshot.IndirectResult.bUsedParametricTail = false;
        Snapshot.RealtimeConvolutionKernelLeft = FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ LeftTap },
            TestSampleRate,
            ConvolutionBlockSize);
        Snapshot.RealtimeConvolutionKernelRight = FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ RightTap },
            TestSampleRate,
            ConvolutionBlockSize);

        FUERayTracingAudioIndirectRenderer Renderer;
        Renderer.Initialize(TestSampleRate, 4.0f);
        Renderer.Reset(TestSampleRate);
        FUERayTracingAudioPreparedRendererTestHarness PreparedRenderer(
            TestSampleRate);
        PreparedRenderer.Configure(Renderer, &Snapshot);

        // Measure the steady-state directional response after the intentional
        // one-block warm-up and 2048-sample fade-in from silence.
        for (int32 SampleIndex = 0; SampleIndex < ConvolutionBlockSize + 2048; ++SampleIndex)
        {
            Renderer.ProcessSample(0.0f);
        }

        TArray<FVector2f> Output;
        Output.Reserve(ConvolutionBlockSize * 4);
        for (int32 SampleIndex = 0; SampleIndex < ConvolutionBlockSize * 4; ++SampleIndex)
        {
            Output.Add(Renderer.ProcessSample(SampleIndex == 0 ? 1.0f : 0.0f));
        }
        return Output;
    };

    const TArray<FVector2f> LeftDominant = RenderDirectionalImpulse(
        0.8f,
        0.2f,
        IndirectMix);
    float LeftPeak = 0.0f;
    float RightPeak = 0.0f;
    float LeftEnergy = 0.0f;
    float RightEnergy = 0.0f;
    for (const FVector2f& Frame : LeftDominant)
    {
        TestTrue(TEXT("Directional convolution remains finite"), FMath::IsFinite(Frame.X) && FMath::IsFinite(Frame.Y));
        LeftPeak = FMath::Max(LeftPeak, FMath::Abs(Frame.X));
        RightPeak = FMath::Max(RightPeak, FMath::Abs(Frame.Y));
        LeftEnergy += Frame.X * Frame.X;
        RightEnergy += Frame.Y * Frame.Y;
    }
    TestTrue(TEXT("Indirect mix scales the left impulse peak"), FMath::IsNearlyEqual(LeftPeak, 0.4f, 1.0e-4f));
    TestTrue(TEXT("Indirect mix scales the right impulse peak"), FMath::IsNearlyEqual(RightPeak, 0.1f, 1.0e-4f));
    TestTrue(TEXT("A left-directed IR remains left-energy dominant"), LeftEnergy > RightEnergy * 10.0f);
    TestTrue(TEXT("Configured early-reflection gain stays within its quantitative bound"), LeftPeak <= 0.4001f);

    const TArray<FVector2f> RightDominant = RenderDirectionalImpulse(
        0.2f,
        0.8f,
        IndirectMix);
    float SwappedLeftEnergy = 0.0f;
    float SwappedRightEnergy = 0.0f;
    for (const FVector2f& Frame : RightDominant)
    {
        SwappedLeftEnergy += Frame.X * Frame.X;
        SwappedRightEnergy += Frame.Y * Frame.Y;
    }
    TestTrue(TEXT("Swapping directional IR channels swaps the rendered energy"), SwappedRightEnergy > SwappedLeftEnergy * 10.0f);
    TestTrue(TEXT("Directional channel energies are symmetric after swapping"), FMath::IsNearlyEqual(LeftEnergy, SwappedRightEnergy, 1.0e-5f));

    const TArray<FVector2f> MakeupGainWet = RenderDirectionalImpulse(
        0.4f,
        0.1f,
        1.5f);
    float MakeupGainLeftPeak = 0.0f;
    for (const FVector2f& Frame : MakeupGainWet)
    {
        MakeupGainLeftPeak = FMath::Max(
            MakeupGainLeftPeak,
            FMath::Abs(Frame.X));
    }
    TestTrue(
        TEXT("Wet send above unity supplies explicit indirect makeup gain"),
        FMath::IsNearlyEqual(MakeupGainLeftPeak, 0.6f, 1.0e-4f));

    FUERayTracingAudioIndirectSimulationResult TailParameters;
    TailParameters.bHasValidPaths = true;
    TailParameters.bUsedParametricTail = true;
    TailParameters.LateReverbGain = 0.6f;
    TailParameters.ParametricDelaySeconds = 0.0f;
    TailParameters.ReverbTimes = FVector(0.2f, 0.25f, 0.3f);
    TailParameters.ParametricEq = FVector::OneVector;

    FUERayTracingAudioLateReverbRenderer LateRenderer;
    LateRenderer.Initialize(TestSampleRate, 0.5f);
    LateRenderer.Reset(TestSampleRate);
    LateRenderer.Configure(&TailParameters, 0.5f);

    TArray<float> Tail;
    Tail.Reserve(TestSampleRate);
    int32 FirstTailSample = INDEX_NONE;
    float TailPeak = 0.0f;
    for (int32 SampleIndex = 0; SampleIndex < TestSampleRate; ++SampleIndex)
    {
        const float Sample = LateRenderer.ProcessSample(SampleIndex == 0 ? 1.0f : 0.0f);
        TestTrue(TEXT("Parametric tail remains finite"), FMath::IsFinite(Sample));
        TailPeak = FMath::Max(TailPeak, FMath::Abs(Sample));
        if (FirstTailSample == INDEX_NONE && FMath::Abs(Sample) > UE_SMALL_NUMBER)
        {
            FirstTailSample = SampleIndex;
        }
        Tail.Add(Sample);
    }

    TestTrue(TEXT("Parametric tail produces delayed energy"), FirstTailSample >= 64);
    const int32 PrimaryCombArrivalSamples[] =
    {
        FMath::Max(
            64,
            FMath::RoundToInt(static_cast<float>(TestSampleRate) * 0.031f))
            + 1,
        FMath::Max(
            64,
            FMath::RoundToInt(static_cast<float>(TestSampleRate) * 0.047f))
            + 1,
        FMath::Max(
            64,
            FMath::RoundToInt(static_cast<float>(TestSampleRate) * 0.071f))
            + 1
    };
    float PrimaryCombArrivalEnergy = 0.0f;
    for (const int32 ArrivalSample : PrimaryCombArrivalSamples)
    {
        if (TestTrue(
                TEXT("Each primary comb arrival is inside the rendered tail"),
                Tail.IsValidIndex(ArrivalSample)))
        {
            PrimaryCombArrivalEnergy +=
                Tail[ArrivalSample] * Tail[ArrivalSample];
        }
    }
    TestTrue(
        TEXT("Three decorrelated comb arrivals preserve equal power with sum/sqrt(N)"),
        FMath::IsNearlyEqual(
            PrimaryCombArrivalEnergy,
            TailParameters.LateReverbGain,
            1.0e-4f));
    const float EarlyTailEnergy = CalculateEnergyRange(
        Tail,
        FMath::Max(FirstTailSample, 0),
        FMath::Max(FirstTailSample, 0) + 2000);
    const float LateTailEnergy = CalculateEnergyRange(Tail, TestSampleRate - 2000, TestSampleRate);
    TestTrue(TEXT("Early tail window contains energy"), EarlyTailEnergy > UE_SMALL_NUMBER);
    TestTrue(TEXT("Late tail energy decays quantitatively"), LateTailEnergy < EarlyTailEnergy * 0.05f);
    TestTrue(
        TEXT("Late reverb energy is converted to a bounded linear amplitude"),
        TailPeak <= FMath::Sqrt(TailParameters.LateReverbGain) + 1.0e-4f);

    FUERayTracingAudioIndirectSimulationResult QuarterEnergyParameters =
        TailParameters;
    QuarterEnergyParameters.LateReverbGain *= 0.25f;
    FUERayTracingAudioLateReverbRenderer QuarterEnergyRenderer;
    QuarterEnergyRenderer.Initialize(TestSampleRate, 0.5f);
    QuarterEnergyRenderer.Reset(TestSampleRate);
    QuarterEnergyRenderer.Configure(&QuarterEnergyParameters, 0.5f);

    float QuarterEnergyPeak = 0.0f;
    float QuarterEnergyOutput = 0.0f;
    for (int32 SampleIndex = 0; SampleIndex < TestSampleRate; ++SampleIndex)
    {
        const float Sample = QuarterEnergyRenderer.ProcessSample(
            SampleIndex == 0 ? 1.0f : 0.0f);
        QuarterEnergyPeak = FMath::Max(
            QuarterEnergyPeak,
            FMath::Abs(Sample));
        QuarterEnergyOutput += Sample * Sample;
    }
    const float TailOutputEnergy = CalculateEnergyRange(
        Tail,
        0,
        Tail.Num());
    TestTrue(
        TEXT("Four times reflected energy produces twice the tail amplitude"),
        FMath::IsNearlyEqual(
            TailPeak,
            QuarterEnergyPeak * 2.0f,
            1.0e-4f));
    TestTrue(
        TEXT("Four times reflected energy produces four times output energy"),
        FMath::IsNearlyEqual(
            TailOutputEnergy,
            QuarterEnergyOutput * 4.0f,
            1.0e-3f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioLateReverbPreparedCapacityTest,
    "UERayTracingAudio.Audio.LateReverbPreparedCapacity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioLateReverbPreparedCapacityTest::RunTest(
    const FString& Parameters)
{
    constexpr int32 TestSampleRate = 8000;
    constexpr float MaxPreDelaySeconds = 0.5f;

    FUERayTracingAudioIndirectSimulationResult TailParameters;
    TailParameters.bHasValidPaths = true;
    TailParameters.bUsedParametricTail = true;
    TailParameters.LateReverbGain = 0.5f;
    TailParameters.ParametricDelaySeconds = 0.0f;
    TailParameters.ReverbTimes = FVector(0.2f, 0.25f, 0.3f);
    TailParameters.ParametricEq = FVector::OneVector;

    FUERayTracingAudioLateReverbRenderer Renderer;
    Renderer.Initialize(TestSampleRate, MaxPreDelaySeconds);
    const int32 PreparedCapacity = Renderer.GetDelayCapacityForTesting();
    TestTrue(
        TEXT("Late reverb prepares the complete pre-delay capacity before rendering"),
        PreparedCapacity > FMath::CeilToInt(
            TestSampleRate * MaxPreDelaySeconds));

    Renderer.Reset(TestSampleRate);
    Renderer.Configure(&TailParameters, MaxPreDelaySeconds);
    bool bProducedSeedTail = false;
    for (int32 SampleIndex = 0; SampleIndex < TestSampleRate; ++SampleIndex)
    {
        const float Output = Renderer.ProcessSample(
            SampleIndex == 0 ? 1.0f : 0.0f);
        bProducedSeedTail |= FMath::Abs(Output) > UE_SMALL_NUMBER;
    }
    TestTrue(TEXT("The prepared renderer still produces a tail"), bProducedSeedTail);

    // Reset must logically invalidate the old delay and comb contents without
    // freeing/reallocating or leaking the previous voice into a reused source.
    Renderer.Reset(TestSampleRate);
    Renderer.Configure(&TailParameters, MaxPreDelaySeconds);
    float ReusedSourcePeak = 0.0f;
    for (int32 SampleIndex = 0; SampleIndex < TestSampleRate; ++SampleIndex)
    {
        ReusedSourcePeak = FMath::Max(
            ReusedSourcePeak,
            FMath::Abs(Renderer.ProcessSample(0.0f)));
    }
    TestTrue(
        TEXT("Source reuse cannot replay stale delay or comb samples"),
        ReusedSourcePeak <= UE_SMALL_NUMBER);
    TestEqual(
        TEXT("Reset and Configure retain the prepared delay capacity"),
        Renderer.GetDelayCapacityForTesting(),
        PreparedCapacity);

    const uint64 OverflowCountBefore = Renderer.GetCapacityOverflowCount();
    TailParameters.ParametricDelaySeconds = MaxPreDelaySeconds + 0.25f;
    Renderer.Configure(&TailParameters, 1.0f);
    TestEqual(
        TEXT("An out-of-capacity pre-delay records one overflow"),
        Renderer.GetCapacityOverflowCount(),
        OverflowCountBefore + 1);
    TestFalse(
        TEXT("An out-of-capacity tail degrades to silence instead of reallocating"),
        Renderer.HasOutput());
    TestEqual(
        TEXT("An overflow cannot grow the prepared delay storage"),
        Renderer.GetDelayCapacityForTesting(),
        PreparedCapacity);

    Renderer.Reset(TestSampleRate);
    TailParameters.ParametricDelaySeconds =
        std::numeric_limits<float>::max();
    const uint64 HugeOverflowCountBefore =
        Renderer.GetCapacityOverflowCount();
    Renderer.Configure(&TailParameters, 1.0f);
    TestEqual(
        TEXT("A huge finite pre-delay is rejected before integer conversion"),
        Renderer.GetCapacityOverflowCount(),
        HugeOverflowCountBefore + 1);
    TestFalse(
        TEXT("A huge finite pre-delay cannot wrap into a valid short delay"),
        Renderer.HasOutput());

    Renderer.Reset(TestSampleRate);
    TailParameters.ParametricDelaySeconds =
        std::numeric_limits<float>::quiet_NaN();
    const uint64 NonFiniteOverflowCountBefore =
        Renderer.GetCapacityOverflowCount();
    Renderer.Configure(&TailParameters, 1.0f);
    TestEqual(
        TEXT("A non-finite pre-delay is rejected explicitly"),
        Renderer.GetCapacityOverflowCount(),
        NonFiniteOverflowCountBefore + 1);
    TestFalse(
        TEXT("A non-finite pre-delay cannot silently become zero delay"),
        Renderer.HasOutput());

    Renderer.Reset(TestSampleRate);
    TailParameters.bUsedParametricTail = false;
    TailParameters.ParametricDelaySeconds =
        std::numeric_limits<float>::max();
    const uint64 BakedMetadataOverflowCountBefore =
        Renderer.GetCapacityOverflowCount();
    Renderer.Configure(&TailParameters, 30.0f);
    TestEqual(
        TEXT("A long convolution-only baked IR does not consume parametric pre-delay capacity"),
        Renderer.GetCapacityOverflowCount(),
        BakedMetadataOverflowCountBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioWetNonFiniteStateRecoveryTest,
    "UERayTracingAudio.Audio.WetNonFiniteStateRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioWetNonFiniteStateRecoveryTest::RunTest(
    const FString& Parameters)
{
    static_cast<void>(Parameters);
    constexpr int32 TestSampleRate = 8000;
    constexpr int32 BlockSize = 8;

    FUERayTracingAudioPartitionedConvolver Convolver;
    Convolver.SetKernel(
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 1.0f, 0.5f },
            TestSampleRate,
            BlockSize));
    Convolver.ProcessSample(
        std::numeric_limits<float>::quiet_NaN());
    bool bConvolutionRecovered = false;
    bool bConvolutionStayedFinite = true;
    for (int32 SampleIndex = 0; SampleIndex < 64; ++SampleIndex)
    {
        const float Output = Convolver.ProcessSample(
            SampleIndex == 16 ? 1.0f : 0.0f);
        bConvolutionStayedFinite &= FMath::IsFinite(Output);
        bConvolutionRecovered |= FMath::Abs(Output) > UE_SMALL_NUMBER;
    }
    TestTrue(
        TEXT("A non-finite convolution input never enters persistent FFT history"),
        bConvolutionStayedFinite);
    TestTrue(
        TEXT("Partitioned convolution recovers for later finite input"),
        bConvolutionRecovered);

    FUERayTracingAudioIndirectSimulationResult TailParameters;
    TailParameters.bHasValidPaths = true;
    TailParameters.bUsedParametricTail = true;
    TailParameters.LateReverbGain = 0.5f;
    TailParameters.ParametricDelaySeconds = 0.0f;
    TailParameters.ReverbTimes = FVector(0.2f, 0.25f, 0.3f);
    TailParameters.ParametricEq = FVector::OneVector;
    FUERayTracingAudioLateReverbRenderer LateRenderer;
    FUERayTracingAudioLateReverbRenderer CleanLateRenderer;
    LateRenderer.Initialize(TestSampleRate, 0.5f);
    CleanLateRenderer.Initialize(TestSampleRate, 0.5f);
    LateRenderer.Configure(&TailParameters, 0.5f);
    CleanLateRenderer.Configure(&TailParameters, 0.5f);
    LateRenderer.ProcessSample(
        std::numeric_limits<float>::infinity());
    CleanLateRenderer.ProcessSample(0.0f);
    bool bLateStayedFinite = true;
    bool bLateRecovered = false;
    bool bLateMatchesCleanState = true;
    for (int32 SampleIndex = 0; SampleIndex < TestSampleRate; ++SampleIndex)
    {
        const float Input = SampleIndex == 1 ? 1.0f : 0.0f;
        const float Output = LateRenderer.ProcessSample(Input);
        const float CleanOutput = CleanLateRenderer.ProcessSample(Input);
        bLateStayedFinite &= FMath::IsFinite(Output);
        bLateRecovered |= FMath::Abs(CleanOutput) > UE_SMALL_NUMBER;
        bLateMatchesCleanState &= FMath::IsNearlyEqual(
            Output,
            CleanOutput,
            1.0e-6f);
    }
    TestTrue(
        TEXT("A non-finite pre-delay input cannot poison comb feedback"),
        bLateStayedFinite);
    TestTrue(
        TEXT("Late reverb recovers for later finite input"),
        bLateRecovered);
    TestTrue(
        TEXT("Replacing non-finite input with silence preserves clean late-reverb state"),
        bLateMatchesCleanState);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioIndirectDataSourceModesTest,
    "UERayTracingAudio.Audio.IndirectDataSourceModes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioIndirectDataSourceModesTest::RunTest(const FString& Parameters)
{
    constexpr int32 TestSampleRate = 8000;
    constexpr int32 BlockSize = 8;
    constexpr int32 CrossfadeSamples = 2048;

    auto BuildKernel = [](TArray<float> ImpulseResponse)
    {
        return FUERayTracingAudioConvolutionKernel::Build(
            ImpulseResponse,
            TestSampleRate,
            BlockSize);
    };
    auto RenderImpulse = [](const FUERayTracingAudioSimulationSnapshot& Snapshot)
    {
        FUERayTracingAudioIndirectRenderer Renderer;
        Renderer.Initialize(TestSampleRate, 4.0f);
        Renderer.Reset(TestSampleRate);
        FUERayTracingAudioPreparedRendererTestHarness PreparedRenderer(
            TestSampleRate);
        PreparedRenderer.Configure(Renderer, &Snapshot);
        for (int32 SampleIndex = 0; SampleIndex < BlockSize + CrossfadeSamples; ++SampleIndex)
        {
            Renderer.ProcessSample(0.0f);
        }
        TArray<FVector2f> Output;
        for (int32 SampleIndex = 0; SampleIndex < 64; ++SampleIndex)
        {
            Output.Add(Renderer.ProcessSample(SampleIndex == 0 ? 1.0f : 0.0f));
        }
        return Output;
    };
    auto ChannelEnergy = [](const TArray<FVector2f>& Frames, const bool bLeft)
    {
        float Energy = 0.0f;
        for (const FVector2f& Frame : Frames)
        {
            const float Sample = bLeft ? Frame.X : Frame.Y;
            Energy += Sample * Sample;
        }
        return Energy;
    };

    FUERayTracingAudioSimulationSnapshot RealtimeSnapshot;
    RealtimeSnapshot.IndirectMix = 1.0f;
    RealtimeSnapshot.IndirectResult.bHasValidPaths = true;
    RealtimeSnapshot.RealtimeConvolutionKernelLeft = BuildKernel({ 0.8f });
    RealtimeSnapshot.RealtimeConvolutionKernelRight = BuildKernel({ 0.2f });
    const TArray<FVector2f> RealtimeOutput = RenderImpulse(RealtimeSnapshot);
    TestTrue(
        TEXT("Realtime mode renders only the realtime left-dominant IR"),
        ChannelEnergy(RealtimeOutput, true) > ChannelEnergy(RealtimeOutput, false) * 10.0f);

    FUERayTracingAudioSimulationSnapshot BakedSnapshot;
    BakedSnapshot.IndirectMix = 1.0f;
    BakedSnapshot.IndirectResult.bHasValidPaths = true;
    BakedSnapshot.BakedConvolutionKernel = BuildKernel({ 0.2f });
    BakedSnapshot.BakedConvolutionKernelRight = BuildKernel({ 0.8f });
    const TArray<FVector2f> BakedOutput = RenderImpulse(BakedSnapshot);
    TestTrue(
        TEXT("Baked mode renders only the baked right-dominant IR"),
        ChannelEnergy(BakedOutput, false) > ChannelEnergy(BakedOutput, true) * 10.0f);

    FUERayTracingAudioSimulationSnapshot HybridSnapshot;
    HybridSnapshot.IndirectMix = 1.0f;
    HybridSnapshot.IndirectResult.bHasValidPaths = true;
    HybridSnapshot.BakedConvolutionKernel = BuildKernel({ 0.6f });
    HybridSnapshot.BakedConvolutionKernelRight = BuildKernel({ 0.1f });
    HybridSnapshot.RealtimeConvolutionKernelLeft = BuildKernel(
        { 0.0f, 0.0f, 0.0f, 0.2f });
    HybridSnapshot.RealtimeConvolutionKernelRight = BuildKernel(
        { 0.0f, 0.0f, 0.0f, 0.7f });
    const TArray<FVector2f> HybridOutput = RenderImpulse(HybridSnapshot);
    TestTrue(TEXT("Hybrid mode keeps baked early energy"), FMath::Abs(HybridOutput[BlockSize].X) > 0.5f);
    TestTrue(TEXT("Hybrid mode adds realtime late energy"), FMath::Abs(HybridOutput[BlockSize + 3].Y) > 0.6f);

    FUERayTracingAudioIndirectRenderer SwitchingRenderer;
    SwitchingRenderer.Initialize(TestSampleRate, 4.0f);
    SwitchingRenderer.Reset(TestSampleRate);
    FUERayTracingAudioPreparedRendererTestHarness PreparedSwitchingRenderer(
        TestSampleRate);
    PreparedSwitchingRenderer.Configure(
        SwitchingRenderer,
        &RealtimeSnapshot);
    for (int32 SampleIndex = 0; SampleIndex < BlockSize + CrossfadeSamples + 32; ++SampleIndex)
    {
        SwitchingRenderer.ProcessSample(1.0f);
    }
    const float BeforeSwitch = SwitchingRenderer.ProcessSample(1.0f).X;
    PreparedSwitchingRenderer.Configure(
        SwitchingRenderer,
        &BakedSnapshot);
    float Previous = BeforeSwitch;
    float MaxSwitchDiscontinuity = 0.0f;
    for (int32 SampleIndex = 0; SampleIndex < BlockSize + CrossfadeSamples + 32; ++SampleIndex)
    {
        const float Current = SwitchingRenderer.ProcessSample(1.0f).X;
        MaxSwitchDiscontinuity = FMath::Max(MaxSwitchDiscontinuity, FMath::Abs(Current - Previous));
        Previous = Current;
    }
    TestTrue(
        TEXT("Realtime to Baked crossfade has no click-sized discontinuity"),
        MaxSwitchDiscontinuity < 0.01f);

    PreparedSwitchingRenderer.Configure(
        SwitchingRenderer,
        nullptr);
    const float FadeStart = SwitchingRenderer.ProcessSample(1.0f).X;
    TestTrue(
        TEXT("Disabling indirect does not jump on the first sample"),
        FMath::Abs(FadeStart - Previous) < 0.01f);
    float FinalDisabledOutput = FadeStart;
    for (int32 SampleIndex = 0; SampleIndex < CrossfadeSamples + 32; ++SampleIndex)
    {
        FinalDisabledOutput = SwitchingRenderer.ProcessSample(1.0f).X;
    }
    TestTrue(
        TEXT("Disabled indirect crossfade removes the old convolution tail"),
        FMath::Abs(FinalDisabledOutput) <= 1.0e-4f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioImpulseResponseAssetContractTest,
    "UERayTracingAudio.Bake.ImpulseResponseAssetContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioImpulseResponseAssetContractTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioBakeSettings Settings;
    Settings.NumRays = 128;
    Settings.MaxBounces = 2;
    Settings.DurationSeconds = 0.05f;
    Settings.SampleRate = 16000;
    Settings.bRequireHardwareRayTracing = false;

    const TArray<float> ReferenceSamples{ 1.0f, 0.5f, -0.25f, 0.125f };
    auto BuildAsset = [&Settings, &ReferenceSamples]()
    {
        UUERayTracingAudioImpulseResponseAsset* Asset =
            NewObject<UUERayTracingAudioImpulseResponseAsset>(GetTransientPackage());
        TArray<float> Samples = ReferenceSamples;
        Asset->Initialize(
            FSoftObjectPath(TEXT("/Game/Maps/AutomationRoom.AutomationRoom")),
            7,
            TEXT("automation-scene-signature"),
            FVector(100.0, 0.0, 120.0),
            FVector(-100.0, 0.0, 120.0),
            Settings,
            EUERayTracingAudioImpulseResponseChannelFormat::Mono,
            1,
            1.0f / static_cast<float>(Settings.SampleRate),
            MoveTemp(Samples));
        return Asset;
    };

    UUERayTracingAudioImpulseResponseAsset* First = BuildAsset();
    UUERayTracingAudioImpulseResponseAsset* Second = BuildAsset();
    FString ValidationError;
    TestTrue(TEXT("First repeat is a valid IR asset"), First->Validate(ValidationError));
    TestTrue(TEXT("Second repeat is a valid IR asset"), Second->Validate(ValidationError));
    TestEqual(TEXT("Repeated inputs preserve sample count"), First->Samples.Num(), Second->Samples.Num());
    for (int32 SampleIndex = 0; SampleIndex < First->Samples.Num(); ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(TEXT("Repeated sample %d is stable"), SampleIndex),
            FMath::IsNearlyEqual(First->Samples[SampleIndex], Second->Samples[SampleIndex], 1.0e-7f));
    }

    TestEqual(TEXT("IR frame count matches mono samples"), First->GetNumFrames(), ReferenceSamples.Num());
    TestTrue(
        TEXT("IR duration matches frame count and sample rate"),
        FMath::IsNearlyEqual(
            First->GetDurationSeconds(),
            static_cast<float>(ReferenceSamples.Num()) / static_cast<float>(Settings.SampleRate),
            1.0e-7f));
    TestTrue(
        TEXT("Repeated IR energy is stable"),
        FMath::IsNearlyEqual(CalculateEnergy(First->Samples), CalculateEnergy(Second->Samples), 1.0e-7f));
    TestTrue(TEXT("IR energy remains positive"), CalculateEnergy(First->Samples) > 0.0f);
    return true;
}

#endif
