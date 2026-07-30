#pragma once

#include "CoreMinimal.h"

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioContextSettings
{
    float ReferenceDistanceCm = 100.0f;
    float MaxDistanceCm = 5000.0f;
    float SpeedOfSoundCmPerSecond = 34300.0f;
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioContext
{
public:
    explicit FUERayTracingAudioContext(
        const FUERayTracingAudioContextSettings& Settings = {});

    void Configure(const FUERayTracingAudioContextSettings& Settings);

    float GetReferenceDistanceCm() const;
    float GetMaxDistanceCm() const;
    float GetSpeedOfSoundCmPerSecond() const;

private:
    float ReferenceDistanceCm;
    float MaxDistanceCm;
    float SpeedOfSoundCmPerSecond;
};
