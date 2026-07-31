#include "Audio/UERayTracingAudioIndirectAudioBridge.h"

#include <atomic>

namespace
{
    constexpr int32 ConvolutionLaneCount =
        static_cast<int32>(
            EUERayTracingAudioConvolutionLane::Count);
    constexpr int32 NormalReturnSlotsPerLane = 4;
    constexpr int32 TotalReturnSlotsPerLane = 8;
    constexpr uint8 AllConvolutionLaneMask =
        (1u << ConvolutionLaneCount) - 1u;
    constexpr uint64 MaxPreparedWorkspaceBytes =
        512ull * 1024ull * 1024ull;

    static_assert(
        std::atomic<
            FUERayTracingAudioPreparedConvolverState*>::is_always_lock_free,
        "Prepared convolver mailboxes must be lock-free.");
    static_assert(
        std::atomic<uint64>::is_always_lock_free,
        "Prepared convolver request metadata must be lock-free.");

    int32 ToLaneIndex(
        const EUERayTracingAudioConvolutionLane Lane)
    {
        const int32 LaneIndex = static_cast<int32>(Lane);
        return LaneIndex >= 0 && LaneIndex < ConvolutionLaneCount
            ? LaneIndex
            : INDEX_NONE;
    }

    uint64 GetLaneRevision(
        const FUERayTracingAudioConvolutionRevisions& Revisions,
        const int32 LaneIndex)
    {
        switch (static_cast<
            EUERayTracingAudioConvolutionLane>(LaneIndex))
        {
        case EUERayTracingAudioConvolutionLane::BakedLeft:
            return Revisions.BakedLeft;
        case EUERayTracingAudioConvolutionLane::BakedRight:
            return Revisions.BakedRight;
        case EUERayTracingAudioConvolutionLane::RealtimeLeft:
            return Revisions.RealtimeLeft;
        case EUERayTracingAudioConvolutionLane::RealtimeRight:
            return Revisions.RealtimeRight;
        default:
            return 0;
        }
    }

    uint64 EstimateWorkspaceBytes(
        const FUERayTracingAudioConvolutionKernel& Kernel)
    {
        const uint64 BlockSize =
            static_cast<uint64>(FMath::Max(
                Kernel.GetBlockSize(),
                0));
        const uint64 TransformSize = BlockSize * 2ull;
        const uint64 NumPartitions =
            static_cast<uint64>(FMath::Max(
                Kernel.GetNumPartitions(),
                0));
        return NumPartitions
                * TransformSize
                * sizeof(FUERayTracingAudioComplexSample)
            + TransformSize
                * sizeof(FUERayTracingAudioComplexSample)
            + BlockSize * 3ull * sizeof(float)
            + NumPartitions
                * sizeof(
                    TArray<FUERayTracingAudioComplexSample>);
    }
}

struct FUERayTracingAudioIndirectAudioBridge::FConvolutionRuntime
{
    struct FLaneState
    {
        std::atomic<uint64> RequestSequence { 0 };
        std::atomic<uint64> RequestedAudioComponentId { 0 };
        std::atomic<uint64> RequestedRevision { 0 };
        std::atomic<
            FUERayTracingAudioPreparedConvolverState*> Ready {
                nullptr
            };
        std::atomic<
            FUERayTracingAudioPreparedConvolverState*> Returned[
                TotalReturnSlotsPerLane];

        // Written only by the logical producer for this SourceId. UE
        // serializes callbacks for one source even when different sources are
        // processed by parallel render tasks.
        uint64 LastRequestedAudioComponentId = 0;
        uint64 LastRequestedRevision = 0;

        // Game-thread-only request cursor.
        uint64 LastServicedRequestSequence = 0;
    };

    struct FSourceState
    {
        FLaneState Lanes[ConvolutionLaneCount];
    };

    struct FTarget
    {
        uint64 Revisions[ConvolutionLaneCount] {};
        FUERayTracingAudioConvolutionKernel::FKernelPtr Kernels[
            ConvolutionLaneCount];
    };

    void Initialize(
        const int32 InNumSources,
        const int32 InSampleRate)
    {
        const int32 RequiredSources =
            FMath::Max(InNumSources, 0);
        const int32 RequiredSampleRate =
            FMath::Max(InSampleRate, 8000);
        if (SourceStates
            && NumSources == RequiredSources
            && SampleRate == RequiredSampleRate)
        {
            return;
        }

        NumSources = RequiredSources;
        SampleRate = RequiredSampleRate;
        SourceStates.Reset(
            NumSources > 0
                ? new FSourceState[NumSources]
                : nullptr);
        Targets.Reset();
        FreeStates.Reset();
        OwnedStates.Reset();
        const int64 RequestedMaximum =
            static_cast<int64>(NumSources)
            * static_cast<int64>(ConvolutionLaneCount)
            * 3ll;
        MaxPreparedStates = static_cast<int32>(
            FMath::Clamp<int64>(
                RequestedMaximum,
                12ll,
                4096ll));
        OwnedStates.Reserve(MaxPreparedStates);
        FreeStates.Reserve(MaxPreparedStates);
        NextServiceSlot = 0;
        PreparedWorkspaceBytes.store(
            0,
            std::memory_order_relaxed);
        ReturnOverflowCount.store(
            0,
            std::memory_order_relaxed);
        PrepareCapacityDropCount.store(
            0,
            std::memory_order_relaxed);
    }

    bool IsValidSource(const int32 SourceId) const
    {
        return SourceStates
            && SourceId >= 0
            && SourceId < NumSources;
    }

    void RecordReturnOverflowAudio()
    {
        ReturnOverflowCount.fetch_add(
            1,
            std::memory_order_relaxed);
        FUERayTracingAudioAudioDiagnostics::
            RecordHardRealtimeCapacityMiss();
    }

    void ReclaimControl(
        FUERayTracingAudioPreparedConvolverState* State)
    {
        if (!State)
        {
            return;
        }
        const uint64 ReleasedBytes =
            State->GetAllocatedBytes();
        State->Prepare(nullptr, 0, 0);
        const uint64 CurrentBytes =
            PreparedWorkspaceBytes.load(
                std::memory_order_relaxed);
        PreparedWorkspaceBytes.store(
            CurrentBytes >= ReleasedBytes
                ? CurrentBytes - ReleasedBytes
                : 0,
            std::memory_order_relaxed);
        FreeStates.Add(State);
    }

    void DrainReturnedStates()
    {
        if (!SourceStates)
        {
            return;
        }
        for (int32 SourceIndex = 0;
            SourceIndex < NumSources;
            ++SourceIndex)
        {
            for (int32 LaneIndex = 0;
                LaneIndex < ConvolutionLaneCount;
                ++LaneIndex)
            {
                FLaneState& Lane =
                    SourceStates[SourceIndex].Lanes[LaneIndex];
                for (std::atomic<
                        FUERayTracingAudioPreparedConvolverState*>&
                    ReturnSlot : Lane.Returned)
                {
                    if (ReturnSlot.load(
                            std::memory_order_acquire))
                    {
                        ReclaimControl(
                            ReturnSlot.exchange(
                                nullptr,
                                std::memory_order_acq_rel));
                    }
                }
            }
        }
    }

    bool TryReturnAudio(
        const int32 SourceId,
        const int32 LaneIndex,
        FUERayTracingAudioPreparedConvolverState* State,
        const bool bAllowEmergencySlots)
    {
        if (!State
            || !IsValidSource(SourceId)
            || LaneIndex < 0
            || LaneIndex >= ConvolutionLaneCount)
        {
            return State == nullptr;
        }

        FLaneState& Lane =
            SourceStates[SourceId].Lanes[LaneIndex];
        const int32 SlotLimit = bAllowEmergencySlots
            ? TotalReturnSlotsPerLane
            : NormalReturnSlotsPerLane;
        for (int32 SlotIndex = 0;
            SlotIndex < SlotLimit;
            ++SlotIndex)
        {
            FUERayTracingAudioPreparedConvolverState* Expected =
                nullptr;
            if (Lane.Returned[SlotIndex].
                compare_exchange_strong(
                    Expected,
                    State,
                    std::memory_order_release,
                    std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }

    int32 CountAvailableReturnSlots(
        const int32 SourceId,
        const int32 LaneIndex) const
    {
        if (!IsValidSource(SourceId)
            || LaneIndex < 0
            || LaneIndex >= ConvolutionLaneCount)
        {
            return 0;
        }
        int32 AvailableSlots = 0;
        const FLaneState& Lane =
            SourceStates[SourceId].Lanes[LaneIndex];
        for (const std::atomic<
                FUERayTracingAudioPreparedConvolverState*>&
            ReturnSlot : Lane.Returned)
        {
            AvailableSlots +=
                ReturnSlot.load(std::memory_order_acquire)
                    == nullptr
                ? 1
                : 0;
        }
        return AvailableSlots;
    }

    void SubmitRequestAudio(
        const int32 SourceId,
        const int32 LaneIndex,
        const uint64 AudioComponentId,
        const uint64 Revision)
    {
        FLaneState& Lane =
            SourceStates[SourceId].Lanes[LaneIndex];
        if (Lane.LastRequestedAudioComponentId
                == AudioComponentId
            && Lane.LastRequestedRevision == Revision)
        {
            return;
        }
        Lane.LastRequestedAudioComponentId =
            AudioComponentId;
        Lane.LastRequestedRevision = Revision;
        Lane.RequestSequence.fetch_add(
            1,
            std::memory_order_seq_cst);
        Lane.RequestedAudioComponentId.store(
            AudioComponentId,
            std::memory_order_relaxed);
        Lane.RequestedRevision.store(
            Revision,
            std::memory_order_relaxed);
        Lane.RequestSequence.fetch_add(
            1,
            std::memory_order_seq_cst);
    }

    bool ReadRequestControl(
        FLaneState& Lane,
        uint64& OutSequence,
        uint64& OutAudioComponentId,
        uint64& OutRevision) const
    {
        const uint64 SequenceBefore =
            Lane.RequestSequence.load(
                std::memory_order_seq_cst);
        if (SequenceBefore == 0
            || (SequenceBefore & 1ull) != 0
            || SequenceBefore
                == Lane.LastServicedRequestSequence)
        {
            return false;
        }
        const uint64 AudioComponentId =
            Lane.RequestedAudioComponentId.load(
                std::memory_order_relaxed);
        const uint64 Revision =
            Lane.RequestedRevision.load(
                std::memory_order_relaxed);
        const uint64 SequenceAfter =
            Lane.RequestSequence.load(
                std::memory_order_seq_cst);
        if (SequenceBefore != SequenceAfter
            || (SequenceAfter & 1ull) != 0)
        {
            return false;
        }
        OutSequence = SequenceAfter;
        OutAudioComponentId = AudioComponentId;
        OutRevision = Revision;
        return true;
    }

    FUERayTracingAudioPreparedConvolverState*
    AcquirePreparedStateControl(
        const uint64 AudioComponentId,
        const uint64 Revision,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            Kernel)
    {
        if (!Kernel
            || Kernel->GetSampleRate() != SampleRate)
        {
            return nullptr;
        }

        const uint64 EstimatedBytes =
            EstimateWorkspaceBytes(*Kernel);
        const uint64 CurrentBytes =
            PreparedWorkspaceBytes.load(
                std::memory_order_relaxed);
        if (EstimatedBytes > MaxPreparedWorkspaceBytes
            || CurrentBytes
                > MaxPreparedWorkspaceBytes - EstimatedBytes)
        {
            PrepareCapacityDropCount.fetch_add(
                1,
                std::memory_order_relaxed);
            FUERayTracingAudioAudioDiagnostics::
                RecordConvolutionPrepareCapacityDrop();
            return nullptr;
        }

        FUERayTracingAudioPreparedConvolverState* State =
            nullptr;
        if (!FreeStates.IsEmpty())
        {
            State = FreeStates.Pop(
                EAllowShrinking::No);
        }
        else if (OwnedStates.Num() < MaxPreparedStates)
        {
            TUniquePtr<
                FUERayTracingAudioPreparedConvolverState>
                NewState = MakeUnique<
                    FUERayTracingAudioPreparedConvolverState>();
            State = NewState.Get();
            OwnedStates.Add(MoveTemp(NewState));
        }
        if (!State)
        {
            PrepareCapacityDropCount.fetch_add(
                1,
                std::memory_order_relaxed);
            FUERayTracingAudioAudioDiagnostics::
                RecordConvolutionPrepareCapacityDrop();
            return nullptr;
        }

        State->Prepare(
            Kernel,
            Revision,
            AudioComponentId);
        const uint64 ActualBytes =
            State->GetAllocatedBytes();
        if (ActualBytes > MaxPreparedWorkspaceBytes
            || CurrentBytes
                > MaxPreparedWorkspaceBytes - ActualBytes)
        {
            // This state has not yet contributed to the accounted total, so
            // clear it directly instead of using ReclaimControl (which would
            // subtract another state's workspace).
            State->Prepare(nullptr, 0, 0);
            FreeStates.Add(State);
            PrepareCapacityDropCount.fetch_add(
                1,
                std::memory_order_relaxed);
            FUERayTracingAudioAudioDiagnostics::
                RecordConvolutionPrepareCapacityDrop();
            return nullptr;
        }
        PreparedWorkspaceBytes.store(
            CurrentBytes + ActualBytes,
            std::memory_order_relaxed);
        return State;
    }

    void ReclaimReadyForComponentControl(
        const uint64 AudioComponentId,
        const uint8 LaneMask)
    {
        if (!SourceStates)
        {
            return;
        }
        for (int32 SourceIndex = 0;
            SourceIndex < NumSources;
            ++SourceIndex)
        {
            for (int32 LaneIndex = 0;
                LaneIndex < ConvolutionLaneCount;
                ++LaneIndex)
            {
                if ((LaneMask & (1u << LaneIndex)) == 0)
                {
                    continue;
                }
                FLaneState& Lane =
                    SourceStates[SourceIndex].Lanes[LaneIndex];
                FUERayTracingAudioPreparedConvolverState*
                    Ready = Lane.Ready.load(
                        std::memory_order_acquire);
                if (!Ready
                    || Ready->GetOwnerKey()
                        != AudioComponentId)
                {
                    continue;
                }
                if (Lane.Ready.compare_exchange_strong(
                        Ready,
                        nullptr,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    ReclaimControl(Ready);
                }
            }
        }
    }

    int32 NumSources = 0;
    int32 SampleRate = 48000;
    int32 MaxPreparedStates = 0;
    int32 NextServiceSlot = 0;
    TUniquePtr<FSourceState[]> SourceStates;
    TMap<uint64, FTarget> Targets;
    TArray<
        TUniquePtr<
            FUERayTracingAudioPreparedConvolverState>>
        OwnedStates;
    TArray<FUERayTracingAudioPreparedConvolverState*>
        FreeStates;
    std::atomic<uint64> PreparedWorkspaceBytes { 0 };
    std::atomic<uint64> ReturnOverflowCount { 0 };
    std::atomic<uint64> PrepareCapacityDropCount { 0 };
};

FUERayTracingAudioIndirectAudioBridge::
    FUERayTracingAudioIndirectAudioBridge()
    : ConvolutionRuntime(
        MakeUnique<FConvolutionRuntime>())
{
}

FUERayTracingAudioIndirectAudioBridge::
    ~FUERayTracingAudioIndirectAudioBridge() = default;

void FUERayTracingAudioIndirectAudioBridge::Initialize(
    const int32 NumSources,
    const int32 InMaxFramesPerCallback,
    const int32 SampleRate)
{
    const int32 RequiredSources = FMath::Max(NumSources, 0);
    const int32 RequiredFrames = FMath::Max(InMaxFramesPerCallback, 0);
    ConvolutionRuntime->Initialize(
        RequiredSources,
        SampleRate);
    if (Sources.Num() == RequiredSources
        && MaxFramesPerCallback == RequiredFrames)
    {
        return;
    }

    Sources.Reset();
    Sources.SetNum(RequiredSources);
    MaxFramesPerCallback = RequiredFrames;
    CapacityOverflowCount.Store(0);
    for (FSourceState& State : Sources)
    {
        State.StereoWet.Reserve(MaxFramesPerCallback);
    }
}

TArrayView<FVector2f> FUERayTracingAudioIndirectAudioBridge::BeginWrite(
    const int32 SourceId,
    const uint64 AudioComponentId,
    const int32 NumFrames)
{
    return BeginWriteInternal(
        SourceId,
        AudioComponentId,
        NumFrames,
        false,
        EUERayTracingAudioRuntimeDataSource::Realtime);
}

TArrayView<FVector2f> FUERayTracingAudioIndirectAudioBridge::BeginWrite(
    const int32 SourceId,
    const uint64 AudioComponentId,
    const int32 NumFrames,
    const EUERayTracingAudioRuntimeDataSource DataSource)
{
    return BeginWriteInternal(
        SourceId,
        AudioComponentId,
        NumFrames,
        true,
        DataSource);
}

TArrayView<FVector2f> FUERayTracingAudioIndirectAudioBridge::BeginWriteInternal(
    const int32 SourceId,
    const uint64 AudioComponentId,
    const int32 NumFrames,
    const bool bHasDataSource,
    const EUERayTracingAudioRuntimeDataSource DataSource)
{
    if (!Sources.IsValidIndex(SourceId) || NumFrames <= 0)
    {
        return TArrayView<FVector2f>();
    }

    FSourceState& State = Sources[SourceId];
    if (NumFrames > MaxFramesPerCallback
        || State.StereoWet.Max() < NumFrames)
    {
        State.AudioComponentId = AudioComponentId;
        State.bReady = false;
        State.bWriting = false;
        State.bHasDataSource = false;
        ++CapacityOverflowCount;
        FUERayTracingAudioAudioDiagnostics::
            RecordHardRealtimeCapacityMiss();
        return TArrayView<FVector2f>();
    }

    State.AudioComponentId = AudioComponentId;
    State.bReady = false;
    State.bWriting = true;
    State.bHasDataSource = bHasDataSource;
    State.DataSource = DataSource;
    State.StereoWet.SetNumUninitialized(NumFrames, EAllowShrinking::No);
    return MakeArrayView(State.StereoWet);
}

void FUERayTracingAudioIndirectAudioBridge::EndWrite(
    const int32 SourceId,
    const uint64 AudioComponentId)
{
    if (!Sources.IsValidIndex(SourceId))
    {
        return;
    }

    FSourceState& State = Sources[SourceId];
    State.bReady = State.bWriting && State.AudioComponentId == AudioComponentId;
    State.bWriting = false;
}

bool FUERayTracingAudioIndirectAudioBridge::Consume(
    const int32 SourceId,
    const uint64 AudioComponentId,
    TArrayView<const FVector2f>& OutStereoWet)
{
    bool bHasDataSource = false;
    EUERayTracingAudioRuntimeDataSource DataSource =
        EUERayTracingAudioRuntimeDataSource::Realtime;
    return Consume(
        SourceId,
        AudioComponentId,
        OutStereoWet,
        bHasDataSource,
        DataSource);
}

bool FUERayTracingAudioIndirectAudioBridge::Consume(
    const int32 SourceId,
    const uint64 AudioComponentId,
    TArrayView<const FVector2f>& OutStereoWet,
    bool& bOutHasDataSource,
    EUERayTracingAudioRuntimeDataSource& OutDataSource)
{
    OutStereoWet = TArrayView<const FVector2f>();
    bOutHasDataSource = false;
    OutDataSource = EUERayTracingAudioRuntimeDataSource::Realtime;
    if (!Sources.IsValidIndex(SourceId))
    {
        return false;
    }

    FSourceState& State = Sources[SourceId];
    if (!State.bReady || State.bWriting || State.AudioComponentId != AudioComponentId)
    {
        return false;
    }

    State.bReady = false;
    OutStereoWet = MakeArrayView(static_cast<const TArray<FVector2f>&>(State.StereoWet));
    bOutHasDataSource = State.bHasDataSource;
    OutDataSource = State.DataSource;
    return true;
}

void FUERayTracingAudioIndirectAudioBridge::ClearSource(const int32 SourceId)
{
    if (Sources.IsValidIndex(SourceId))
    {
        FSourceState& State = Sources[SourceId];
        State.AudioComponentId = 0;
        State.StereoWet.SetNum(0, EAllowShrinking::No);
        State.bWriting = false;
        State.bReady = false;
        State.bHasDataSource = false;
        State.DataSource = EUERayTracingAudioRuntimeDataSource::Realtime;
    }
}

uint64 FUERayTracingAudioIndirectAudioBridge::GetCapacityOverflowCount() const
{
    return CapacityOverflowCount.Load();
}

void FUERayTracingAudioIndirectAudioBridge::
    PublishConvolutionTargets(
        const uint64 AudioComponentId,
        const FUERayTracingAudioConvolutionRevisions&
            InRevisions,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            BakedLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            BakedRight,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            RealtimeLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            RealtimeRight)
{
    if (AudioComponentId == 0)
    {
        return;
    }

    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        EffectiveBakedRight =
            BakedRight ? BakedRight : BakedLeft;
    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        EffectiveRealtimeRight =
            RealtimeRight
                ? RealtimeRight
                : RealtimeLeft;
    FUERayTracingAudioConvolutionRevisions EffectiveRevisions =
        InRevisions;
    if (!BakedRight
        && EffectiveRevisions.BakedRight == 0)
    {
        EffectiveRevisions.BakedRight =
            EffectiveRevisions.BakedLeft;
    }
    if (!RealtimeRight
        && EffectiveRevisions.RealtimeRight == 0)
    {
        EffectiveRevisions.RealtimeRight =
            EffectiveRevisions.RealtimeLeft;
    }

    const FUERayTracingAudioConvolutionKernel::FKernelPtr*
        EffectiveKernels[ConvolutionLaneCount] =
        {
            &BakedLeft,
            &EffectiveBakedRight,
            &RealtimeLeft,
            &EffectiveRealtimeRight
        };
    const FConvolutionRuntime::FTarget* ExistingTarget =
        ConvolutionRuntime->Targets.Find(
            AudioComponentId);
    uint8 ChangedLaneMask = 0;
    for (int32 LaneIndex = 0;
        LaneIndex < ConvolutionLaneCount;
        ++LaneIndex)
    {
        const uint64 NewRevision =
            GetLaneRevision(
                EffectiveRevisions,
                LaneIndex);
        const uint64 ExistingRevision =
            ExistingTarget
                ? ExistingTarget->Revisions[LaneIndex]
                : 0;
        const FUERayTracingAudioConvolutionKernel*
            ExistingKernel =
                ExistingTarget
                    ? ExistingTarget->Kernels[
                        LaneIndex].Get()
                    : nullptr;
        if (NewRevision != ExistingRevision
            || (*EffectiveKernels[LaneIndex]).Get()
                != ExistingKernel)
        {
            ChangedLaneMask |=
                static_cast<uint8>(1u << LaneIndex);
        }
    }
    if (ChangedLaneMask == 0)
    {
        return;
    }

    FConvolutionRuntime::FTarget& Target =
        ConvolutionRuntime->Targets.FindOrAdd(
            AudioComponentId);
    for (int32 LaneIndex = 0;
        LaneIndex < ConvolutionLaneCount;
        ++LaneIndex)
    {
        if ((ChangedLaneMask & (1u << LaneIndex)) == 0)
        {
            continue;
        }
        Target.Revisions[LaneIndex] =
            GetLaneRevision(
                EffectiveRevisions,
                LaneIndex);
        Target.Kernels[LaneIndex] =
            *EffectiveKernels[LaneIndex];
    }
}

void FUERayTracingAudioIndirectAudioBridge::
    PublishConvolutionTargets(
        const uint64 AudioComponentId,
        const uint64 Revision,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            BakedLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            BakedRight,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            RealtimeLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            RealtimeRight)
{
    FUERayTracingAudioConvolutionRevisions Revisions;
    Revisions.BakedLeft = Revision;
    Revisions.BakedRight = Revision;
    Revisions.RealtimeLeft = Revision;
    Revisions.RealtimeRight = Revision;
    PublishConvolutionTargets(
        AudioComponentId,
        Revisions,
        BakedLeft,
        BakedRight,
        RealtimeLeft,
        RealtimeRight);
}

void FUERayTracingAudioIndirectAudioBridge::
    RemoveConvolutionTargets(
        const uint64 AudioComponentId)
{
    ConvolutionRuntime->DrainReturnedStates();
    ConvolutionRuntime->ReclaimReadyForComponentControl(
        AudioComponentId,
        AllConvolutionLaneMask);
    ConvolutionRuntime->Targets.Remove(AudioComponentId);
}

void FUERayTracingAudioIndirectAudioBridge::
    ServiceConvolutionGameThread(
        const int32 PrepareBudget)
{
    ConvolutionRuntime->DrainReturnedStates();
    if (!ConvolutionRuntime->SourceStates)
    {
        return;
    }

    const int32 BoundedBudget =
        FMath::Max(PrepareBudget, 0);
    const int32 TotalServiceSlots =
        ConvolutionRuntime->NumSources
        * ConvolutionLaneCount;
    if (BoundedBudget == 0
        || TotalServiceSlots <= 0)
    {
        return;
    }

    int32 NumPrepared = 0;
    int32 NumVisited = 0;
    while (NumVisited < TotalServiceSlots
        && NumPrepared < BoundedBudget)
    {
        const int32 ServiceSlot =
            ConvolutionRuntime->NextServiceSlot;
        ConvolutionRuntime->NextServiceSlot =
            (ServiceSlot + 1) % TotalServiceSlots;
        ++NumVisited;
        const int32 SourceIndex =
            ServiceSlot / ConvolutionLaneCount;
        const int32 LaneIndex =
            ServiceSlot % ConvolutionLaneCount;
        FConvolutionRuntime::FLaneState& Lane =
            ConvolutionRuntime->SourceStates[
                SourceIndex].Lanes[LaneIndex];
        uint64 RequestSequence = 0;
        uint64 AudioComponentId = 0;
        uint64 Revision = 0;
        if (!ConvolutionRuntime->ReadRequestControl(
                Lane,
                RequestSequence,
                AudioComponentId,
                Revision))
        {
            continue;
        }

        FConvolutionRuntime::FTarget* Target =
            ConvolutionRuntime->Targets.Find(
                AudioComponentId);
        if (AudioComponentId == 0)
        {
            ConvolutionRuntime->ReclaimControl(
                Lane.Ready.exchange(
                    nullptr,
                    std::memory_order_acq_rel));
            Lane.LastServicedRequestSequence =
                RequestSequence;
            continue;
        }
        if (!Target
            || Target->Revisions[LaneIndex]
                != Revision)
        {
            // Publication can legitimately lag the first audio callback.
            // Keep the request pending so a later target recipe can satisfy
            // it without requiring the audio producer to repost an otherwise
            // identical owner/revision pair.
            continue;
        }
        const FUERayTracingAudioConvolutionKernel::
            FKernelPtr& Kernel =
                Target->Kernels[LaneIndex];
        if (!Kernel
            || Kernel->GetSampleRate()
                != ConvolutionRuntime->SampleRate)
        {
            // A matching recipe may still be replaced (for example after an
            // audio-device sample-rate transition). Retrying is bounded by
            // the fixed source/lane table and performs no allocation.
            continue;
        }

        FUERayTracingAudioPreparedConvolverState*
            ExistingReady = Lane.Ready.load(
                std::memory_order_acquire);
        if (ExistingReady
            && ExistingReady->GetOwnerKey()
                == AudioComponentId
            && ExistingReady->GetRevision()
                == Revision)
        {
            Lane.LastServicedRequestSequence =
                RequestSequence;
            continue;
        }
        if (ExistingReady
            && Lane.Ready.compare_exchange_strong(
                ExistingReady,
                nullptr,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            ConvolutionRuntime->ReclaimControl(
                ExistingReady);
        }

        FUERayTracingAudioPreparedConvolverState*
            Prepared =
                ConvolutionRuntime->
                    AcquirePreparedStateControl(
                        AudioComponentId,
                        Revision,
                        Kernel);
        if (!Prepared)
        {
            continue;
        }
        FUERayTracingAudioPreparedConvolverState*
            Expected = nullptr;
        if (!Lane.Ready.compare_exchange_strong(
                Expected,
                Prepared,
                std::memory_order_release,
                std::memory_order_acquire))
        {
            ConvolutionRuntime->ReclaimControl(
                Prepared);
            continue;
        }
        Lane.LastServicedRequestSequence =
            RequestSequence;
        ++NumPrepared;
    }
}

EUERayTracingAudioConvolverConfigureResult
FUERayTracingAudioIndirectAudioBridge::ConfigureConvolver(
    const int32 SourceId,
    const uint64 AudioComponentId,
    const uint64 Revision,
    const bool bHasKernel,
    const EUERayTracingAudioConvolutionLane Lane,
    FUERayTracingAudioPreparedCrossfadingConvolver&
        Convolver,
    const int32 CrossfadeSamples)
{
    const int32 LaneIndex = ToLaneIndex(Lane);
    if (!ConvolutionRuntime->IsValidSource(SourceId)
        || LaneIndex == INDEX_NONE)
    {
        return
            EUERayTracingAudioConvolverConfigureResult::
                InvalidSource;
    }

    if (FUERayTracingAudioPreparedConvolverState*
        Retired = Convolver.PeekRetiredState())
    {
        if (!ConvolutionRuntime->TryReturnAudio(
                SourceId,
                LaneIndex,
                Retired,
                false))
        {
            ConvolutionRuntime->
                RecordReturnOverflowAudio();
            return
                EUERayTracingAudioConvolverConfigureResult::
                    ReturnQueueFull;
        }
        Convolver.TakeRetiredState();
    }

    if (!bHasKernel)
    {
        ConvolutionRuntime->SubmitRequestAudio(
            SourceId,
            LaneIndex,
            0,
            0);
    }
    if (Convolver.GetCurrentRevision() == Revision
        && Convolver.GetCurrentOwnerKey()
            == AudioComponentId)
    {
        return
            EUERayTracingAudioConvolverConfigureResult::
                Unchanged;
    }
    if (!Convolver.CanAcceptTransition())
    {
        return
            EUERayTracingAudioConvolverConfigureResult::
                Busy;
    }
    if (!bHasKernel)
    {
        return Convolver.AdoptSilence(
                Revision,
                AudioComponentId,
                CrossfadeSamples)
            ? EUERayTracingAudioConvolverConfigureResult::
                Silenced
            : EUERayTracingAudioConvolverConfigureResult::
                Busy;
    }

    FConvolutionRuntime::FLaneState& LaneState =
        ConvolutionRuntime->SourceStates[
            SourceId].Lanes[LaneIndex];
    FUERayTracingAudioPreparedConvolverState* Ready =
        LaneState.Ready.load(
            std::memory_order_acquire);
    if (Ready)
    {
        // Reserve a bounded return slot before claiming the mailbox. The
        // control thread can only make more slots available, and callbacks for
        // one SourceId are serialized, so a claimed pointer can always be
        // returned without loss if it is stale or adoption unexpectedly fails.
        if (ConvolutionRuntime->CountAvailableReturnSlots(
                SourceId,
                LaneIndex) == 0)
        {
            ConvolutionRuntime->
                RecordReturnOverflowAudio();
            return
                EUERayTracingAudioConvolverConfigureResult::
                    ReturnQueueFull;
        }
        Ready = LaneState.Ready.exchange(
            nullptr,
            std::memory_order_acq_rel);
        if (Ready
            && Ready->GetOwnerKey() == AudioComponentId
            && Ready->GetRevision() == Revision
            && Convolver.AdoptPreparedState(
                Ready,
                CrossfadeSamples))
        {
            return
                EUERayTracingAudioConvolverConfigureResult::
                    Adopted;
        }
        if (Ready)
        {
            if (!ConvolutionRuntime->TryReturnAudio(
                    SourceId,
                    LaneIndex,
                    Ready,
                    true))
            {
                // The audio producer is serialized per SourceId and the
                // control thread only empties return slots, so the reservation
                // above makes this unreachable without memory corruption.
                ConvolutionRuntime->
                    RecordReturnOverflowAudio();
                return
                    EUERayTracingAudioConvolverConfigureResult::
                        ReturnQueueFull;
            }
        }
    }

    ConvolutionRuntime->SubmitRequestAudio(
        SourceId,
        LaneIndex,
        AudioComponentId,
        Revision);
    return
        EUERayTracingAudioConvolverConfigureResult::
            Requested;
}

bool FUERayTracingAudioIndirectAudioBridge::ReleaseConvolver(
    const int32 SourceId,
    const EUERayTracingAudioConvolutionLane Lane,
    FUERayTracingAudioPreparedCrossfadingConvolver&
        Convolver)
{
    const int32 LaneIndex = ToLaneIndex(Lane);
    if (!ConvolutionRuntime->IsValidSource(SourceId)
        || LaneIndex == INDEX_NONE
        || ConvolutionRuntime->CountAvailableReturnSlots(
            SourceId,
            LaneIndex) < 3)
    {
        ConvolutionRuntime->
            RecordReturnOverflowAudio();
        return false;
    }

    FUERayTracingAudioPreparedConvolverState*
        Detached[3] = { nullptr, nullptr, nullptr };
    const int32 NumDetached =
        Convolver.DetachAllStates(
            Detached,
            UE_ARRAY_COUNT(Detached));
    for (int32 Index = 0;
        Index < NumDetached;
        ++Index)
    {
        if (!ConvolutionRuntime->TryReturnAudio(
                SourceId,
                LaneIndex,
                Detached[Index],
                true))
        {
            ConvolutionRuntime->
                RecordReturnOverflowAudio();
            return false;
        }
    }

    FConvolutionRuntime::FLaneState& LaneState =
        ConvolutionRuntime->SourceStates[
            SourceId].Lanes[LaneIndex];
    ConvolutionRuntime->SubmitRequestAudio(
        SourceId,
        LaneIndex,
        0,
        0);
    return true;
}

uint64 FUERayTracingAudioIndirectAudioBridge::
    GetPreparedWorkspaceBytes() const
{
    return ConvolutionRuntime->
        PreparedWorkspaceBytes.load(
            std::memory_order_relaxed);
}

uint64 FUERayTracingAudioIndirectAudioBridge::
    GetConvolutionReturnOverflowCount() const
{
    return ConvolutionRuntime->
        ReturnOverflowCount.load(
            std::memory_order_relaxed);
}

uint64 FUERayTracingAudioIndirectAudioBridge::
    GetConvolutionPrepareCapacityDropCount() const
{
    return ConvolutionRuntime->
        PrepareCapacityDropCount.load(
            std::memory_order_relaxed);
}

#if WITH_DEV_AUTOMATION_TESTS
int32 FUERayTracingAudioIndirectAudioBridge::GetSourceCapacityForTesting(
    const int32 SourceId) const
{
    return Sources.IsValidIndex(SourceId)
        ? Sources[SourceId].StereoWet.Max()
        : 0;
}

int32 FUERayTracingAudioIndirectAudioBridge::
    GetPreparedStateCountForTesting() const
{
    return ConvolutionRuntime->OwnedStates.Num();
}
#endif
