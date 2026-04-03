#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"

class UUERayTracingAudioOcclusionSettings;

struct FUERayTracingAudioOcclusionSource
{
    bool bApplyDistanceAttenuation = true;
    bool bApplyAirAbsorption = true;
    bool bApplyOcclusion = true;
    float PreviousGain = 1.0f;
};

class FUERayTracingAudioOcclusionPlugin : public IAudioOcclusion
{
public:
    virtual void Initialize(const FAudioPluginInitializationParams InitializationParams) override;
    virtual void OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, const uint32 NumChannels, UOcclusionPluginSourceSettingsBase* InSettings) override;
    virtual void OnReleaseSource(const uint32 SourceId) override;
    virtual void ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData) override;

private:
    TArray<FUERayTracingAudioOcclusionSource> Sources;
};

class FUERayTracingAudioOcclusionPluginFactory : public IAudioOcclusionFactory
{
public:
    virtual FString GetDisplayName() override;
    virtual bool SupportsPlatform(const FString& PlatformName) override;
    virtual UClass* GetCustomOcclusionSettingsClass() const override;
    virtual TAudioOcclusionPtr CreateNewOcclusionPlugin(FAudioDevice* OwningDevice) override;
};
