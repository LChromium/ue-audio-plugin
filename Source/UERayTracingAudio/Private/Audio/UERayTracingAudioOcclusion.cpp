#include "Audio/UERayTracingAudioOcclusion.h"

#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioAudioDiagnosticsInternal.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Settings/UERayTracingAudioOcclusionSettings.h"
#include "UERayTracingAudioModule.h"

namespace
{
    constexpr float MaxRuntimeParametricPreDelaySeconds = 4.0f;

    void ResetOcclusionSourceState(
        FUERayTracingAudioOcclusionSource& Source,
        const int32 SampleRate)
    {
        Source.bApplyDistanceAttenuation = true;
        Source.bApplyAirAbsorption = true;
        Source.bApplyOcclusion = true;
        Source.bHasAppliedSnapshotGain = false;
        Source.bHasRenderedAudio = false;
        Source.PreviousBandGains = FVector::OneVector;
        Source.PreviousBroadbandGain = 1.0f;
        Source.NumChannels = 0;
        Source.SampleRate = SampleRate;
        Source.ActiveAudioComponentId = 0;
        Source.AirAbsorptionProcessor.Reset();
        Source.IndirectRenderer.Reset(SampleRate);
    }
}

FUERayTracingAudioOcclusionPlugin::FUERayTracingAudioOcclusionPlugin(
    TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> InSnapshotRegistry,
    TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> InIndirectAudioBridge,
    const FVector2f& InCrossoversHz,
    FAudioDevice* InOwningDevice)
    : SnapshotRegistry(MoveTemp(InSnapshotRegistry))
    , IndirectAudioBridge(MoveTemp(InIndirectAudioBridge))
    , CrossoversHz(InCrossoversHz)
    , OwningDevice(InOwningDevice)
{
}

FUERayTracingAudioOcclusionPlugin::FUERayTracingAudioOcclusionPlugin(
    TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> InSnapshotRegistry,
    TSharedRef<
        FUERayTracingAudioIndirectAudioBridge,
        ESPMode::ThreadSafe> InIndirectAudioBridge,
    FAudioDevice* InOwningDevice)
    : FUERayTracingAudioOcclusionPlugin(
        MoveTemp(InSnapshotRegistry),
        MoveTemp(InIndirectAudioBridge),
        FVector2f(500.0f, 4000.0f),
        InOwningDevice)
{
}

void FUERayTracingAudioOcclusionPlugin::Initialize(const FAudioPluginInitializationParams InitializationParams)
{
    SampleRate = FMath::Max(static_cast<int32>(InitializationParams.SampleRate), 8000);
    const float RuntimeNyquist = FMath::Max(
        static_cast<float>(SampleRate) * 0.5f,
        40.0f);
    CrossoversHz.X = FMath::Clamp(
        FMath::IsFinite(CrossoversHz.X) ? CrossoversHz.X : 500.0f,
        20.0f,
        RuntimeNyquist);
    CrossoversHz.Y = FMath::Clamp(
        FMath::IsFinite(CrossoversHz.Y) ? CrossoversHz.Y : 4000.0f,
        CrossoversHz.X,
        RuntimeNyquist);
    Sources.Reset();
    Sources.SetNum(InitializationParams.NumSources);
    for (FUERayTracingAudioOcclusionSource& Source : Sources)
    {
        Source.IndirectRenderer.Initialize(
            SampleRate,
            MaxRuntimeParametricPreDelaySeconds);
        ResetOcclusionSourceState(Source, SampleRate);
    }
    IndirectAudioBridge->Initialize(
        static_cast<int32>(InitializationParams.NumSources),
        static_cast<int32>(InitializationParams.BufferLength),
        SampleRate);
    bShutdown = false;
}

void FUERayTracingAudioOcclusionPlugin::Shutdown()
{
    if (bShutdown)
    {
        return;
    }
    bShutdown = true;
    for (int32 SourceId = 0;
        SourceId < Sources.Num();
        ++SourceId)
    {
        Sources[SourceId].IndirectRenderer.
            ReleasePreparedStates(
                IndirectAudioBridge.Get(),
                SourceId);
    }
    IndirectAudioBridge->ServiceConvolutionGameThread(
        MAX_int32);
    Sources.Reset();
    if (OwningDevice)
    {
        FUERayTracingAudioModule::Get().
            UnregisterAudioDevice(OwningDevice);
        OwningDevice = nullptr;
    }
}

void FUERayTracingAudioOcclusionPlugin::OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, const uint32 NumChannels, UOcclusionPluginSourceSettingsBase* InSettings)
{
    if (!Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        return;
    }

    FUERayTracingAudioOcclusionSource& Source = Sources[SourceId];
    Source.IndirectRenderer.ReleasePreparedStates(
        IndirectAudioBridge.Get(),
        static_cast<int32>(SourceId));
    ResetOcclusionSourceState(Source, SampleRate);
    const UUERayTracingAudioOcclusionSettings* Settings = Cast<UUERayTracingAudioOcclusionSettings>(InSettings);
    Source.bApplyDistanceAttenuation = Settings ? Settings->bApplyDistanceAttenuation : true;
    Source.bApplyAirAbsorption = Settings ? Settings->bApplyAirAbsorption : true;
    Source.bApplyOcclusion = Settings ? Settings->bApplyOcclusion : true;
    Source.NumChannels = static_cast<int32>(NumChannels);
    Source.AirAbsorptionProcessor.Initialize(
        SampleRate,
        Source.NumChannels,
        CrossoversHz.X,
        CrossoversHz.Y);
}

void FUERayTracingAudioOcclusionPlugin::OnReleaseSource(const uint32 SourceId)
{
    if (!Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        return;
    }

    Sources[SourceId].IndirectRenderer.ReleasePreparedStates(
        IndirectAudioBridge.Get(),
        static_cast<int32>(SourceId));
    ResetOcclusionSourceState(
        Sources[SourceId],
        SampleRate);
    IndirectAudioBridge->ClearSource(static_cast<int32>(SourceId));
}

void FUERayTracingAudioOcclusionPlugin::ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData)
{
    FUERayTracingAudioAudioDiagnostics::
        RecordHardRealtimeCallback();
    if (!InputData.AudioBuffer)
    {
        if (!OutputData.AudioBuffer.IsEmpty())
        {
            FMemory::Memzero(
                OutputData.AudioBuffer.GetData(),
                OutputData.AudioBuffer.Num() * sizeof(float));
        }
        return;
    }

    const int32 NumInputSamples = InputData.AudioBuffer->Num();
    const int32 NumOutputSamples = OutputData.AudioBuffer.Num();
    if (!Sources.IsValidIndex(static_cast<int32>(InputData.SourceId)) || InputData.NumChannels <= 0)
    {
        const int32 NumSamplesToCopy = FMath::Min(NumInputSamples, NumOutputSamples);
        for (int32 SampleIndex = 0; SampleIndex < NumSamplesToCopy; ++SampleIndex)
        {
            OutputData.AudioBuffer[SampleIndex] = (*InputData.AudioBuffer)[SampleIndex];
        }
        for (int32 SampleIndex = NumSamplesToCopy; SampleIndex < NumOutputSamples; ++SampleIndex)
        {
            OutputData.AudioBuffer[SampleIndex] = 0.0f;
        }
        return;
    }

    FUERayTracingAudioOcclusionSource& SourceState = Sources[InputData.SourceId];
    const int32 NumChannels = InputData.NumChannels;
    if (SourceState.ActiveAudioComponentId
        != InputData.AudioComponentId)
    {
        const bool bApplyDistanceAttenuation = SourceState.bApplyDistanceAttenuation;
        const bool bApplyAirAbsorption = SourceState.bApplyAirAbsorption;
        const bool bApplyOcclusion = SourceState.bApplyOcclusion;
        SourceState.IndirectRenderer.ReleasePreparedStates(
            IndirectAudioBridge.Get(),
            InputData.SourceId);
        ResetOcclusionSourceState(SourceState, SampleRate);
        SourceState.bApplyDistanceAttenuation = bApplyDistanceAttenuation;
        SourceState.bApplyAirAbsorption = bApplyAirAbsorption;
        SourceState.bApplyOcclusion = bApplyOcclusion;
        SourceState.NumChannels = NumChannels;
        SourceState.ActiveAudioComponentId =
            InputData.AudioComponentId;
    }
    else if (SourceState.NumChannels != NumChannels
        || SourceState.SampleRate != SampleRate)
    {
        const bool bApplyDistanceAttenuation =
            SourceState.bApplyDistanceAttenuation;
        const bool bApplyAirAbsorption =
            SourceState.bApplyAirAbsorption;
        const bool bApplyOcclusion =
            SourceState.bApplyOcclusion;
        const uint64 ActiveAudioComponentId =
            SourceState.ActiveAudioComponentId;
        SourceState.IndirectRenderer.ReleasePreparedStates(
            IndirectAudioBridge.Get(),
            InputData.SourceId);
        ResetOcclusionSourceState(
            SourceState,
            SampleRate);
        SourceState.bApplyDistanceAttenuation =
            bApplyDistanceAttenuation;
        SourceState.bApplyAirAbsorption =
            bApplyAirAbsorption;
        SourceState.bApplyOcclusion =
            bApplyOcclusion;
        SourceState.NumChannels = NumChannels;
        SourceState.ActiveAudioComponentId =
            ActiveAudioComponentId;
    }

    float TargetBroadbandGain = 1.0f;
    FVector TargetBandGains = FVector::OneVector;
    const FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr Snapshot =
        SnapshotRegistry->Read(InputData.AudioComponentId);
    const bool bHasValidDirectResult =
        Snapshot.IsValid()
        && Snapshot->DirectResult.bHasListener;
    if (bHasValidDirectResult)
    {
        const FUERayTracingAudioDirectSimulationResult& Result = Snapshot->DirectResult;
        const float Broadband =
            (SourceState.bApplyDistanceAttenuation
                ? Result.DistanceAttenuation
                : 1.0f)
            * (SourceState.bApplyOcclusion
                ? Result.Occlusion
                : 1.0f);
        const FVector Air = SourceState.bApplyAirAbsorption
            ? Result.AirAbsorption
            : FVector::OneVector;
        const FVector UnclampedBandGains = Broadband * Air;
        TargetBroadbandGain = FMath::IsFinite(Broadband)
            ? FMath::Clamp(Broadband, 0.0f, 4.0f)
            : 1.0f;
        TargetBandGains.X = FMath::IsFinite(UnclampedBandGains.X)
            ? FMath::Clamp(UnclampedBandGains.X, 0.0f, 4.0f)
            : 1.0f;
        TargetBandGains.Y = FMath::IsFinite(UnclampedBandGains.Y)
            ? FMath::Clamp(UnclampedBandGains.Y, 0.0f, 4.0f)
            : 1.0f;
        TargetBandGains.Z = FMath::IsFinite(UnclampedBandGains.Z)
            ? FMath::Clamp(UnclampedBandGains.Z, 0.0f, 4.0f)
            : 1.0f;
        if (!SourceState.bHasAppliedSnapshotGain)
        {
            // If the first callback already has a valid acoustic result, begin
            // at that physical gain. If audio previously rendered without one,
            // preserve continuity and ramp from unity across this buffer.
            if (!SourceState.bHasRenderedAudio)
            {
                SourceState.PreviousBandGains =
                    TargetBandGains;
                SourceState.PreviousBroadbandGain =
                    TargetBroadbandGain;
            }
            SourceState.bHasAppliedSnapshotGain = true;
        }
    }
    SourceState.IndirectRenderer.ConfigurePrepared(
        IndirectAudioBridge.Get(),
        InputData.SourceId,
        InputData.AudioComponentId,
        Snapshot.Get());

    const int32 NumFrames = FMath::Min(NumInputSamples, NumOutputSamples) / NumChannels;
    const bool bCanProcessThreeBand =
        SourceState.AirAbsorptionProcessor.CanProcess(
            NumChannels);
    if (!bCanProcessThreeBand)
    {
        FUERayTracingAudioAudioDiagnostics::
            RecordHardRealtimeCapacityMiss();
    }
    TArrayView<FVector2f> StereoWetFrames = Snapshot
        ? IndirectAudioBridge->BeginWrite(
            InputData.SourceId,
            InputData.AudioComponentId,
            NumFrames,
            Snapshot->DataSource)
        : IndirectAudioBridge->BeginWrite(
            InputData.SourceId,
            InputData.AudioComponentId,
            NumFrames);
    const bool bCanBridgeStereoWet = StereoWetFrames.Num() == NumFrames;
    float PeakAbsoluteInput = 0.0f;
    float PeakAbsoluteWet = 0.0f;
    float PeakAbsoluteOutput = 0.0f;
    float MaxBandGainStep = 0.0f;
    double InputSquareSum = 0.0;
    double WetSquareSum = 0.0;
    double DirectSquareSum = 0.0;
    uint64 FiniteInputSampleCount = 0;
    uint64 FiniteWetSampleCount = 0;
    uint64 FiniteDirectSampleCount = 0;
    uint64 NonFiniteWetSampleCount = 0;
    uint64 NonFiniteDirectSampleCount = 0;
    uint64 NonFiniteOutputSampleCount = 0;
    uint64 OverUnitDirectSampleCount = 0;
    uint64 OverUnitOutputSampleCount = 0;
    const FUERayTracingAudioDirectDiagnosticsTargetToken
        DirectDiagnosticsTarget =
            FUERayTracingAudioAudioDiagnosticsInternal::
                CaptureTarget(
                    InputData.AudioComponentId);
    const bool bRecordAudioDiagnostics =
        DirectDiagnosticsTarget.IsValid();
    FVector PreviousDiagnosticBandGains =
        SourceState.PreviousBandGains;
    float PreviousDiagnosticBroadbandGain =
        SourceState.PreviousBroadbandGain;
    for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
    {
        const int32 SampleIndex = FrameIndex * NumChannels;
        const float Alpha = static_cast<float>(FrameIndex + 1) / static_cast<float>(FMath::Max(NumFrames, 1));
        const FVector BandGains = FMath::Lerp(
            SourceState.PreviousBandGains,
            TargetBandGains,
            Alpha);
        const float BroadbandGain = FMath::Lerp(
            SourceState.PreviousBroadbandGain,
            TargetBroadbandGain,
            Alpha);
        if (bRecordAudioDiagnostics)
        {
            if (bCanProcessThreeBand)
            {
                const FVector BandGainStep =
                    BandGains - PreviousDiagnosticBandGains;
                MaxBandGainStep = FMath::Max(
                    MaxBandGainStep,
                    FMath::Max(
                        FMath::Abs(BandGainStep.X),
                        FMath::Max(
                            FMath::Abs(BandGainStep.Y),
                            FMath::Abs(BandGainStep.Z))));
                PreviousDiagnosticBandGains = BandGains;
            }
            else
            {
                MaxBandGainStep = FMath::Max(
                    MaxBandGainStep,
                    FMath::Abs(
                        BroadbandGain
                        - PreviousDiagnosticBroadbandGain));
                PreviousDiagnosticBroadbandGain =
                    BroadbandGain;
            }
        }
        float MonoInput = 0.0f;
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const float ChannelInput =
                (*InputData.AudioBuffer)[SampleIndex + ChannelIndex];
            MonoInput += ChannelInput;
            if (bRecordAudioDiagnostics
                && FMath::IsFinite(ChannelInput))
            {
                PeakAbsoluteInput = FMath::Max(
                    PeakAbsoluteInput,
                    FMath::Abs(ChannelInput));
                InputSquareSum +=
                    static_cast<double>(ChannelInput)
                    * static_cast<double>(ChannelInput);
                ++FiniteInputSampleCount;
            }
        }
        MonoInput /= static_cast<float>(NumChannels);
        MonoInput = FMath::IsFinite(MonoInput)
            ? MonoInput
            : 0.0f;

        const FVector2f IndirectWet = SourceState.IndirectRenderer.ProcessSample(MonoInput);
        if (!FMath::IsFinite(IndirectWet.X))
        {
            ++NonFiniteWetSampleCount;
        }
        else
        {
            if (bRecordAudioDiagnostics)
            {
                PeakAbsoluteWet = FMath::Max(
                    PeakAbsoluteWet,
                    FMath::Abs(IndirectWet.X));
                WetSquareSum +=
                    static_cast<double>(IndirectWet.X)
                    * static_cast<double>(IndirectWet.X);
                ++FiniteWetSampleCount;
            }
        }
        if (!FMath::IsFinite(IndirectWet.Y))
        {
            ++NonFiniteWetSampleCount;
        }
        else
        {
            if (bRecordAudioDiagnostics)
            {
                PeakAbsoluteWet = FMath::Max(
                    PeakAbsoluteWet,
                    FMath::Abs(IndirectWet.Y));
                WetSquareSum +=
                    static_cast<double>(IndirectWet.Y)
                    * static_cast<double>(IndirectWet.Y);
                ++FiniteWetSampleCount;
            }
        }
        if (StereoWetFrames.IsValidIndex(FrameIndex))
        {
            StereoWetFrames[FrameIndex] = IndirectWet;
        }
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const float ChannelWet = NumChannels == 1 && !bCanBridgeStereoWet
                ? 0.0f
                : NumChannels == 1
                ? 0.5f * (IndirectWet.X + IndirectWet.Y)
                : (ChannelIndex == 0
                    ? IndirectWet.X
                    : (ChannelIndex == 1
                        ? IndirectWet.Y
                        : 0.5f * (IndirectWet.X + IndirectWet.Y)));
            const float DirectSample = bCanProcessThreeBand
                ? SourceState.AirAbsorptionProcessor.ProcessSample(
                    (*InputData.AudioBuffer)[SampleIndex + ChannelIndex],
                    ChannelIndex,
                    BandGains)
                : (*InputData.AudioBuffer)[SampleIndex + ChannelIndex]
                    * BroadbandGain;
            if (bRecordAudioDiagnostics)
            {
                if (FMath::IsFinite(DirectSample))
                {
                    DirectSquareSum +=
                        static_cast<double>(DirectSample)
                        * static_cast<double>(DirectSample);
                    ++FiniteDirectSampleCount;
                    if (FMath::Abs(DirectSample) > 1.0f)
                    {
                        ++OverUnitDirectSampleCount;
                    }
                }
                else
                {
                    ++NonFiniteDirectSampleCount;
                }
            }
            const float OutputSample = DirectSample + ChannelWet;
            if (FMath::IsFinite(OutputSample))
            {
                OutputData.AudioBuffer[SampleIndex + ChannelIndex] =
                    OutputSample;
                if (bRecordAudioDiagnostics)
                {
                    const float AbsoluteOutput = FMath::Abs(OutputSample);
                    PeakAbsoluteOutput = FMath::Max(
                        PeakAbsoluteOutput,
                        AbsoluteOutput);
                    if (AbsoluteOutput > 1.0f)
                    {
                        ++OverUnitOutputSampleCount;
                    }
                }
            }
            else
            {
                OutputData.AudioBuffer[SampleIndex + ChannelIndex] = 0.0f;
                ++NonFiniteOutputSampleCount;
            }
        }
    }

    const int32 NumProcessedSamples = NumFrames * NumChannels;
    for (int32 SampleIndex = NumProcessedSamples; SampleIndex < NumOutputSamples; ++SampleIndex)
    {
        OutputData.AudioBuffer[SampleIndex] = 0.0f;
    }
    if (bRecordAudioDiagnostics)
    {
        const float DirectRms = FiniteDirectSampleCount > 0
            ? FMath::Sqrt(static_cast<float>(
                DirectSquareSum
                / static_cast<double>(FiniteDirectSampleCount)))
            : 0.0f;
        FUERayTracingAudioAudioDiagnosticsInternal::RecordDirectBuffer(
            DirectDiagnosticsTarget,
            NumFrames,
            PeakAbsoluteInput,
            DirectRms,
            MaxBandGainStep,
            NonFiniteDirectSampleCount,
            OverUnitDirectSampleCount);
    }
    if (Snapshot && bRecordAudioDiagnostics)
    {
        const float InputRms = FiniteInputSampleCount > 0
            ? FMath::Sqrt(static_cast<float>(
                InputSquareSum
                / static_cast<double>(FiniteInputSampleCount)))
            : 0.0f;
        const float WetRms = FiniteWetSampleCount > 0
            ? FMath::Sqrt(static_cast<float>(
                WetSquareSum
                / static_cast<double>(FiniteWetSampleCount)))
            : 0.0f;
        FUERayTracingAudioAudioDiagnostics::RecordBuffer(
            Snapshot->DataSource,
            InputData.AudioComponentId,
            NumFrames,
            PeakAbsoluteInput,
            PeakAbsoluteWet,
            InputRms,
            WetRms,
            NonFiniteWetSampleCount + NonFiniteOutputSampleCount,
            PeakAbsoluteOutput,
            OverUnitOutputSampleCount);
    }

    IndirectAudioBridge->EndWrite(InputData.SourceId, InputData.AudioComponentId);

    if (NumFrames > 0)
    {
        SourceState.PreviousBandGains =
            TargetBandGains;
        SourceState.PreviousBroadbandGain =
            TargetBroadbandGain;
        SourceState.bHasRenderedAudio = true;
    }
}

FUERayTracingAudioOcclusionPluginFactory::
    FUERayTracingAudioOcclusionPluginFactory(
        const FVector2f& InCrossoversHz)
    : CrossoversHz(InCrossoversHz)
{
}

FString FUERayTracingAudioOcclusionPluginFactory::GetDisplayName()
{
    return TEXT("UE Ray Tracing Audio Occlusion");
}

bool FUERayTracingAudioOcclusionPluginFactory::SupportsPlatform(const FString& PlatformName)
{
    return PlatformName == TEXT("Windows") || PlatformName == TEXT("Linux") || PlatformName == TEXT("Mac");
}

UClass* FUERayTracingAudioOcclusionPluginFactory::GetCustomOcclusionSettingsClass() const
{
    return UUERayTracingAudioOcclusionSettings::StaticClass();
}

TAudioOcclusionPtr FUERayTracingAudioOcclusionPluginFactory::CreateNewOcclusionPlugin(FAudioDevice* OwningDevice)
{
    FUERayTracingAudioModule::Get().RegisterAudioDevice(OwningDevice);
    FUERayTracingAudioModule& Module = FUERayTracingAudioModule::Get();
    return TAudioOcclusionPtr(new FUERayTracingAudioOcclusionPlugin(
        FUERayTracingAudioModule::GetManager().
            GetSnapshotRegistrySharedRef(),
        Module.GetOrCreateIndirectAudioBridge(OwningDevice),
        CrossoversHz,
        OwningDevice));
}
