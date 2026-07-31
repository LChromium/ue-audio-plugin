#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"
#include "Audio/UERayTracingAudioIndirectRenderer.h"
#include "Audio/UERayTracingAudioThreeBandAirAbsorption.h"

class UUERayTracingAudioOcclusionSettings;
class FUERayTracingAudioSimulationSnapshotRegistry;
class FUERayTracingAudioIndirectAudioBridge;

struct FUERayTracingAudioOcclusionSource
{
    bool bApplyDistanceAttenuation = true;
    bool bApplyAirAbsorption = true;
    bool bApplyOcclusion = true;
    bool bHasAppliedSnapshotGain = false;
    bool bHasRenderedAudio = false;
    FVector PreviousBandGains = FVector::OneVector;
    int32 NumChannels = 0;
    int32 SampleRate = 48000;
    uint64 ActiveAudioComponentId = 0;
    FUERayTracingAudioThreeBandAirAbsorption AirAbsorptionProcessor;
    FUERayTracingAudioIndirectRenderer IndirectRenderer;
};

class FUERayTracingAudioOcclusionPlugin : public IAudioOcclusion
{
public:
    FUERayTracingAudioOcclusionPlugin(
        TSharedRef<
            FUERayTracingAudioSimulationSnapshotRegistry,
            ESPMode::ThreadSafe> InSnapshotRegistry,
        TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> InIndirectAudioBridge,
        const FVector2f& InCrossoversHz,
        FAudioDevice* InOwningDevice = nullptr);
    FUERayTracingAudioOcclusionPlugin(
        TSharedRef<
            FUERayTracingAudioSimulationSnapshotRegistry,
            ESPMode::ThreadSafe> InSnapshotRegistry,
        TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> InIndirectAudioBridge,
        FAudioDevice* InOwningDevice = nullptr);
    virtual void Initialize(const FAudioPluginInitializationParams InitializationParams) override;
    virtual void Shutdown() override;
    virtual void OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, const uint32 NumChannels, UOcclusionPluginSourceSettingsBase* InSettings) override;
    virtual void OnReleaseSource(const uint32 SourceId) override;
    virtual void ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData) override;

private:
    TArray<FUERayTracingAudioOcclusionSource> Sources;
    TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry;
    TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> IndirectAudioBridge;
    FVector2f CrossoversHz;
    FAudioDevice* OwningDevice = nullptr;
    int32 SampleRate = 48000;
    bool bShutdown = false;
};

class FUERayTracingAudioOcclusionPluginFactory : public IAudioOcclusionFactory
{
public:
    explicit FUERayTracingAudioOcclusionPluginFactory(
        const FVector2f& InCrossoversHz);
    virtual FString GetDisplayName() override;
    virtual bool SupportsPlatform(const FString& PlatformName) override;
    virtual UClass* GetCustomOcclusionSettingsClass() const override;
    virtual TAudioOcclusionPtr CreateNewOcclusionPlugin(FAudioDevice* OwningDevice) override;

private:
    FVector2f CrossoversHz;
};
