#include "Validation/UERayTracingAudioValidationSoundWave.h"

#include "ActiveSound.h"
#include "Audio.h"
#include "Sound/SoundGenerator.h"

struct FUERayTracingAudioValidationGenerationStats
{
    TAtomic<uint64> CallbackCount { 0 };
    TAtomic<uint64> NonSilentCallbackCount { 0 };
};

namespace
{
    class FUERayTracingAudioValidationSoundGenerator final : public ISoundGenerator
    {
    public:
        explicit FUERayTracingAudioValidationSoundGenerator(
            const TArray<float>& InSamples,
            const int32 InDesiredSampleCount,
            TSharedRef<
                FUERayTracingAudioValidationGenerationStats,
                ESPMode::ThreadSafe> InStats)
            : Samples(InSamples)
            , DesiredSampleCount(FMath::Max(InDesiredSampleCount, 1))
            , Stats(MoveTemp(InStats))
        {
        }

        virtual int32 OnGenerateAudio(float* OutAudio, const int32 NumSamples) override
        {
            if (!OutAudio || NumSamples <= 0 || Samples.IsEmpty())
            {
                return 0;
            }

            ++Stats->CallbackCount;
            int32 SamplesCopied = 0;
            float PeakAbsoluteSample = 0.0f;
            while (SamplesCopied < NumSamples)
            {
                const int32 ChunkSampleCount = FMath::Min(
                    NumSamples - SamplesCopied,
                    Samples.Num() - SourceSampleIndex);
                FMemory::Memcpy(
                    OutAudio + SamplesCopied,
                    Samples.GetData() + SourceSampleIndex,
                    ChunkSampleCount * sizeof(float));
                for (int32 ChunkSampleIndex = 0;
                    ChunkSampleIndex < ChunkSampleCount;
                    ++ChunkSampleIndex)
                {
                    const float Sample =
                        Samples[SourceSampleIndex + ChunkSampleIndex];
                    if (FMath::IsFinite(Sample))
                    {
                        PeakAbsoluteSample = FMath::Max(
                            PeakAbsoluteSample,
                            FMath::Abs(Sample));
                    }
                }
                SamplesCopied += ChunkSampleCount;
                SourceSampleIndex =
                    (SourceSampleIndex + ChunkSampleCount) % Samples.Num();
            }
            if (PeakAbsoluteSample > 1.0e-8f)
            {
                ++Stats->NonSilentCallbackCount;
            }
            return NumSamples;
        }

        virtual int32 GetDesiredNumSamplesToRenderPerCallback() const override
        {
            return DesiredSampleCount;
        }

    private:
        TArray<float> Samples;
        int32 DesiredSampleCount = 1;
        TSharedRef<
            FUERayTracingAudioValidationGenerationStats,
            ESPMode::ThreadSafe> Stats;
        int32 SourceSampleIndex = 0;
    };
}

void UUERayTracingAudioValidationSoundProxy::Initialize(
    USoundWave& InSourceSoundWave)
{
    SourceSoundWave = &InSourceSoundWave;
    bEnableBaseSubmix = true;
}

bool UUERayTracingAudioValidationSoundProxy::IsPlayable() const
{
    return IsValid(SourceSoundWave) && SourceSoundWave->IsPlayable();
}

float UUERayTracingAudioValidationSoundProxy::GetDuration() const
{
    return IsValid(SourceSoundWave) ? SourceSoundWave->GetDuration() : 0.0f;
}

bool UUERayTracingAudioValidationSoundProxy::GetSoundWavesWithCookedAnalysisData(
    TArray<USoundWave*>& OutSoundWaves)
{
    return IsValid(SourceSoundWave)
        && SourceSoundWave->GetSoundWavesWithCookedAnalysisData(OutSoundWaves);
}

void UUERayTracingAudioValidationSoundProxy::Parse(
    FAudioDevice* AudioDevice,
    const UPTRINT NodeWaveInstanceHash,
    FActiveSound& ActiveSound,
    const FSoundParseParameters& ParseParams,
    TArray<FWaveInstance*>& WaveInstances)
{
    if (!IsValid(SourceSoundWave))
    {
        return;
    }

    FSoundParseParameters MixerSafeParseParams(ParseParams);
    MixerSafeParseParams.bEnableSendToAudioLink = false;
    const int32 FirstParsedWaveIndex = WaveInstances.Num();
    SourceSoundWave->Parse(
        AudioDevice,
        NodeWaveInstanceHash,
        ActiveSound,
        MixerSafeParseParams,
        WaveInstances);
    ++ParseCount;
    for (int32 WaveIndex = FirstParsedWaveIndex;
        WaveIndex < WaveInstances.Num();
        ++WaveIndex)
    {
        FWaveInstance* WaveInstance = WaveInstances[WaveIndex];
        if (!WaveInstance)
        {
            continue;
        }
        ++ParsedWaveCount;
        if (WaveInstance->bShouldUseAudioLink)
        {
            ++AudioLinkOverrideCount;
        }
        WaveInstance->bShouldUseAudioLink = false;
        MaxActualVolume = FMath::Max(
            MaxActualVolume,
            WaveInstance->GetActualVolume());
        LastVolume = WaveInstance->GetVolume();
        LastVolumeMultiplier = WaveInstance->GetVolumeMultiplier();
        LastDistanceAttenuation = WaveInstance->GetDistanceAttenuation();
        LastOcclusionAttenuation = WaveInstance->GetOcclusionAttenuation();
    }
}

uint64 UUERayTracingAudioValidationSoundProxy::GetParseCount() const
{
    return ParseCount.Load();
}

uint64 UUERayTracingAudioValidationSoundProxy::GetParsedWaveCount() const
{
    return ParsedWaveCount.Load();
}

uint64 UUERayTracingAudioValidationSoundProxy::GetAudioLinkOverrideCount() const
{
    return AudioLinkOverrideCount.Load();
}

float UUERayTracingAudioValidationSoundProxy::GetMaxActualVolume() const
{
    return MaxActualVolume;
}

float UUERayTracingAudioValidationSoundProxy::GetLastVolume() const
{
    return LastVolume;
}

float UUERayTracingAudioValidationSoundProxy::GetLastVolumeMultiplier() const
{
    return LastVolumeMultiplier;
}

float UUERayTracingAudioValidationSoundProxy::GetLastDistanceAttenuation() const
{
    return LastDistanceAttenuation;
}

float UUERayTracingAudioValidationSoundProxy::GetLastOcclusionAttenuation() const
{
    return LastOcclusionAttenuation;
}

void UUERayTracingAudioValidationSoundWave::InitializeSamples(
    TArray<float>&& InSamples,
    const int32)
{
    Samples = MoveTemp(InSamples);
    GeneratedSampleCount.Store(0);
    GenerationStats = MakeShared<
        FUERayTracingAudioValidationGenerationStats,
        ESPMode::ThreadSafe>();
}

void UUERayTracingAudioValidationSoundWave::Parse(
    FAudioDevice* AudioDevice,
    const UPTRINT NodeWaveInstanceHash,
    FActiveSound& ActiveSound,
    const FSoundParseParameters& ParseParams,
    TArray<FWaveInstance*>& WaveInstances)
{
    // UE 5.7 leaves this bit uninitialized in FSoundParseParameters. The
    // attenuation path combines it with |=, so a false override alone cannot
    // reliably prevent AudioLink from consuming and zeroing the source buffer.
    FSoundParseParameters MixerSafeParseParams(ParseParams);
    MixerSafeParseParams.bEnableSendToAudioLink = false;
    Super::Parse(
        AudioDevice,
        NodeWaveInstanceHash,
        ActiveSound,
        MixerSafeParseParams,
        WaveInstances);
}

ISoundGeneratorPtr UUERayTracingAudioValidationSoundWave::CreateSoundGenerator(
    const FSoundGeneratorInitParams& InParams)
{
    if (Samples.IsEmpty() || !GenerationStats.IsValid())
    {
        return nullptr;
    }
    return MakeShared<
        FUERayTracingAudioValidationSoundGenerator,
        ESPMode::ThreadSafe>(
            Samples,
            FMath::Max(InParams.NumFramesPerCallback, 1)
                * FMath::Max(InParams.NumChannels, 1),
            GenerationStats.ToSharedRef());
}

int32 UUERayTracingAudioValidationSoundWave::OnGeneratePCMAudio(
    TArray<uint8>& OutAudio,
    const int32 NumSamples)
{
    if (NumSamples <= 0 || Samples.IsEmpty())
    {
        OutAudio.Reset();
        return 0;
    }

    OutAudio.SetNumUninitialized(NumSamples * sizeof(float));
    float* Destination = reinterpret_cast<float*>(OutAudio.GetData());
    int32 SourceSample = GeneratedSampleCount.Load() % Samples.Num();
    int32 SamplesCopied = 0;
    while (SamplesCopied < NumSamples)
    {
        const int32 ChunkSampleCount = FMath::Min(
            NumSamples - SamplesCopied,
            Samples.Num() - SourceSample);
        FMemory::Memcpy(
            Destination + SamplesCopied,
            Samples.GetData() + SourceSample,
            ChunkSampleCount * sizeof(float));
        SamplesCopied += ChunkSampleCount;
        SourceSample = (SourceSample + ChunkSampleCount) % Samples.Num();
    }

    GeneratedSampleCount.Store(SourceSample);
    return NumSamples;
}

Audio::EAudioMixerStreamDataFormat::Type
UUERayTracingAudioValidationSoundWave::GetGeneratedPCMDataFormat() const
{
    return Audio::EAudioMixerStreamDataFormat::Float;
}

uint64 UUERayTracingAudioValidationSoundWave::GetGeneratorCallbackCount() const
{
    return GenerationStats.IsValid()
        ? GenerationStats->CallbackCount.Load()
        : 0;
}

uint64 UUERayTracingAudioValidationSoundWave::GetNonSilentGeneratorCallbackCount() const
{
    return GenerationStats.IsValid()
        ? GenerationStats->NonSilentCallbackCount.Load()
        : 0;
}
