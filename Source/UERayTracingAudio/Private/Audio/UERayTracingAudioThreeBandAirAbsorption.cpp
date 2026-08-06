#include "Audio/UERayTracingAudioThreeBandAirAbsorption.h"

namespace
{
    float CalculateFilterCoefficient(
        const float CutoffHz,
        const float SampleRate)
    {
        return 1.0f - FMath::Exp(
            -2.0f * PI * CutoffHz / SampleRate);
    }

    float SaturateToFiniteFloat(const double Value)
    {
        constexpr float MaximumFloat = TNumericLimits<float>::Max();
        if (Value > static_cast<double>(MaximumFloat))
        {
            return MaximumFloat;
        }
        if (Value < -static_cast<double>(MaximumFloat))
        {
            return -MaximumFloat;
        }
        return FMath::IsFinite(Value)
            ? static_cast<float>(Value)
            : 0.0f;
    }
}

void FUERayTracingAudioThreeBandAirAbsorption::Initialize(
    const int32 SampleRate,
    const int32 NumChannels,
    const float LowMidCrossoverHz,
    const float MidHighCrossoverHz)
{
    const float ValidSampleRate =
        static_cast<float>(FMath::Max(SampleRate, 1));
    LowMidCoefficient = CalculateFilterCoefficient(
        LowMidCrossoverHz,
        ValidSampleRate);
    MidHighCoefficient = CalculateFilterCoefficient(
        MidHighCrossoverHz,
        ValidSampleRate);
    ChannelStates.SetNumZeroed(FMath::Max(NumChannels, 0));
}

void FUERayTracingAudioThreeBandAirAbsorption::Reset()
{
    for (FChannelState& State : ChannelStates)
    {
        State.LowMid = 0.0f;
        State.MidHigh = 0.0f;
    }
}

bool FUERayTracingAudioThreeBandAirAbsorption::CanProcess(
    const int32 NumChannels) const
{
    return NumChannels > 0
        && NumChannels <= ChannelStates.Num();
}

float FUERayTracingAudioThreeBandAirAbsorption::ProcessSample(
    const float Input,
    const int32 ChannelIndex,
    const FVector& BandGains)
{
    if (!ChannelStates.IsValidIndex(ChannelIndex))
    {
        return Input;
    }
    if (!FMath::IsFinite(Input))
    {
        return 0.0f;
    }

    FChannelState& State = ChannelStates[ChannelIndex];
    State.LowMid +=
        LowMidCoefficient * (Input - State.LowMid);
    State.MidHigh +=
        MidHighCoefficient * (Input - State.MidHigh);
    const float Low = State.LowMid;
    const float Mid = State.MidHigh - State.LowMid;
    const float High = Input - State.MidHigh;
    const float LowGain = FMath::IsFinite(BandGains.X)
        ? BandGains.X
        : 1.0f;
    const float MidGain = FMath::IsFinite(BandGains.Y)
        ? BandGains.Y
        : 1.0f;
    const float HighGain = FMath::IsFinite(BandGains.Z)
        ? BandGains.Z
        : 1.0f;
    return SaturateToFiniteFloat(
        static_cast<double>(Low) * static_cast<double>(LowGain)
        + static_cast<double>(Mid) * static_cast<double>(MidGain)
        + static_cast<double>(High) * static_cast<double>(HighGain));
}
