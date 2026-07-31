#pragma once

#include "CoreMinimal.h"
#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Simulation/UERayTracingAudioSimulator.h"

#if WITH_UERAYTRACINGAUDIO_VALIDATION

enum class EUERayTracingAudioDirectSweepPhase : uint8
{
    Idle,
    ClearHold,
    EnteringWall,
    OccludedHold,
    Returning,
    Restoring,
    Complete,
    Failed
};

struct FUERayTracingAudioDirectSweepMetrics
{
    void Reset();
    void Observe(
        uint64 DirectGeneration,
        const FUERayTracingAudioDirectSimulationResult& Result);
    bool Passes(
        const FUERayTracingAudioDirectAudioStats& AudioStats,
        bool bHardwareObserved,
        bool bRestored) const;

    int32 GetGenerationCount() const { return GenerationCount; }
    float GetDistanceMinCm() const
    {
        return GenerationCount > 0 ? DistanceMinCm : 0.0f;
    }
    float GetDistanceMaxCm() const
    {
        return GenerationCount > 0 ? DistanceMaxCm : 0.0f;
    }
    float GetVisibilityMin() const
    {
        return GenerationCount > 0 ? VisibilityMin : 0.0f;
    }
    float GetVisibilityMax() const
    {
        return GenerationCount > 0 ? VisibilityMax : 0.0f;
    }
    float GetGainMin() const
    {
        return GenerationCount > 0 ? GainMin : 0.0f;
    }
    float GetGainMax() const
    {
        return GenerationCount > 0 ? GainMax : 0.0f;
    }

private:
    uint64 LastDirectGeneration = 0;
    int32 GenerationCount = 0;
    float DistanceMinCm = 0.0f;
    float DistanceMaxCm = 0.0f;
    float VisibilityMin = 0.0f;
    float VisibilityMax = 0.0f;
    float GainMin = 0.0f;
    float GainMax = 0.0f;
    bool bFiniteObservations = true;
    bool bSawHardwareObservation = false;
    bool bAllObservationsUsedHardware = true;
    bool bSawInitialClear = false;
    bool bSawOccludedAfterClear = false;
    bool bSawReturnedClear = false;
};

class FUERayTracingAudioDirectSweepTrajectory
{
public:
    static FVector Evaluate(
        const FVector& ListenerLocation,
        float NormalizedProgress);
};

class FUERayTracingAudioDirectSweepPolicy
{
public:
    static bool ShouldStartAutomatic(
        bool bRequested,
        bool bStarted,
        bool bValidationOwner,
        bool bBaselineValidationLogged,
        bool bHasDirectResult,
        uint64 DirectGeneration,
        bool bHardwareDirect);
    static bool HasHardwareWaitTimedOut(
        bool bRequested,
        bool bStarted,
        bool bValidationOwner,
        double ElapsedSeconds,
        double TimeoutSeconds);
};

#endif
