#include "API/UERayTracingAudioContext.h"

FUERayTracingAudioContext::FUERayTracingAudioContext(
    const FUERayTracingAudioContextSettings& Settings)
{
    Configure(Settings);
}

void FUERayTracingAudioContext::Configure(
    const FUERayTracingAudioContextSettings& Settings)
{
    ReferenceDistanceCm = Settings.ReferenceDistanceCm;
    MaxDistanceCm = Settings.MaxDistanceCm;
    SpeedOfSoundCmPerSecond = Settings.SpeedOfSoundCmPerSecond;
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
