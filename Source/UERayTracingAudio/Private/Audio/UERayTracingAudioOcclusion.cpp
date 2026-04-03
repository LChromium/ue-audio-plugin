#include "Audio/UERayTracingAudioOcclusion.h"

#include "Components/AudioComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Settings/UERayTracingAudioOcclusionSettings.h"
#include "UERayTracingAudioModule.h"

void FUERayTracingAudioOcclusionPlugin::Initialize(const FAudioPluginInitializationParams InitializationParams)
{
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
}

void FUERayTracingAudioOcclusionPlugin::OnReleaseSource(const uint32 SourceId)
{
    if (!Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        return;
    }

    Sources[SourceId].PreviousGain = 1.0f;
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
            }
        }
    }

    const int32 NumSamples = OutputData.AudioBuffer.Num();
    for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
    {
        const float Alpha = static_cast<float>(SampleIndex + 1) / static_cast<float>(NumSamples);
        const float Gain = FMath::Lerp(SourceState.PreviousGain, TargetGain, Alpha);
        OutputData.AudioBuffer[SampleIndex] = (*InputData.AudioBuffer)[SampleIndex] * Gain;
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
