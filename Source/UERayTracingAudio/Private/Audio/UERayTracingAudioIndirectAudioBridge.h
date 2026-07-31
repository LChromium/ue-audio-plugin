#pragma once

#include "CoreMinimal.h"
#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioConvolution.h"

enum class EUERayTracingAudioConvolutionLane : uint8
{
    BakedLeft = 0,
    BakedRight,
    RealtimeLeft,
    RealtimeRight,
    Count
};

enum class EUERayTracingAudioConvolverConfigureResult : uint8
{
    InvalidSource = 0,
    Unchanged,
    Requested,
    Adopted,
    Silenced,
    Busy,
    ReturnQueueFull
};

class FUERayTracingAudioIndirectAudioBridge
{
public:
    FUERayTracingAudioIndirectAudioBridge();
    ~FUERayTracingAudioIndirectAudioBridge();

    FUERayTracingAudioIndirectAudioBridge(
        const FUERayTracingAudioIndirectAudioBridge&) = delete;
    FUERayTracingAudioIndirectAudioBridge& operator=(
        const FUERayTracingAudioIndirectAudioBridge&) = delete;

    void Initialize(
        int32 NumSources,
        int32 MaxFramesPerCallback,
        int32 SampleRate = 48000);
    TArrayView<FVector2f> BeginWrite(int32 SourceId, uint64 AudioComponentId, int32 NumFrames);
    TArrayView<FVector2f> BeginWrite(
        int32 SourceId,
        uint64 AudioComponentId,
        int32 NumFrames,
        EUERayTracingAudioRuntimeDataSource DataSource);
    void EndWrite(int32 SourceId, uint64 AudioComponentId);
    bool Consume(
        int32 SourceId,
        uint64 AudioComponentId,
        TArrayView<const FVector2f>& OutStereoWet);
    bool Consume(
        int32 SourceId,
        uint64 AudioComponentId,
        TArrayView<const FVector2f>& OutStereoWet,
        bool& bOutHasDataSource,
        EUERayTracingAudioRuntimeDataSource& OutDataSource);
    void ClearSource(int32 SourceId);
    uint64 GetCapacityOverflowCount() const;

    void PublishConvolutionTargets(
        uint64 AudioComponentId,
        const FUERayTracingAudioConvolutionRevisions& Revisions,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& BakedLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& BakedRight,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& RealtimeLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& RealtimeRight);
    void PublishConvolutionTargets(
        uint64 AudioComponentId,
        uint64 Revision,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& BakedLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& BakedRight,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& RealtimeLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& RealtimeRight);
    void RemoveConvolutionTargets(uint64 AudioComponentId);
    void ServiceConvolutionGameThread(int32 PrepareBudget = 8);
    EUERayTracingAudioConvolverConfigureResult ConfigureConvolver(
        int32 SourceId,
        uint64 AudioComponentId,
        uint64 Revision,
        bool bHasKernel,
        EUERayTracingAudioConvolutionLane Lane,
        FUERayTracingAudioPreparedCrossfadingConvolver& Convolver,
        int32 CrossfadeSamples = 2048);
    bool ReleaseConvolver(
        int32 SourceId,
        EUERayTracingAudioConvolutionLane Lane,
        FUERayTracingAudioPreparedCrossfadingConvolver& Convolver);
    uint64 GetPreparedWorkspaceBytes() const;
    uint64 GetConvolutionReturnOverflowCount() const;
    uint64 GetConvolutionPrepareCapacityDropCount() const;

#if WITH_DEV_AUTOMATION_TESTS
    int32 GetSourceCapacityForTesting(int32 SourceId) const;
    int32 GetPreparedStateCountForTesting() const;
#endif

private:
    TArrayView<FVector2f> BeginWriteInternal(
        int32 SourceId,
        uint64 AudioComponentId,
        int32 NumFrames,
        bool bHasDataSource,
        EUERayTracingAudioRuntimeDataSource DataSource);

    struct FSourceState
    {
        uint64 AudioComponentId = 0;
        TArray<FVector2f> StereoWet;
        bool bWriting = false;
        bool bReady = false;
        bool bHasDataSource = false;
        EUERayTracingAudioRuntimeDataSource DataSource =
            EUERayTracingAudioRuntimeDataSource::Realtime;
    };

    TArray<FSourceState> Sources;
    int32 MaxFramesPerCallback = 0;
    TAtomic<uint64> CapacityOverflowCount { 0 };

    struct FConvolutionRuntime;
    TUniquePtr<FConvolutionRuntime> ConvolutionRuntime;
};
