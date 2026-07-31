#include "Audio/UERayTracingAudioOcclusion.h"

#include "Components/AudioComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Settings/UERayTracingAudioOcclusionSettings.h"
#include "UERayTracingAudioModule.h"

FUERayTracingAudioOcclusionPlugin::FUERayTracingAudioOcclusionPlugin(
    TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> InSnapshotRegistry,
    TSharedRef<
        FUERayTracingAudioIndirectAudioBridge,
        ESPMode::ThreadSafe> InIndirectAudioBridge,
    const FVector2f& InCrossoversHz,
    FAudioDevice* InOwningDevice)
    : CrossoversHz(InCrossoversHz)
    , OwningDevice(InOwningDevice)
{
}

FUERayTracingAudioOcclusionPlugin::FUERayTracingAudioOcclusionPlugin(
    const FVector2f& InCrossoversHz,
    FAudioDevice* InOwningDevice)
    : CrossoversHz(InCrossoversHz)
    , OwningDevice(InOwningDevice)
{
}

void FUERayTracingAudioOcclusionPlugin::Initialize(const FAudioPluginInitializationParams InitializationParams)
{
    SampleRate = static_cast<int32>(InitializationParams.SampleRate);
    Sources.SetNum(InitializationParams.NumSources);
}

void FUERayTracingAudioOcclusionPlugin::OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, const uint32 NumChannels, UOcclusionPluginSourceSettingsBase* InSettings)
{
    if (!Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        return;
    }

    FUERayTracingAudioOcclusionSource& Source = Sources[SourceId];
    const UUERayTracingAudioOcclusionSettings* Settings = Cast<UUERayTracingAudioOcclusionSettings>(InSettings);
    Source.bApplyDistanceAttenuation = Settings ? Settings->bApplyDistanceAttenuation : true;
    Source.bApplyAirAbsorption = Settings ? Settings->bApplyAirAbsorption : true;
    Source.bApplyOcclusion = Settings ? Settings->bApplyOcclusion : true;
    Source.PreviousBandGains = FVector::OneVector;
    Source.NumChannels = static_cast<int32>(NumChannels);
    Source.SampleRate = SampleRate;
    Source.DelayWriteIndex = 0;
    Source.AirAbsorptionProcessor.Initialize(
        SampleRate,
        Source.NumChannels,
        CrossoversHz.X,
        CrossoversHz.Y);
    Source.DelayBuffer.Reset();
    Source.CombBuffers.Reset();
    Source.CombWriteIndices.Reset();
}

void FUERayTracingAudioOcclusionPlugin::OnReleaseSource(const uint32 SourceId)
{
    if (!Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        return;
    }

    Sources[SourceId].PreviousBandGains = FVector::OneVector;
    Sources[SourceId].DelayWriteIndex = 0;
    Sources[SourceId].AirAbsorptionProcessor.Reset();
    Sources[SourceId].DelayBuffer.Reset();
    Sources[SourceId].CombBuffers.Reset();
    Sources[SourceId].CombWriteIndices.Reset();
}

void FUERayTracingAudioOcclusionPlugin::EnsureDelayCapacity(FUERayTracingAudioOcclusionSource& SourceState, float DurationSeconds)
{
    const int32 RequiredDelaySamples = FMath::Max(
        512,
        FMath::CeilToInt(FMath::Max(DurationSeconds, 0.25f) * static_cast<float>(SourceState.SampleRate)) + 256);

    if (SourceState.DelayBuffer.Num() != RequiredDelaySamples)
    {
        SourceState.DelayBuffer.SetNumZeroed(RequiredDelaySamples);
        SourceState.DelayWriteIndex = 0;
    }

    const int32 CombSizes[] =
    {
        FMath::Max(64, FMath::RoundToInt(static_cast<float>(SourceState.SampleRate) * 0.031f)),
        FMath::Max(64, FMath::RoundToInt(static_cast<float>(SourceState.SampleRate) * 0.047f)),
        FMath::Max(64, FMath::RoundToInt(static_cast<float>(SourceState.SampleRate) * 0.071f))
    };

    if (SourceState.CombBuffers.Num() != UE_ARRAY_COUNT(CombSizes))
    {
        SourceState.CombBuffers.SetNum(UE_ARRAY_COUNT(CombSizes));
        SourceState.CombWriteIndices.Init(0, UE_ARRAY_COUNT(CombSizes));
    }

    for (int32 CombIndex = 0; CombIndex < UE_ARRAY_COUNT(CombSizes); ++CombIndex)
    {
        if (SourceState.CombBuffers[CombIndex].Num() != CombSizes[CombIndex])
        {
            SourceState.CombBuffers[CombIndex].SetNumZeroed(CombSizes[CombIndex]);
            SourceState.CombWriteIndices[CombIndex] = 0;
        }
    }
}

float FUERayTracingAudioOcclusionPlugin::ReadDelayedSample(const FUERayTracingAudioOcclusionSource& SourceState, int32 DelaySamples) const
{
    if (SourceState.DelayBuffer.IsEmpty())
    {
        return 0.0f;
    }

    const int32 ClampedDelaySamples = FMath::Clamp(DelaySamples, 1, SourceState.DelayBuffer.Num() - 1);
    int32 ReadIndex = SourceState.DelayWriteIndex - ClampedDelaySamples;
    if (ReadIndex < 0)
    {
        ReadIndex += SourceState.DelayBuffer.Num() * (1 + (-ReadIndex / SourceState.DelayBuffer.Num()));
    }

    return SourceState.DelayBuffer[ReadIndex % SourceState.DelayBuffer.Num()];
}

float FUERayTracingAudioOcclusionPlugin::RenderIndirectSample(
    FUERayTracingAudioOcclusionSource& SourceState,
    const FUERayTracingAudioIndirectSimulationResult& IndirectResult,
    float MonoInput,
    float IndirectMix)
{
    if (SourceState.DelayBuffer.Num() == 0 || !IndirectResult.bHasValidPaths || IndirectMix <= 0.0f)
    {
        return 0.0f;
    }

    const int32 DelayBufferSize = SourceState.DelayBuffer.Num();
    SourceState.DelayBuffer[SourceState.DelayWriteIndex] = MonoInput;

    float ImpulseWet = 0.0f;
    for (int32 BinIndex = 0; BinIndex < IndirectResult.ReconstructedImpulseResponse.Num(); ++BinIndex)
    {
        const float TapAmplitude = IndirectResult.ReconstructedImpulseResponse[BinIndex];
        if (TapAmplitude <= KINDA_SMALL_NUMBER)
        {
            continue;
        }

        const float DelaySeconds = (static_cast<float>(BinIndex) + 0.5f) * IndirectResult.ImpulseResponseBinDurationSeconds;
        const int32 DelaySamples = FMath::Clamp(
            FMath::RoundToInt(DelaySeconds * static_cast<float>(SourceState.SampleRate)),
            1,
            DelayBufferSize - 1);
        ImpulseWet += ReadDelayedSample(SourceState, DelaySamples) * TapAmplitude;
    }

    float LateWet = 0.0f;
    if (IndirectResult.bUsedParametricTail && SourceState.CombBuffers.Num() == 3)
    {
        const int32 PreDelaySamples = FMath::Clamp(
            FMath::RoundToInt(IndirectResult.ParametricDelaySeconds * static_cast<float>(SourceState.SampleRate)),
            1,
            DelayBufferSize - 1);
        const float PreDelayedInput = ReadDelayedSample(SourceState, PreDelaySamples);

        for (int32 CombIndex = 0; CombIndex < SourceState.CombBuffers.Num(); ++CombIndex)
        {
            TArray<float>& CombBuffer = SourceState.CombBuffers[CombIndex];
            int32& WriteIndex = SourceState.CombWriteIndices[CombIndex];
            const int32 BufferSize = CombBuffer.Num();
            const float BandRT60 = FMath::Max(IndirectResult.ReverbTimes[CombIndex], 0.1f);
            const float BandEq = IndirectResult.ParametricEq[CombIndex];
            const float DelaySeconds = static_cast<float>(BufferSize) / static_cast<float>(SourceState.SampleRate);
            const float Feedback = FMath::Clamp(FMath::Pow(0.001f, DelaySeconds / BandRT60), 0.0f, 0.97f);

            const float DelayedSample = CombBuffer[WriteIndex];
            CombBuffer[WriteIndex] = (PreDelayedInput * BandEq) + (DelayedSample * Feedback);
            WriteIndex = (WriteIndex + 1) % BufferSize;
            LateWet += DelayedSample * BandEq;
        }

        LateWet = (LateWet / static_cast<float>(SourceState.CombBuffers.Num())) * IndirectResult.LateReverbGain;
    }

    SourceState.DelayWriteIndex = (SourceState.DelayWriteIndex + 1) % DelayBufferSize;
    return (ImpulseWet + LateWet) * IndirectMix;
}

void FUERayTracingAudioOcclusionPlugin::ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData)
{
    if (!Sources.IsValidIndex(static_cast<int32>(InputData.SourceId)))
    {
        for (int32 SampleIndex = 0; SampleIndex < OutputData.AudioBuffer.Num(); ++SampleIndex)
        {
            OutputData.AudioBuffer[SampleIndex] = (*InputData.AudioBuffer)[SampleIndex];
        }
        return;
    }

    FUERayTracingAudioOcclusionSource& SourceState = Sources[InputData.SourceId];

    float TargetBroadbandGain = 1.0f;
    FVector TargetBandGains = FVector::OneVector;
    float IndirectMix = 0.0f;
    FUERayTracingAudioIndirectSimulationResult IndirectResult;
    if (UAudioComponent* AudioComponent = UAudioComponent::GetAudioComponentFromID(InputData.AudioComponentId))
    {
        if (AActor* Owner = AudioComponent->GetOwner())
        {
            if (UUERayTracingAudioSourceComponent* SourceComponent = Owner->FindComponentByClass<UUERayTracingAudioSourceComponent>())
            {
                const FUERayTracingAudioDirectSimulationResult& Result = SourceComponent->GetDirectSoundResult();
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

                IndirectResult = SourceComponent->GetIndirectSoundResult();
                IndirectMix = SourceComponent->GetIndirectMix();
                EnsureDelayCapacity(SourceState, SourceComponent->GetIndirectDurationSeconds());
            }
        }
    }

    const int32 NumSamples = OutputData.AudioBuffer.Num();
    const int32 NumChannels = FMath::Max(InputData.NumChannels, 1);
    const bool bCanProcessThreeBand =
        SourceState.AirAbsorptionProcessor.CanProcess(
            NumChannels);
    for (int32 SampleIndex = 0; SampleIndex < NumSamples; SampleIndex += NumChannels)
    {
        const float Alpha = static_cast<float>(SampleIndex + NumChannels) / static_cast<float>(NumSamples);
        const FVector BandGains = FMath::Lerp(
            SourceState.PreviousBandGains,
            TargetBandGains,
            Alpha);
        float MonoInput = 0.0f;
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            MonoInput += (*InputData.AudioBuffer)[SampleIndex + ChannelIndex];
        }
        MonoInput /= static_cast<float>(NumChannels);

        const float IndirectWet = RenderIndirectSample(SourceState, IndirectResult, MonoInput, IndirectMix);
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const float DirectSample = bCanProcessThreeBand
                ? SourceState.AirAbsorptionProcessor.ProcessSample(
                    (*InputData.AudioBuffer)[SampleIndex + ChannelIndex],
                    ChannelIndex,
                    BandGains)
                : (*InputData.AudioBuffer)[SampleIndex + ChannelIndex]
                    * TargetBroadbandGain;
            OutputData.AudioBuffer[SampleIndex + ChannelIndex] =
                DirectSample + IndirectWet;
        }
    }

    SourceState.PreviousBandGains = TargetBandGains;
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
    return TAudioOcclusionPtr(new FUERayTracingAudioOcclusionPlugin(
        CrossoversHz,
        OwningDevice));
}
