#include "Audio/UERayTracingAudioSpatialization.h"

#include "Settings/UERayTracingAudioSpatializationSettings.h"
#include "UERayTracingAudioModule.h"

void FUERayTracingAudioSpatializationPlugin::Initialize(const FAudioPluginInitializationParams InitializationParams)
{
    bInitialized = true;
}

bool FUERayTracingAudioSpatializationPlugin::IsSpatializationEffectInitialized() const
{
    return bInitialized;
}

void FUERayTracingAudioSpatializationPlugin::OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, USpatializationPluginSourceSettingsBase* InSettings)
{
}

void FUERayTracingAudioSpatializationPlugin::OnReleaseSource(const uint32 SourceId)
{
}

void FUERayTracingAudioSpatializationPlugin::ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData)
{
    const int32 NumSamples = FMath::Min(OutputData.AudioBuffer.Num(), InputData.AudioBuffer->Num());
    for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
    {
        OutputData.AudioBuffer[SampleIndex] = (*InputData.AudioBuffer)[SampleIndex];
    }
}

FString FUERayTracingAudioSpatializationPluginFactory::GetDisplayName()
{
    return TEXT("UE Ray Tracing Audio Spatialization");
}

bool FUERayTracingAudioSpatializationPluginFactory::SupportsPlatform(const FString& PlatformName)
{
    return PlatformName == TEXT("Windows") || PlatformName == TEXT("Linux") || PlatformName == TEXT("Mac");
}

UClass* FUERayTracingAudioSpatializationPluginFactory::GetCustomSpatializationSettingsClass() const
{
    return UUERayTracingAudioSpatializationSettings::StaticClass();
}

TAudioSpatializationPtr FUERayTracingAudioSpatializationPluginFactory::CreateNewSpatializationPlugin(FAudioDevice* OwningDevice)
{
    FUERayTracingAudioModule::Get().RegisterAudioDevice(OwningDevice);
    return TAudioSpatializationPtr(new FUERayTracingAudioSpatializationPlugin());
}
