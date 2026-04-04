#include "Audio/UERayTracingAudioOcclusion.h"

#include "Components/AudioComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Settings/UERayTracingAudioOcclusionSettings.h"
#include "UERayTracingAudioModule.h"

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
    Source.PreviousGain = 1.0f;
    Source.NumChannels = static_cast<int32>(NumChannels);
    Source.SampleRate = SampleRate;
    Source.DelayWriteIndex = 0;
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

    Sources[SourceId].PreviousGain = 1.0f;
    Sources[SourceId].DelayWriteIndex = 0;
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

    float EarlyWet = 0.0f;
    for (int32 TapIndex = 0; TapIndex < IndirectResult.EarlyReflectionDelaySeconds.Num(); ++TapIndex)
    {
        const int32 DelaySamples = FMath::Clamp(
            FMath::RoundToInt(IndirectResult.EarlyReflectionDelaySeconds[TapIndex] * static_cast<float>(SourceState.SampleRate)),
            1,
            DelayBufferSize - 1);
        int32 ReadIndex = SourceState.DelayWriteIndex - DelaySamples;
        if (ReadIndex < 0)
        {
            ReadIndex += DelayBufferSize * (1 + (-ReadIndex / DelayBufferSize));
        }
        ReadIndex %= DelayBufferSize;
        EarlyWet += SourceState.DelayBuffer[ReadIndex] * IndirectResult.EarlyReflectionGains[TapIndex];
    }

    float LateWet = 0.0f;
    if (IndirectResult.bUsedParametricTail && SourceState.CombBuffers.Num() == 3)
    {
        const float AverageRT60 = FMath::Max((IndirectResult.ReverbTimes.X + IndirectResult.ReverbTimes.Y + IndirectResult.ReverbTimes.Z) / 3.0f, 0.1f);
        for (int32 CombIndex = 0; CombIndex < SourceState.CombBuffers.Num(); ++CombIndex)
        {
            TArray<float>& CombBuffer = SourceState.CombBuffers[CombIndex];
            int32& WriteIndex = SourceState.CombWriteIndices[CombIndex];
            const int32 BufferSize = CombBuffer.Num();
            const float DelaySeconds = static_cast<float>(BufferSize) / static_cast<float>(SourceState.SampleRate);
            const float Feedback = FMath::Clamp(FMath::Pow(0.001f, DelaySeconds / AverageRT60), 0.0f, 0.95f);

            const float DelayedSample = CombBuffer[WriteIndex];
            CombBuffer[WriteIndex] = MonoInput + (DelayedSample * Feedback);
            WriteIndex = (WriteIndex + 1) % BufferSize;
            LateWet += DelayedSample;
        }

        LateWet = (LateWet / static_cast<float>(SourceState.CombBuffers.Num())) * IndirectResult.LateReverbGain;
    }

    SourceState.DelayWriteIndex = (SourceState.DelayWriteIndex + 1) % DelayBufferSize;
    return (EarlyWet + LateWet) * IndirectMix;
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

    float TargetGain = 1.0f;
    float IndirectMix = 0.0f;
    FUERayTracingAudioIndirectSimulationResult IndirectResult;
    if (UAudioComponent* AudioComponent = UAudioComponent::GetAudioComponentFromID(InputData.AudioComponentId))
    {
        if (AActor* Owner = AudioComponent->GetOwner())
        {
            if (UUERayTracingAudioSourceComponent* SourceComponent = Owner->FindComponentByClass<UUERayTracingAudioSourceComponent>())
            {
                const FUERayTracingAudioDirectSimulationResult& Result = SourceComponent->GetDirectSoundResult();
                TargetGain = 1.0f;

                if (SourceState.bApplyDistanceAttenuation)
                {
                    TargetGain *= Result.DistanceAttenuation;
                }

                if (SourceState.bApplyAirAbsorption)
                {
                    const float AirAverage = (Result.AirAbsorption.X + Result.AirAbsorption.Y + Result.AirAbsorption.Z) / 3.0f;
                    TargetGain *= AirAverage;
                }

                if (SourceState.bApplyOcclusion)
                {
                    TargetGain *= Result.Occlusion;
                }

                IndirectResult = SourceComponent->GetIndirectSoundResult();
                IndirectMix = SourceComponent->GetIndirectMix();
                EnsureDelayCapacity(SourceState, SourceComponent->GetIndirectDurationSeconds());
            }
        }
    }

    const int32 NumSamples = OutputData.AudioBuffer.Num();
    const int32 NumChannels = FMath::Max(InputData.NumChannels, 1);
    for (int32 SampleIndex = 0; SampleIndex < NumSamples; SampleIndex += NumChannels)
    {
        const float Alpha = static_cast<float>(SampleIndex + NumChannels) / static_cast<float>(NumSamples);
        const float Gain = FMath::Lerp(SourceState.PreviousGain, TargetGain, Alpha);
        float MonoInput = 0.0f;
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            MonoInput += (*InputData.AudioBuffer)[SampleIndex + ChannelIndex];
        }
        MonoInput /= static_cast<float>(NumChannels);

        const float IndirectWet = RenderIndirectSample(SourceState, IndirectResult, MonoInput, IndirectMix);
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            OutputData.AudioBuffer[SampleIndex + ChannelIndex] = ((*InputData.AudioBuffer)[SampleIndex + ChannelIndex] * Gain) + IndirectWet;
        }
    }

    SourceState.PreviousGain = TargetGain;
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
    return TAudioOcclusionPtr(new FUERayTracingAudioOcclusionPlugin());
}
