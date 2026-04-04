#include "Simulation/UERayTracingAudioSimulator.h"

#include "GameFramework/Actor.h"
#include "Scene/UERayTracingAudioScene.h"
#include "Simulation/UERayTracingAudioReflectionSimulator.h"

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
        FVector BandGain = FVector::ZeroVector;
    };

    struct FMinimalEnergyFieldHistory
    {
        TArray<FVector> SmoothedDelayBinEnergy;
        float DurationSeconds = 0.0f;
        float DelayBinDurationSeconds = 0.0f;
    };

    TMap<const AActor*, FMinimalEnergyFieldHistory> GIndirectEnergyFieldHistory;

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

    float GetMonoEnergy(const FVector& BandEnergy)
    {
        return (BandEnergy.X + BandEnergy.Y + BandEnergy.Z) / 3.0f;
    }

    int32 GetDelayBinIndex(const FUERayTracingAudioMinimalEnergyField& EnergyField, float DelaySeconds)
    {
        if (EnergyField.DelayBinEnergy.IsEmpty() || EnergyField.DelayBinDurationSeconds <= UE_KINDA_SMALL_NUMBER)
        {
            return INDEX_NONE;
        }

        const int32 RawIndex = FMath::FloorToInt(DelaySeconds / EnergyField.DelayBinDurationSeconds);
        return FMath::Clamp(RawIndex, 0, EnergyField.DelayBinEnergy.Num() - 1);
    }

    FUERayTracingAudioMinimalEnergyField BuildMinimalEnergyField(
        const TArray<FIndirectPathContribution>& Contributions,
        const FUERayTracingAudioIndirectSimulationInput& Input,
        float EarlyLateSplitSeconds)
    {
        FUERayTracingAudioMinimalEnergyField EnergyField;
        EnergyField.DurationSeconds = Input.DurationSeconds;
        EnergyField.EarlyLateSplitSeconds = EarlyLateSplitSeconds;
        EnergyField.DelayBinEnergy.Init(FVector::ZeroVector, FMath::Max(Input.NumDelayBins, 1));
        EnergyField.DelayBinDurationSeconds = Input.DurationSeconds / static_cast<float>(EnergyField.DelayBinEnergy.Num());
        EnergyField.EarliestArrivalSeconds = Input.DurationSeconds;

        for (const FIndirectPathContribution& Contribution : Contributions)
        {
            const int32 DelayBinIndex = GetDelayBinIndex(EnergyField, Contribution.DelaySeconds);
            if (DelayBinIndex == INDEX_NONE)
            {
                continue;
            }

            EnergyField.DelayBinEnergy[DelayBinIndex] += Contribution.BandGain;
            EnergyField.EarliestArrivalSeconds = FMath::Min(EnergyField.EarliestArrivalSeconds, Contribution.DelaySeconds);
        }

        if (Contributions.IsEmpty())
        {
            EnergyField.EarliestArrivalSeconds = 0.0f;
        }

        return EnergyField;
    }

    void ApplyTemporalSmoothing(
        const FUERayTracingAudioIndirectSimulationInput& Input,
        FUERayTracingAudioMinimalEnergyField& EnergyField,
        bool& bOutUsedTemporalSmoothing)
    {
        bOutUsedTemporalSmoothing = false;

        if (!Input.SourceActor || EnergyField.DelayBinEnergy.IsEmpty())
        {
            return;
        }

        FMinimalEnergyFieldHistory& History = GIndirectEnergyFieldHistory.FindOrAdd(Input.SourceActor);
        if (History.SmoothedDelayBinEnergy.Num() != EnergyField.DelayBinEnergy.Num()
            || !FMath::IsNearlyEqual(History.DurationSeconds, EnergyField.DurationSeconds)
            || !FMath::IsNearlyEqual(History.DelayBinDurationSeconds, EnergyField.DelayBinDurationSeconds))
        {
            History.SmoothedDelayBinEnergy = EnergyField.DelayBinEnergy;
            History.DurationSeconds = EnergyField.DurationSeconds;
            History.DelayBinDurationSeconds = EnergyField.DelayBinDurationSeconds;
            return;
        }

        const float DeltaTimeSeconds = FMath::Max(Input.DeltaTimeSeconds, 1.0f / 120.0f);
        const float SmoothingTimeConstant = 0.2f;
        const float Alpha = FMath::Clamp(1.0f - FMath::Exp(-DeltaTimeSeconds / SmoothingTimeConstant), 0.0f, 1.0f);

        for (int32 BinIndex = 0; BinIndex < EnergyField.DelayBinEnergy.Num(); ++BinIndex)
        {
            History.SmoothedDelayBinEnergy[BinIndex] = FMath::Lerp(
                History.SmoothedDelayBinEnergy[BinIndex],
                EnergyField.DelayBinEnergy[BinIndex],
                Alpha);
        }

        EnergyField.DelayBinEnergy = History.SmoothedDelayBinEnergy;
        bOutUsedTemporalSmoothing = true;
    }

    float EstimateBandReverbTime(
        const FUERayTracingAudioMinimalEnergyField& EnergyField,
        int32 StartBinIndex,
        int32 BandIndex)
    {
        if (EnergyField.DelayBinEnergy.IsEmpty() || !EnergyField.DelayBinEnergy.IsValidIndex(StartBinIndex))
        {
            return 0.0f;
        }

        float PeakEnergy = 0.0f;
        for (int32 BinIndex = StartBinIndex; BinIndex < EnergyField.DelayBinEnergy.Num(); ++BinIndex)
        {
            PeakEnergy = FMath::Max(PeakEnergy, EnergyField.DelayBinEnergy[BinIndex][BandIndex]);
        }

        if (PeakEnergy <= KINDA_SMALL_NUMBER)
        {
            return 0.0f;
        }

        const float Threshold = PeakEnergy * 0.001f;
        int32 LastActiveBinIndex = INDEX_NONE;
        for (int32 BinIndex = EnergyField.DelayBinEnergy.Num() - 1; BinIndex >= StartBinIndex; --BinIndex)
        {
            if (EnergyField.DelayBinEnergy[BinIndex][BandIndex] >= Threshold)
            {
                LastActiveBinIndex = BinIndex;
                break;
            }
        }

        if (LastActiveBinIndex == INDEX_NONE)
        {
            return 0.0f;
        }

        const float Duration = static_cast<float>(LastActiveBinIndex - StartBinIndex + 1) * EnergyField.DelayBinDurationSeconds;
        return FMath::Clamp(Duration, 0.15f, FMath::Max(EnergyField.DurationSeconds * 2.0f, 0.15f));
    }

    void ExtractEarlyReflectionTaps(
        const FUERayTracingAudioMinimalEnergyField& EnergyField,
        int32 EarlyLateSplitBinIndex,
        int32 MaxEarlyReflectionTaps,
        TArray<float>& OutDelaySeconds,
        TArray<float>& OutGains,
        float& OutEarlyReflectionGain)
    {
        struct FEarlyTapCandidate
        {
            int32 BinIndex = INDEX_NONE;
            float MonoEnergy = 0.0f;
        };

        TArray<FEarlyTapCandidate> Candidates;
        for (int32 BinIndex = 0; BinIndex < EarlyLateSplitBinIndex && BinIndex < EnergyField.DelayBinEnergy.Num(); ++BinIndex)
        {
            const float MonoEnergy = GetMonoEnergy(EnergyField.DelayBinEnergy[BinIndex]);
            if (MonoEnergy <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            OutEarlyReflectionGain += MonoEnergy;

            const float Previous = (BinIndex > 0) ? GetMonoEnergy(EnergyField.DelayBinEnergy[BinIndex - 1]) : 0.0f;
            const float Next = (BinIndex + 1 < EnergyField.DelayBinEnergy.Num()) ? GetMonoEnergy(EnergyField.DelayBinEnergy[BinIndex + 1]) : 0.0f;
            if (MonoEnergy >= Previous && MonoEnergy >= Next)
            {
                FEarlyTapCandidate& Candidate = Candidates.AddDefaulted_GetRef();
                Candidate.BinIndex = BinIndex;
                Candidate.MonoEnergy = MonoEnergy;
            }
        }

        if (Candidates.IsEmpty())
        {
            for (int32 BinIndex = 0; BinIndex < EarlyLateSplitBinIndex && BinIndex < EnergyField.DelayBinEnergy.Num(); ++BinIndex)
            {
                const float MonoEnergy = GetMonoEnergy(EnergyField.DelayBinEnergy[BinIndex]);
                if (MonoEnergy <= KINDA_SMALL_NUMBER)
                {
                    continue;
                }

                FEarlyTapCandidate& Candidate = Candidates.AddDefaulted_GetRef();
                Candidate.BinIndex = BinIndex;
                Candidate.MonoEnergy = MonoEnergy;
            }
        }

        Candidates.Sort([](const FEarlyTapCandidate& A, const FEarlyTapCandidate& B)
        {
            if (!FMath::IsNearlyEqual(A.MonoEnergy, B.MonoEnergy))
            {
                return A.MonoEnergy > B.MonoEnergy;
            }
            return A.BinIndex < B.BinIndex;
        });

        const int32 NumTaps = FMath::Min(MaxEarlyReflectionTaps, Candidates.Num());
        OutDelaySeconds.Reserve(NumTaps);
        OutGains.Reserve(NumTaps);

        Candidates.SetNum(NumTaps, EAllowShrinking::No);
        Candidates.Sort([](const FEarlyTapCandidate& A, const FEarlyTapCandidate& B)
        {
            return A.BinIndex < B.BinIndex;
        });

        for (const FEarlyTapCandidate& Candidate : Candidates)
        {
            OutDelaySeconds.Add((static_cast<float>(Candidate.BinIndex) + 0.5f) * EnergyField.DelayBinDurationSeconds);
            OutGains.Add(Candidate.MonoEnergy);
        }
    }

    void ReconstructImpulseResponse(
        const FUERayTracingAudioMinimalEnergyField& EnergyField,
        int32 EndBinIndexExclusive,
        TArray<float>& OutImpulseResponse)
    {
        OutImpulseResponse.Init(0.0f, EnergyField.DelayBinEnergy.Num());

        const int32 ClampedEndBinIndexExclusive = FMath::Clamp(EndBinIndexExclusive, 0, EnergyField.DelayBinEnergy.Num());
        for (int32 BinIndex = 0; BinIndex < ClampedEndBinIndexExclusive; ++BinIndex)
        {
            const float MonoEnergy = GetMonoEnergy(EnergyField.DelayBinEnergy[BinIndex]);
            if (MonoEnergy <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            OutImpulseResponse[BinIndex] = FMath::Sqrt(MonoEnergy);
        }
    }

    void DeriveIndirectResultFromEnergyField(
        const FUERayTracingAudioIndirectSimulationInput& Input,
        FUERayTracingAudioMinimalEnergyField&& EnergyField,
        int32 NumValidPaths,
        bool bUsedTemporalSmoothing,
        FUERayTracingAudioIndirectSimulationResult& OutResult)
    {
        OutResult.EnergyField = MoveTemp(EnergyField);
        OutResult.NumValidPaths = NumValidPaths;
        OutResult.bUsedTemporalSmoothing = bUsedTemporalSmoothing;
        OutResult.EarliestArrivalSeconds = 0.0f;

        const int32 EarlyLateSplitBinIndex = FMath::Clamp(
            FMath::CeilToInt(OutResult.EnergyField.EarlyLateSplitSeconds / FMath::Max(OutResult.EnergyField.DelayBinDurationSeconds, UE_KINDA_SMALL_NUMBER)),
            1,
            OutResult.EnergyField.DelayBinEnergy.Num());

        FVector TotalBandEnergy = FVector::ZeroVector;
        float WeightedDelay = 0.0f;
        float TotalMonoEnergy = 0.0f;

        for (int32 BinIndex = 0; BinIndex < OutResult.EnergyField.DelayBinEnergy.Num(); ++BinIndex)
        {
            const FVector& BinEnergy = OutResult.EnergyField.DelayBinEnergy[BinIndex];
            const float MonoEnergy = GetMonoEnergy(BinEnergy);
            if (MonoEnergy <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const float DelaySeconds = (static_cast<float>(BinIndex) + 0.5f) * OutResult.EnergyField.DelayBinDurationSeconds;
            if (OutResult.EarliestArrivalSeconds <= 0.0f)
            {
                OutResult.EarliestArrivalSeconds = DelaySeconds;
            }
            TotalBandEnergy += BinEnergy;
            TotalMonoEnergy += MonoEnergy;
            WeightedDelay += DelaySeconds * MonoEnergy;

            if (BinIndex >= EarlyLateSplitBinIndex)
            {
                OutResult.LateReverbGain += MonoEnergy;
            }
        }

        OutResult.IndirectGain = FMath::Clamp(TotalMonoEnergy, 0.0f, 1.0f);
        OutResult.bHasValidPaths = NumValidPaths > 0 || OutResult.IndirectGain > KINDA_SMALL_NUMBER;
        OutResult.AverageDelaySeconds = (TotalMonoEnergy > KINDA_SMALL_NUMBER) ? (WeightedDelay / TotalMonoEnergy) : 0.0f;
        OutResult.ParametricDelaySeconds = OutResult.EarliestArrivalSeconds;
        OutResult.ImpulseResponseBinDurationSeconds = OutResult.EnergyField.DelayBinDurationSeconds;

        ExtractEarlyReflectionTaps(
            OutResult.EnergyField,
            EarlyLateSplitBinIndex,
            Input.MaxEarlyReflectionTaps,
            OutResult.EarlyReflectionDelaySeconds,
            OutResult.EarlyReflectionGains,
            OutResult.EarlyReflectionGain);

        FVector AverageBandEnergy = FVector::ZeroVector;
        if (OutResult.EnergyField.DelayBinEnergy.Num() > 0)
        {
            AverageBandEnergy = TotalBandEnergy / static_cast<float>(OutResult.EnergyField.DelayBinEnergy.Num());
        }

        const float AverageEnergy = FMath::Max(GetMonoEnergy(AverageBandEnergy), KINDA_SMALL_NUMBER);
        OutResult.ParametricEq.X = FMath::Clamp(AverageBandEnergy.X / AverageEnergy, 0.25f, 2.0f);
        OutResult.ParametricEq.Y = FMath::Clamp(AverageBandEnergy.Y / AverageEnergy, 0.25f, 2.0f);
        OutResult.ParametricEq.Z = FMath::Clamp(AverageBandEnergy.Z / AverageEnergy, 0.25f, 2.0f);

        const int32 ImpulseResponseEndBinIndexExclusive = OutResult.bUsedHybrid
            ? EarlyLateSplitBinIndex
            : OutResult.EnergyField.DelayBinEnergy.Num();
        ReconstructImpulseResponse(
            OutResult.EnergyField,
            ImpulseResponseEndBinIndexExclusive,
            OutResult.ReconstructedImpulseResponse);

        if (OutResult.bUsedParametricTail)
        {
            const int32 LateStartBinIndex = FMath::Clamp(EarlyLateSplitBinIndex - 1, 0, FMath::Max(OutResult.EnergyField.DelayBinEnergy.Num() - 1, 0));
            OutResult.ReverbTimes.X = EstimateBandReverbTime(OutResult.EnergyField, LateStartBinIndex, 0);
            OutResult.ReverbTimes.Y = EstimateBandReverbTime(OutResult.EnergyField, LateStartBinIndex, 1);
            OutResult.ReverbTimes.Z = EstimateBandReverbTime(OutResult.EnergyField, LateStartBinIndex, 2);

            if (OutResult.ReverbTimes.IsNearlyZero())
            {
                const float EnergyScaleX = FMath::Clamp(TotalBandEnergy.X * 12.0f, 0.0f, 1.0f);
                const float EnergyScaleY = FMath::Clamp(TotalBandEnergy.Y * 12.0f, 0.0f, 1.0f);
                const float EnergyScaleZ = FMath::Clamp(TotalBandEnergy.Z * 12.0f, 0.0f, 1.0f);
                OutResult.ReverbTimes.X = FMath::Clamp(Input.DurationSeconds * FMath::Lerp(0.35f, 1.5f, EnergyScaleX), 0.15f, Input.DurationSeconds * 2.0f);
                OutResult.ReverbTimes.Y = FMath::Clamp(Input.DurationSeconds * FMath::Lerp(0.35f, 1.5f, EnergyScaleY), 0.15f, Input.DurationSeconds * 2.0f);
                OutResult.ReverbTimes.Z = FMath::Clamp(Input.DurationSeconds * FMath::Lerp(0.35f, 1.5f, EnergyScaleZ), 0.15f, Input.DurationSeconds * 2.0f);
            }

            if (Input.EffectType == EUERayTracingAudioIndirectEffectType::Parametric)
            {
                OutResult.EarlyReflectionDelaySeconds.Reset();
                OutResult.EarlyReflectionGains.Reset();
                OutResult.EarlyReflectionGain = 0.0f;
            }
        }
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

    FUERayTracingAudioReflectionSimulator ReflectionSimulator(Context);
    FUERayTracingAudioMinimalEnergyField EnergyField;
    int32 NumValidContributions = 0;
    ReflectionSimulator.Simulate(RayTracingDevice, Input, Result.HybridTransitionSeconds, EnergyField, NumValidContributions);
    ApplyTemporalSmoothing(Input, EnergyField, Result.bUsedTemporalSmoothing);
    DeriveIndirectResultFromEnergyField(Input, MoveTemp(EnergyField), NumValidContributions, Result.bUsedTemporalSmoothing, Result);

    return Result;
}
