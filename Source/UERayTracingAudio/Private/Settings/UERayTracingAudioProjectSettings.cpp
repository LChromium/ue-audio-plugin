#include "Settings/UERayTracingAudioProjectSettings.h"

UUERayTracingAudioProjectSettings::UUERayTracingAudioProjectSettings()
    : ReferenceDistanceCm(100.0f)
    , MaxDistanceCm(5000.0f)
    , SpeedOfSoundCmPerSecond(34300.0f)
    , AirAbsorptionLowMidCrossoverHz(500.0f)
    , AirAbsorptionMidHighCrossoverHz(4000.0f)
{
}

FUERayTracingAudioContextSettings
UUERayTracingAudioProjectSettings::GetValidatedContextSettings() const
{
    FUERayTracingAudioContextSettings Result;
    Result.ReferenceDistanceCm = FMath::Max(ReferenceDistanceCm, 1.0f);
    Result.MaxDistanceCm = FMath::Max(MaxDistanceCm, Result.ReferenceDistanceCm);
    Result.SpeedOfSoundCmPerSecond = FMath::Max(SpeedOfSoundCmPerSecond, 1.0f);
    return Result;
}

FVector2f UUERayTracingAudioProjectSettings::GetValidatedAirAbsorptionCrossoversHz(
    const float SampleRate) const
{
    const float Nyquist = FMath::Max(SampleRate * 0.5f, 40.0f);
    const float LowMid = FMath::Clamp(AirAbsorptionLowMidCrossoverHz, 20.0f, Nyquist);
    const float MidHigh = FMath::Clamp(AirAbsorptionMidHighCrossoverHz, LowMid, Nyquist);
    return FVector2f(LowMid, MidHigh);
}

FName UUERayTracingAudioProjectSettings::GetCategoryName() const
{
    return TEXT("Plugins");
}
