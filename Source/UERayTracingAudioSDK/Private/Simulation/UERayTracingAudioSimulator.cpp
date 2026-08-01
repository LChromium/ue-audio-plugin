#include "Simulation/UERayTracingAudioSimulator.h"

#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Scene/UERayTracingAudioScene.h"
#include "Simulation/UERayTracingAudioReflectionSimulator.h"

namespace
{
    // Acoustic energy is routinely normalized per ray and can remain meaningful
    // several orders of magnitude below UE's geometry-oriented KINDA_SMALL_NUMBER.
    constexpr float MinimumIndirectEnergy = 1e-12f;

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
        TArray<FVector> SmoothedDelayBinDirection;
        float DurationSeconds = 0.0f;
        float DelayBinDurationSeconds = 0.0f;
        double LastUpdateTimeSeconds = 0.0;
    };

    TMap<TWeakObjectPtr<AActor>, FMinimalEnergyFieldHistory> GIndirectEnergyFieldHistory;

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
        if (EnergyField.DelayBinEnergy.IsEmpty() || EnergyField.DelayBinDurationSeconds <= UE_SMALL_NUMBER)
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
        EnergyField.DelayBinDirection.Init(FVector::ZeroVector, EnergyField.DelayBinEnergy.Num());
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

        const double NowSeconds = FPlatformTime::Seconds();
        for (auto HistoryIt = GIndirectEnergyFieldHistory.CreateIterator(); HistoryIt; ++HistoryIt)
        {
            if (!HistoryIt.Key().IsValid() || NowSeconds - HistoryIt.Value().LastUpdateTimeSeconds > 10.0)
            {
                HistoryIt.RemoveCurrent();
            }
        }

        const TWeakObjectPtr<AActor> SourceKey(const_cast<AActor*>(Input.SourceActor));
        FMinimalEnergyFieldHistory& History = GIndirectEnergyFieldHistory.FindOrAdd(SourceKey);
        if (History.SmoothedDelayBinEnergy.Num() != EnergyField.DelayBinEnergy.Num()
            || !FMath::IsNearlyEqual(History.DurationSeconds, EnergyField.DurationSeconds)
            || !FMath::IsNearlyEqual(History.DelayBinDurationSeconds, EnergyField.DelayBinDurationSeconds)
            || NowSeconds - History.LastUpdateTimeSeconds > 2.0)
        {
            History.SmoothedDelayBinEnergy = EnergyField.DelayBinEnergy;
            History.SmoothedDelayBinDirection = EnergyField.DelayBinDirection;
            if (History.SmoothedDelayBinDirection.Num() != EnergyField.DelayBinEnergy.Num())
            {
                History.SmoothedDelayBinDirection.Init(FVector::ZeroVector, EnergyField.DelayBinEnergy.Num());
            }
            History.DurationSeconds = EnergyField.DurationSeconds;
            History.DelayBinDurationSeconds = EnergyField.DelayBinDurationSeconds;
            History.LastUpdateTimeSeconds = NowSeconds;
            return;
        }

        const float DeltaTimeSeconds = FMath::Max(Input.DeltaTimeSeconds, 1.0f / 120.0f);
        constexpr float AttackTimeConstant = 0.12f;
        constexpr float ReleaseTimeConstant = 0.35f;

        for (int32 BinIndex = 0; BinIndex < EnergyField.DelayBinEnergy.Num(); ++BinIndex)
        {
            const float PreviousMonoEnergy = GetMonoEnergy(History.SmoothedDelayBinEnergy[BinIndex]);
            const float TargetMonoEnergy = GetMonoEnergy(EnergyField.DelayBinEnergy[BinIndex]);
            const float TimeConstant = TargetMonoEnergy >= PreviousMonoEnergy
                ? AttackTimeConstant
                : ReleaseTimeConstant;
            const float Alpha = FMath::Clamp(
                1.0f - FMath::Exp(-DeltaTimeSeconds / TimeConstant),
                0.0f,
                1.0f);
            History.SmoothedDelayBinEnergy[BinIndex] = FMath::Lerp(
                History.SmoothedDelayBinEnergy[BinIndex],
                EnergyField.DelayBinEnergy[BinIndex],
                Alpha);
            if (EnergyField.DelayBinDirection.IsValidIndex(BinIndex))
            {
                History.SmoothedDelayBinDirection[BinIndex] = FMath::Lerp(
                    History.SmoothedDelayBinDirection[BinIndex],
                    EnergyField.DelayBinDirection[BinIndex],
                    Alpha);
            }
        }

        EnergyField.DelayBinEnergy = History.SmoothedDelayBinEnergy;
        EnergyField.DelayBinDirection = History.SmoothedDelayBinDirection;
        History.LastUpdateTimeSeconds = NowSeconds;
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

        if (PeakEnergy <= MinimumIndirectEnergy)
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
            if (MonoEnergy <= MinimumIndirectEnergy)
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
                if (MonoEnergy <= MinimumIndirectEnergy)
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
            if (MonoEnergy <= MinimumIndirectEnergy)
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
            FMath::CeilToInt(
                OutResult.EnergyField.EarlyLateSplitSeconds
                    / FMath::Max(OutResult.EnergyField.DelayBinDurationSeconds, UE_SMALL_NUMBER)
                - 0.5f),
            1,
            OutResult.EnergyField.DelayBinEnergy.Num());

        FVector TotalBandEnergy = FVector::ZeroVector;
        float WeightedDelay = 0.0f;
        float TotalMonoEnergy = 0.0f;
        float TotalDirectionalMoment = 0.0f;
        float DominantDirectionalMoment = 0.0f;

        for (int32 BinIndex = 0; BinIndex < OutResult.EnergyField.DelayBinEnergy.Num(); ++BinIndex)
        {
            const FVector& BinEnergy = OutResult.EnergyField.DelayBinEnergy[BinIndex];
            const float MonoEnergy = GetMonoEnergy(BinEnergy);
            if (MonoEnergy <= MinimumIndirectEnergy)
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

            if (OutResult.EnergyField.DelayBinDirection.IsValidIndex(BinIndex))
            {
                const FVector& DirectionMoment = OutResult.EnergyField.DelayBinDirection[BinIndex];
                const float DirectionalMoment = DirectionMoment.Length();
                if (DirectionalMoment > MinimumIndirectEnergy)
                {
                    ++OutResult.DirectionalBinCount;
                    TotalDirectionalMoment += DirectionalMoment;
                    if (DirectionalMoment > DominantDirectionalMoment)
                    {
                        DominantDirectionalMoment = DirectionalMoment;
                        OutResult.DominantArrivalDirection =
                            DirectionMoment / DirectionalMoment;
                    }
                }
            }

            if (BinIndex >= EarlyLateSplitBinIndex)
            {
                OutResult.LateReverbGain += MonoEnergy;
            }
        }

        OutResult.IndirectGain = FMath::Clamp(TotalMonoEnergy, 0.0f, 1.0f);
        OutResult.DirectionalEnergyRatio = TotalMonoEnergy > MinimumIndirectEnergy
            ? FMath::Clamp(TotalDirectionalMoment / TotalMonoEnergy, 0.0f, 1.0f)
            : 0.0f;
        OutResult.bHasValidPaths = NumValidPaths > 0 || OutResult.IndirectGain > MinimumIndirectEnergy;
        OutResult.AverageDelaySeconds = (TotalMonoEnergy > MinimumIndirectEnergy) ? (WeightedDelay / TotalMonoEnergy) : 0.0f;
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

        const float AverageEnergy = FMath::Max(GetMonoEnergy(AverageBandEnergy), MinimumIndirectEnergy);
        OutResult.ParametricEq.X = FMath::Clamp(AverageBandEnergy.X / AverageEnergy, 0.25f, 2.0f);
        OutResult.ParametricEq.Y = FMath::Clamp(AverageBandEnergy.Y / AverageEnergy, 0.25f, 2.0f);
        OutResult.ParametricEq.Z = FMath::Clamp(AverageBandEnergy.Z / AverageEnergy, 0.25f, 2.0f);

        const int32 HybridTransitionBinIndex = FMath::Clamp(
            FMath::CeilToInt(OutResult.HybridTransitionSeconds
                / FMath::Max(OutResult.EnergyField.DelayBinDurationSeconds, UE_SMALL_NUMBER)),
            1,
            OutResult.EnergyField.DelayBinEnergy.Num());
        const int32 ImpulseResponseEndBinIndexExclusive = OutResult.bUsedHybrid
            ? HybridTransitionBinIndex
            : OutResult.EnergyField.DelayBinEnergy.Num();
        ReconstructImpulseResponse(
            OutResult.EnergyField,
            ImpulseResponseEndBinIndexExclusive,
            OutResult.ReconstructedImpulseResponse);

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

            if (OutResult.bUsedParametricTail
                && Input.EffectType == EUERayTracingAudioIndirectEffectType::Parametric)
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
    FUERayTracingAudioDirectSimulationQuery Query = BuildDirectSoundQuery(RayTracingDevice, Input);

    FUERayTracingAudioTraceRequest TraceRequest;
    TraceRequest.World = Input.World;
    TraceRequest.Scene = Input.Scene;
    TraceRequest.IgnoredActor = Input.ListenerActor;
    TraceRequest.SecondaryIgnoredActor = Input.SourceActor;

    TArray<bool> HitResults;
    RayTracingDevice.TraceRays(
        TraceRequest,
        Query.Rays,
        HitResults,
        &Query.BaseResult.bUsedHardwareRayTracing);
    return FinalizeDirectSound(Input, MoveTemp(Query), MoveTemp(HitResults));
}

FUERayTracingAudioDirectSimulationQuery FUERayTracingAudioSimulator::BuildDirectSoundQuery(
    const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
    const FUERayTracingAudioDirectSimulationInput& Input) const
{
    FUERayTracingAudioDirectSimulationQuery Query;
    FUERayTracingAudioDirectSimulationResult& Result = Query.BaseResult;
    Result.bHasListener = true;
    Result.DistanceCm = FVector::Distance(Input.ListenerLocation, Input.SourceLocation);
    Result.bRayTracingAvailable = RayTracingDevice.IsRayTracingAvailable();

    const float SafeDistance = FMath::Max(Result.DistanceCm, Context.GetReferenceDistanceCm());
    const float FalloffRatio = SafeDistance / FMath::Max(Context.GetReferenceDistanceCm(), 1.0f);
    // Free-field intensity falls as 1/r^2, while the audio renderer consumes
    // linear pressure/amplitude. Applying intensity directly to samples
    // attenuated distant direct sound twice.
    Result.DistanceAttenuation = 1.0f / FalloffRatio;
    Result.DistanceAttenuation = FMath::Clamp(Result.DistanceAttenuation, 0.0f, 1.0f);

    const float DistanceMeters = Result.DistanceCm / 100.0f;
    Result.AirAbsorption.X = FMath::Exp(-Input.AirAbsorptionPerMeter.X * DistanceMeters);
    Result.AirAbsorption.Y = FMath::Exp(-Input.AirAbsorptionPerMeter.Y * DistanceMeters);
    Result.AirAbsorption.Z = FMath::Exp(-Input.AirAbsorptionPerMeter.Z * DistanceMeters);

    // The configured maximum distance is an audible-range contract, not just
    // a ray length hint. Keep the boundary inclusive and avoid submitting any
    // propagation work once the source is strictly outside it.
    if (Result.DistanceCm > Context.GetMaxDistanceCm())
    {
        Result.DistanceAttenuation = 0.0f;
        Result.OverallGain = 0.0f;
        return Query;
    }

    if (Input.bUseVolumetricOcclusion && Input.SourceRadiusCm > UE_KINDA_SMALL_NUMBER && Input.NumOcclusionSamples > 1)
    {
        Query.bVolumetric = true;
        Query.NumSamplePoints = Input.NumOcclusionSamples;
        Query.Rays.Reserve(Input.NumOcclusionSamples * 2);

        TArray<FVector> SamplePoints;
        SamplePoints.Reserve(Input.NumOcclusionSamples);

        for (int32 SampleIndex = 0; SampleIndex < Input.NumOcclusionSamples; ++SampleIndex)
        {
            const FVector LocalSample = GenerateSphereVolumeSample(SampleIndex) * Input.SourceRadiusCm;
            SamplePoints.Add(Input.SourceLocation + LocalSample);
        }

        for (const FVector& SamplePoint : SamplePoints)
        {
            Query.Rays.Add(FUERayTracingAudioRay{Input.SourceLocation, SamplePoint});
        }

        for (const FVector& SamplePoint : SamplePoints)
        {
            Query.Rays.Add(FUERayTracingAudioRay{Input.ListenerLocation, SamplePoint});
        }
    }
    else
    {
        Query.Rays.Add(FUERayTracingAudioRay{Input.ListenerLocation, Input.SourceLocation});
    }

    return Query;
}

FUERayTracingAudioDirectSimulationResult FUERayTracingAudioSimulator::FinalizeDirectSound(
    const FUERayTracingAudioDirectSimulationInput& Input,
    FUERayTracingAudioDirectSimulationQuery&& Query,
    TArray<bool>&& HitResults) const
{
    FUERayTracingAudioDirectSimulationResult Result = MoveTemp(Query.BaseResult);
    float Visibility = 1.0f;

    if (Query.bVolumetric && Query.NumSamplePoints > 0 && HitResults.Num() == Query.Rays.Num())
    {
        int32 NumValidSamples = 0;
        int32 NumVisibleSamples = 0;
        for (int32 SampleIndex = 0; SampleIndex < Query.NumSamplePoints; ++SampleIndex)
        {
            if (HitResults[SampleIndex])
            {
                continue;
            }

            ++NumValidSamples;
            if (!HitResults[SampleIndex + Query.NumSamplePoints])
            {
                ++NumVisibleSamples;
            }
        }

        Visibility = (NumValidSamples > 0)
            ? static_cast<float>(NumVisibleSamples) / static_cast<float>(NumValidSamples)
            : 0.0f;
    }
    else if (!Query.bVolumetric && !HitResults.IsEmpty())
    {
        Visibility = HitResults[0] ? 0.0f : 1.0f;
    }

    Result.DirectVisibility = FMath::Clamp(Visibility, 0.0f, 1.0f);
    Result.bIsOccluded = Result.DirectVisibility < (1.0f - KINDA_SMALL_NUMBER);

    if (Input.bHardOcclusion)
    {
        // Hard mode: fully occluded → near 0; no OccludedGain floor
        Result.Occlusion = Result.DirectVisibility;
    }
    else
    {
        // Soft mode: lerp from OccludedGain floor to 1 so there's always some bleed
        Result.Occlusion = FMath::Lerp(FMath::Clamp(Input.OccludedGain, 0.0f, 1.0f), 1.0f, Result.DirectVisibility);
    }

    const float AirAverage = (Result.AirAbsorption.X + Result.AirAbsorption.Y + Result.AirAbsorption.Z) / 3.0f;
    Result.OverallGain = Result.DistanceAttenuation * AirAverage * Result.Occlusion;
    Result.OverallGain = FMath::Clamp(Result.OverallGain, 0.0f, 1.0f);

    return Result;
}

FUERayTracingAudioIndirectSimulationResult FUERayTracingAudioSimulator::SimulateIndirectSound(
    const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
    const FUERayTracingAudioIndirectSimulationInput& Input) const
{
    // This synchronous entry point is the explicit CPU fallback. Realtime and
    // bake hardware work must use SubmitIndirectEnergyField so the Game Thread
    // never waits for a render command or GPU readback.
    static_cast<void>(RayTracingDevice);

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
    const float EarlyLateSplitSeconds = FMath::Clamp(
        Input.EarlyLateSplitSeconds,
        Input.DurationSeconds / static_cast<float>(FMath::Max(Input.NumDelayBins, 1)),
        Input.DurationSeconds);
    ReflectionSimulator.Simulate(Input, EarlyLateSplitSeconds, EnergyField, NumValidContributions);
    ApplyTemporalSmoothing(Input, EnergyField, Result.bUsedTemporalSmoothing);
    DeriveIndirectResultFromEnergyField(Input, MoveTemp(EnergyField), NumValidContributions, Result.bUsedTemporalSmoothing, Result);

    return Result;
}

FUERayTracingAudioIndirectSimulationResult FUERayTracingAudioSimulator::FinalizeIndirectSound(
    const FUERayTracingAudioIndirectSimulationInput& Input,
    FUERayTracingAudioEnergyFieldTraceResult&& TraceResult) const
{
    FUERayTracingAudioIndirectSimulationResult Result;
    Result.bHasListener = true;
    Result.bUsedHybrid = Input.EffectType == EUERayTracingAudioIndirectEffectType::Hybrid;
    Result.bUsedParametricTail = Input.EffectType != EUERayTracingAudioIndirectEffectType::Convolution;
    Result.HybridTransitionSeconds = Input.DurationSeconds * FMath::Clamp(Input.HybridTransitionRatio, 0.05f, 0.95f);

    FUERayTracingAudioMinimalEnergyField EnergyField;
    EnergyField.DurationSeconds = Input.DurationSeconds;
    EnergyField.EarlyLateSplitSeconds = FMath::Clamp(
        Input.EarlyLateSplitSeconds,
        Input.DurationSeconds / static_cast<float>(FMath::Max(Input.NumDelayBins, 1)),
        Input.DurationSeconds);
    EnergyField.DelayBinEnergy = MoveTemp(TraceResult.DelayBinEnergy);
    if (EnergyField.DelayBinEnergy.IsEmpty())
    {
        EnergyField.DelayBinEnergy.Init(FVector::ZeroVector, FMath::Max(Input.NumDelayBins, 1));
    }
    EnergyField.DelayBinDirection = MoveTemp(TraceResult.DelayBinDirection);
    if (EnergyField.DelayBinDirection.Num() != EnergyField.DelayBinEnergy.Num())
    {
        EnergyField.DelayBinDirection.Init(FVector::ZeroVector, EnergyField.DelayBinEnergy.Num());
    }
    EnergyField.DelayBinDurationSeconds = Input.DurationSeconds / static_cast<float>(EnergyField.DelayBinEnergy.Num());
    EnergyField.EarliestArrivalSeconds = TraceResult.EarliestArrivalSeconds;

    ApplyTemporalSmoothing(Input, EnergyField, Result.bUsedTemporalSmoothing);
    DeriveIndirectResultFromEnergyField(
        Input,
        MoveTemp(EnergyField),
        TraceResult.NumValidContributions,
        Result.bUsedTemporalSmoothing,
        Result);
    return Result;
}
