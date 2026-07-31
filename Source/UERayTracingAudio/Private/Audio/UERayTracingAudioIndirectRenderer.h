#pragma once

#include "CoreMinimal.h"
#include "Audio/UERayTracingAudioConvolution.h"

struct FUERayTracingAudioIndirectSimulationResult;
struct FUERayTracingAudioSimulationSnapshot;
class FUERayTracingAudioIndirectAudioBridge;

class FUERayTracingAudioEarlyReflectionRenderer
{
public:
    void ReleasePreparedStates(
        FUERayTracingAudioIndirectAudioBridge& Bridge,
        int32 SourceId);
    void ConfigurePrepared(
        FUERayTracingAudioIndirectAudioBridge& Bridge,
        int32 SourceId,
        uint64 AudioComponentId,
        const FUERayTracingAudioSimulationSnapshot* Snapshot,
        int32 SampleRate);

    FVector2f ProcessSample(float MonoInput);
    bool HasOutput() const;

private:
    FUERayTracingAudioPreparedCrossfadingConvolver
        BakedLeftConvolver;
    FUERayTracingAudioPreparedCrossfadingConvolver
        BakedRightConvolver;
    FUERayTracingAudioPreparedCrossfadingConvolver
        RealtimeLeftConvolver;
    FUERayTracingAudioPreparedCrossfadingConvolver
        RealtimeRightConvolver;
};

class FUERayTracingAudioLateReverbRenderer
{
public:
    void Initialize(int32 InSampleRate, float MaxPreDelaySeconds);
    void Reset(int32 InSampleRate);
    void Configure(
        const FUERayTracingAudioIndirectSimulationResult* IndirectResult,
        float DurationSeconds);
    float ProcessSample(float MonoInput);
    bool HasOutput() const;
    uint64 GetCapacityOverflowCount() const;

#if WITH_DEV_AUTOMATION_TESTS
    int32 GetDelayCapacityForTesting() const;
#endif

private:
    float ReadDelayedSample(int32 DelaySamples) const;
    void ClearCachedState(bool bClearCombBuffers);

    int32 SampleRate = 48000;
    int32 DelayWriteIndex = 0;
    int32 DelaySamplesWritten = 0;
    TArray<float> DelayBuffer;
    TArray<TArray<float>> CombBuffers;
    int32 CombWriteIndices[3] = { 0, 0, 0 };
    int32 CombSamplesWritten[3] = { 0, 0, 0 };
    bool bCapacityPrepared = false;
    bool bCapacityOverflowActive = false;
    TAtomic<uint64> CapacityOverflowCount { 0 };

    bool bHasCurrentPaths = false;
    bool bUseCurrentParametricTail = false;
    float CurrentLateReverbGain = 0.0f;
    float CurrentParametricDelaySeconds = 0.0f;
    FVector CurrentReverbTimes = FVector::ZeroVector;
    FVector CurrentParametricEq = FVector::OneVector;

    bool bHasCachedReverbState = false;
    float CachedLateReverbGain = 0.0f;
    float CachedParametricDelaySeconds = 0.0f;
    FVector CachedReverbTimes = FVector::ZeroVector;
    FVector CachedParametricEq = FVector::OneVector;
    int32 CachedReverbTailSamplesRemaining = 0;
};

class FUERayTracingAudioIndirectRenderer
{
public:
    void Initialize(int32 InSampleRate, float MaxPreDelaySeconds);
    void Reset(int32 InSampleRate);
    void ReleasePreparedStates(
        FUERayTracingAudioIndirectAudioBridge& Bridge,
        int32 SourceId);
    void ConfigurePrepared(
        FUERayTracingAudioIndirectAudioBridge& Bridge,
        int32 SourceId,
        uint64 AudioComponentId,
        const FUERayTracingAudioSimulationSnapshot* Snapshot);

    FVector2f ProcessSample(float MonoInput);
    bool HasOutput() const;

private:
    void ConfigureAcousticMix(
        const FUERayTracingAudioSimulationSnapshot* Snapshot);

    static constexpr int32 MixCrossfadeSamples = 2048;

    int32 SampleRate = 48000;
    float IndirectMix = 0.0f;
    float IndirectMixRampStart = 0.0f;
    float TargetIndirectMix = 0.0f;
    int32 IndirectMixRampSamplesRemaining = 0;
    FUERayTracingAudioEarlyReflectionRenderer EarlyReflections;
    FUERayTracingAudioLateReverbRenderer LateReverb;
};
