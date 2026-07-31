#pragma once

#include "CoreMinimal.h"

namespace UERayTracingAudioConvolutionLimits
{
    // Runtime convolution is deliberately bounded. A full baked IR remains in
    // its asset/offline output; samples beyond this early window are rendered
    // by the parameterized late-reverb path.
    inline constexpr int32 RuntimeBlockSize = 1024;
    inline constexpr int32 MaxRuntimePartitionsPerLane = 4;
    inline constexpr int32 MaxRuntimeImpulseSamples =
        RuntimeBlockSize * MaxRuntimePartitionsPerLane;
}

// A kernel lane changes independently from the other three lanes. Keeping a
// separate stable revision for every lane prevents a frequently refreshed
// realtime IR from rebuilding an unchanged (and often much larger) baked IR.
struct UERAYTRACINGAUDIO_API FUERayTracingAudioConvolutionRevisions
{
    uint64 BakedLeft = 0;
    uint64 BakedRight = 0;
    uint64 RealtimeLeft = 0;
    uint64 RealtimeRight = 0;
};

struct FUERayTracingAudioComplexSample
{
    float Real = 0.0f;
    float Imag = 0.0f;

    FUERayTracingAudioComplexSample operator+(const FUERayTracingAudioComplexSample& Other) const;
    FUERayTracingAudioComplexSample operator-(const FUERayTracingAudioComplexSample& Other) const;
    FUERayTracingAudioComplexSample operator*(const FUERayTracingAudioComplexSample& Other) const;
    FUERayTracingAudioComplexSample& operator+=(const FUERayTracingAudioComplexSample& Other);
};

class UERAYTRACINGAUDIO_API FUERayTracingAudioConvolutionKernel
{
public:
    using FKernelPtr = TSharedPtr<const FUERayTracingAudioConvolutionKernel, ESPMode::ThreadSafe>;

    static FKernelPtr Build(
        const TArray<float>& ImpulseResponse,
        int32 SampleRate,
        int32 BlockSize =
            UERayTracingAudioConvolutionLimits::RuntimeBlockSize);

    int32 GetSampleRate() const;
    int32 GetBlockSize() const;
    int32 GetNumPartitions() const;
    float GetDurationSeconds() const;
    float GetOriginalDurationSeconds() const;
    bool WasRuntimeTailTruncated() const;

private:
    friend class FUERayTracingAudioPartitionedConvolver;

    int32 SampleRate = 0;
    int32 BlockSize = 0;
    int32 NumImpulseSamples = 0;
    int32 OriginalNumImpulseSamples = 0;
    bool bRuntimeTailTruncated = false;
    TArray<TArray<FUERayTracingAudioComplexSample>> FrequencyPartitions;
};

class UERAYTRACINGAUDIO_API FUERayTracingAudioPartitionedConvolver
{
public:
    void SetKernel(FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel);
    float ProcessSample(float InputSample);
    void Reset();
    bool HasOutput() const;
    int32 GetBlockSize() const;
    uint64 GetAllocatedBytes() const;

#if WITH_DEV_AUTOMATION_TESTS
    uint64 GetStorageFingerprintForTesting() const;
#endif

private:
    void ProcessBlock();

    FUERayTracingAudioConvolutionKernel::FKernelPtr Kernel;
    TArray<TArray<FUERayTracingAudioComplexSample>> InputHistory;
    TArray<FUERayTracingAudioComplexSample> FrequencyOutput;
    TArray<float> InputBlock;
    TArray<float> OutputBlock;
    TArray<float> Overlap;
    int32 InputFill = 0;
    int32 OutputReadIndex = 0;
    int32 HistoryWriteIndex = 0;
};

// Owns every allocation and kernel reference required by one convolution
// stream. Prepare is a control-thread operation. Once published, audio code
// only calls ProcessSample and passes the stable raw address between bounded
// mailboxes; it never copies, moves, clears, or destroys this object.
class UERAYTRACINGAUDIO_API FUERayTracingAudioPreparedConvolverState
{
public:
    FUERayTracingAudioPreparedConvolverState() = default;
    ~FUERayTracingAudioPreparedConvolverState() = default;

    FUERayTracingAudioPreparedConvolverState(
        const FUERayTracingAudioPreparedConvolverState&) = delete;
    FUERayTracingAudioPreparedConvolverState& operator=(
        const FUERayTracingAudioPreparedConvolverState&) = delete;
    FUERayTracingAudioPreparedConvolverState(
        FUERayTracingAudioPreparedConvolverState&&) = delete;
    FUERayTracingAudioPreparedConvolverState& operator=(
        FUERayTracingAudioPreparedConvolverState&&) = delete;

    void Prepare(
        FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel,
        uint64 InRevision,
        uint64 InOwnerKey = 0);
    float ProcessSample(float InputSample);
    bool HasOutput() const;
    int32 GetBlockSize() const;
    uint64 GetRevision() const;
    uint64 GetOwnerKey() const;
    uint64 GetAllocatedBytes() const;

#if WITH_DEV_AUTOMATION_TESTS
    uint64 GetStorageFingerprintForTesting() const;
#endif

private:
    FUERayTracingAudioPartitionedConvolver Convolver;
    uint64 Revision = 0;
    uint64 OwnerKey = 0;
};

// Audio-thread-only lease switcher. It deliberately stores raw pointers: the
// device runtime owns the prepared states and reclaims detached/retired leases
// on the game thread. No method here allocates or changes a TSharedPtr count.
class UERAYTRACINGAUDIO_API FUERayTracingAudioPreparedCrossfadingConvolver
{
public:
    bool CanAcceptTransition() const;
    bool AdoptPreparedState(
        FUERayTracingAudioPreparedConvolverState* InState,
        int32 InCrossfadeSamples = 2048);
    bool AdoptSilence(
        uint64 InRevision,
        uint64 InOwnerKey,
        int32 InCrossfadeSamples = 2048);
    float ProcessSample(float InputSample);
    FUERayTracingAudioPreparedConvolverState* PeekRetiredState() const;
    FUERayTracingAudioPreparedConvolverState* TakeRetiredState();
    int32 DetachAllStates(
        FUERayTracingAudioPreparedConvolverState** OutStates,
        int32 MaxOutStates);
    bool HasOutput() const;
    uint64 GetCurrentRevision() const;
    uint64 GetCurrentOwnerKey() const;

private:
    bool StartTransition(
        FUERayTracingAudioPreparedConvolverState* InState,
        uint64 InRevision,
        uint64 InOwnerKey,
        int32 InCrossfadeSamples);

    FUERayTracingAudioPreparedConvolverState* Current = nullptr;
    FUERayTracingAudioPreparedConvolverState* Previous = nullptr;
    FUERayTracingAudioPreparedConvolverState* Retired = nullptr;
    uint64 CurrentRevision = 0;
    uint64 CurrentOwnerKey = 0;
    int32 CrossfadeSamples = 0;
    int32 CrossfadeSamplesRemaining = 0;
    int32 WarmupSamplesRemaining = 0;
};

class UERAYTRACINGAUDIO_API FUERayTracingAudioCrossfadingConvolver
{
public:
    void SetKernel(
        FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel,
        int32 InCrossfadeSamples = 2048);
    float ProcessSample(float InputSample);
    void Reset();
    bool HasOutput() const;

private:
    void StartTransition(
        FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel,
        int32 InCrossfadeSamples);
    void StartPendingTransition();

    FUERayTracingAudioConvolutionKernel::FKernelPtr CurrentKernel;
    FUERayTracingAudioConvolutionKernel::FKernelPtr PreviousKernel;
    FUERayTracingAudioConvolutionKernel::FKernelPtr PendingKernel;
    FUERayTracingAudioPartitionedConvolver Current;
    FUERayTracingAudioPartitionedConvolver Previous;
    int32 CrossfadeSamples = 0;
    int32 CrossfadeSamplesRemaining = 0;
    int32 WarmupSamplesRemaining = 0;
    int32 PendingCrossfadeSamples = 0;
    bool bHasPendingKernel = false;
};
