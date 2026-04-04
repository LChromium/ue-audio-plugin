#include "Simulation/UERayTracingAudioSimulator.h"

#include "GameFramework/Actor.h"
#include "Scene/UERayTracingAudioScene.h"

namespace
{
    struct FAcousticSceneHit
    {
        bool bHit = false;
        FVector Location = FVector::ZeroVector;
        FVector Normal = FVector::UpVector;
        FVector Absorption = FVector::ZeroVector;
        float Distance = 0.0f;
    };

    struct FIndirectPathContribution
    {
        float DelaySeconds = 0.0f;
        float MonoGain = 0.0f;
        FVector BandGain = FVector::ZeroVector;
    };

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

    FVector GenerateSphereDirectionSample(int32 SampleIndex)
    {
        const float U = RadicalInverse(2, SampleIndex + 1);
        const float V = RadicalInverse(3, SampleIndex + 1);
        const float CosTheta = 1.0f - 2.0f * U;
        const float SinTheta = FMath::Sqrt(FMath::Max(0.0f, 1.0f - (CosTheta * CosTheta)));
        const float Phi = 2.0f * PI * V;
        return FVector(
            SinTheta * FMath::Cos(Phi),
            SinTheta * FMath::Sin(Phi),
            CosTheta).GetSafeNormal();
    }

    bool IntersectTriangle(
        const FVector& RayOrigin,
        const FVector& RayDirection,
        const FVector& A,
        const FVector& B,
        const FVector& C,
        float MaxDistance,
        float& OutDistance,
        FVector& OutNormal)
    {
        const FVector Edge1 = B - A;
        const FVector Edge2 = C - A;
        const FVector PVec = FVector::CrossProduct(RayDirection, Edge2);
        const float Determinant = FVector::DotProduct(Edge1, PVec);
        if (FMath::Abs(Determinant) < KINDA_SMALL_NUMBER)
        {
            return false;
        }

        const float InverseDeterminant = 1.0f / Determinant;
        const FVector TVec = RayOrigin - A;
        const float U = FVector::DotProduct(TVec, PVec) * InverseDeterminant;
        if (U < 0.0f || U > 1.0f)
        {
            return false;
        }

        const FVector QVec = FVector::CrossProduct(TVec, Edge1);
        const float V = FVector::DotProduct(RayDirection, QVec) * InverseDeterminant;
        if (V < 0.0f || U + V > 1.0f)
        {
            return false;
        }

        const float Distance = FVector::DotProduct(Edge2, QVec) * InverseDeterminant;
        if (Distance <= UE_KINDA_SMALL_NUMBER || Distance >= MaxDistance)
        {
            return false;
        }

        OutDistance = Distance;
        OutNormal = FVector::CrossProduct(Edge1, Edge2).GetSafeNormal();
        if (FVector::DotProduct(OutNormal, RayDirection) > 0.0f)
        {
            OutNormal *= -1.0f;
        }
        return true;
    }

    bool IntersectBounds(
        const FVector& RayOrigin,
        const FVector& RayDirection,
        const FBox& Bounds,
        float MaxDistance,
        float& OutDistance,
        FVector& OutNormal)
    {
        float TMin = 0.0f;
        float TMax = MaxDistance;
        FVector HitNormal = FVector::ZeroVector;

        for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
        {
            const float OriginAxis = RayOrigin[AxisIndex];
            const float DirectionAxis = RayDirection[AxisIndex];
            const float MinAxis = Bounds.Min[AxisIndex];
            const float MaxAxis = Bounds.Max[AxisIndex];

            if (FMath::IsNearlyZero(DirectionAxis))
            {
                if (OriginAxis < MinAxis || OriginAxis > MaxAxis)
                {
                    return false;
                }
                continue;
            }

            const float InverseDirection = 1.0f / DirectionAxis;
            float T1 = (MinAxis - OriginAxis) * InverseDirection;
            float T2 = (MaxAxis - OriginAxis) * InverseDirection;
            FVector AxisNormal1 = FVector::ZeroVector;
            FVector AxisNormal2 = FVector::ZeroVector;
            AxisNormal1[AxisIndex] = -1.0f;
            AxisNormal2[AxisIndex] = 1.0f;

            if (T1 > T2)
            {
                Swap(T1, T2);
                Swap(AxisNormal1, AxisNormal2);
            }

            if (T1 > TMin)
            {
                TMin = T1;
                HitNormal = AxisNormal1;
            }

            TMax = FMath::Min(TMax, T2);
            if (TMin > TMax)
            {
                return false;
            }
        }

        if (TMin <= UE_KINDA_SMALL_NUMBER || TMin >= MaxDistance)
        {
            return false;
        }

        OutDistance = TMin;
        OutNormal = HitNormal.IsNearlyZero() ? FVector::UpVector : HitNormal;
        return true;
    }

    bool TraceAcousticScene(
        const FUERayTracingAudioScene& Scene,
        const FVector& RayOrigin,
        const FVector& RayDirection,
        float MaxDistance,
        FAcousticSceneHit& OutHit)
    {
        OutHit = FAcousticSceneHit();

        if (Scene.IsEmpty())
        {
            return false;
        }

        float ClosestDistance = MaxDistance;
        bool bHasHit = false;

        for (const FUERayTracingAudioGeometryExport& GeometryExport : Scene.GetStaticGeometry())
        {
            if (!GeometryExport.bVisibleForDirectSound)
            {
                continue;
            }

            if (GeometryExport.bUseStaticMeshTriangles && GeometryExport.HasTriangleMesh())
            {
                for (int32 Index = 0; Index + 2 < GeometryExport.Indices.Num(); Index += 3)
                {
                    const FVector& A = GeometryExport.Vertices[GeometryExport.Indices[Index]];
                    const FVector& B = GeometryExport.Vertices[GeometryExport.Indices[Index + 1]];
                    const FVector& C = GeometryExport.Vertices[GeometryExport.Indices[Index + 2]];

                    float Distance = 0.0f;
                    FVector Normal = FVector::UpVector;
                    if (IntersectTriangle(RayOrigin, RayDirection, A, B, C, ClosestDistance, Distance, Normal))
                    {
                        ClosestDistance = Distance;
                        OutHit.bHit = true;
                        OutHit.Distance = Distance;
                        OutHit.Location = RayOrigin + (RayDirection * Distance);
                        OutHit.Normal = Normal;
                        OutHit.Absorption = GeometryExport.Absorption;
                        bHasHit = true;
                    }
                }
            }
            else if (GeometryExport.Bounds.IsValid)
            {
                float Distance = 0.0f;
                FVector Normal = FVector::UpVector;
                if (IntersectBounds(RayOrigin, RayDirection, GeometryExport.Bounds, ClosestDistance, Distance, Normal))
                {
                    ClosestDistance = Distance;
                    OutHit.bHit = true;
                    OutHit.Distance = Distance;
                    OutHit.Location = RayOrigin + (RayDirection * Distance);
                    OutHit.Normal = Normal;
                    OutHit.Absorption = GeometryExport.Absorption;
                    bHasHit = true;
                }
            }
        }

        return bHasHit;
    }

    bool IsVisibleInAcousticScene(
        const FUERayTracingAudioScene& Scene,
        const FVector& Start,
        const FVector& End)
    {
        const FVector Segment = End - Start;
        const float Distance = Segment.Length();
        if (Distance <= UE_KINDA_SMALL_NUMBER)
        {
            return true;
        }

        FAcousticSceneHit Hit;
        return !TraceAcousticScene(Scene, Start, Segment / Distance, Distance - 1.0f, Hit);
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

FUERayTracingAudioIndirectSimulationResult FUERayTracingAudioSimulator::SimulateIndirectSound(
    const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
    const FUERayTracingAudioIndirectSimulationInput& Input) const
{
    FUERayTracingAudioIndirectSimulationResult Result;
    Result.bHasListener = true;
    Result.bUsedHybrid = Input.EffectType == EUERayTracingAudioIndirectEffectType::Hybrid;
    Result.bUsedParametricTail = Input.EffectType != EUERayTracingAudioIndirectEffectType::Convolution;
    Result.HybridTransitionSeconds = Input.DurationSeconds * FMath::Clamp(Input.HybridTransitionRatio, 0.05f, 0.95f);

    if (!Input.Scene || Input.Scene->IsEmpty() || Input.NumReflectionRays <= 0 || Input.MaxReflectionBounces <= 0 || Input.DurationSeconds <= 0.0f)
    {
        return Result;
    }

    const float ReferenceDistance = FMath::Max(Context.GetReferenceDistanceCm(), 1.0f);
    const float SpeedOfSound = FMath::Max(Context.GetSpeedOfSoundCmPerSecond(), 1.0f);
    const float MaxTraceDistance = FMath::Max(Context.GetMaxDistanceCm(), 100.0f);
    const FVector SourceOffset = Input.SourceForward.GetSafeNormal().IsNearlyZero()
        ? FVector::UpVector
        : Input.SourceForward.GetSafeNormal();
    const bool bCanUseHardwareRayTracing = RayTracingDevice.IsRayTracingAvailable();

    FUERayTracingAudioTraceRequest TraceRequest;
    TraceRequest.World = Input.World;
    TraceRequest.Scene = Input.Scene;
    TraceRequest.IgnoredActor = Input.SourceActor;
    TraceRequest.SecondaryIgnoredActor = Input.ListenerActor;

    auto BuildSceneHitFromDetailed = [Input](const FUERayTracingAudioDetailedTraceHit& DetailedHit, FAcousticSceneHit& OutHit) -> bool
    {
        if (!DetailedHit.bHit || !Input.Scene || !Input.Scene->GetStaticGeometry().IsValidIndex(DetailedHit.GeometryIndex))
        {
            return false;
        }

        const FUERayTracingAudioGeometryExport& GeometryExport = Input.Scene->GetStaticGeometry()[DetailedHit.GeometryIndex];
        OutHit = FAcousticSceneHit();
        OutHit.bHit = true;
        OutHit.Location = DetailedHit.Location;
        OutHit.Normal = DetailedHit.Normal;
        OutHit.Distance = DetailedHit.Distance;
        OutHit.Absorption = GeometryExport.Absorption;
        return true;
    };

    auto TraceBounceBatch = [&](const TArray<FUERayTracingAudioRay>& Rays, TArray<FAcousticSceneHit>& OutBatchHits) -> bool
    {
        OutBatchHits.SetNum(Rays.Num());

        if (bCanUseHardwareRayTracing)
        {
            TArray<FUERayTracingAudioDetailedTraceHit> DetailedHits;
            if (RayTracingDevice.TraceDetailedRays(TraceRequest, Rays, DetailedHits) && DetailedHits.Num() == Rays.Num())
            {
                for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
                {
                    BuildSceneHitFromDetailed(DetailedHits[RayIndex], OutBatchHits[RayIndex]);
                }
                return true;
            }
        }

        for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
        {
            const FVector Segment = Rays[RayIndex].End - Rays[RayIndex].Start;
            const float Distance = Segment.Length();
            if (Distance <= UE_KINDA_SMALL_NUMBER)
            {
                OutBatchHits[RayIndex] = FAcousticSceneHit();
                continue;
            }

            TraceAcousticScene(*Input.Scene, Rays[RayIndex].Start, Segment / Distance, Distance, OutBatchHits[RayIndex]);
        }

        return true;
    };

    auto CheckVisibilityBatch = [&](const TArray<FUERayTracingAudioRay>& Rays, TArray<bool>& OutVisibility) -> bool
    {
        OutVisibility.SetNum(Rays.Num());

        if (bCanUseHardwareRayTracing)
        {
            TArray<FUERayTracingAudioDetailedTraceHit> DetailedHits;
            if (RayTracingDevice.TraceDetailedRays(TraceRequest, Rays, DetailedHits) && DetailedHits.Num() == Rays.Num())
            {
                for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
                {
                    OutVisibility[RayIndex] = !DetailedHits[RayIndex].bHit;
                }
                return true;
            }
        }

        for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
        {
            OutVisibility[RayIndex] = IsVisibleInAcousticScene(*Input.Scene, Rays[RayIndex].Start, Rays[RayIndex].End);
        }

        return true;
    };

    TArray<FIndirectPathContribution> Contributions;
    Contributions.Reserve(Input.NumReflectionRays * Input.MaxReflectionBounces);

    FVector TotalBandEnergy = FVector::ZeroVector;
    float TotalMonoEnergy = 0.0f;
    float WeightedDelay = 0.0f;

    struct FIndirectPathState
    {
        FVector RayOrigin = FVector::ZeroVector;
        FVector RayDirection = FVector::ForwardVector;
        FVector BandEnergy = FVector::OneVector;
        float TravelDistance = 0.0f;
    };

    TArray<FIndirectPathState> ActivePaths;
    ActivePaths.Reserve(Input.NumReflectionRays);

    for (int32 RayIndex = 0; RayIndex < Input.NumReflectionRays; ++RayIndex)
    {
        FIndirectPathState& Path = ActivePaths.AddDefaulted_GetRef();
        Path.RayDirection = GenerateSphereDirectionSample(RayIndex);
        Path.RayOrigin = Input.SourceLocation + (Path.RayDirection + SourceOffset) * (Input.SourceRadiusCm * 0.05f);
    }

    for (int32 BounceIndex = 0; BounceIndex < Input.MaxReflectionBounces && ActivePaths.Num() > 0; ++BounceIndex)
    {
        TArray<FUERayTracingAudioRay> BounceRays;
        BounceRays.Reserve(ActivePaths.Num());
        for (const FIndirectPathState& Path : ActivePaths)
        {
            BounceRays.Add(FUERayTracingAudioRay{ Path.RayOrigin, Path.RayOrigin + (Path.RayDirection * MaxTraceDistance) });
        }

        TArray<FAcousticSceneHit> BounceHits;
        TraceBounceBatch(BounceRays, BounceHits);

        TArray<FUERayTracingAudioRay> VisibilityRays;
        TArray<int32> VisibilityPathIndices;
        VisibilityRays.Reserve(ActivePaths.Num());
        VisibilityPathIndices.Reserve(ActivePaths.Num());

        for (int32 PathIndex = 0; PathIndex < ActivePaths.Num(); ++PathIndex)
        {
            const FAcousticSceneHit& Hit = BounceHits[PathIndex];
            if (!Hit.bHit)
            {
                continue;
            }

            const FIndirectPathState& Path = ActivePaths[PathIndex];
            const FVector VisibilityStart = Hit.Location + (Hit.Normal * 1.0f);
            VisibilityRays.Add(FUERayTracingAudioRay{ VisibilityStart, Input.ListenerLocation });
            VisibilityPathIndices.Add(PathIndex);
        }

        TArray<bool> VisibilityResults;
        CheckVisibilityBatch(VisibilityRays, VisibilityResults);

        for (int32 VisibilityIndex = 0; VisibilityIndex < VisibilityPathIndices.Num(); ++VisibilityIndex)
        {
            const int32 PathIndex = VisibilityPathIndices[VisibilityIndex];
            const FAcousticSceneHit& Hit = BounceHits[PathIndex];
            FIndirectPathState& Path = ActivePaths[PathIndex];

            Path.TravelDistance += Hit.Distance;
            Path.BandEnergy.X *= FMath::Clamp(1.0f - Hit.Absorption.X, 0.0f, 1.0f);
            Path.BandEnergy.Y *= FMath::Clamp(1.0f - Hit.Absorption.Y, 0.0f, 1.0f);
            Path.BandEnergy.Z *= FMath::Clamp(1.0f - Hit.Absorption.Z, 0.0f, 1.0f);

            const FVector VisibilityStart = Hit.Location + (Hit.Normal * 1.0f);
            const FVector ToListener = Input.ListenerLocation - VisibilityStart;
            const float ListenerDistance = ToListener.Length();

            if (ListenerDistance > UE_KINDA_SMALL_NUMBER && VisibilityResults.IsValidIndex(VisibilityIndex) && VisibilityResults[VisibilityIndex])
            {
                const float TotalDistance = Path.TravelDistance + ListenerDistance;
                const float DelaySeconds = TotalDistance / SpeedOfSound;
                if (DelaySeconds <= Input.DurationSeconds)
                {
                    const float DistanceMeters = TotalDistance / 100.0f;
                    FVector AirAttenuation;
                    AirAttenuation.X = FMath::Exp(-Input.AirAbsorptionPerMeter.X * DistanceMeters);
                    AirAttenuation.Y = FMath::Exp(-Input.AirAbsorptionPerMeter.Y * DistanceMeters);
                    AirAttenuation.Z = FMath::Exp(-Input.AirAbsorptionPerMeter.Z * DistanceMeters);

                    const FVector PathBandGain = Path.BandEnergy * AirAttenuation;
                    const float DistanceRatio = FMath::Max(TotalDistance, ReferenceDistance) / ReferenceDistance;
                    const float GeometricAttenuation = 1.0f / FMath::Square(DistanceRatio);
                    const float BounceAttenuation = 1.0f / static_cast<float>(BounceIndex + 1);
                    const float MonoGain = ((PathBandGain.X + PathBandGain.Y + PathBandGain.Z) / 3.0f)
                        * GeometricAttenuation
                        * BounceAttenuation
                        / static_cast<float>(Input.NumReflectionRays);

                    if (MonoGain > KINDA_SMALL_NUMBER)
                    {
                        FIndirectPathContribution& Contribution = Contributions.AddDefaulted_GetRef();
                        Contribution.DelaySeconds = DelaySeconds;
                        Contribution.MonoGain = MonoGain;
                        Contribution.BandGain = PathBandGain * (GeometricAttenuation * BounceAttenuation / static_cast<float>(Input.NumReflectionRays));

                        TotalMonoEnergy += MonoGain;
                        TotalBandEnergy += Contribution.BandGain;
                        WeightedDelay += DelaySeconds * MonoGain;
                    }
                }
            }
        }

        TArray<FIndirectPathState> NextPaths;
        NextPaths.Reserve(ActivePaths.Num());
        for (int32 PathIndex = 0; PathIndex < ActivePaths.Num(); ++PathIndex)
        {
            const FAcousticSceneHit& Hit = BounceHits[PathIndex];
            if (!Hit.bHit)
            {
                continue;
            }

            FIndirectPathState NextPath = ActivePaths[PathIndex];
            NextPath.RayOrigin = Hit.Location + (Hit.Normal * 1.0f);
            NextPath.RayDirection = ActivePaths[PathIndex].RayDirection.MirrorByVector(Hit.Normal).GetSafeNormal();
            if (!NextPath.RayDirection.IsNearlyZero())
            {
                NextPaths.Add(NextPath);
            }
        }

        ActivePaths = MoveTemp(NextPaths);
    }

    Contributions.Sort([](const FIndirectPathContribution& A, const FIndirectPathContribution& B)
    {
        return A.DelaySeconds < B.DelaySeconds;
    });

    Result.NumValidPaths = Contributions.Num();
    Result.bHasValidPaths = Contributions.Num() > 0;
    Result.IndirectGain = FMath::Clamp(TotalMonoEnergy, 0.0f, 1.0f);
    Result.AverageDelaySeconds = (TotalMonoEnergy > KINDA_SMALL_NUMBER) ? (WeightedDelay / TotalMonoEnergy) : 0.0f;

    const int32 NumEarlyTaps = FMath::Min(Input.MaxEarlyReflectionTaps, Contributions.Num());
    Result.EarlyReflectionDelaySeconds.Reserve(NumEarlyTaps);
    Result.EarlyReflectionGains.Reserve(NumEarlyTaps);

    for (int32 ContributionIndex = 0; ContributionIndex < Contributions.Num(); ++ContributionIndex)
    {
        const FIndirectPathContribution& Contribution = Contributions[ContributionIndex];
        if (ContributionIndex < NumEarlyTaps)
        {
            Result.EarlyReflectionDelaySeconds.Add(Contribution.DelaySeconds);
            Result.EarlyReflectionGains.Add(Contribution.MonoGain);
            Result.EarlyReflectionGain += Contribution.MonoGain;
        }

        if (Contribution.DelaySeconds >= Result.HybridTransitionSeconds)
        {
            Result.LateReverbGain += Contribution.MonoGain;
        }
    }

    if (Result.bUsedParametricTail)
    {
        const float EnergyScaleX = FMath::Clamp(TotalBandEnergy.X * 12.0f, 0.0f, 1.0f);
        const float EnergyScaleY = FMath::Clamp(TotalBandEnergy.Y * 12.0f, 0.0f, 1.0f);
        const float EnergyScaleZ = FMath::Clamp(TotalBandEnergy.Z * 12.0f, 0.0f, 1.0f);
        Result.ReverbTimes.X = FMath::Clamp(Input.DurationSeconds * FMath::Lerp(0.35f, 1.5f, EnergyScaleX), 0.15f, Input.DurationSeconds * 2.0f);
        Result.ReverbTimes.Y = FMath::Clamp(Input.DurationSeconds * FMath::Lerp(0.35f, 1.5f, EnergyScaleY), 0.15f, Input.DurationSeconds * 2.0f);
        Result.ReverbTimes.Z = FMath::Clamp(Input.DurationSeconds * FMath::Lerp(0.35f, 1.5f, EnergyScaleZ), 0.15f, Input.DurationSeconds * 2.0f);

        if (Input.EffectType == EUERayTracingAudioIndirectEffectType::Parametric)
        {
            Result.EarlyReflectionDelaySeconds.Reset();
            Result.EarlyReflectionGains.Reset();
            Result.EarlyReflectionGain = 0.0f;
        }
    }

    return Result;
}
