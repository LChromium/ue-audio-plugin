#include "Simulation/UERayTracingAudioReflectionSimulator.h"

#include "GameFramework/Actor.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "RHIResourceUtils.h"
#include "Scene/UERayTracingAudioScene.h"
#include "ShaderParameterStruct.h"

struct FShadeAndGatherPathInputGPU
{
    FVector4f RayOriginAndTravelDistance = FVector4f::Zero();
    FVector4f RayDirectionAndBounceIndex = FVector4f::Zero();
    FVector4f ThroughputAndHitDistance = FVector4f::Zero();
    FVector4f HitLocationAndHitValid = FVector4f::Zero();
    FVector4f HitNormalAndOccluded = FVector4f::Zero();
    FVector4f AbsorptionAndPadding = FVector4f::Zero();
};

struct FShadeAndGatherPathOutputGPU
{
    FVector4f NextOriginAndTravelDistance = FVector4f::Zero();
    FVector4f NextDirectionAndAlive = FVector4f::Zero();
    FVector4f ThroughputAndPadding = FVector4f::Zero();
};

class FUERayTracingAudioShadeAndGatherCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FUERayTracingAudioShadeAndGatherCS);
    SHADER_USE_PARAMETER_STRUCT(FUERayTracingAudioShadeAndGatherCS, FGlobalShader);

public:
    static constexpr uint32 ThreadGroupSizeX = 64;

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_X"), ThreadGroupSizeX);
    }

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(StructuredBuffer<FShadeAndGatherPathInputGPU>, PathInputs)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FShadeAndGatherPathOutputGPU>, PathOutputs)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, EnergyBins)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, ContributionCounter)
        SHADER_PARAMETER(uint32, NumPaths)
        SHADER_PARAMETER(uint32, NumDelayBins)
        SHADER_PARAMETER(uint32, NumReflectionRays)
        SHADER_PARAMETER(float, SpeedOfSound)
        SHADER_PARAMETER(float, ReferenceDistance)
        SHADER_PARAMETER(float, DurationSeconds)
        SHADER_PARAMETER(float, DelayBinDurationSeconds)
        SHADER_PARAMETER(float, EnergyQuantizationScale)
        SHADER_PARAMETER(FVector3f, SourceLocation)
        SHADER_PARAMETER(FVector3f, AirAbsorptionPerMeter)
    END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FUERayTracingAudioShadeAndGatherCS, "/Plugin/UERayTracingAudio/Private/Simulation/UERayTracingAudioEnergyField.usf", "ShadeAndGatherCS", SF_Compute);

namespace UERayTracingAudioReflectionSimulatorPrivate
{
    constexpr uint32 EnergyQuantizationScale = 1000000u;

    struct FAcousticSceneHit
    {
        bool bHit = false;
        FVector Location = FVector::ZeroVector;
        FVector Normal = FVector::UpVector;
        FVector Absorption = FVector::ZeroVector;
        float Distance = 0.0f;
    };

    struct FReflectionPathState
    {
        FVector RayOrigin = FVector::ZeroVector;
        FVector RayDirection = FVector::ForwardVector;
        FVector Throughput = FVector::OneVector;
        float TravelDistance = 0.0f;
    };

    struct FSourceGatherCandidate
    {
        FVector VisibilityStart = FVector::ZeroVector;
        FVector Throughput = FVector::ZeroVector;
        float TravelDistance = 0.0f;
        int32 BounceIndex = 0;
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

    int32 GetDelayBinIndex(const FUERayTracingAudioMinimalEnergyField& EnergyField, float DelaySeconds)
    {
        if (EnergyField.DelayBinEnergy.IsEmpty() || EnergyField.DelayBinDurationSeconds <= UE_KINDA_SMALL_NUMBER)
        {
            return INDEX_NONE;
        }

        return FMath::Clamp(
            FMath::FloorToInt(DelaySeconds / EnergyField.DelayBinDurationSeconds),
            0,
            EnergyField.DelayBinEnergy.Num() - 1);
    }

    bool DispatchShadeAndGatherOnGPU_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const TArray<FShadeAndGatherPathInputGPU>& PathInputs,
        const FUERayTracingAudioIndirectSimulationInput& Input,
        float SpeedOfSound,
        float ReferenceDistance,
        TArray<FReflectionPathState>& OutNextPaths,
        TArray<FVector>& OutEnergyBins,
        int32& OutContributionCount)
    {
        OutNextPaths.Reset();
        OutEnergyBins.Init(FVector::ZeroVector, FMath::Max(Input.NumDelayBins, 1));
        OutContributionCount = 0;

        if (PathInputs.IsEmpty())
        {
            return true;
        }

        FBufferRHIRef InputBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
            RHICmdList,
            TEXT("UERayTracingAudioShadeAndGatherInput"),
            BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
            ERHIAccess::SRVMask,
            MakeConstArrayView(PathInputs));

        FShaderResourceViewRHIRef InputBufferView = RHICmdList.CreateShaderResourceView(
            InputBuffer,
            FRHIViewDesc::CreateBufferSRV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(FShadeAndGatherPathInputGPU))
                .SetNumElements(PathInputs.Num()));

        const FRHIBufferCreateDesc OutputBufferCreateDesc =
            FRHIBufferCreateDesc::CreateStructured<FShadeAndGatherPathOutputGPU>(TEXT("UERayTracingAudioShadeAndGatherOutput"), PathInputs.Num())
            .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy)
            .SetInitialState(ERHIAccess::UAVCompute);
        FBufferRHIRef OutputBuffer = RHICmdList.CreateBuffer(OutputBufferCreateDesc);
        FUnorderedAccessViewRHIRef OutputBufferUAV = RHICmdList.CreateUnorderedAccessView(
            OutputBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(FShadeAndGatherPathOutputGPU))
                .SetNumElements(PathInputs.Num()));

        const uint32 NumEnergyValues = static_cast<uint32>(OutEnergyBins.Num() * 3);
        const FRHIBufferCreateDesc EnergyBufferCreateDesc =
            FRHIBufferCreateDesc::CreateStructured<uint32>(TEXT("UERayTracingAudioEnergyBins"), NumEnergyValues)
            .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy)
            .SetInitialState(ERHIAccess::UAVCompute);
        FBufferRHIRef EnergyBuffer = RHICmdList.CreateBuffer(EnergyBufferCreateDesc);
        FUnorderedAccessViewRHIRef EnergyBufferUAV = RHICmdList.CreateUnorderedAccessView(
            EnergyBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(uint32))
                .SetNumElements(NumEnergyValues));

        const FRHIBufferCreateDesc CounterBufferCreateDesc =
            FRHIBufferCreateDesc::CreateStructured<uint32>(TEXT("UERayTracingAudioContributionCounter"), 1)
            .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy)
            .SetInitialState(ERHIAccess::UAVCompute);
        FBufferRHIRef CounterBuffer = RHICmdList.CreateBuffer(CounterBufferCreateDesc);
        FUnorderedAccessViewRHIRef CounterBufferUAV = RHICmdList.CreateUnorderedAccessView(
            CounterBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(uint32))
                .SetNumElements(1));

        RHICmdList.ClearUAVUint(EnergyBufferUAV, FUintVector4(0u, 0u, 0u, 0u));
        RHICmdList.ClearUAVUint(CounterBufferUAV, FUintVector4(0u, 0u, 0u, 0u));

        TShaderMapRef<FUERayTracingAudioShadeAndGatherCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FUERayTracingAudioShadeAndGatherCS::FParameters Parameters;
        Parameters.PathInputs = InputBufferView;
        Parameters.PathOutputs = OutputBufferUAV;
        Parameters.EnergyBins = EnergyBufferUAV;
        Parameters.ContributionCounter = CounterBufferUAV;
        Parameters.NumPaths = static_cast<uint32>(PathInputs.Num());
        Parameters.NumDelayBins = static_cast<uint32>(OutEnergyBins.Num());
        Parameters.NumReflectionRays = static_cast<uint32>(Input.NumReflectionRays);
        Parameters.SpeedOfSound = SpeedOfSound;
        Parameters.ReferenceDistance = ReferenceDistance;
        Parameters.DurationSeconds = Input.DurationSeconds;
        Parameters.DelayBinDurationSeconds = Input.DurationSeconds / static_cast<float>(OutEnergyBins.Num());
        Parameters.EnergyQuantizationScale = static_cast<float>(EnergyQuantizationScale);
        Parameters.SourceLocation = FVector3f(Input.SourceLocation);
        Parameters.AirAbsorptionPerMeter = FVector3f(Input.AirAbsorptionPerMeter);

        FComputeShaderUtils::Dispatch(
            RHICmdList,
            ComputeShader,
            Parameters,
            FIntVector(FMath::DivideAndRoundUp(PathInputs.Num(), static_cast<int32>(FUERayTracingAudioShadeAndGatherCS::ThreadGroupSizeX)), 1, 1));

        RHICmdList.Transition(FRHITransitionInfo(OutputBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
        RHICmdList.Transition(FRHITransitionInfo(EnergyBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
        RHICmdList.Transition(FRHITransitionInfo(CounterBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
        RHICmdList.BlockUntilGPUIdle();

        const FShadeAndGatherPathOutputGPU* OutputData = static_cast<const FShadeAndGatherPathOutputGPU*>(
            RHICmdList.LockBuffer(OutputBuffer, 0, sizeof(FShadeAndGatherPathOutputGPU) * PathInputs.Num(), RLM_ReadOnly));
        const uint32* EnergyData = static_cast<const uint32*>(
            RHICmdList.LockBuffer(EnergyBuffer, 0, sizeof(uint32) * NumEnergyValues, RLM_ReadOnly));
        const uint32* CounterData = static_cast<const uint32*>(
            RHICmdList.LockBuffer(CounterBuffer, 0, sizeof(uint32), RLM_ReadOnly));

        if (!OutputData || !EnergyData || !CounterData)
        {
            if (OutputData)
            {
                RHICmdList.UnlockBuffer(OutputBuffer);
            }
            if (EnergyData)
            {
                RHICmdList.UnlockBuffer(EnergyBuffer);
            }
            if (CounterData)
            {
                RHICmdList.UnlockBuffer(CounterBuffer);
            }
            return false;
        }

        OutContributionCount = static_cast<int32>(CounterData[0]);
        for (int32 BinIndex = 0; BinIndex < OutEnergyBins.Num(); ++BinIndex)
        {
            const int32 BaseIndex = BinIndex * 3;
            OutEnergyBins[BinIndex].X = static_cast<float>(EnergyData[BaseIndex]) / static_cast<float>(EnergyQuantizationScale);
            OutEnergyBins[BinIndex].Y = static_cast<float>(EnergyData[BaseIndex + 1]) / static_cast<float>(EnergyQuantizationScale);
            OutEnergyBins[BinIndex].Z = static_cast<float>(EnergyData[BaseIndex + 2]) / static_cast<float>(EnergyQuantizationScale);
        }

        OutNextPaths.Reserve(PathInputs.Num());
        for (int32 PathIndex = 0; PathIndex < PathInputs.Num(); ++PathIndex)
        {
            const FShadeAndGatherPathOutputGPU& Output = OutputData[PathIndex];
            if (Output.NextDirectionAndAlive.W < 0.5f)
            {
                continue;
            }

            FReflectionPathState& NextPath = OutNextPaths.AddDefaulted_GetRef();
            NextPath.RayOrigin = FVector(Output.NextOriginAndTravelDistance.X, Output.NextOriginAndTravelDistance.Y, Output.NextOriginAndTravelDistance.Z);
            NextPath.TravelDistance = Output.NextOriginAndTravelDistance.W;
            NextPath.RayDirection = FVector(Output.NextDirectionAndAlive.X, Output.NextDirectionAndAlive.Y, Output.NextDirectionAndAlive.Z).GetSafeNormal();
            NextPath.Throughput = FVector(Output.ThroughputAndPadding.X, Output.ThroughputAndPadding.Y, Output.ThroughputAndPadding.Z);
        }

        RHICmdList.UnlockBuffer(OutputBuffer);
        RHICmdList.UnlockBuffer(EnergyBuffer);
        RHICmdList.UnlockBuffer(CounterBuffer);
        return true;
    }

    bool DispatchShadeAndGatherOnGPU(
        const TArray<FShadeAndGatherPathInputGPU>& PathInputs,
        const FUERayTracingAudioIndirectSimulationInput& Input,
        float SpeedOfSound,
        float ReferenceDistance,
        TArray<FReflectionPathState>& OutNextPaths,
        TArray<FVector>& OutEnergyBins,
        int32& OutContributionCount)
    {
        bool bSucceeded = false;

        ENQUEUE_RENDER_COMMAND(UERayTracingAudioShadeAndGather)(
            [&PathInputs, &Input, SpeedOfSound, ReferenceDistance, &OutNextPaths, &OutEnergyBins, &OutContributionCount, &bSucceeded](FRHICommandListImmediate& RHICmdList)
            {
                bSucceeded = DispatchShadeAndGatherOnGPU_RenderThread(
                    RHICmdList,
                    PathInputs,
                    Input,
                    SpeedOfSound,
                    ReferenceDistance,
                    OutNextPaths,
                    OutEnergyBins,
                    OutContributionCount);
            });

        FlushRenderingCommands();
        return bSucceeded;
    }
}

using namespace UERayTracingAudioReflectionSimulatorPrivate;

FUERayTracingAudioReflectionSimulator::FUERayTracingAudioReflectionSimulator(const FUERayTracingAudioContext& InContext)
    : Context(InContext)
{
}

void FUERayTracingAudioReflectionSimulator::Simulate(
    const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
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
    OutEnergyField.DelayBinDurationSeconds = Input.DurationSeconds / static_cast<float>(OutEnergyField.DelayBinEnergy.Num());
    OutEnergyField.EarliestArrivalSeconds = 0.0f;

    if (!Input.Scene || Input.Scene->IsEmpty() || Input.NumReflectionRays <= 0 || Input.MaxReflectionBounces <= 0 || Input.DurationSeconds <= 0.0f)
    {
        return;
    }

    const bool bCanUseHardwareRayTracing = RayTracingDevice.IsRayTracingAvailable();
    const float ReferenceDistance = FMath::Max(Context.GetReferenceDistanceCm(), 1.0f);
    const float SpeedOfSound = FMath::Max(Context.GetSpeedOfSoundCmPerSecond(), 1.0f);
    const float MaxTraceDistance = FMath::Max(Context.GetMaxDistanceCm(), 100.0f);
    const FVector ListenerOffset = Input.ListenerForward.GetSafeNormal().IsNearlyZero()
        ? FVector::UpVector
        : Input.ListenerForward.GetSafeNormal();

    if (bCanUseHardwareRayTracing)
    {
        FUERayTracingAudioEnergyFieldTraceRequest EnergyFieldTraceRequest;
        EnergyFieldTraceRequest.World = Input.World;
        EnergyFieldTraceRequest.Scene = Input.Scene;
        EnergyFieldTraceRequest.ListenerLocation = Input.ListenerLocation;
        EnergyFieldTraceRequest.ListenerForward = Input.ListenerForward;
        EnergyFieldTraceRequest.SourceLocation = Input.SourceLocation;
        EnergyFieldTraceRequest.AirAbsorptionPerMeter = Input.AirAbsorptionPerMeter;
        EnergyFieldTraceRequest.ListenerActor = Input.ListenerActor;
        EnergyFieldTraceRequest.SourceActor = Input.SourceActor;
        EnergyFieldTraceRequest.NumReflectionRays = Input.NumReflectionRays;
        EnergyFieldTraceRequest.MaxReflectionBounces = Input.MaxReflectionBounces;
        EnergyFieldTraceRequest.NumDelayBins = Input.NumDelayBins;
        EnergyFieldTraceRequest.DurationSeconds = Input.DurationSeconds;
        EnergyFieldTraceRequest.ReferenceDistance = ReferenceDistance;
        EnergyFieldTraceRequest.SpeedOfSound = SpeedOfSound;
        EnergyFieldTraceRequest.MaxTraceDistance = MaxTraceDistance;

        FUERayTracingAudioEnergyFieldTraceResult EnergyFieldTraceResult;
        if (RayTracingDevice.SimulateIndirectEnergyField(EnergyFieldTraceRequest, EnergyFieldTraceResult))
        {
            OutEnergyField.DelayBinEnergy = MoveTemp(EnergyFieldTraceResult.DelayBinEnergy);
            OutEnergyField.EarliestArrivalSeconds = EnergyFieldTraceResult.EarliestArrivalSeconds;
            OutNumValidContributions = EnergyFieldTraceResult.NumValidContributions;
            return;
        }
    }

    FUERayTracingAudioTraceRequest TraceRequest;
    TraceRequest.World = Input.World;
    TraceRequest.Scene = Input.Scene;
    TraceRequest.IgnoredActor = Input.ListenerActor;
    TraceRequest.SecondaryIgnoredActor = Input.SourceActor;

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

    auto QueryIntersections = [&](const TArray<FUERayTracingAudioRay>& Rays, TArray<FAcousticSceneHit>& OutHits)
    {
        OutHits.SetNum(Rays.Num());

        if (bCanUseHardwareRayTracing)
        {
            TArray<FUERayTracingAudioDetailedTraceHit> DetailedHits;
            if (RayTracingDevice.TraceDetailedRays(TraceRequest, Rays, DetailedHits) && DetailedHits.Num() == Rays.Num())
            {
                for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
                {
                    BuildSceneHitFromDetailed(DetailedHits[RayIndex], OutHits[RayIndex]);
                }
                return;
            }
        }

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

        if (bCanUseHardwareRayTracing)
        {
            TArray<FUERayTracingAudioDetailedTraceHit> DetailedHits;
            if (RayTracingDevice.TraceDetailedRays(TraceRequest, Rays, DetailedHits) && DetailedHits.Num() == Rays.Num())
            {
                for (int32 RayIndex = 0; RayIndex < Rays.Num(); ++RayIndex)
                {
                    OutOccluded[RayIndex] = DetailedHits[RayIndex].bHit;
                }
                return;
            }
        }

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

    TArray<FReflectionPathState> ActivePaths;
    ActivePaths.Reserve(Input.NumReflectionRays);
    for (int32 RayIndex = 0; RayIndex < Input.NumReflectionRays; ++RayIndex)
    {
        FReflectionPathState& Path = ActivePaths.AddDefaulted_GetRef();
        Path.RayDirection = GenerateSphereDirectionSample(RayIndex);
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

        if (bCanUseHardwareRayTracing)
        {
            TArray<FShadeAndGatherPathInputGPU> PathInputs;
            PathInputs.Reserve(ActivePaths.Num());

            for (int32 PathIndex = 0; PathIndex < ActivePaths.Num(); ++PathIndex)
            {
                const FReflectionPathState& Path = ActivePaths[PathIndex];
                const FAcousticSceneHit& Hit = BounceHits[PathIndex];
                const bool bOccluded = OccludedResults.IsValidIndex(PathIndex) ? OccludedResults[PathIndex] : true;

                FShadeAndGatherPathInputGPU& PathInput = PathInputs.AddDefaulted_GetRef();
                PathInput.RayOriginAndTravelDistance = FVector4f(FVector3f(Path.RayOrigin), Path.TravelDistance);
                PathInput.RayDirectionAndBounceIndex = FVector4f(FVector3f(Path.RayDirection), static_cast<float>(BounceIndex));
                PathInput.ThroughputAndHitDistance = FVector4f(FVector3f(Path.Throughput), Hit.Distance);
                PathInput.HitLocationAndHitValid = FVector4f(FVector3f(Hit.Location), Hit.bHit ? 1.0f : 0.0f);
                PathInput.HitNormalAndOccluded = FVector4f(FVector3f(Hit.Normal), bOccluded ? 1.0f : 0.0f);
                PathInput.AbsorptionAndPadding = FVector4f(FVector3f(Hit.Absorption), 0.0f);
            }

            TArray<FVector> BounceEnergyBins;
            int32 BounceContributionCount = 0;
            if (DispatchShadeAndGatherOnGPU(PathInputs, Input, SpeedOfSound, ReferenceDistance, NextPaths, BounceEnergyBins, BounceContributionCount))
            {
                for (int32 BinIndex = 0; BinIndex < OutEnergyField.DelayBinEnergy.Num() && BinIndex < BounceEnergyBins.Num(); ++BinIndex)
                {
                    OutEnergyField.DelayBinEnergy[BinIndex] += BounceEnergyBins[BinIndex];
                    if (OutEnergyField.EarliestArrivalSeconds <= 0.0f && !BounceEnergyBins[BinIndex].IsNearlyZero())
                    {
                        OutEnergyField.EarliestArrivalSeconds = (static_cast<float>(BinIndex) + 0.5f) * OutEnergyField.DelayBinDurationSeconds;
                    }
                }

                OutNumValidContributions += BounceContributionCount;
                ActivePaths = MoveTemp(NextPaths);
                continue;
            }
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
            NextPath.Throughput.X *= FMath::Clamp(1.0f - Hit.Absorption.X, 0.0f, 1.0f);
            NextPath.Throughput.Y *= FMath::Clamp(1.0f - Hit.Absorption.Y, 0.0f, 1.0f);
            NextPath.Throughput.Z *= FMath::Clamp(1.0f - Hit.Absorption.Z, 0.0f, 1.0f);

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

                        if (MonoEnergy > KINDA_SMALL_NUMBER)
                        {
                            const int32 DelayBinIndex = GetDelayBinIndex(OutEnergyField, DelaySeconds);
                            if (DelayBinIndex != INDEX_NONE)
                            {
                                OutEnergyField.DelayBinEnergy[DelayBinIndex] += BandGain;
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
            NextPath.RayDirection = ActivePaths[PathIndex].RayDirection.MirrorByVector(Hit.Normal).GetSafeNormal();
            if (!NextPath.RayDirection.IsNearlyZero())
            {
                NextPaths.Add(NextPath);
            }
        }

        ActivePaths = MoveTemp(NextPaths);
    }
}
