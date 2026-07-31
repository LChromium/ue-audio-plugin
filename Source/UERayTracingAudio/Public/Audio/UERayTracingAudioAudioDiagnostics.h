#pragma once

#include "CoreMinimal.h"

enum class EUERayTracingAudioRuntimeDataSource : uint8
{
    Realtime = 0,
    Baked = 1,
    Hybrid = 2
};

struct FUERayTracingAudioDataSourceAudioStats
{
    uint64 BufferCount = 0;
    uint64 FrameCount = 0;
    uint64 NonSilentInputBufferCount = 0;
    uint64 NonSilentBufferCount = 0;
    uint64 RmsMeasuredBufferCount = 0;
    uint64 AudibleWetBufferCount = 0;
    uint64 MaxConsecutiveInaudibleWetBufferCount = 0;
    uint64 WetPresentInputBufferCount = 0;
    uint64 MaxConsecutiveSilentWetBufferCount = 0;
    uint64 NonFiniteSampleCount = 0;
    uint64 OverUnitOutputSampleCount = 0;
    float MaxInputRms = 0.0f;
    float MaxWetRms = 0.0f;
    float MaxOutputPeak = 0.0f;
    float MaxWetToInputRmsRatio = 0.0f;
    float IntegratedWetToInputRmsRatio = 0.0f;
};

struct FUERayTracingAudioHardRealtimeStats
{
    uint64 AudioCallbackCount = 0;
    uint64 CallbackCapacityMissCount = 0;
    uint64 ConvolutionPrepareCapacityDropCount = 0;
};

struct FUERayTracingAudioDirectAudioStats
{
    uint64 BufferCount = 0;
    uint64 NonSilentInputBufferCount = 0;
    uint64 DirectPresentInputBufferCount = 0;
    uint64 MaxConsecutiveSilentDirectBufferCount = 0;
    uint64 NonFiniteDirectSampleCount = 0;
    uint64 OverUnitDirectSampleCount = 0;
    float MaxBandGainStep = 0.0f;
};

// Lock-free counters written once per audio buffer. They intentionally avoid
// UObject access, logging, allocation, or console-variable mutation from the
// audio render thread. Reset publishes a new epoch; Read returns an empty
// snapshot until the first buffer in that epoch is recorded, and otherwise
// returns fields from one consistent completed audio-thread update.
class UERAYTRACINGAUDIO_API FUERayTracingAudioAudioDiagnostics
{
public:
    static constexpr float AudibleWetToInputRmsRatio = 0.05f;

    static void SetTargetAudioComponentId(uint64 AudioComponentId);
    static bool IsEnabledFor(uint64 AudioComponentId);
    static void Reset(EUERayTracingAudioRuntimeDataSource DataSource);
    static void RecordBuffer(
        EUERayTracingAudioRuntimeDataSource DataSource,
        uint64 AudioComponentId,
        int32 NumFrames,
        float PeakAbsoluteInput,
        float PeakAbsoluteWet,
        float InputRms,
        float WetRms,
        uint64 NonFiniteSampleCount,
        float PeakAbsolutePreSpatializationOutput = 0.0f,
        uint64 PreSpatializationOverUnitSampleCount = 0);
    static void RecordFinalOutput(
        EUERayTracingAudioRuntimeDataSource DataSource,
        uint64 AudioComponentId,
        float PeakAbsoluteOutput,
        uint64 OverUnitOutputSampleCount,
        uint64 NonFiniteOutputSampleCount);
    static FUERayTracingAudioDataSourceAudioStats Read(
        EUERayTracingAudioRuntimeDataSource DataSource);

    static void ResetDirect();
    static void RecordDirectBuffer(
        uint64 AudioComponentId,
        int32 NumFrames,
        float PeakAbsoluteInput,
        float DirectRms,
        float MaxBandGainStep,
        uint64 NonFiniteDirectSampleCount,
        uint64 OverUnitDirectSampleCount);
    static FUERayTracingAudioDirectAudioStats ReadDirect();

    // These counters cover dynamic resource pressure while the separate
    // pre-build source audit rejects locks, heap operations, shared ownership,
    // blocking calls, and UObject access in the callback call chain.
    static void ResetHardRealtime();
    static void RecordHardRealtimeCallback();
    static void RecordHardRealtimeCapacityMiss();
    static void RecordConvolutionPrepareCapacityDrop();
    static FUERayTracingAudioHardRealtimeStats ReadHardRealtime();
};
