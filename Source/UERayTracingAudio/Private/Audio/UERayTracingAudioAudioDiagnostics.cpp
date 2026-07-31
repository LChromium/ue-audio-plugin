#include "Audio/UERayTracingAudioAudioDiagnostics.h"

namespace
{
    constexpr int32 DataSourceCount = 3;
    constexpr double RmsFixedPointScale = 1000000000.0;
    constexpr double IntegratedMeanSquareFixedPointScale = 1000000000.0;
    constexpr float RmsPresenceThreshold = 1.0e-8f;
    constexpr int32 MaximumSnapshotReadAttempts = 8;

    struct FAtomicDataSourceStats
    {
        // Reset only advances RequestedEpoch. The single audio-thread writer
        // clears and publishes the new epoch inside SnapshotSequence's write
        // interval, so an in-flight pre-reset buffer cannot become visible as
        // post-reset data.
        TAtomic<uint64> RequestedEpoch{ 1 };
        TAtomic<uint64> PublishedEpoch{ 0 };
        TAtomic<uint64> SnapshotSequence{ 0 };
        TAtomic<uint64> BufferCount{ 0 };
        TAtomic<uint64> FrameCount{ 0 };
        TAtomic<uint64> NonSilentInputBufferCount{ 0 };
        TAtomic<uint64> NonSilentBufferCount{ 0 };
        TAtomic<uint64> RmsMeasuredBufferCount{ 0 };
        TAtomic<uint64> AudibleWetBufferCount{ 0 };
        TAtomic<uint64> CurrentConsecutiveInaudibleWetBufferCount{ 0 };
        TAtomic<uint64> MaxConsecutiveInaudibleWetBufferCount{ 0 };
        TAtomic<uint64> WetPresentInputBufferCount{ 0 };
        TAtomic<uint64> CurrentConsecutiveSilentWetBufferCount{ 0 };
        TAtomic<uint64> MaxConsecutiveSilentWetBufferCount{ 0 };
        TAtomic<uint64> IntegratedInputMeanSquareFixed{ 0 };
        TAtomic<uint64> IntegratedWetMeanSquareFixed{ 0 };
        TAtomic<uint64> NonFiniteSampleCount{ 0 };
        TAtomic<uint64> FinalOutputBufferCount{ 0 };
        TAtomic<uint64> OverUnitOutputSampleCount{ 0 };
        TAtomic<uint64> PreSpatializationOverUnitSampleCount{ 0 };
        TAtomic<uint64> MaxInputRmsFixed{ 0 };
        TAtomic<uint64> MaxWetRmsFixed{ 0 };
        TAtomic<uint64> MaxOutputPeakFixed{ 0 };
        TAtomic<uint64> MaxPreSpatializationOutputPeakFixed{ 0 };
        TAtomic<uint64> MaxWetToInputRmsRatioFixed{ 0 };
    };

    FAtomicDataSourceStats GDataSourceStats[DataSourceCount];
    TAtomic<uint64> GTargetAudioComponentId{ 0 };
    TAtomic<uint64> GHardRealtimeAudioCallbackCount{ 0 };
    TAtomic<uint64> GHardRealtimeCallbackCapacityMissCount{ 0 };
    TAtomic<uint64>
        GConvolutionPrepareCapacityDropCount{ 0 };

    int32 GetDataSourceIndex(const EUERayTracingAudioRuntimeDataSource DataSource)
    {
        return FMath::Clamp(static_cast<int32>(DataSource), 0, DataSourceCount - 1);
    }

    uint64 QuantizeRms(const float Value)
    {
        return FMath::IsFinite(Value) && Value > 0.0f
            ? static_cast<uint64>(
                FMath::Min(
                    static_cast<double>(Value),
                    1024.0)
                * RmsFixedPointScale)
            : 0;
    }

    uint64 QuantizeWeightedMeanSquare(
        const float Rms,
        const int32 NumFrames)
    {
        if (!FMath::IsFinite(Rms)
            || Rms <= 0.0f
            || NumFrames <= 0)
        {
            return 0;
        }

        const double ClampedRms = FMath::Min(
            static_cast<double>(Rms),
            1024.0);
        const double FixedMeanSquare =
            ClampedRms
            * ClampedRms
            * static_cast<double>(NumFrames)
            * IntegratedMeanSquareFixedPointScale;
        const uint64 Maximum = TNumericLimits<uint64>::Max();
        return FixedMeanSquare
            >= static_cast<double>(Maximum)
            ? Maximum
            : static_cast<uint64>(FixedMeanSquare);
    }

    void ClearCountersForWriter(FAtomicDataSourceStats& Stats)
    {
        Stats.BufferCount.Store(0);
        Stats.FrameCount.Store(0);
        Stats.NonSilentInputBufferCount.Store(0);
        Stats.NonSilentBufferCount.Store(0);
        Stats.RmsMeasuredBufferCount.Store(0);
        Stats.AudibleWetBufferCount.Store(0);
        Stats.CurrentConsecutiveInaudibleWetBufferCount.Store(0);
        Stats.MaxConsecutiveInaudibleWetBufferCount.Store(0);
        Stats.WetPresentInputBufferCount.Store(0);
        Stats.CurrentConsecutiveSilentWetBufferCount.Store(0);
        Stats.MaxConsecutiveSilentWetBufferCount.Store(0);
        Stats.IntegratedInputMeanSquareFixed.Store(0);
        Stats.IntegratedWetMeanSquareFixed.Store(0);
        Stats.NonFiniteSampleCount.Store(0);
        Stats.FinalOutputBufferCount.Store(0);
        Stats.OverUnitOutputSampleCount.Store(0);
        Stats.PreSpatializationOverUnitSampleCount.Store(0);
        Stats.MaxInputRmsFixed.Store(0);
        Stats.MaxWetRmsFixed.Store(0);
        Stats.MaxOutputPeakFixed.Store(0);
        Stats.MaxPreSpatializationOutputPeakFixed.Store(0);
        Stats.MaxWetToInputRmsRatioFixed.Store(0);
    }

    void AddSaturating(
        TAtomic<uint64>& Target,
        const uint64 Value)
    {
        const uint64 Current = Target.Load();
        const uint64 Maximum = TNumericLimits<uint64>::Max();
        Target.Store(
            Value > Maximum - Current
                ? Maximum
                : Current + Value);
    }

    void StoreMaximum(TAtomic<uint64>& Target, const uint64 Value)
    {
        // SnapshotSequence admits at most one audio-thread writer. Atomic
        // storage keeps game-thread reads safe without locks, allocation, or
        // UObject access.
        if (Value > Target.Load())
        {
            Target.Store(Value);
        }
    }
}

void FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
    const uint64 AudioComponentId)
{
    GTargetAudioComponentId.Store(AudioComponentId);
}

bool FUERayTracingAudioAudioDiagnostics::IsEnabledFor(
    const uint64 AudioComponentId)
{
    const uint64 TargetAudioComponentId =
        GTargetAudioComponentId.Load();
    return TargetAudioComponentId != 0
        && TargetAudioComponentId == AudioComponentId;
}

void FUERayTracingAudioAudioDiagnostics::Reset(
    const EUERayTracingAudioRuntimeDataSource DataSource)
{
    FAtomicDataSourceStats& Stats = GDataSourceStats[GetDataSourceIndex(DataSource)];
    ++Stats.RequestedEpoch;
}

void FUERayTracingAudioAudioDiagnostics::RecordBuffer(
    const EUERayTracingAudioRuntimeDataSource DataSource,
    const uint64 AudioComponentId,
    const int32 NumFrames,
    const float PeakAbsoluteInput,
    const float PeakAbsoluteWet,
    const float InputRms,
    const float WetRms,
    const uint64 NonFiniteSampleCount,
    const float PeakAbsolutePreSpatializationOutput,
    const uint64 PreSpatializationOverUnitSampleCount)
{
    if (!IsEnabledFor(AudioComponentId))
    {
        return;
    }

    FAtomicDataSourceStats& Stats = GDataSourceStats[GetDataSourceIndex(DataSource)];
    uint64 StableSequence = Stats.SnapshotSequence.Load();
    const uint64 WriteSequence = StableSequence + 1;
    if ((StableSequence & 1ULL) != 0
        || !Stats.SnapshotSequence.CompareExchange(
            StableSequence,
            WriteSequence))
    {
        // Diagnostics never make an audio callback wait. An unexpected
        // overlapping writer drops this statistics buffer and tries again on
        // the next callback.
        return;
    }
    const uint64 RequestedEpoch = Stats.RequestedEpoch.Load();
    if (Stats.PublishedEpoch.Load() != RequestedEpoch)
    {
        ClearCountersForWriter(Stats);
    }

    ++Stats.BufferCount;
    Stats.FrameCount += static_cast<uint64>(FMath::Max(NumFrames, 0));
    if (FMath::IsFinite(PeakAbsoluteInput) && PeakAbsoluteInput > 1.0e-8f)
    {
        ++Stats.NonSilentInputBufferCount;
    }
    if (FMath::IsFinite(PeakAbsoluteWet) && PeakAbsoluteWet > 1.0e-8f)
    {
        ++Stats.NonSilentBufferCount;
    }
    Stats.NonFiniteSampleCount += NonFiniteSampleCount;
    AddSaturating(
        Stats.PreSpatializationOverUnitSampleCount,
        PreSpatializationOverUnitSampleCount);
    StoreMaximum(Stats.MaxInputRmsFixed, QuantizeRms(InputRms));
    StoreMaximum(Stats.MaxWetRmsFixed, QuantizeRms(WetRms));
    StoreMaximum(
        Stats.MaxPreSpatializationOutputPeakFixed,
        QuantizeRms(PeakAbsolutePreSpatializationOutput));
    const float WetToInputRmsRatio =
        InputRms > UE_SMALL_NUMBER
            ? WetRms / InputRms
            : 0.0f;
    const bool bInputRmsPresent =
        NumFrames > 0
        && FMath::IsFinite(InputRms)
        && InputRms > RmsPresenceThreshold;
    if (bInputRmsPresent)
    {
        ++Stats.RmsMeasuredBufferCount;
        if (FMath::IsFinite(WetToInputRmsRatio)
            && WetToInputRmsRatio
                >= FUERayTracingAudioAudioDiagnostics::AudibleWetToInputRmsRatio)
        {
            ++Stats.AudibleWetBufferCount;
            Stats.CurrentConsecutiveInaudibleWetBufferCount.Store(0);
        }
        else
        {
            const uint64 ConsecutiveInaudibleCount =
                Stats.CurrentConsecutiveInaudibleWetBufferCount.Load() + 1;
            Stats.CurrentConsecutiveInaudibleWetBufferCount.Store(
                ConsecutiveInaudibleCount);
            StoreMaximum(
                Stats.MaxConsecutiveInaudibleWetBufferCount,
                ConsecutiveInaudibleCount);
        }

        const bool bWetRmsPresent =
            FMath::IsFinite(WetRms)
            && WetRms > RmsPresenceThreshold;
        if (bWetRmsPresent)
        {
            AddSaturating(Stats.WetPresentInputBufferCount, 1);
            Stats.CurrentConsecutiveSilentWetBufferCount.Store(0);
        }
        else
        {
            const uint64 CurrentSilentCount =
                Stats.CurrentConsecutiveSilentWetBufferCount.Load();
            const uint64 ConsecutiveSilentCount =
                CurrentSilentCount < TNumericLimits<uint64>::Max()
                    ? CurrentSilentCount + 1
                    : CurrentSilentCount;
            Stats.CurrentConsecutiveSilentWetBufferCount.Store(
                ConsecutiveSilentCount);
            StoreMaximum(
                Stats.MaxConsecutiveSilentWetBufferCount,
                ConsecutiveSilentCount);
        }
    }
    else
    {
        // A buffer without measurable input cannot demonstrate a wet-path
        // dropout and breaks a run of consecutive input-bearing buffers.
        Stats.CurrentConsecutiveSilentWetBufferCount.Store(0);
    }
    AddSaturating(
        Stats.IntegratedInputMeanSquareFixed,
        QuantizeWeightedMeanSquare(InputRms, NumFrames));
    AddSaturating(
        Stats.IntegratedWetMeanSquareFixed,
        QuantizeWeightedMeanSquare(WetRms, NumFrames));
    StoreMaximum(
        Stats.MaxWetToInputRmsRatioFixed,
        QuantizeRms(WetToInputRmsRatio));
    Stats.PublishedEpoch.Store(RequestedEpoch);
    Stats.SnapshotSequence.Store(WriteSequence + 1);
}

void FUERayTracingAudioAudioDiagnostics::RecordFinalOutput(
    const EUERayTracingAudioRuntimeDataSource DataSource,
    const uint64 AudioComponentId,
    const float PeakAbsoluteOutput,
    const uint64 OverUnitOutputSampleCount,
    const uint64 NonFiniteOutputSampleCount)
{
    if (!IsEnabledFor(AudioComponentId))
    {
        return;
    }

    FAtomicDataSourceStats& Stats =
        GDataSourceStats[GetDataSourceIndex(DataSource)];
    uint64 StableSequence = Stats.SnapshotSequence.Load();
    const uint64 WriteSequence = StableSequence + 1;
    if ((StableSequence & 1ULL) != 0
        || !Stats.SnapshotSequence.CompareExchange(
            StableSequence,
            WriteSequence))
    {
        return;
    }

    const uint64 RequestedEpoch = Stats.RequestedEpoch.Load();
    if (Stats.PublishedEpoch.Load() == RequestedEpoch)
    {
        AddSaturating(Stats.FinalOutputBufferCount, 1);
        AddSaturating(
            Stats.OverUnitOutputSampleCount,
            OverUnitOutputSampleCount);
        AddSaturating(
            Stats.NonFiniteSampleCount,
            NonFiniteOutputSampleCount);
        StoreMaximum(
            Stats.MaxOutputPeakFixed,
            QuantizeRms(PeakAbsoluteOutput));
    }
    // A reset between Occlusion and Spatialization intentionally drops this
    // final-output observation. The next complete buffer initializes and
    // publishes the new epoch without exposing mixed-epoch statistics.
    Stats.SnapshotSequence.Store(WriteSequence + 1);
}

FUERayTracingAudioDataSourceAudioStats FUERayTracingAudioAudioDiagnostics::Read(
    const EUERayTracingAudioRuntimeDataSource DataSource)
{
    const FAtomicDataSourceStats& Stats = GDataSourceStats[GetDataSourceIndex(DataSource)];
    for (int32 Attempt = 0;
        Attempt < MaximumSnapshotReadAttempts;
        ++Attempt)
    {
        const uint64 RequestedEpochBefore = Stats.RequestedEpoch.Load();
        const uint64 SequenceBefore = Stats.SnapshotSequence.Load();
        if ((SequenceBefore & 1ULL) != 0)
        {
            continue;
        }

        const uint64 PublishedEpoch = Stats.PublishedEpoch.Load();
        FUERayTracingAudioDataSourceAudioStats Result;
        Result.BufferCount = Stats.BufferCount.Load();
        Result.FrameCount = Stats.FrameCount.Load();
        Result.NonSilentInputBufferCount = Stats.NonSilentInputBufferCount.Load();
        Result.NonSilentBufferCount = Stats.NonSilentBufferCount.Load();
        Result.RmsMeasuredBufferCount = Stats.RmsMeasuredBufferCount.Load();
        Result.AudibleWetBufferCount = Stats.AudibleWetBufferCount.Load();
        Result.MaxConsecutiveInaudibleWetBufferCount =
            Stats.MaxConsecutiveInaudibleWetBufferCount.Load();
        Result.WetPresentInputBufferCount =
            Stats.WetPresentInputBufferCount.Load();
        Result.MaxConsecutiveSilentWetBufferCount =
            Stats.MaxConsecutiveSilentWetBufferCount.Load();
        Result.NonFiniteSampleCount = Stats.NonFiniteSampleCount.Load();
        const uint64 FinalOutputBufferCount =
            Stats.FinalOutputBufferCount.Load();
        const uint64 FinalOverUnitOutputSampleCount =
            Stats.OverUnitOutputSampleCount.Load();
        const uint64 PreSpatializationOverUnitSampleCount =
            Stats.PreSpatializationOverUnitSampleCount.Load();
        const uint64 MaxInputRmsFixed = Stats.MaxInputRmsFixed.Load();
        const uint64 MaxWetRmsFixed = Stats.MaxWetRmsFixed.Load();
        const uint64 FinalMaxOutputPeakFixed =
            Stats.MaxOutputPeakFixed.Load();
        const uint64 MaxPreSpatializationOutputPeakFixed =
            Stats.MaxPreSpatializationOutputPeakFixed.Load();
        const uint64 MaxWetToInputRmsRatioFixed =
            Stats.MaxWetToInputRmsRatioFixed.Load();
        const uint64 IntegratedWetMeanSquareFixed =
            Stats.IntegratedWetMeanSquareFixed.Load();
        const uint64 IntegratedInputMeanSquareFixed =
            Stats.IntegratedInputMeanSquareFixed.Load();

        const uint64 SequenceAfter = Stats.SnapshotSequence.Load();
        const uint64 RequestedEpochAfter = Stats.RequestedEpoch.Load();
        if (SequenceBefore != SequenceAfter
            || (SequenceAfter & 1ULL) != 0
            || RequestedEpochBefore != RequestedEpochAfter
            || PublishedEpoch != RequestedEpochAfter)
        {
            continue;
        }

        Result.MaxInputRms = static_cast<float>(
            static_cast<double>(MaxInputRmsFixed)
            / RmsFixedPointScale);
        Result.MaxWetRms = static_cast<float>(
            static_cast<double>(MaxWetRmsFixed)
            / RmsFixedPointScale);
        const bool bHasFinalSpatializationOutput =
            FinalOutputBufferCount > 0;
        Result.OverUnitOutputSampleCount =
            bHasFinalSpatializationOutput
                ? FinalOverUnitOutputSampleCount
                : PreSpatializationOverUnitSampleCount;
        const uint64 SelectedMaxOutputPeakFixed =
            bHasFinalSpatializationOutput
                ? FinalMaxOutputPeakFixed
                : MaxPreSpatializationOutputPeakFixed;
        Result.MaxOutputPeak = static_cast<float>(
            static_cast<double>(SelectedMaxOutputPeakFixed)
            / RmsFixedPointScale);
        Result.MaxWetToInputRmsRatio = static_cast<float>(
            static_cast<double>(MaxWetToInputRmsRatioFixed)
            / RmsFixedPointScale);
        Result.IntegratedWetToInputRmsRatio =
            IntegratedInputMeanSquareFixed > 0
                ? static_cast<float>(FMath::Sqrt(
                    static_cast<double>(IntegratedWetMeanSquareFixed)
                    / static_cast<double>(IntegratedInputMeanSquareFixed)))
                : 0.0f;
        return Result;
    }

    // An empty result is a consistent snapshot for an unpublished reset epoch
    // or a writer that remained active throughout the bounded read attempts.
    return {};
}

void FUERayTracingAudioAudioDiagnostics::ResetHardRealtime()
{
    GHardRealtimeAudioCallbackCount.Store(0);
    GHardRealtimeCallbackCapacityMissCount.Store(0);
    GConvolutionPrepareCapacityDropCount.Store(0);
}

void FUERayTracingAudioAudioDiagnostics::
    RecordHardRealtimeCallback()
{
    ++GHardRealtimeAudioCallbackCount;
}

void FUERayTracingAudioAudioDiagnostics::
    RecordHardRealtimeCapacityMiss()
{
    ++GHardRealtimeCallbackCapacityMissCount;
}

void FUERayTracingAudioAudioDiagnostics::
    RecordConvolutionPrepareCapacityDrop()
{
    ++GConvolutionPrepareCapacityDropCount;
}

FUERayTracingAudioHardRealtimeStats
FUERayTracingAudioAudioDiagnostics::ReadHardRealtime()
{
    FUERayTracingAudioHardRealtimeStats Result;
    Result.AudioCallbackCount =
        GHardRealtimeAudioCallbackCount.Load();
    Result.CallbackCapacityMissCount =
        GHardRealtimeCallbackCapacityMissCount.Load();
    Result.ConvolutionPrepareCapacityDropCount =
        GConvolutionPrepareCapacityDropCount.Load();
    return Result;
}
