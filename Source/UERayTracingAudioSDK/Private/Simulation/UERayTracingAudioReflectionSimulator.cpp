#include "Simulation/UERayTracingAudioReflectionSimulator.h"

#include "Scene/UERayTracingAudioScene.h"

namespace UERayTracingAudioReflectionSimulatorPrivate
{
    constexpr float MinimumReflectionPathEnergy = 1e-8f;

    struct FAcousticSceneHit
    {
        bool bHit = false;
        FVector Location = FVector::ZeroVector;
        FVector Normal = FVector::UpVector;
        FVector Absorption = FVector::ZeroVector;
        FVector Transmission = FVector::ZeroVector;
        float Scattering = 0.0f;
        float Distance = 0.0f;
    };

    struct FReflectionPathState
    {
        FVector RayOrigin = FVector::ZeroVector;
        FVector RayDirection = FVector::ForwardVector;
        FVector ListenerDirection = FVector::ForwardVector;
        FVector Throughput = FVector::OneVector;
        float TravelDistance = 0.0f;
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

    FVector GenerateMaterialBounceDirection(
        const FVector& IncomingDirection,
        const FVector& SurfaceNormal,
        float Scattering,
        int32 SampleIndex)
    {
        const FVector SpecularDirection = IncomingDirection.MirrorByVector(SurfaceNormal).GetSafeNormal();
        FVector DiffuseDirection = GenerateSphereDirectionSample(SampleIndex);
        if (FVector::DotProduct(DiffuseDirection, SurfaceNormal) < 0.0f)
        {
            DiffuseDirection *= -1.0f;
        }
        DiffuseDirection = (DiffuseDirection + SurfaceNormal).GetSafeNormal();
        return FMath::Lerp(SpecularDirection, DiffuseDirection, FMath::Clamp(Scattering, 0.0f, 1.0f)).GetSafeNormal();
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
            if (!GeometryExport.bVisibleForIndirectSound)
            {
                continue;
            }

            if (GeometryExport.bUseStaticMeshTriangles && GeometryExport.HasTriangleMesh())
            {
                for (int32 Index = 0; Index + 2 < GeometryExport.Indices.Num(); Index += 3)
                {
                    const FVector A = GeometryExport.GetVertexWorldPosition(GeometryExport.Indices[Index]);
                    const FVector B = GeometryExport.GetVertexWorldPosition(GeometryExport.Indices[Index + 1]);
                    const FVector C = GeometryExport.GetVertexWorldPosition(GeometryExport.Indices[Index + 2]);

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
                        OutHit.Transmission = GeometryExport.Transmission;
                        OutHit.Scattering = GeometryExport.Scattering;
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
                    OutHit.Transmission = GeometryExport.Transmission;
                    OutHit.Scattering = GeometryExport.Scattering;
                    bHasHit = true;
                }
            }
        }

        return bHasHit;
    }

    int32 GetDelayBinIndex(const FUERayTracingAudioMinimalEnergyField& EnergyField, float DelaySeconds)
    {
        if (EnergyField.DelayBinEnergy.IsEmpty() || EnergyField.DelayBinDurationSeconds <= UE_SMALL_NUMBER)
        {
            return INDEX_NONE;
        }

        return FMath::Clamp(
            FMath::FloorToInt(DelaySeconds / EnergyField.DelayBinDurationSeconds),
            0,
            EnergyField.DelayBinEnergy.Num() - 1);
    }

}

using namespace UERayTracingAudioReflectionSimulatorPrivate;

FUERayTracingAudioReflectionSimulator::FUERayTracingAudioReflectionSimulator(const FUERayTracingAudioContext& InContext)
    : Context(InContext)
{
}

void FUERayTracingAudioReflectionSimulator::Simulate(
    const FUERayTracingAudioIndirectSimulationInput& Input,
    float EarlyLateSplitSeconds,
    FUERayTracingAudioMinimalEnergyField& OutEnergyField,
    int32& OutNumValidContributions) const
{
    OutNumValidContributions = 0;
    OutEnergyField = FUERayTracingAudioMinimalEnergyField();
    OutEnergyField.DurationSeconds = Input.DurationSeconds;
    OutEnergyField.EarlyLateSplitSeconds = EarlyLateSplitSeconds;
    OutEnergyField.DelayBinEnergy.Init(FVector::ZeroVector, FMath::Max(Input.NumDelayBins, 1));
    OutEnergyField.DelayBinDirection.Init(FVector::ZeroVector, OutEnergyField.DelayBinEnergy.Num());
    OutEnergyField.DelayBinDurationSeconds = Input.DurationSeconds / static_cast<float>(OutEnergyField.DelayBinEnergy.Num());
    OutEnergyField.EarliestArrivalSeconds = 0.0f;

    if (!Input.Scene || Input.Scene->IsEmpty() || Input.NumReflectionRays <= 0 || Input.MaxReflectionBounces <= 0 || Input.DurationSeconds <= 0.0f)
    {
        return;
    }
    if (Input.Scene->HasInvalidGeometryForUsage(
        EUERayTracingAudioGeometryUsage::Indirect))
    {
        return;
    }

    const float ReferenceDistance = FMath::Max(Context.GetReferenceDistanceCm(), 1.0f);
    const float SpeedOfSound = FMath::Max(Context.GetSpeedOfSoundCmPerSecond(), 1.0f);
    const float MaxTraceDistance = FMath::Max(Context.GetMaxDistanceCm(), 100.0f);
    const FVector ListenerOffset = Input.ListenerForward.GetSafeNormal().IsNearlyZero()
        ? FVector::UpVector
        : Input.ListenerForward.GetSafeNormal();

    auto QueryIntersections = [&](const TArray<FUERayTracingAudioRay>& Rays, TArray<FAcousticSceneHit>& OutHits)
    {
        OutHits.SetNum(Rays.Num());

        for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
        {
            const FVector Segment = Rays[RayIndex].End - Rays[RayIndex].Start;
            const float Distance = Segment.Length();
            if (Distance <= UE_KINDA_SMALL_NUMBER)
            {
                OutHits[RayIndex] = FAcousticSceneHit();
                continue;
            }

            TraceAcousticScene(*Input.Scene, Rays[RayIndex].Start, Segment / Distance, Distance, OutHits[RayIndex]);
        }
    };

    auto QueryOcclusion = [&](const TArray<FUERayTracingAudioRay>& Rays, TArray<bool>& OutOccluded)
    {
        OutOccluded.SetNum(Rays.Num());

        for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
        {
            const FVector Segment = Rays[RayIndex].End - Rays[RayIndex].Start;
            const float Distance = Segment.Length();
            if (Distance <= UE_KINDA_SMALL_NUMBER)
            {
                OutOccluded[RayIndex] = false;
                continue;
            }

            FAcousticSceneHit Hit;
            OutOccluded[RayIndex] = TraceAcousticScene(*Input.Scene, Rays[RayIndex].Start, Segment / Distance, Distance - 1.0f, Hit);
        }
    };

    // add data structure
    TArray<FReflectionPathState> ActivePaths;
    ActivePaths.Reserve(Input.NumReflectionRays);
    for (int32 RayIndex = 0; RayIndex < Input.NumReflectionRays; ++RayIndex)
    {
        FReflectionPathState& Path = ActivePaths.AddDefaulted_GetRef();
        Path.RayDirection = GenerateSphereDirectionSample(RayIndex);
        Path.ListenerDirection = Path.RayDirection;
        Path.RayOrigin = Input.ListenerLocation + (Path.RayDirection + ListenerOffset) * 1.0f;
    }

    for (int32 BounceIndex = 0; BounceIndex < Input.MaxReflectionBounces && ActivePaths.Num() > 0; ++BounceIndex)
    {
        TArray<FUERayTracingAudioRay> BounceRays;
        BounceRays.Reserve(ActivePaths.Num());
        for (const FReflectionPathState& Path : ActivePaths)
        {
            BounceRays.Add(FUERayTracingAudioRay{ Path.RayOrigin, Path.RayOrigin + (Path.RayDirection * MaxTraceDistance) });
        }

        TArray<FAcousticSceneHit> BounceHits;
        QueryIntersections(BounceRays, BounceHits);

        TArray<FReflectionPathState> NextPaths;
        TArray<FUERayTracingAudioRay> SourceRays;
        TArray<int32> SourceRayPathIndices;
        NextPaths.Reserve(ActivePaths.Num());
        SourceRays.Reserve(ActivePaths.Num());
        SourceRayPathIndices.Reserve(ActivePaths.Num());

        for (int32 PathIndex = 0; PathIndex < ActivePaths.Num(); ++PathIndex)
        {
            const FAcousticSceneHit& Hit = BounceHits[PathIndex];
            if (!Hit.bHit)
            {
                continue;
            }

            const FVector VisibilityStart = Hit.Location + (Hit.Normal * 1.0f);
            SourceRays.Add(FUERayTracingAudioRay{ VisibilityStart, Input.SourceLocation });
            SourceRayPathIndices.Add(PathIndex);
        }

        TArray<bool> SourceOcclusionHits;
        QueryOcclusion(SourceRays, SourceOcclusionHits);
        TArray<bool> OccludedResults;
        OccludedResults.Init(true, ActivePaths.Num());
        for (int32 SourceRayIndex = 0; SourceRayIndex < SourceRayPathIndices.Num(); ++SourceRayIndex)
        {
            const int32 PathIndex = SourceRayPathIndices[SourceRayIndex];
            OccludedResults[PathIndex] = SourceOcclusionHits.IsValidIndex(SourceRayIndex) ? SourceOcclusionHits[SourceRayIndex] : true;
        }

        for (int32 PathIndex = 0; PathIndex < ActivePaths.Num(); ++PathIndex)
        {
            const FAcousticSceneHit& Hit = BounceHits[PathIndex];
            if (!Hit.bHit)
            {
                continue;
            }

            FReflectionPathState NextPath = ActivePaths[PathIndex];
            NextPath.TravelDistance += Hit.Distance;
            const FVector Reflection = FVector::OneVector - Hit.Absorption - Hit.Transmission;
            NextPath.Throughput.X *= FMath::Clamp(Reflection.X, 0.0f, 1.0f);
            NextPath.Throughput.Y *= FMath::Clamp(Reflection.Y, 0.0f, 1.0f);
            NextPath.Throughput.Z *= FMath::Clamp(Reflection.Z, 0.0f, 1.0f);

            const FVector VisibilityStart = Hit.Location + (Hit.Normal * 1.0f);
            const bool bOccluded = OccludedResults.IsValidIndex(PathIndex) ? OccludedResults[PathIndex] : true;

            if (!bOccluded)
            {
                const float SourceDistance = FVector::Distance(VisibilityStart, Input.SourceLocation);
                if (SourceDistance > UE_KINDA_SMALL_NUMBER)
                {
                    const float TotalDistance = NextPath.TravelDistance + SourceDistance;
                    const float DelaySeconds = TotalDistance / SpeedOfSound;
                    if (DelaySeconds <= Input.DurationSeconds)
                    {
                        const float DistanceMeters = TotalDistance / 100.0f;
                        FVector AirAttenuation;
                        AirAttenuation.X = FMath::Exp(-Input.AirAbsorptionPerMeter.X * DistanceMeters);
                        AirAttenuation.Y = FMath::Exp(-Input.AirAbsorptionPerMeter.Y * DistanceMeters);
                        AirAttenuation.Z = FMath::Exp(-Input.AirAbsorptionPerMeter.Z * DistanceMeters);

                        const float DistanceRatio = FMath::Max(TotalDistance, ReferenceDistance) / ReferenceDistance;
                        const float GeometricAttenuation = 1.0f / FMath::Square(DistanceRatio);
                        const float BounceAttenuation = 1.0f / static_cast<float>(BounceIndex + 1);
                        const FVector BandGain = NextPath.Throughput
                            * AirAttenuation
                            * (GeometricAttenuation * BounceAttenuation / static_cast<float>(Input.NumReflectionRays));
                        const float MonoEnergy = (BandGain.X + BandGain.Y + BandGain.Z) / 3.0f;

                        if (MonoEnergy > UERayTracingAudioReflectionSimulatorPrivate::MinimumReflectionPathEnergy)
                        {
                            const int32 DelayBinIndex = GetDelayBinIndex(OutEnergyField, DelaySeconds);
                            if (DelayBinIndex != INDEX_NONE)
                            {
                                OutEnergyField.DelayBinEnergy[DelayBinIndex] += BandGain;
                                OutEnergyField.DelayBinDirection[DelayBinIndex] += NextPath.ListenerDirection * MonoEnergy;
                                OutEnergyField.EarliestArrivalSeconds = (OutEnergyField.EarliestArrivalSeconds <= 0.0f)
                                    ? DelaySeconds
                                    : FMath::Min(OutEnergyField.EarliestArrivalSeconds, DelaySeconds);
                                ++OutNumValidContributions;
                            }
                        }
                    }
                }
            }

            NextPath.RayOrigin = VisibilityStart;
            NextPath.RayDirection = UERayTracingAudioReflectionSimulatorPrivate::GenerateMaterialBounceDirection(
                ActivePaths[PathIndex].RayDirection,
                Hit.Normal,
                Hit.Scattering,
                PathIndex + (BounceIndex * FMath::Max(ActivePaths.Num(), 1)));
            if (!NextPath.RayDirection.IsNearlyZero())
            {
                NextPaths.Add(NextPath);
            }
        }

        ActivePaths = MoveTemp(NextPaths);
    }

}
