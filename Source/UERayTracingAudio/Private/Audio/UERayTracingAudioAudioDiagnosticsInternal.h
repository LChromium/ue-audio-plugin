#pragma once

#include "CoreMinimal.h"

struct FUERayTracingAudioDirectDiagnosticsTargetToken
{
    uint64 AudioComponentId = 0;
    uint64 Generation = 0;

    bool IsValid() const
    {
        return AudioComponentId != 0
            && Generation != 0
            && (Generation & 1ULL) == 0;
    }
};

// Module-private seam shared by the diagnostics implementation and its
// deterministic concurrency regression test. The token contains plain values
// only and never owns or dereferences engine objects.
class FUERayTracingAudioAudioDiagnosticsInternal
{
public:
    static FUERayTracingAudioDirectDiagnosticsTargetToken CaptureTarget(
        uint64 AudioComponentId);

    static void RecordDirectBuffer(
        FUERayTracingAudioDirectDiagnosticsTargetToken Target,
        int32 NumFrames,
        float PeakAbsoluteInput,
        float DirectRms,
        float MaxBandGainStep,
        uint64 NonFiniteDirectSampleCount,
        uint64 OverUnitDirectSampleCount);

#if WITH_DEV_AUTOMATION_TESTS
    static void SeedDirectCountersForTesting(
        uint64 BufferCount,
        uint64 NonSilentInputBufferCount,
        uint64 DirectPresentInputBufferCount,
        uint64 CurrentConsecutiveSilentDirectBufferCount,
        uint64 MaxConsecutiveSilentDirectBufferCount,
        uint64 NonFiniteDirectSampleCount,
        uint64 OverUnitDirectSampleCount);
#endif
};
