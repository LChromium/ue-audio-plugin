#pragma once

#include "CoreMinimal.h"
#include "Sound/SoundWaveProcedural.h"
#include "UERayTracingAudioValidationSoundWave.generated.h"

struct FUERayTracingAudioValidationGenerationStats;

UCLASS()
class UUERayTracingAudioValidationSoundProxy final : public USoundBase
{
    GENERATED_BODY()

public:
    void Initialize(USoundWave& InSourceSoundWave);
    virtual bool IsPlayable() const override;
    virtual float GetDuration() const override;
    virtual bool GetSoundWavesWithCookedAnalysisData(
        TArray<USoundWave*>& OutSoundWaves) override;
    virtual void Parse(
        class FAudioDevice* AudioDevice,
        UPTRINT NodeWaveInstanceHash,
        FActiveSound& ActiveSound,
        const FSoundParseParameters& ParseParams,
        TArray<FWaveInstance*>& WaveInstances) override;
    uint64 GetParseCount() const;
    uint64 GetParsedWaveCount() const;
    uint64 GetAudioLinkOverrideCount() const;
    float GetMaxActualVolume() const;
    float GetLastVolume() const;
    float GetLastVolumeMultiplier() const;
    float GetLastDistanceAttenuation() const;
    float GetLastOcclusionAttenuation() const;

private:
    UPROPERTY(Transient)
    TObjectPtr<USoundWave> SourceSoundWave;
    TAtomic<uint64> ParseCount { 0 };
    TAtomic<uint64> ParsedWaveCount { 0 };
    TAtomic<uint64> AudioLinkOverrideCount { 0 };
    float MaxActualVolume = 0.0f;
    float LastVolume = 0.0f;
    float LastVolumeMultiplier = 0.0f;
    float LastDistanceAttenuation = 0.0f;
    float LastOcclusionAttenuation = 0.0f;
};

UCLASS()
class UUERayTracingAudioValidationSoundWave final : public USoundWaveProcedural
{
    GENERATED_BODY()

public:
    void InitializeSamples(TArray<float>&& InSamples, int32 NumPrebufferSamples);
    virtual void Parse(
        class FAudioDevice* AudioDevice,
        UPTRINT NodeWaveInstanceHash,
        FActiveSound& ActiveSound,
        const FSoundParseParameters& ParseParams,
        TArray<FWaveInstance*>& WaveInstances) override;
    virtual ISoundGeneratorPtr CreateSoundGenerator(
        const FSoundGeneratorInitParams& InParams) override;
    virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
    virtual Audio::EAudioMixerStreamDataFormat::Type GetGeneratedPCMDataFormat() const override;
    uint64 GetGeneratorCallbackCount() const;
    uint64 GetNonSilentGeneratorCallbackCount() const;

private:
    TArray<float> Samples;
    TAtomic<int32> GeneratedSampleCount { 0 };
    TSharedPtr<FUERayTracingAudioValidationGenerationStats, ESPMode::ThreadSafe>
        GenerationStats;
};
