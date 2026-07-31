#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Simulation/UERayTracingAudioSimulator.h"
#include "Audio/UERayTracingAudioConvolution.h"
#include "Audio/UERayTracingAudioAudioDiagnostics.h"

#include <atomic>

struct UERAYTRACINGAUDIO_API FUERayTracingAudioSimulationSnapshot
{
    FUERayTracingAudioDirectSimulationResult DirectResult;
    FUERayTracingAudioIndirectSimulationResult IndirectResult;
    FUERayTracingAudioConvolutionKernel::FKernelPtr BakedConvolutionKernel;
    FUERayTracingAudioConvolutionKernel::FKernelPtr BakedConvolutionKernelRight;
    FUERayTracingAudioConvolutionKernel::FKernelPtr RealtimeConvolutionKernelLeft;
    FUERayTracingAudioConvolutionKernel::FKernelPtr RealtimeConvolutionKernelRight;
    EUERayTracingAudioRuntimeDataSource DataSource =
        EUERayTracingAudioRuntimeDataSource::Realtime;
    float IndirectMix = 0.0f;
    float IndirectDurationSeconds = 1.0f;
    FUERayTracingAudioConvolutionRevisions ConvolutionRevisions;
    // Aggregate revision retained for compatibility with snapshots authored by
    // older tests/tools. Production audio rendering uses the lane revisions.
    uint64 ConvolutionRevision = 0;
    uint64 Generation = 0;
};

class UERAYTRACINGAUDIO_API FUERayTracingAudioSimulationSnapshotRegistry
{
public:
    class FSnapshotReadHandle
    {
    public:
        FSnapshotReadHandle() = default;
        ~FSnapshotReadHandle();
        FSnapshotReadHandle(FSnapshotReadHandle&& Other) noexcept;
        FSnapshotReadHandle& operator=(FSnapshotReadHandle&& Other) noexcept;

        FSnapshotReadHandle(const FSnapshotReadHandle&) = delete;
        FSnapshotReadHandle& operator=(const FSnapshotReadHandle&) = delete;

        bool IsValid() const;
        explicit operator bool() const;
        const FUERayTracingAudioSimulationSnapshot* Get() const;
        const FUERayTracingAudioSimulationSnapshot* operator->() const;

    private:
        friend class FUERayTracingAudioSimulationSnapshotRegistry;

        FSnapshotReadHandle(
            const FUERayTracingAudioSimulationSnapshot* InSnapshot,
            std::atomic<uint32>* InReaderCount);
        void Release();

        const FUERayTracingAudioSimulationSnapshot* Snapshot = nullptr;
        std::atomic<uint32>* ReaderCount = nullptr;
    };

    using FSnapshotPtr = FSnapshotReadHandle;

    FUERayTracingAudioSimulationSnapshotRegistry();
    ~FUERayTracingAudioSimulationSnapshotRegistry();

    FUERayTracingAudioSimulationSnapshotRegistry(
        const FUERayTracingAudioSimulationSnapshotRegistry&) = delete;
    FUERayTracingAudioSimulationSnapshotRegistry& operator=(
        const FUERayTracingAudioSimulationSnapshotRegistry&) = delete;

    bool Publish(uint64 AudioComponentId, FUERayTracingAudioSimulationSnapshot&& Snapshot);
    FSnapshotPtr Read(uint64 AudioComponentId) const;
    void Remove(uint64 AudioComponentId);
    void Reset();
    uint64 GetDroppedPublishCount() const;
    uint64 GetReadRetryCount() const;

#if WITH_DEV_AUTOMATION_TESTS
    bool IsReaderPinningLockFreeForTesting() const;
#endif

private:
    struct FEntry;

    int32 FindEntry(uint64 AudioComponentId) const;
    int32 FindOrReserveEntry(uint64 AudioComponentId);

    static constexpr int32 EntryCapacity = 1024;
    static constexpr int32 SnapshotSlotsPerEntry = 3;
    static constexpr int32 MaxReadAttempts = 8;

    TUniquePtr<FEntry[]> Entries;
    FCriticalSection WriterLock;
    mutable std::atomic<uint64> ReadRetryCount { 0 };
    std::atomic<uint64> DroppedPublishCount { 0 };
};
