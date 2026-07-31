#pragma once

#include "CoreMinimal.h"

class FUERayTracingAudioThreeBandAirAbsorption
{
public:
    void Initialize(
        int32 SampleRate,
        int32 NumChannels,
        float LowMidCrossoverHz,
        float MidHighCrossoverHz);
    void Reset();
    bool CanProcess(int32 NumChannels) const;
    float ProcessSample(
        float Input,
        int32 ChannelIndex,
        const FVector& BandGains);

private:
    struct FChannelState
    {
        float LowMid = 0.0f;
        float MidHigh = 0.0f;
    };

    TArray<FChannelState> ChannelStates;
    float LowMidCoefficient = 0.0f;
    float MidHighCoefficient = 0.0f;
};
