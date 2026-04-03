#include "API/UERayTracingAudioContext.h"

FUERayTracingAudioContext::FUERayTracingAudioContext()
    : ReferenceDistanceCm(100.0f)
    , MaxDistanceCm(5000.0f)
    , SpeedOfSoundCmPerSecond(34300.0f)
{
}

float FUERayTracingAudioContext::GetReferenceDistanceCm() const
{
    return ReferenceDistanceCm;
}

float FUERayTracingAudioContext::GetMaxDistanceCm() const
{
    return MaxDistanceCm;
}

float FUERayTracingAudioContext::GetSpeedOfSoundCmPerSecond() const
{
    return SpeedOfSoundCmPerSecond;
}
