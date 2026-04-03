#pragma once

#include "CoreMinimal.h"

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioContext
{
public:
    FUERayTracingAudioContext();

    float GetReferenceDistanceCm() const;
    float GetMaxDistanceCm() const;
    float GetSpeedOfSoundCmPerSecond() const;

private:
    float ReferenceDistanceCm;
    float MaxDistanceCm;
    float SpeedOfSoundCmPerSecond;
};
