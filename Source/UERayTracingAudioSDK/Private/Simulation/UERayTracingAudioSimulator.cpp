#include "Simulation/UERayTracingAudioSimulator.h"

#include "GameFramework/Actor.h"
#include "Scene/UERayTracingAudioScene.h"

namespace
{
    float RadicalInverse(int32 Base, int32 Index)
    {
        float InverseBase = 1.0f / static_cast<float>(Base);
        float Fraction = InverseBase;
        float Result = 0.0f;

        while (Index > 0)
        {
            Result += static_cast<float>(Index % Base) * Fraction;
            Index /= Base;
            Fraction *= InverseBase;
        }

        return Result;
    }

    FVector GenerateSphereVolumeSample(int32 SampleIndex)
    {
        const float U = RadicalInverse(2, SampleIndex + 1);
        const float V = RadicalInverse(3, SampleIndex + 1);
        const float W = RadicalInverse(5, SampleIndex + 1);
        const float Radius = FMath::Pow(U, 1.0f / 3.0f);
        const float CosTheta = 1.0f - 2.0f * V;
        const float SinTheta = FMath::Sqrt(FMath::Max(0.0f, 1.0f - (CosTheta * CosTheta)));
        const float Phi = 2.0f * PI * W;
        return FVector(
            Radius * SinTheta * FMath::Cos(Phi),
            Radius * SinTheta * FMath::Sin(Phi),
            Radius * CosTheta);
    }
}

FUERayTracingAudioSimulator::FUERayTracingAudioSimulator(const FUERayTracingAudioContext& InContext)
    : Context(InContext)
{
}

FUERayTracingAudioDirectSimulationResult FUERayTracingAudioSimulator::SimulateDirectSound(
    const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
    const FUERayTracingAudioDirectSimulationInput& Input) const
{
    FUERayTracingAudioDirectSimulationResult Result;
    Result.bHasListener = true;
    Result.DistanceCm = FVector::Distance(Input.ListenerLocation, Input.SourceLocation);
    Result.bRayTracingAvailable = RayTracingDevice.IsRayTracingAvailable();

    const float SafeDistance = FMath::Max(Result.DistanceCm, Context.GetReferenceDistanceCm());
    const float FalloffRatio = SafeDistance / FMath::Max(Context.GetReferenceDistanceCm(), 1.0f);
    Result.DistanceAttenuation = 1.0f / FMath::Square(FalloffRatio);
    Result.DistanceAttenuation = FMath::Clamp(Result.DistanceAttenuation, 0.0f, 1.0f);

    const float DistanceMeters = Result.DistanceCm / 100.0f;
    Result.AirAbsorption.X = FMath::Exp(-Input.AirAbsorptionPerMeter.X * DistanceMeters);
    Result.AirAbsorption.Y = FMath::Exp(-Input.AirAbsorptionPerMeter.Y * DistanceMeters);
    Result.AirAbsorption.Z = FMath::Exp(-Input.AirAbsorptionPerMeter.Z * DistanceMeters);

    FUERayTracingAudioTraceRequest TraceRequest;
    TraceRequest.World = Input.World;
    TraceRequest.Scene = Input.Scene;
    TraceRequest.IgnoredActor = Input.ListenerActor;
    TraceRequest.SecondaryIgnoredActor = Input.SourceActor;

    float Visibility = 1.0f;
    if (Input.bUseVolumetricOcclusion && Input.SourceRadiusCm > UE_KINDA_SMALL_NUMBER && Input.NumOcclusionSamples > 1)
    {
        TArray<FUERayTracingAudioRay> Rays;
        Rays.Reserve(Input.NumOcclusionSamples * 2);

        TArray<FVector> SamplePoints;
        SamplePoints.Reserve(Input.NumOcclusionSamples);

        for (int32 SampleIndex = 0; SampleIndex < Input.NumOcclusionSamples; ++SampleIndex)
        {
            const FVector LocalSample = GenerateSphereVolumeSample(SampleIndex) * Input.SourceRadiusCm;
            SamplePoints.Add(Input.SourceLocation + LocalSample);
        }

        for (const FVector& SamplePoint : SamplePoints)
        {
            Rays.Add(FUERayTracingAudioRay{Input.SourceLocation, SamplePoint});
        }

        for (const FVector& SamplePoint : SamplePoints)
        {
            Rays.Add(FUERayTracingAudioRay{Input.ListenerLocation, SamplePoint});
        }

        TArray<bool> HitResults;
        if (RayTracingDevice.TraceRays(TraceRequest, Rays, HitResults) && HitResults.Num() == Rays.Num())
        {
            int32 NumValidSamples = 0;
            int32 NumVisibleSamples = 0;
            for (int32 SampleIndex = 0; SampleIndex < SamplePoints.Num(); ++SampleIndex)
            {
                const bool bSampleOccludedFromSource = HitResults[SampleIndex];
                if (bSampleOccludedFromSource)
                {
                    continue;
                }

                ++NumValidSamples;

                const bool bSampleOccludedFromListener = HitResults[SampleIndex + SamplePoints.Num()];
                if (!bSampleOccludedFromListener)
                {
                    ++NumVisibleSamples;
                }
            }

            Visibility = (NumValidSamples > 0)
                ? static_cast<float>(NumVisibleSamples) / static_cast<float>(NumValidSamples)
                : 0.0f;
        }
    }
    else
    {
        TraceRequest.Start = Input.ListenerLocation;
        TraceRequest.End = Input.SourceLocation;

        FUERayTracingAudioTraceHit TraceHit;
        Visibility = RayTracingDevice.TraceDirectPath(TraceRequest, TraceHit) ? 0.0f : 1.0f;
    }

    Result.DirectVisibility = FMath::Clamp(Visibility, 0.0f, 1.0f);
    Result.bIsOccluded = Result.DirectVisibility < (1.0f - KINDA_SMALL_NUMBER);
    Result.Occlusion = FMath::Lerp(FMath::Clamp(Input.OccludedGain, 0.0f, 1.0f), 1.0f, Result.DirectVisibility);

    const float AirAverage = (Result.AirAbsorption.X + Result.AirAbsorption.Y + Result.AirAbsorption.Z) / 3.0f;
    Result.OverallGain = Result.DistanceAttenuation * AirAverage * Result.Occlusion;
    Result.OverallGain = FMath::Clamp(Result.OverallGain, 0.0f, 1.0f);

    return Result;
}
