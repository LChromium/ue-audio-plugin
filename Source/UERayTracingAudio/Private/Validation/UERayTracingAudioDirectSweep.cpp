#include "Validation/UERayTracingAudioDirectSweep.h"

#if WITH_UERAYTRACINGAUDIO_VALIDATION

namespace
{
    constexpr int32 MinimumDirectGenerations = 8;
    constexpr float MinimumSweepDistanceCm = 198.0f;
    constexpr float MaximumSweepDistanceCm = 202.0f;
    constexpr float OccludedVisibilityThreshold = 0.10f;
    constexpr float ClearVisibilityThreshold = 0.90f;
    constexpr float MaximumDirectGainStep = 0.01f;
}

void FUERayTracingAudioDirectSweepMetrics::Reset()
{
    LastDirectGeneration = 0;
    GenerationCount = 0;
    DistanceMinCm = TNumericLimits<float>::Max();
    DistanceMaxCm = TNumericLimits<float>::Lowest();
    VisibilityMin = TNumericLimits<float>::Max();
    VisibilityMax = TNumericLimits<float>::Lowest();
    GainMin = TNumericLimits<float>::Max();
    GainMax = TNumericLimits<float>::Lowest();
    bFiniteObservations = true;
    bSawHardwareObservation = false;
    bAllObservationsUsedHardware = true;
    bSawInitialClear = false;
    bSawOccludedAfterClear = false;
    bSawReturnedClear = false;
}

void FUERayTracingAudioDirectSweepMetrics::Observe(
    const uint64 DirectGeneration,
    const FUERayTracingAudioDirectSimulationResult& Result)
{
    if (DirectGeneration == 0
        || DirectGeneration <= LastDirectGeneration
        || !Result.bHasListener)
    {
        return;
    }

    LastDirectGeneration = DirectGeneration;
    ++GenerationCount;
    bSawHardwareObservation |=
        Result.bUsedHardwareRayTracing;
    bAllObservationsUsedHardware &=
        Result.bUsedHardwareRayTracing;
    if (!FMath::IsFinite(Result.DistanceCm)
        || !FMath::IsFinite(Result.DirectVisibility)
        || !FMath::IsFinite(Result.OverallGain))
    {
        bFiniteObservations = false;
        return;
    }

    DistanceMinCm = FMath::Min(
        DistanceMinCm,
        Result.DistanceCm);
    DistanceMaxCm = FMath::Max(
        DistanceMaxCm,
        Result.DistanceCm);
    VisibilityMin = FMath::Min(
        VisibilityMin,
        Result.DirectVisibility);
    VisibilityMax = FMath::Max(
        VisibilityMax,
        Result.DirectVisibility);
    GainMin = FMath::Min(GainMin, Result.OverallGain);
    GainMax = FMath::Max(GainMax, Result.OverallGain);

    if (!bSawInitialClear
        && Result.DirectVisibility >= ClearVisibilityThreshold)
    {
        bSawInitialClear = true;
    }
    else if (bSawInitialClear
        && !bSawOccludedAfterClear
        && Result.DirectVisibility <= OccludedVisibilityThreshold)
    {
        bSawOccludedAfterClear = true;
    }
    else if (bSawOccludedAfterClear
        && Result.DirectVisibility >= ClearVisibilityThreshold)
    {
        bSawReturnedClear = true;
    }
}

bool FUERayTracingAudioDirectSweepMetrics::Passes(
    const FUERayTracingAudioDirectAudioStats& AudioStats,
    const bool bHardwareObserved,
    const bool bRestored) const
{
    return GenerationCount >= MinimumDirectGenerations
        && bFiniteObservations
        && DistanceMinCm >= MinimumSweepDistanceCm
        && DistanceMaxCm <= MaximumSweepDistanceCm
        && VisibilityMin <= OccludedVisibilityThreshold
        && VisibilityMax >= ClearVisibilityThreshold
        && GainMin > 0.0f
        && bSawInitialClear
        && bSawOccludedAfterClear
        && bSawReturnedClear
        && AudioStats.BufferCount > 0
        && AudioStats.NonSilentInputBufferCount > 0
        && AudioStats.DirectPresentInputBufferCount
            == AudioStats.NonSilentInputBufferCount
        && AudioStats.MaxConsecutiveSilentDirectBufferCount == 0
        && AudioStats.NonFiniteDirectSampleCount == 0
        && AudioStats.OverUnitDirectSampleCount == 0
        && FMath::IsFinite(AudioStats.MaxBandGainStep)
        && AudioStats.MaxBandGainStep <= MaximumDirectGainStep
        && bHardwareObserved
        && bSawHardwareObservation
        && bAllObservationsUsedHardware
        && bRestored;
}

FVector FUERayTracingAudioDirectSweepTrajectory::Evaluate(
    const FVector& ListenerLocation,
    const float NormalizedProgress)
{
    const float Alpha = FMath::Clamp(
        NormalizedProgress,
        0.0f,
        1.0f);
    const float Angle = FMath::Lerp(
        PI * 0.5f,
        0.0f,
        Alpha);
    return ListenerLocation + FVector(
        200.0f * FMath::Cos(Angle),
        200.0f * FMath::Sin(Angle),
        0.0f);
}

bool FUERayTracingAudioDirectSweepPolicy::ShouldStartAutomatic(
    const bool bRequested,
    const bool bStarted,
    const bool bValidationOwner,
    const bool bBaselineValidationLogged,
    const bool bHasDirectResult,
    const uint64 DirectGeneration,
    const bool bHardwareDirect)
{
    return bRequested
        && !bStarted
        && bValidationOwner
        && bBaselineValidationLogged
        && bHasDirectResult
        && DirectGeneration != 0
        && bHardwareDirect;
}

bool FUERayTracingAudioDirectSweepPolicy::
    HasHardwareWaitTimedOut(
        const bool bRequested,
        const bool bStarted,
        const bool bValidationOwner,
        const double ElapsedSeconds,
        const double TimeoutSeconds)
{
    return bRequested
        && !bStarted
        && bValidationOwner
        && FMath::IsFinite(ElapsedSeconds)
        && FMath::IsFinite(TimeoutSeconds)
        && TimeoutSeconds >= 0.0
        && ElapsedSeconds > TimeoutSeconds;
}

#endif
