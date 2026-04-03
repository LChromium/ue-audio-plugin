#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"

class FUERayTracingAudioSpatializationPlugin : public IAudioSpatialization
{
public:
    virtual void Initialize(const FAudioPluginInitializationParams InitializationParams) override;
    virtual bool IsSpatializationEffectInitialized() const override;
    virtual void OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, USpatializationPluginSourceSettingsBase* InSettings) override;
    virtual void OnReleaseSource(const uint32 SourceId) override;
    virtual void ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData) override;

private:
    bool bInitialized = false;
};

class FUERayTracingAudioSpatializationPluginFactory : public IAudioSpatializationFactory
{
public:
    virtual FString GetDisplayName() override;
    virtual bool SupportsPlatform(const FString& PlatformName) override;
    virtual UClass* GetCustomSpatializationSettingsClass() const override;
    virtual TAudioSpatializationPtr CreateNewSpatializationPlugin(FAudioDevice* OwningDevice) override;
};
