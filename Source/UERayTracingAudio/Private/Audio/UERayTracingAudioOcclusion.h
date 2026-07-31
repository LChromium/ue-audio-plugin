#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"
#include "Audio/UERayTracingAudioThreeBandAirAbsorption.h"
#include "Simulation/UERayTracingAudioSimulator.h"

class UUERayTracingAudioOcclusionSettings;
class FUERayTracingAudioSimulationSnapshotRegistry;
class FUERayTracingAudioIndirectAudioBridge;

struct FUERayTracingAudioOcclusionSource
{
    bool bApplyDistanceAttenuation = true;
    bool bApplyAirAbsorption = true;
    bool bApplyOcclusion = true;
    FVector PreviousBandGains = FVector::OneVector;
    int32 NumChannels = 0;
    int32 SampleRate = 48000;
    int32 DelayWriteIndex = 0;
    FUERayTracingAudioThreeBandAirAbsorption AirAbsorptionProcessor;
    TArray<float> DelayBuffer;
    TArray<TArray<float>> CombBuffers;
    TArray<int32> CombWriteIndices;
};

class FUERayTracingAudioOcclusionPlugin : public IAudioOcclusion
{
public:
    FUERayTracingAudioOcclusionPlugin(
        TSharedRef<
            FUERayTracingAudioSimulationSnapshotRegistry,
            ESPMode::ThreadSafe> InSnapshotRegistry,
        TSharedRef<
            FUERayTracingAudioIndirectAudioBridge,
            ESPMode::ThreadSafe> InIndirectAudioBridge,
        const FVector2f& InCrossoversHz,
        FAudioDevice* InOwningDevice = nullptr);
    explicit FUERayTracingAudioOcclusionPlugin(
        const FVector2f& InCrossoversHz,
        FAudioDevice* InOwningDevice = nullptr);
    virtual void Initialize(const FAudioPluginInitializationParams InitializationParams) override;
    virtual void OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, const uint32 NumChannels, UOcclusionPluginSourceSettingsBase* InSettings) override;
    virtual void OnReleaseSource(const uint32 SourceId) override;
    virtual void ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData) override;

private:
    void EnsureDelayCapacity(FUERayTracingAudioOcclusionSource& SourceState, float DurationSeconds);
    float ReadDelayedSample(const FUERayTracingAudioOcclusionSource& SourceState, int32 DelaySamples) const;
    float RenderIndirectSample(
        FUERayTracingAudioOcclusionSource& SourceState,
        const FUERayTracingAudioIndirectSimulationResult& IndirectResult,
        float MonoInput,
        float IndirectMix);

    TArray<FUERayTracingAudioOcclusionSource> Sources;
    FVector2f CrossoversHz;
    FAudioDevice* OwningDevice = nullptr;
    int32 SampleRate = 48000;
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
