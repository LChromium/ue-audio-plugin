#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"

class FUERayTracingAudioIndirectAudioBridge;

struct FUERayTracingAudioSpatializationSource
{
    bool bEnableSpatialization = true;
    float PanningStrength = 1.0f;
    int32 NumInputChannels = 1;
    float PreviousLeftGain = UE_INV_SQRT_2;
    float PreviousRightGain = UE_INV_SQRT_2;
};

class FUERayTracingAudioSpatializationPlugin : public IAudioSpatialization
{
public:
    explicit FUERayTracingAudioSpatializationPlugin(
        TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> InIndirectAudioBridge,
        FAudioDevice* InOwningDevice = nullptr);
    virtual void Initialize(const FAudioPluginInitializationParams InitializationParams) override;
    virtual void Shutdown() override;
    virtual bool IsSpatializationEffectInitialized() const override;
    virtual void OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, USpatializationPluginSourceSettingsBase* InSettings) override;
    virtual void OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, const uint32 NumChannels, USpatializationPluginSourceSettingsBase* InSettings) override;
    virtual void OnReleaseSource(const uint32 SourceId) override;
    virtual void ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData) override;

private:
    static void CalculateStereoGains(
        const FSpatializationParams* SpatializationParams,
        bool bEnableSpatialization,
        float PanningStrength,
        float& OutLeftGain,
        float& OutRightGain);

    bool bInitialized = false;
    int32 NumOutputChannels = 2;
    int32 SampleRate = 48000;
    TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> IndirectAudioBridge;
    TArray<FUERayTracingAudioSpatializationSource> Sources;
    FAudioDevice* OwningDevice = nullptr;
    bool bShutdown = false;
};

class FUERayTracingAudioSpatializationPluginFactory : public IAudioSpatializationFactory
{
public:
    virtual FString GetDisplayName() override;
    virtual bool SupportsPlatform(const FString& PlatformName) override;
    virtual UClass* GetCustomSpatializationSettingsClass() const override;
    virtual TAudioSpatializationPtr CreateNewSpatializationPlugin(FAudioDevice* OwningDevice) override;
};
