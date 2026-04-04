#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"
#include "Simulation/UERayTracingAudioSimulator.h"

class UUERayTracingAudioOcclusionSettings;

struct FUERayTracingAudioOcclusionSource
{
    bool bApplyDistanceAttenuation = true;
    bool bApplyAirAbsorption = true;
    bool bApplyOcclusion = true;
    float PreviousGain = 1.0f;
    int32 NumChannels = 0;
    int32 SampleRate = 48000;
    int32 DelayWriteIndex = 0;
    TArray<float> DelayBuffer;
    TArray<TArray<float>> CombBuffers;
    TArray<int32> CombWriteIndices;
};

class FUERayTracingAudioOcclusionPlugin : public IAudioOcclusion
{
public:
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
    int32 SampleRate = 48000;
};

class FUERayTracingAudioOcclusionPluginFactory : public IAudioOcclusionFactory
{
public:
    virtual FString GetDisplayName() override;
    virtual bool SupportsPlatform(const FString& PlatformName) override;
    virtual UClass* GetCustomOcclusionSettingsClass() const override;
    virtual TAudioOcclusionPtr CreateNewOcclusionPlugin(FAudioDevice* OwningDevice) override;
};
