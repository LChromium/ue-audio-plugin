#include "RayTracing/UERayTracingAudioRayTracingDevice.h"

#include "BuiltInRayTracingShaders.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PipelineStateCache.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIResourceUtils.h"
#include "RayTracingDefinitions.h"
#include "RayTracingInstanceBufferUtil.h"
#include "RayTracingPayloadType.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "Scene/UERayTracingAudioScene.h"
#include "ShaderParameterStruct.h"
#include "Simulation/UERayTracingAudioEnergyFieldShaders.h"
#include "UERayTracingAudioSDKModule.h"

#if RHI_RAYTRACING
class FUERayTracingAudioOcclusionRGS : public FBuiltInRayTracingShader
{
    DECLARE_GLOBAL_SHADER(FUERayTracingAudioOcclusionRGS);
    SHADER_USE_ROOT_PARAMETER_STRUCT(FUERayTracingAudioOcclusionRGS, FBuiltInRayTracingShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
        SHADER_PARAMETER_SRV(StructuredBuffer<FBasicRayData>, Rays)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<uint>, OcclusionOutput)
    END_SHADER_PARAMETER_STRUCT()

    static ERayTracingPayloadType GetRayTracingPayloadType(const int32 PermutationId)
    {
        return ERayTracingPayloadType::Default;
    }
};

IMPLEMENT_GLOBAL_SHADER(FUERayTracingAudioOcclusionRGS, "/Engine/Private/RayTracing/RayTracingBuiltInShaders.usf", "OcclusionMainRGS", SF_RayGen);

class FUERayTracingAudioIntersectionCHS : public FBuiltInRayTracingShader
{
    DECLARE_GLOBAL_SHADER(FUERayTracingAudioIntersectionCHS);
    SHADER_USE_ROOT_PARAMETER_STRUCT(FUERayTracingAudioIntersectionCHS, FBuiltInRayTracingShader);

    using FParameters = FEmptyShaderParameters;

    static ERayTracingPayloadType GetRayTracingPayloadType(const int32 PermutationId)
    {
        return ERayTracingPayloadType::Default;
    }
};

IMPLEMENT_GLOBAL_SHADER(FUERayTracingAudioIntersectionCHS, "/Engine/Private/RayTracing/RayTracingBuiltInShaders.usf", "IntersectionMainCHS", SF_RayHitGroup);

class FUERayTracingAudioIntersectionRGS : public FBuiltInRayTracingShader
{
    DECLARE_GLOBAL_SHADER(FUERayTracingAudioIntersectionRGS);
    SHADER_USE_ROOT_PARAMETER_STRUCT(FUERayTracingAudioIntersectionRGS, FBuiltInRayTracingShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_SRV(RaytracingAccelerationStructure, TLAS)
        SHADER_PARAMETER_SRV(StructuredBuffer<FBasicRayData>, Rays)
        SHADER_PARAMETER_UAV(RWStructuredBuffer<FIntersectionPayload>, IntersectionOutput)
    END_SHADER_PARAMETER_STRUCT()

    static ERayTracingPayloadType GetRayTracingPayloadType(const int32 PermutationId)
    {
        return ERayTracingPayloadType::Default;
    }
};

IMPLEMENT_GLOBAL_SHADER(FUERayTracingAudioIntersectionRGS, "/Engine/Private/RayTracing/RayTracingBuiltInShaders.usf", "IntersectionMainRGS", SF_RayGen);
#endif

namespace
{
    constexpr uint32 EnergyQuantizationScale = 1000000u;

    bool GHasLoggedHardwareRayTracingPath = false;
    bool GHasLoggedPhysicsFallbackPath = false;
    bool GHasLoggedIndirectHardwareRayTracingPath = false;
    bool GHasLoggedIndirectCpuFallbackPath = false;

    struct FUERayTracingAudioBasicRay
    {
        float Origin[3];
        uint32 Mask;
        float Direction[3];
        float TFar;
    };

    struct FUERayTracingAudioDetailedRay
    {
        float Origin[3];
        uint32 Mask;
        float Direction[3];
        float TFar;
    };

    struct FUERayTracingAudioDetailedTraceResult
    {
        float HitT = -1.0f;
        uint32 PrimitiveIndex = 0;
        uint32 InstanceIndex = 0;
        float Barycentrics[2] = { 0.0f, 0.0f };
    };

    struct FHardwareReflectionPathState
    {
        FVector RayOrigin = FVector::ZeroVector;
        FVector RayDirection = FVector::ForwardVector;
        FVector Throughput = FVector::OneVector;
        float TravelDistance = 0.0f;
    };

    struct FHardwareShadedBounceState
    {
        FHardwareReflectionPathState NextPath;
        FUERayTracingAudioRay ShadowRay;
        int32 BounceIndex = 0;
        bool bHasHit = false;
        bool bHasShadowRay = false;
        int32 GeometryIndex = INDEX_NONE;
    };

    float RayTracingDeviceRadicalInverse(int32 Base, int32 Index)
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

    FVector RayTracingDeviceGenerateSphereDirectionSample(int32 SampleIndex)
    {
        const float U = RayTracingDeviceRadicalInverse(2, SampleIndex + 1);
        const float V = RayTracingDeviceRadicalInverse(3, SampleIndex + 1);
        const float CosTheta = 1.0f - 2.0f * U;
        const float SinTheta = FMath::Sqrt(FMath::Max(0.0f, 1.0f - (CosTheta * CosTheta)));
        const float Phi = 2.0f * PI * V;
        return FVector(
            SinTheta * FMath::Cos(Phi),
            SinTheta * FMath::Sin(Phi),
            CosTheta).GetSafeNormal();
    }

    void GenerateListenerReflectionPaths(
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        TArray<FHardwareReflectionPathState>& OutPaths)
    {
        OutPaths.Reset();
        OutPaths.Reserve(Request.NumReflectionRays);

        const FVector ListenerOffset = Request.ListenerForward.GetSafeNormal().IsNearlyZero()
            ? FVector::UpVector
            : Request.ListenerForward.GetSafeNormal();

        for (int32 RayIndex = 0; RayIndex < Request.NumReflectionRays; ++RayIndex)
        {
            FHardwareReflectionPathState& Path = OutPaths.AddDefaulted_GetRef();
            Path.RayDirection = RayTracingDeviceGenerateSphereDirectionSample(RayIndex);
            Path.RayOrigin = Request.ListenerLocation + (Path.RayDirection + ListenerOffset) * 1.0f;
        }
    }

    void BuildBounceRaysFromReflectionPaths(
        const TArray<FHardwareReflectionPathState>& Paths,
        float MaxTraceDistance,
        TArray<FUERayTracingAudioRay>& OutRays)
    {
        OutRays.Reset();
        OutRays.Reserve(Paths.Num());

        for (const FHardwareReflectionPathState& Path : Paths)
        {
            OutRays.Add(FUERayTracingAudioRay{ Path.RayOrigin, Path.RayOrigin + (Path.RayDirection * MaxTraceDistance) });
        }
    }

    void ShadeAndBounceHardwareReflectionPaths(
        const TArray<FHardwareReflectionPathState>& ActivePaths,
        const TArray<FUERayTracingAudioDetailedTraceHit>& BounceHits,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        int32 BounceIndex,
        TArray<FHardwareShadedBounceState>& OutShadedStates,
        TArray<FHardwareReflectionPathState>& OutNextPaths)
    {
        OutShadedStates.Reset();
        OutShadedStates.SetNum(ActivePaths.Num());
        OutNextPaths.Reset();
        OutNextPaths.Reserve(ActivePaths.Num());

        for (int32 PathIndex = 0; PathIndex < ActivePaths.Num(); ++PathIndex)
        {
            FHardwareShadedBounceState& ShadedState = OutShadedStates[PathIndex];
            ShadedState.BounceIndex = BounceIndex;

            if (!BounceHits.IsValidIndex(PathIndex))
            {
                continue;
            }

            const FUERayTracingAudioDetailedTraceHit& Hit = BounceHits[PathIndex];
            if (!Hit.bHit || !Geometry.IsValidIndex(Hit.GeometryIndex))
            {
                continue;
            }

            const FVector Absorption = Geometry[Hit.GeometryIndex].Absorption;

            ShadedState.bHasHit = true;
            ShadedState.GeometryIndex = Hit.GeometryIndex;
            ShadedState.NextPath = ActivePaths[PathIndex];
            ShadedState.NextPath.TravelDistance += Hit.Distance;
            ShadedState.NextPath.Throughput.X *= FMath::Clamp(1.0f - Absorption.X, 0.0f, 1.0f);
            ShadedState.NextPath.Throughput.Y *= FMath::Clamp(1.0f - Absorption.Y, 0.0f, 1.0f);
            ShadedState.NextPath.Throughput.Z *= FMath::Clamp(1.0f - Absorption.Z, 0.0f, 1.0f);
            ShadedState.NextPath.RayOrigin = Hit.Location + (Hit.Normal * 1.0f);
            ShadedState.NextPath.RayDirection = ActivePaths[PathIndex].RayDirection.MirrorByVector(Hit.Normal).GetSafeNormal();

            ShadedState.bHasShadowRay = true;
            ShadedState.ShadowRay.Start = ShadedState.NextPath.RayOrigin;
            ShadedState.ShadowRay.End = Request.SourceLocation;

            if (!ShadedState.NextPath.RayDirection.IsNearlyZero())
            {
                OutNextPaths.Add(ShadedState.NextPath);
            }
        }
    }

    void BuildShadowRaysFromShadedStates(
        const TArray<FHardwareShadedBounceState>& ShadedStates,
        TArray<FUERayTracingAudioRay>& OutShadowRays,
        TArray<int32>& OutShadowRayStateIndices)
    {
        OutShadowRays.Reset();
        OutShadowRayStateIndices.Reset();
        OutShadowRays.Reserve(ShadedStates.Num());
        OutShadowRayStateIndices.Reserve(ShadedStates.Num());

        for (int32 StateIndex = 0; StateIndex < ShadedStates.Num(); ++StateIndex)
        {
            if (!ShadedStates[StateIndex].bHasShadowRay)
            {
                continue;
            }

            OutShadowRays.Add(ShadedStates[StateIndex].ShadowRay);
            OutShadowRayStateIndices.Add(StateIndex);
        }
    }

    void GatherEnergyFieldFromShadowResults(
        const TArray<FHardwareShadedBounceState>& ShadedStates,
        const TArray<bool>& ShadowOcclusionHits,
        const TArray<int32>& ShadowRayStateIndices,
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        FUERayTracingAudioEnergyFieldTraceResult& OutResult)
    {
        const float DelayBinDurationSeconds = Request.DurationSeconds / static_cast<float>(FMath::Max(OutResult.DelayBinEnergy.Num(), 1));

        for (int32 ShadowRayIndex = 0; ShadowRayIndex < ShadowRayStateIndices.Num(); ++ShadowRayIndex)
        {
            if (!ShadowOcclusionHits.IsValidIndex(ShadowRayIndex) || ShadowOcclusionHits[ShadowRayIndex])
            {
                continue;
            }

            const int32 StateIndex = ShadowRayStateIndices[ShadowRayIndex];
            if (!ShadedStates.IsValidIndex(StateIndex))
            {
                continue;
            }

            const FHardwareShadedBounceState& ShadedState = ShadedStates[StateIndex];
            const float SourceDistance = FVector::Distance(ShadedState.ShadowRay.Start, Request.SourceLocation);
            if (SourceDistance <= UE_KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const float TotalDistance = ShadedState.NextPath.TravelDistance + SourceDistance;
            const float DelaySeconds = TotalDistance / FMath::Max(Request.SpeedOfSound, 1.0f);
            if (DelaySeconds > Request.DurationSeconds)
            {
                continue;
            }

            const float DistanceMeters = TotalDistance / 100.0f;
            FVector AirAttenuation;
            AirAttenuation.X = FMath::Exp(-Request.AirAbsorptionPerMeter.X * DistanceMeters);
            AirAttenuation.Y = FMath::Exp(-Request.AirAbsorptionPerMeter.Y * DistanceMeters);
            AirAttenuation.Z = FMath::Exp(-Request.AirAbsorptionPerMeter.Z * DistanceMeters);

            const float DistanceRatio = FMath::Max(TotalDistance, Request.ReferenceDistance) / FMath::Max(Request.ReferenceDistance, 1.0f);
            const float GeometricAttenuation = 1.0f / FMath::Square(DistanceRatio);
            const float BounceAttenuation = 1.0f / static_cast<float>(ShadedState.BounceIndex + 1);
            const FVector BandGain = ShadedState.NextPath.Throughput
                * AirAttenuation
                * (GeometricAttenuation * BounceAttenuation / static_cast<float>(Request.NumReflectionRays));
            const float MonoEnergy = (BandGain.X + BandGain.Y + BandGain.Z) / 3.0f;
            if (MonoEnergy <= KINDA_SMALL_NUMBER)
            {
                continue;
            }

            const int32 DelayBinIndex = FMath::Clamp(
                FMath::FloorToInt(DelaySeconds / DelayBinDurationSeconds),
                0,
                OutResult.DelayBinEnergy.Num() - 1);
            OutResult.DelayBinEnergy[DelayBinIndex] += BandGain;
            OutResult.EarliestArrivalSeconds = (OutResult.EarliestArrivalSeconds <= 0.0f)
                ? DelaySeconds
                : FMath::Min(OutResult.EarliestArrivalSeconds, DelaySeconds);
            ++OutResult.NumValidContributions;
        }
    }

    bool DispatchGenerateListenerRaysOnGPU_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        TArray<FHardwareReflectionPathState>& OutPaths)
    {
        OutPaths.Reset();
        if (Request.NumReflectionRays <= 0)
        {
            return true;
        }

        const FRHIBufferCreateDesc OutputBufferCreateDesc =
            FRHIBufferCreateDesc::CreateStructured<FReflectionPathStateGPU>(TEXT("UERayTracingAudioGeneratedPaths"), Request.NumReflectionRays)
            .AddUsage(EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy)
            .SetInitialState(ERHIAccess::UAVCompute);
        FBufferRHIRef OutputBuffer = RHICmdList.CreateBuffer(OutputBufferCreateDesc);
        FUnorderedAccessViewRHIRef OutputBufferUAV = RHICmdList.CreateUnorderedAccessView(
            OutputBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(FReflectionPathStateGPU))
                .SetNumElements(Request.NumReflectionRays));

        TShaderMapRef<FUERayTracingAudioGenerateListenerRaysCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FUERayTracingAudioGenerateListenerRaysCS::FParameters Parameters;
        Parameters.OutPaths = OutputBufferUAV;
        Parameters.NumPaths = static_cast<uint32>(Request.NumReflectionRays);
        Parameters.ListenerLocation = FVector3f(Request.ListenerLocation);
        Parameters.ListenerForward = FVector3f(Request.ListenerForward);

        FComputeShaderUtils::Dispatch(
            RHICmdList,
            ComputeShader,
            Parameters,
            FIntVector(FMath::DivideAndRoundUp(Request.NumReflectionRays, static_cast<int32>(FUERayTracingAudioGenerateListenerRaysCS::ThreadGroupSizeX)), 1, 1));

        RHICmdList.Transition(FRHITransitionInfo(OutputBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
        RHICmdList.BlockUntilGPUIdle();

        const FReflectionPathStateGPU* OutputData = static_cast<const FReflectionPathStateGPU*>(
            RHICmdList.LockBuffer(OutputBuffer, 0, sizeof(FReflectionPathStateGPU) * Request.NumReflectionRays, RLM_ReadOnly));
        if (!OutputData)
        {
            return false;
        }

        OutPaths.Reserve(Request.NumReflectionRays);
        for (int32 PathIndex = 0; PathIndex < Request.NumReflectionRays; ++PathIndex)
        {
            const FReflectionPathStateGPU& PathGPU = OutputData[PathIndex];
            FHardwareReflectionPathState& Path = OutPaths.AddDefaulted_GetRef();
            Path.RayOrigin = FVector(PathGPU.OriginAndTravelDistance.X, PathGPU.OriginAndTravelDistance.Y, PathGPU.OriginAndTravelDistance.Z);
            Path.TravelDistance = PathGPU.OriginAndTravelDistance.W;
            Path.RayDirection = FVector(PathGPU.DirectionAndAlive.X, PathGPU.DirectionAndAlive.Y, PathGPU.DirectionAndAlive.Z).GetSafeNormal();
            Path.Throughput = FVector(PathGPU.ThroughputAndPadding.X, PathGPU.ThroughputAndPadding.Y, PathGPU.ThroughputAndPadding.Z);
        }

        RHICmdList.UnlockBuffer(OutputBuffer);
        return true;
    }

    bool DispatchShadeAndGatherOnGPU_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const TArray<FShadeAndGatherPathInputGPU>& PathInputs,
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        TArray<FHardwareReflectionPathState>& OutNextPaths,
        TArray<FVector>& OutEnergyBins,
        int32& OutContributionCount)
    {
        OutNextPaths.Reset();
        OutEnergyBins.Init(FVector::ZeroVector, FMath::Max(Request.NumDelayBins, 1));
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
        Parameters.NumReflectionRays = static_cast<uint32>(Request.NumReflectionRays);
        Parameters.SpeedOfSound = Request.SpeedOfSound;
        Parameters.ReferenceDistance = Request.ReferenceDistance;
        Parameters.DurationSeconds = Request.DurationSeconds;
        Parameters.DelayBinDurationSeconds = Request.DurationSeconds / static_cast<float>(OutEnergyBins.Num());
        Parameters.EnergyQuantizationScale = static_cast<float>(EnergyQuantizationScale);
        Parameters.SourceLocation = FVector3f(Request.SourceLocation);
        Parameters.AirAbsorptionPerMeter = FVector3f(Request.AirAbsorptionPerMeter);

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

            FHardwareReflectionPathState& NextPath = OutNextPaths.AddDefaulted_GetRef();
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

    bool TraceRaysWithPhysics(const FUERayTracingAudioTraceRequest& Request, const TArray<FUERayTracingAudioRay>& Rays, TArray<bool>& OutHits)
    {
        OutHits.Reset();

        if (!IsValid(Request.World))
        {
            return false;
        }

        FCollisionQueryParams QueryParams(TEXT("UERayTracingAudioTrace"), true);
        QueryParams.bReturnPhysicalMaterial = false;

        if (IsValid(Request.IgnoredActor))
        {
            QueryParams.AddIgnoredActor(Request.IgnoredActor);
        }

        if (IsValid(Request.SecondaryIgnoredActor))
        {
            QueryParams.AddIgnoredActor(Request.SecondaryIgnoredActor);
        }

        OutHits.Reserve(Rays.Num());

        for (const FUERayTracingAudioRay& Ray : Rays)
        {
            FHitResult HitResult;
            const bool bHit = Request.World->LineTraceSingleByChannel(HitResult, Ray.Start, Ray.End, ECC_Visibility, QueryParams);
            OutHits.Add(bHit);
        }

        return true;
    }

#if RHI_RAYTRACING
    struct FUERayTracingAudioOcclusionPipeline
    {
        FRayTracingPipelineState* PipelineState = nullptr;
        FShaderBindingTableRHIRef ShaderBindingTable;
        TShaderRef<FUERayTracingAudioOcclusionRGS> RayGenerationShader;
    };

    struct FUERayTracingAudioDetailedPipeline
    {
        FRayTracingPipelineState* PipelineState = nullptr;
        FShaderBindingTableRHIRef ShaderBindingTable;
        TShaderRef<FUERayTracingAudioIntersectionRGS> RayGenerationShader;
    };

    void AppendBoxGeometry(const FBox& Bounds, TArray<FVector3f>& OutVertices, TArray<uint32>& OutIndices)
    {
        const uint32 BaseVertex = static_cast<uint32>(OutVertices.Num());
        const FVector3f Min = FVector3f(Bounds.Min);
        const FVector3f Max = FVector3f(Bounds.Max);

        OutVertices.Add(FVector3f(Min.X, Min.Y, Min.Z));
        OutVertices.Add(FVector3f(Max.X, Min.Y, Min.Z));
        OutVertices.Add(FVector3f(Max.X, Max.Y, Min.Z));
        OutVertices.Add(FVector3f(Min.X, Max.Y, Min.Z));
        OutVertices.Add(FVector3f(Min.X, Min.Y, Max.Z));
        OutVertices.Add(FVector3f(Max.X, Min.Y, Max.Z));
        OutVertices.Add(FVector3f(Max.X, Max.Y, Max.Z));
        OutVertices.Add(FVector3f(Min.X, Max.Y, Max.Z));

        const uint32 CubeIndices[] =
        {
            0, 2, 1, 0, 3, 2,
            4, 5, 6, 4, 6, 7,
            0, 1, 5, 0, 5, 4,
            1, 2, 6, 1, 6, 5,
            2, 3, 7, 2, 7, 6,
            3, 0, 4, 3, 4, 7,
        };

        for (uint32 Index : CubeIndices)
        {
            OutIndices.Add(BaseVertex + Index);
        }
    }

    void AppendTriangleGeometry(const FUERayTracingAudioGeometryExport& GeometryExport, TArray<FVector3f>& OutVertices, TArray<uint32>& OutIndices)
    {
        const uint32 BaseVertex = static_cast<uint32>(OutVertices.Num());
        OutVertices.Reserve(OutVertices.Num() + GeometryExport.Vertices.Num());
        OutIndices.Reserve(OutIndices.Num() + GeometryExport.Indices.Num());

        for (const FVector& Vertex : GeometryExport.Vertices)
        {
            OutVertices.Add(FVector3f(Vertex));
        }

        for (uint32 Index : GeometryExport.Indices)
        {
            OutIndices.Add(BaseVertex + Index);
        }
    }

    void BuildGeometryArrays(const FUERayTracingAudioGeometryExport& GeometryExport, TArray<FVector3f>& OutVertices, TArray<uint32>& OutIndices)
    {
        OutVertices.Reset();
        OutIndices.Reset();

        if (GeometryExport.bUseStaticMeshTriangles && GeometryExport.HasTriangleMesh())
        {
            AppendTriangleGeometry(GeometryExport, OutVertices, OutIndices);
        }
        else if (GeometryExport.Bounds.IsValid)
        {
            AppendBoxGeometry(GeometryExport.Bounds, OutVertices, OutIndices);
        }
    }

    FUERayTracingAudioOcclusionPipeline CreateOcclusionPipeline(FRHICommandListImmediate& RHICmdList)
    {
        FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
        const TShaderRef<FUERayTracingAudioOcclusionRGS> RayGenerationShader = ShaderMap->GetShader<FUERayTracingAudioOcclusionRGS>();
        const TShaderRef<FUERayTracingAudioIntersectionCHS> HitGroupShader = ShaderMap->GetShader<FUERayTracingAudioIntersectionCHS>();
        const TShaderRef<FDefaultPayloadMS> MissShader = ShaderMap->GetShader<FDefaultPayloadMS>();

        FRayTracingPipelineStateInitializer PipelineInitializer;
        FRHIRayTracingShader* RayGenerationTable[] = { RayGenerationShader.GetRayTracingShader() };
        PipelineInitializer.SetRayGenShaderTable(RayGenerationTable);
        FRHIRayTracingShader* HitGroupTable[] = { HitGroupShader.GetRayTracingShader() };
        PipelineInitializer.SetHitGroupTable(HitGroupTable);
        FRHIRayTracingShader* MissShaderTable[] = { MissShader.GetRayTracingShader() };
        PipelineInitializer.SetMissShaderTable(MissShaderTable);

        FRayTracingShaderBindingTableInitializer ShaderBindingTableInitializer;
        ShaderBindingTableInitializer.ShaderBindingMode = ERayTracingShaderBindingMode::RTPSO;
        ShaderBindingTableInitializer.HitGroupIndexingMode = ERayTracingHitGroupIndexingMode::Disallow;
        ShaderBindingTableInitializer.NumGeometrySegments = 1;
        ShaderBindingTableInitializer.NumShaderSlotsPerGeometrySegment = RAY_TRACING_NUM_SHADER_SLOTS;
        ShaderBindingTableInitializer.NumMissShaderSlots = 1;
        ShaderBindingTableInitializer.NumCallableShaderSlots = 0;
        ShaderBindingTableInitializer.LocalBindingDataSize = PipelineInitializer.GetMaxLocalBindingDataSize();

        FUERayTracingAudioOcclusionPipeline Result;
        Result.PipelineState = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, PipelineInitializer);
        Result.ShaderBindingTable = RHICmdList.CreateRayTracingShaderBindingTable(ShaderBindingTableInitializer);
        Result.RayGenerationShader = RayGenerationShader;
        return Result;
    }

    FUERayTracingAudioDetailedPipeline CreateDetailedPipeline(FRHICommandListImmediate& RHICmdList, uint32 NumGeometrySegments)
    {
        FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
        const TShaderRef<FUERayTracingAudioIntersectionRGS> RayGenerationShader = ShaderMap->GetShader<FUERayTracingAudioIntersectionRGS>();
        const TShaderRef<FUERayTracingAudioIntersectionCHS> HitGroupShader = ShaderMap->GetShader<FUERayTracingAudioIntersectionCHS>();
        const TShaderRef<FDefaultPayloadMS> MissShader = ShaderMap->GetShader<FDefaultPayloadMS>();

        FRayTracingPipelineStateInitializer PipelineInitializer;
        FRHIRayTracingShader* RayGenerationTable[] = { RayGenerationShader.GetRayTracingShader() };
        PipelineInitializer.SetRayGenShaderTable(RayGenerationTable);
        FRHIRayTracingShader* HitGroupTable[] = { HitGroupShader.GetRayTracingShader() };
        PipelineInitializer.SetHitGroupTable(HitGroupTable);
        FRHIRayTracingShader* MissShaderTable[] = { MissShader.GetRayTracingShader() };
        PipelineInitializer.SetMissShaderTable(MissShaderTable);

        FRayTracingShaderBindingTableInitializer ShaderBindingTableInitializer;
        ShaderBindingTableInitializer.ShaderBindingMode = ERayTracingShaderBindingMode::RTPSO;
        ShaderBindingTableInitializer.HitGroupIndexingMode = ERayTracingHitGroupIndexingMode::Disallow;
        ShaderBindingTableInitializer.NumGeometrySegments = FMath::Max<uint32>(NumGeometrySegments, 1u);
        ShaderBindingTableInitializer.NumShaderSlotsPerGeometrySegment = RAY_TRACING_NUM_SHADER_SLOTS;
        ShaderBindingTableInitializer.NumMissShaderSlots = 1;
        ShaderBindingTableInitializer.NumCallableShaderSlots = 0;
        ShaderBindingTableInitializer.LocalBindingDataSize = PipelineInitializer.GetMaxLocalBindingDataSize();

        FUERayTracingAudioDetailedPipeline Result;
        Result.PipelineState = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, PipelineInitializer);
        Result.ShaderBindingTable = RHICmdList.CreateRayTracingShaderBindingTable(ShaderBindingTableInitializer);
        Result.RayGenerationShader = RayGenerationShader;
        return Result;
    }

    void DispatchOcclusionRays(
        FRHICommandListImmediate& RHICmdList,
        FRHIShaderResourceView* SceneView,
        FRHIShaderResourceView* RayBufferView,
        FRHIUnorderedAccessView* ResultBufferView,
        uint32 NumRays)
    {
        FUERayTracingAudioOcclusionPipeline Pipeline = CreateOcclusionPipeline(RHICmdList);
        RHICmdList.SetDefaultRayTracingHitGroup(Pipeline.ShaderBindingTable, Pipeline.PipelineState, 0);
        RHICmdList.SetRayTracingMissShader(Pipeline.ShaderBindingTable, 0, Pipeline.PipelineState, 0, 0, nullptr, 0);
        RHICmdList.CommitShaderBindingTable(Pipeline.ShaderBindingTable);

        FUERayTracingAudioOcclusionRGS::FParameters Parameters;
        Parameters.TLAS = SceneView;
        Parameters.Rays = RayBufferView;
        Parameters.OcclusionOutput = ResultBufferView;

        FRHIBatchedShaderParameters& GlobalResources = RHICmdList.GetScratchShaderParameters();
        SetShaderParameters(GlobalResources, Pipeline.RayGenerationShader, Parameters);
        RHICmdList.RayTraceDispatch(Pipeline.PipelineState, Pipeline.RayGenerationShader.GetRayTracingShader(), Pipeline.ShaderBindingTable, GlobalResources, NumRays, 1);
    }

    void DispatchDetailedRays(
        FRHICommandListImmediate& RHICmdList,
        FRHIShaderResourceView* SceneView,
        FRHIShaderResourceView* RayBufferView,
        FRHIUnorderedAccessView* ResultBufferView,
        uint32 NumRays,
        uint32 NumGeometrySegments)
    {
        FUERayTracingAudioDetailedPipeline Pipeline = CreateDetailedPipeline(RHICmdList, NumGeometrySegments);
        RHICmdList.SetDefaultRayTracingHitGroup(Pipeline.ShaderBindingTable, Pipeline.PipelineState, 0);
        RHICmdList.SetRayTracingMissShader(Pipeline.ShaderBindingTable, 0, Pipeline.PipelineState, 0, 0, nullptr, 0);
        RHICmdList.CommitShaderBindingTable(Pipeline.ShaderBindingTable);

        FUERayTracingAudioIntersectionRGS::FParameters Parameters;
        Parameters.TLAS = SceneView;
        Parameters.Rays = RayBufferView;
        Parameters.IntersectionOutput = ResultBufferView;

        FRHIBatchedShaderParameters& GlobalResources = RHICmdList.GetScratchShaderParameters();
        SetShaderParameters(GlobalResources, Pipeline.RayGenerationShader, Parameters);
        RHICmdList.RayTraceDispatch(Pipeline.PipelineState, Pipeline.RayGenerationShader.GetRayTracingShader(), Pipeline.ShaderBindingTable, GlobalResources, NumRays, 1);
    }

    void BuildBasicRayBufferData(const TArray<FUERayTracingAudioRay>& Rays, TArray<FUERayTracingAudioBasicRay>& OutRayData)
    {
        OutRayData.Reset();
        OutRayData.Reserve(Rays.Num());

        for (const FUERayTracingAudioRay& Ray : Rays)
        {
            const FVector Delta = Ray.End - Ray.Start;
            const float Distance = Delta.Length();
            const FVector Direction = (Distance > UE_SMALL_NUMBER) ? (Delta / Distance) : FVector::ForwardVector;

            FUERayTracingAudioBasicRay& ShaderRay = OutRayData.AddDefaulted_GetRef();
            ShaderRay.Origin[0] = Ray.Start.X;
            ShaderRay.Origin[1] = Ray.Start.Y;
            ShaderRay.Origin[2] = Ray.Start.Z;
            ShaderRay.Mask = 0xFFFFFFFF;
            ShaderRay.Direction[0] = Direction.X;
            ShaderRay.Direction[1] = Direction.Y;
            ShaderRay.Direction[2] = Direction.Z;
            ShaderRay.TFar = Distance;
        }
    }

    void BuildDetailedRayBufferData(const TArray<FUERayTracingAudioRay>& Rays, TArray<FUERayTracingAudioDetailedRay>& OutRayData)
    {
        OutRayData.Reset();
        OutRayData.Reserve(Rays.Num());

        for (const FUERayTracingAudioRay& Ray : Rays)
        {
            const FVector Delta = Ray.End - Ray.Start;
            const float Distance = Delta.Length();
            const FVector Direction = (Distance > UE_SMALL_NUMBER) ? (Delta / Distance) : FVector::ForwardVector;

            FUERayTracingAudioDetailedRay& ShaderRay = OutRayData.AddDefaulted_GetRef();
            ShaderRay.Origin[0] = Ray.Start.X;
            ShaderRay.Origin[1] = Ray.Start.Y;
            ShaderRay.Origin[2] = Ray.Start.Z;
            ShaderRay.TFar = Distance;
            ShaderRay.Mask = 0xFF;
            ShaderRay.Direction[0] = Direction.X;
            ShaderRay.Direction[1] = Direction.Y;
            ShaderRay.Direction[2] = Direction.Z;
        }
    }

    bool GetPrimitiveTriangle(
        const FUERayTracingAudioGeometryExport& GeometryExport,
        uint32 PrimitiveIndex,
        FVector& OutA,
        FVector& OutB,
        FVector& OutC)
    {
        TArray<FVector3f> Vertices;
        TArray<uint32> Indices;
        BuildGeometryArrays(GeometryExport, Vertices, Indices);

        const uint32 BaseIndex = PrimitiveIndex * 3;
        if (BaseIndex + 2 >= static_cast<uint32>(Indices.Num()))
        {
            return false;
        }

        const uint32 Index0 = Indices[BaseIndex];
        const uint32 Index1 = Indices[BaseIndex + 1];
        const uint32 Index2 = Indices[BaseIndex + 2];
        if (!Vertices.IsValidIndex(Index0) || !Vertices.IsValidIndex(Index1) || !Vertices.IsValidIndex(Index2))
        {
            return false;
        }

        OutA = FVector(Vertices[Index0]);
        OutB = FVector(Vertices[Index1]);
        OutC = FVector(Vertices[Index2]);
        return true;
    }

    bool BuildRayTracingSceneFromGeometry(
        FRHICommandListImmediate& RHICmdList,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        FRHIShaderResourceView*& OutSceneView,
        FRayTracingSceneRHIRef& OutRayTracingScene,
        TArray<FBufferRHIRef>& OutVertexBuffers,
        TArray<FBufferRHIRef>& OutIndexBuffers,
        TArray<FRayTracingGeometryRHIRef>& OutRayTracingGeometries,
        FBufferRHIRef& OutSceneBuffer,
        FBufferRHIRef& OutScratchBuffer,
        FRWBufferStructured& OutInstanceBuffer,
        FRWBufferStructured& OutHitGroupContributionsBuffer,
        uint32& OutNumGeometrySegments,
        TArray<int32>& OutInstanceToGeometryIndex)
    {
        TArray<FRayTracingGeometryInstance> Instances;
        TArray<FMatrix> InstanceTransforms;
        Instances.Reserve(Geometry.Num());
        InstanceTransforms.Reserve(Geometry.Num());

        TArray<FVector3f> Vertices;
        TArray<uint32> Indices;

        for (int32 GeometryIndex = 0; GeometryIndex < Geometry.Num(); ++GeometryIndex)
        {
            const FUERayTracingAudioGeometryExport& GeometryExport = Geometry[GeometryIndex];
            if (!GeometryExport.bVisibleForDirectSound)
            {
                continue;
            }

            BuildGeometryArrays(GeometryExport, Vertices, Indices);
            if (Indices.IsEmpty())
            {
                continue;
            }

            FBufferRHIRef VertexBuffer = UE::RHIResourceUtils::CreateVertexBufferFromArray(
                RHICmdList,
                TEXT("UERayTracingAudioVertexBuffer"),
                EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource,
                MakeConstArrayView(Vertices));

            FBufferRHIRef IndexBuffer = UE::RHIResourceUtils::CreateIndexBufferFromArray(
                RHICmdList,
                TEXT("UERayTracingAudioIndexBuffer"),
                EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource,
                MakeConstArrayView(Indices));

            FRayTracingGeometryInitializer GeometryInitializer;
            GeometryInitializer.DebugName = FName(TEXT("UERayTracingAudioBLAS"));
            GeometryInitializer.IndexBuffer = IndexBuffer;
            GeometryInitializer.GeometryType = RTGT_Triangles;
            GeometryInitializer.bFastBuild = false;

            FRayTracingGeometrySegment Segment;
            Segment.VertexBuffer = VertexBuffer;
            Segment.VertexBufferStride = sizeof(FVector3f);
            Segment.NumPrimitives = Indices.Num() / 3;
            Segment.MaxVertices = Vertices.Num();
            GeometryInitializer.Segments.Add(Segment);
            GeometryInitializer.TotalPrimitiveCount = Segment.NumPrimitives;

            FRayTracingGeometryRHIRef RayTracingGeometry = RHICmdList.CreateRayTracingGeometry(GeometryInitializer);
            RHICmdList.BuildAccelerationStructure(RayTracingGeometry);

            OutVertexBuffers.Add(VertexBuffer);
            OutIndexBuffers.Add(IndexBuffer);
            OutRayTracingGeometries.Add(RayTracingGeometry);

            const int32 TransformIndex = InstanceTransforms.Add(FMatrix::Identity);
            FRayTracingGeometryInstance& Instance = Instances.AddDefaulted_GetRef();
            Instance.GeometryRHI = RayTracingGeometry;
            Instance.NumTransforms = 1;
            Instance.Transforms = MakeArrayView(&InstanceTransforms[TransformIndex], 1);
            Instance.InstanceContributionToHitGroupIndex = 0;
            Instance.DefaultUserData = static_cast<uint32>(GeometryIndex);
            Instance.Mask = 0xFF;
            OutInstanceToGeometryIndex.Add(GeometryIndex);
        }

        if (Instances.IsEmpty())
        {
            OutSceneView = nullptr;
            OutNumGeometrySegments = 0;
            return true;
        }

        FRayTracingInstanceBufferBuilder InstanceBufferBuilder;
        InstanceBufferBuilder.Init(MakeArrayView(Instances), FVector::ZeroVector);

        FRayTracingSceneInitializer SceneInitializer;
        SceneInitializer.DebugName = FName(TEXT("UERayTracingAudioTLAS"));
        SceneInitializer.MaxNumInstances = InstanceBufferBuilder.GetMaxNumInstances();
        SceneInitializer.BuildFlags = ERayTracingAccelerationStructureFlags::FastTrace;

        OutRayTracingScene = RHICreateRayTracingScene(MoveTemp(SceneInitializer));
        const FRayTracingSceneInitializer& RayTracingSceneInitializer = OutRayTracingScene->GetInitializer();
        const FRayTracingAccelerationStructureSize SceneSizeInfo = RHICalcRayTracingSceneSize(RayTracingSceneInitializer);

        const FRHIBufferCreateDesc SceneBufferCreateDesc =
            FRHIBufferCreateDesc::Create(TEXT("UERayTracingAudioSceneBuffer"), static_cast<uint32>(SceneSizeInfo.ResultSize), 0, EBufferUsageFlags::AccelerationStructure)
            .SetInitialState(ERHIAccess::BVHWrite);
        OutSceneBuffer = RHICmdList.CreateBuffer(SceneBufferCreateDesc);

        const FRHIBufferCreateDesc ScratchBufferCreateDesc =
            FRHIBufferCreateDesc::Create(TEXT("UERayTracingAudioScratchBuffer"), static_cast<uint32>(SceneSizeInfo.BuildScratchSize), GRHIRayTracingScratchBufferAlignment, EBufferUsageFlags::UnorderedAccess)
            .SetInitialState(ERHIAccess::UAVCompute);
        OutScratchBuffer = RHICmdList.CreateBuffer(ScratchBufferCreateDesc);

        OutInstanceBuffer.Initialize(RHICmdList, TEXT("UERayTracingAudioInstanceBuffer"), GRHIRayTracingInstanceDescriptorSize, RayTracingSceneInitializer.MaxNumInstances);

        if (GRHIGlobals.RayTracing.RequiresSeparateHitGroupContributionsBuffer)
        {
            OutHitGroupContributionsBuffer.Initialize(RHICmdList, TEXT("UERayTracingAudioHitGroupContributions"), sizeof(uint32), RayTracingSceneInitializer.MaxNumInstances);
        }

        InstanceBufferBuilder.FillRayTracingInstanceUploadBuffer(RHICmdList);
        InstanceBufferBuilder.FillAccelerationStructureAddressesBuffer(RHICmdList);
        InstanceBufferBuilder.BuildRayTracingInstanceBuffer(
            RHICmdList,
            nullptr,
            nullptr,
            OutInstanceBuffer.UAV,
            GRHIGlobals.RayTracing.RequiresSeparateHitGroupContributionsBuffer ? OutHitGroupContributionsBuffer.UAV : nullptr,
            RayTracingSceneInitializer.MaxNumInstances,
            false,
            nullptr,
            0,
            nullptr);

        RHICmdList.BindAccelerationStructureMemory(OutRayTracingScene, OutSceneBuffer, 0);

        FRayTracingSceneBuildParams BuildParams;
        BuildParams.Scene = OutRayTracingScene;
        BuildParams.ScratchBuffer = OutScratchBuffer;
        BuildParams.ScratchBufferOffset = 0;
        BuildParams.InstanceBuffer = OutInstanceBuffer.Buffer;
        BuildParams.InstanceBufferOffset = 0;
        BuildParams.ReferencedGeometries = InstanceBufferBuilder.GetReferencedGeometries();
        BuildParams.NumInstances = RayTracingSceneInitializer.MaxNumInstances;

        if (GRHIGlobals.RayTracing.RequiresSeparateHitGroupContributionsBuffer)
        {
            BuildParams.HitGroupContributionsBuffer = OutHitGroupContributionsBuffer.Buffer;
            BuildParams.HitGroupContributionsBufferOffset = 0;
        }

        RHICmdList.Transition(FRHITransitionInfo(OutInstanceBuffer.Buffer, ERHIAccess::UAVMask, ERHIAccess::SRVCompute));
        RHICmdList.BuildAccelerationStructure(BuildParams);
        RHICmdList.Transition(FRHITransitionInfo(OutRayTracingScene.GetReference(), ERHIAccess::BVHWrite, ERHIAccess::BVHRead));

        FShaderResourceViewInitializer SceneViewInitializer(OutSceneBuffer, OutRayTracingScene, 0);
        OutSceneView = RHICmdList.CreateShaderResourceView(SceneViewInitializer);
        OutNumGeometrySegments = static_cast<uint32>(Instances.Num());
        return true;
    }

    bool TraceRaysWithHardwareRayTracing_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FRHIShaderResourceView* SceneView,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<bool>& OutHits)
    {
        OutHits.Init(false, Rays.Num());

        if (!SceneView || Rays.IsEmpty())
        {
            return true;
        }

        TArray<FUERayTracingAudioBasicRay> RayData;
        BuildBasicRayBufferData(Rays, RayData);

        FBufferRHIRef RayBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
            RHICmdList,
            TEXT("UERayTracingAudioRayBuffer"),
            BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
            ERHIAccess::SRVMask,
            MakeConstArrayView(RayData));

        FShaderResourceViewRHIRef RayBufferView = RHICmdList.CreateShaderResourceView(
            RayBuffer,
            FRHIViewDesc::CreateBufferSRV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(FUERayTracingAudioBasicRay))
                .SetNumElements(RayData.Num()));

        const FRHIBufferCreateDesc ResultBufferCreateDesc =
            FRHIBufferCreateDesc::CreateStructured<uint32>(TEXT("UERayTracingAudioResultBuffer"), RayData.Num())
            .AddUsage(EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess)
            .SetInitialState(ERHIAccess::UAVMask);

        FBufferRHIRef ResultBuffer = RHICmdList.CreateBuffer(ResultBufferCreateDesc);
        FUnorderedAccessViewRHIRef ResultBufferView = RHICmdList.CreateUnorderedAccessView(
            ResultBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(uint32))
                .SetNumElements(RayData.Num()));

        DispatchOcclusionRays(RHICmdList, SceneView, RayBufferView, ResultBufferView, RayData.Num());
        RHICmdList.BlockUntilGPUIdle();

        const uint32* MappedResults = static_cast<const uint32*>(RHICmdList.LockBuffer(ResultBuffer, 0, sizeof(uint32) * RayData.Num(), RLM_ReadOnly));
        if (!MappedResults)
        {
            return false;
        }

        for (int32 Index = 0; Index < OutHits.Num(); ++Index)
        {
            OutHits[Index] = (MappedResults[Index] != 0);
        }

        RHICmdList.UnlockBuffer(ResultBuffer);
        return true;
    }

    bool TraceRaysWithHardwareRayTracing_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<bool>& OutHits)
    {
        OutHits.Init(false, Rays.Num());

        if (Geometry.IsEmpty() || Rays.IsEmpty())
        {
            return true;
        }

        TArray<FBufferRHIRef> VertexBuffers;
        TArray<FBufferRHIRef> IndexBuffers;
        TArray<FRayTracingGeometryRHIRef> RayTracingGeometries;
        FRayTracingSceneRHIRef RayTracingScene;
        FBufferRHIRef SceneBuffer;
        FBufferRHIRef ScratchBuffer;
        FRWBufferStructured InstanceBuffer;
        FRWBufferStructured HitGroupContributionsBuffer;
        TArray<int32> DummyInstanceToGeometryIndex;
        FRHIShaderResourceView* SceneView = nullptr;
        uint32 NumGeometrySegments = 0;
        if (!BuildRayTracingSceneFromGeometry(
            RHICmdList,
            Geometry,
            SceneView,
            RayTracingScene,
            VertexBuffers,
            IndexBuffers,
            RayTracingGeometries,
            SceneBuffer,
            ScratchBuffer,
            InstanceBuffer,
            HitGroupContributionsBuffer,
            NumGeometrySegments,
            DummyInstanceToGeometryIndex))
        {
            return false;
        }

        if (!SceneView || NumGeometrySegments == 0)
        {
            return true;
        }

        return TraceRaysWithHardwareRayTracing_RenderThread(RHICmdList, SceneView, Rays, OutHits);
    }

    bool TraceDetailedRaysWithHardwareRayTracing_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FRHIShaderResourceView* SceneView,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const TArray<int32>& InstanceToGeometryIndex,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<FUERayTracingAudioDetailedTraceHit>& OutHits)
    {
        OutHits.Init(FUERayTracingAudioDetailedTraceHit(), Rays.Num());

        if (!SceneView || Rays.IsEmpty())
        {
            return true;
        }

        TArray<FUERayTracingAudioDetailedRay> RayData;
        BuildDetailedRayBufferData(Rays, RayData);

        FBufferRHIRef RayBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
            RHICmdList,
            TEXT("UERayTracingAudioIndirectRayBuffer"),
            BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer,
            ERHIAccess::SRVMask,
            MakeConstArrayView(RayData));

        FShaderResourceViewRHIRef RayBufferView = RHICmdList.CreateShaderResourceView(
            RayBuffer,
            FRHIViewDesc::CreateBufferSRV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(FUERayTracingAudioDetailedRay))
                .SetNumElements(RayData.Num()));

        const FRHIBufferCreateDesc ResultBufferCreateDesc =
            FRHIBufferCreateDesc::CreateStructured<FUERayTracingAudioDetailedTraceResult>(TEXT("UERayTracingAudioIndirectResultBuffer"), RayData.Num())
            .AddUsage(EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess)
            .SetInitialState(ERHIAccess::UAVMask);

        FBufferRHIRef ResultBuffer = RHICmdList.CreateBuffer(ResultBufferCreateDesc);
        FUnorderedAccessViewRHIRef ResultBufferView = RHICmdList.CreateUnorderedAccessView(
            ResultBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(FUERayTracingAudioDetailedTraceResult))
                .SetNumElements(RayData.Num()));

        DispatchDetailedRays(RHICmdList, SceneView, RayBufferView, ResultBufferView, RayData.Num(), static_cast<uint32>(FMath::Max(InstanceToGeometryIndex.Num(), 1)));
        RHICmdList.BlockUntilGPUIdle();

        const FUERayTracingAudioDetailedTraceResult* MappedResults =
            static_cast<const FUERayTracingAudioDetailedTraceResult*>(RHICmdList.LockBuffer(ResultBuffer, 0, sizeof(FUERayTracingAudioDetailedTraceResult) * RayData.Num(), RLM_ReadOnly));
        if (!MappedResults)
        {
            return false;
        }

        for (int32 Index = 0; Index < OutHits.Num(); ++Index)
        {
            const FUERayTracingAudioDetailedTraceResult& Result = MappedResults[Index];
            FUERayTracingAudioDetailedTraceHit& OutHit = OutHits[Index];
            OutHit.bHit = Result.HitT >= 0.0f
                && InstanceToGeometryIndex.IsValidIndex(static_cast<int32>(Result.InstanceIndex))
                && Geometry.IsValidIndex(InstanceToGeometryIndex[Result.InstanceIndex]);
            OutHit.Distance = OutHit.bHit ? Result.HitT : 0.0f;
            OutHit.GeometryIndex = OutHit.bHit ? InstanceToGeometryIndex[Result.InstanceIndex] : INDEX_NONE;
            OutHit.Location = Rays[Index].Start + ((Rays[Index].End - Rays[Index].Start).GetSafeNormal() * OutHit.Distance);
            OutHit.Normal = FVector::UpVector;

            if (OutHit.bHit)
            {
                FVector A;
                FVector B;
                FVector C;
                if (GetPrimitiveTriangle(Geometry[OutHit.GeometryIndex], Result.PrimitiveIndex, A, B, C))
                {
                    OutHit.Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
                    if (FVector::DotProduct(OutHit.Normal, (Rays[Index].End - Rays[Index].Start).GetSafeNormal()) > 0.0f)
                    {
                        OutHit.Normal *= -1.0f;
                    }
                }
            }
        }

        RHICmdList.UnlockBuffer(ResultBuffer);
        return true;
    }

    bool TraceDetailedRaysWithHardwareRayTracing_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<FUERayTracingAudioDetailedTraceHit>& OutHits)
    {
        OutHits.Init(FUERayTracingAudioDetailedTraceHit(), Rays.Num());

        if (Geometry.IsEmpty() || Rays.IsEmpty())
        {
            return true;
        }

        TArray<FBufferRHIRef> VertexBuffers;
        TArray<FBufferRHIRef> IndexBuffers;
        TArray<FRayTracingGeometryRHIRef> RayTracingGeometries;
        FRayTracingSceneRHIRef RayTracingScene;
        FBufferRHIRef SceneBuffer;
        FBufferRHIRef ScratchBuffer;
        FRWBufferStructured InstanceBuffer;
        FRWBufferStructured HitGroupContributionsBuffer;
        TArray<int32> InstanceToGeometryIndex;
        FRHIShaderResourceView* SceneView = nullptr;
        uint32 NumGeometrySegments = 0;
        if (!BuildRayTracingSceneFromGeometry(
            RHICmdList,
            Geometry,
            SceneView,
            RayTracingScene,
            VertexBuffers,
            IndexBuffers,
            RayTracingGeometries,
            SceneBuffer,
            ScratchBuffer,
            InstanceBuffer,
            HitGroupContributionsBuffer,
            NumGeometrySegments,
            InstanceToGeometryIndex))
        {
            return false;
        }

        if (!SceneView || NumGeometrySegments == 0)
        {
            return true;
        }

        return TraceDetailedRaysWithHardwareRayTracing_RenderThread(RHICmdList, SceneView, Geometry, InstanceToGeometryIndex, Rays, OutHits);
    }

    bool TraceRaysWithHardwareRayTracing(
        const FUERayTracingAudioTraceRequest& Request,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<bool>& OutHits)
    {
        if (!Request.Scene)
        {
            return false;
        }

        const TArray<FUERayTracingAudioGeometryExport> Geometry = Request.Scene->GetStaticGeometry();
        bool bSucceeded = false;

        ENQUEUE_RENDER_COMMAND(UERayTracingAudioTraceRays)(
            [&Geometry, &Rays, &OutHits, &bSucceeded](FRHICommandListImmediate& RHICmdList)
            {
                bSucceeded = TraceRaysWithHardwareRayTracing_RenderThread(RHICmdList, Geometry, Rays, OutHits);
            });

        FlushRenderingCommands();
        return bSucceeded;
    }

    bool TraceDetailedRaysWithHardwareRayTracing(
        const FUERayTracingAudioTraceRequest& Request,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<FUERayTracingAudioDetailedTraceHit>& OutHits)
    {
        if (!Request.Scene)
        {
            return false;
        }

        const TArray<FUERayTracingAudioGeometryExport> Geometry = Request.Scene->GetStaticGeometry();
        bool bSucceeded = false;

        ENQUEUE_RENDER_COMMAND(UERayTracingAudioTraceDetailedRays)(
            [&Geometry, &Rays, &OutHits, &bSucceeded](FRHICommandListImmediate& RHICmdList)
            {
                bSucceeded = TraceDetailedRaysWithHardwareRayTracing_RenderThread(RHICmdList, Geometry, Rays, OutHits);
            });

        FlushRenderingCommands();
        return bSucceeded;
    }

    bool SimulateIndirectEnergyFieldWithHardwareRayTracing_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        FUERayTracingAudioEnergyFieldTraceResult& OutResult)
    {
        OutResult = FUERayTracingAudioEnergyFieldTraceResult();
        OutResult.DelayBinEnergy.Init(FVector::ZeroVector, FMath::Max(Request.NumDelayBins, 1));

        if (Geometry.IsEmpty() || Request.NumReflectionRays <= 0 || Request.MaxReflectionBounces <= 0 || Request.DurationSeconds <= 0.0f)
        {
            return true;
        }

        TArray<FBufferRHIRef> VertexBuffers;
        TArray<FBufferRHIRef> IndexBuffers;
        TArray<FRayTracingGeometryRHIRef> RayTracingGeometries;
        FRayTracingSceneRHIRef RayTracingScene;
        FBufferRHIRef SceneBuffer;
        FBufferRHIRef ScratchBuffer;
        FRWBufferStructured InstanceBuffer;
        FRWBufferStructured HitGroupContributionsBuffer;
        TArray<int32> InstanceToGeometryIndex;
        FRHIShaderResourceView* SceneView = nullptr;
        uint32 NumGeometrySegments = 0;
        if (!BuildRayTracingSceneFromGeometry(
            RHICmdList,
            Geometry,
            SceneView,
            RayTracingScene,
            VertexBuffers,
            IndexBuffers,
            RayTracingGeometries,
            SceneBuffer,
            ScratchBuffer,
            InstanceBuffer,
            HitGroupContributionsBuffer,
            NumGeometrySegments,
            InstanceToGeometryIndex))
        {
            return false;
        }

        if (!SceneView || NumGeometrySegments == 0)
        {
            return true;
        }

        TArray<FHardwareReflectionPathState> ReflectionPathBuffers[2];
        if (!DispatchGenerateListenerRaysOnGPU_RenderThread(RHICmdList, Request, ReflectionPathBuffers[0]))
        {
            GenerateListenerReflectionPaths(Request, ReflectionPathBuffers[0]);
        }
        int32 CurrentPathBufferIndex = 0;

        for (int32 BounceIndex = 0; BounceIndex < Request.MaxReflectionBounces && ReflectionPathBuffers[CurrentPathBufferIndex].Num() > 0; ++BounceIndex)
        {
            TArray<FUERayTracingAudioRay> BounceRays;
            BuildBounceRaysFromReflectionPaths(ReflectionPathBuffers[CurrentPathBufferIndex], Request.MaxTraceDistance, BounceRays);

            TArray<FUERayTracingAudioDetailedTraceHit> BounceHits;
            if (!TraceDetailedRaysWithHardwareRayTracing_RenderThread(RHICmdList, SceneView, Geometry, InstanceToGeometryIndex, BounceRays, BounceHits))
            {
                return false;
            }

            TArray<FUERayTracingAudioRay> ShadowRays;
            TArray<int32> ShadowRayPathIndices;
            ShadowRays.Reserve(ReflectionPathBuffers[CurrentPathBufferIndex].Num());
            ShadowRayPathIndices.Reserve(ReflectionPathBuffers[CurrentPathBufferIndex].Num());

            for (int32 PathIndex = 0; PathIndex < ReflectionPathBuffers[CurrentPathBufferIndex].Num(); ++PathIndex)
            {
                if (!BounceHits.IsValidIndex(PathIndex) || !BounceHits[PathIndex].bHit)
                {
                    continue;
                }

                const FVector ShadowRayStart = BounceHits[PathIndex].Location + (BounceHits[PathIndex].Normal * 1.0f);
                ShadowRays.Add(FUERayTracingAudioRay{ ShadowRayStart, Request.SourceLocation });
                ShadowRayPathIndices.Add(PathIndex);
            }

            TArray<bool> ShadowOcclusionHits;
            if (!TraceRaysWithHardwareRayTracing_RenderThread(RHICmdList, SceneView, ShadowRays, ShadowOcclusionHits))
            {
                return false;
            }

            TArray<bool> OccludedResults;
            OccludedResults.Init(true, ReflectionPathBuffers[CurrentPathBufferIndex].Num());
            for (int32 ShadowRayIndex = 0; ShadowRayIndex < ShadowRayPathIndices.Num(); ++ShadowRayIndex)
            {
                OccludedResults[ShadowRayPathIndices[ShadowRayIndex]] = ShadowOcclusionHits.IsValidIndex(ShadowRayIndex)
                    ? ShadowOcclusionHits[ShadowRayIndex]
                    : true;
            }

            const int32 NextPathBufferIndex = 1 - CurrentPathBufferIndex;
            TArray<FShadeAndGatherPathInputGPU> PathInputs;
            PathInputs.Reserve(ReflectionPathBuffers[CurrentPathBufferIndex].Num());
            for (int32 PathIndex = 0; PathIndex < ReflectionPathBuffers[CurrentPathBufferIndex].Num(); ++PathIndex)
            {
                const FHardwareReflectionPathState& Path = ReflectionPathBuffers[CurrentPathBufferIndex][PathIndex];
                const bool bHasHit = BounceHits.IsValidIndex(PathIndex) && BounceHits[PathIndex].bHit;
                const bool bOccluded = OccludedResults.IsValidIndex(PathIndex) ? OccludedResults[PathIndex] : true;
                const FVector Absorption = (bHasHit && Geometry.IsValidIndex(BounceHits[PathIndex].GeometryIndex))
                    ? Geometry[BounceHits[PathIndex].GeometryIndex].Absorption
                    : FVector::ZeroVector;

                FShadeAndGatherPathInputGPU& PathInput = PathInputs.AddDefaulted_GetRef();
                PathInput.RayOriginAndTravelDistance = FVector4f(FVector3f(Path.RayOrigin), Path.TravelDistance);
                PathInput.RayDirectionAndBounceIndex = FVector4f(FVector3f(Path.RayDirection), static_cast<float>(BounceIndex));
                PathInput.ThroughputAndHitDistance = FVector4f(FVector3f(Path.Throughput), bHasHit ? BounceHits[PathIndex].Distance : 0.0f);
                PathInput.HitLocationAndHitValid = FVector4f(bHasHit ? FVector3f(BounceHits[PathIndex].Location) : FVector3f::ZeroVector, bHasHit ? 1.0f : 0.0f);
                PathInput.HitNormalAndOccluded = FVector4f(bHasHit ? FVector3f(BounceHits[PathIndex].Normal) : FVector3f::ZeroVector, bOccluded ? 1.0f : 0.0f);
                PathInput.AbsorptionAndPadding = FVector4f(FVector3f(Absorption), 0.0f);
            }

            TArray<FVector> BounceEnergyBins;
            int32 BounceContributionCount = 0;
            if (DispatchShadeAndGatherOnGPU_RenderThread(
                RHICmdList,
                PathInputs,
                Request,
                ReflectionPathBuffers[NextPathBufferIndex],
                BounceEnergyBins,
                BounceContributionCount))
            {
                for (int32 BinIndex = 0; BinIndex < OutResult.DelayBinEnergy.Num() && BinIndex < BounceEnergyBins.Num(); ++BinIndex)
                {
                    OutResult.DelayBinEnergy[BinIndex] += BounceEnergyBins[BinIndex];
                    if (OutResult.EarliestArrivalSeconds <= 0.0f && !BounceEnergyBins[BinIndex].IsNearlyZero())
                    {
                        OutResult.EarliestArrivalSeconds = (static_cast<float>(BinIndex) + 0.5f) * (Request.DurationSeconds / static_cast<float>(OutResult.DelayBinEnergy.Num()));
                    }
                }

                OutResult.NumValidContributions += BounceContributionCount;
            }
            else
            {
                TArray<FHardwareShadedBounceState> ShadedStates;
                ShadeAndBounceHardwareReflectionPaths(
                    ReflectionPathBuffers[CurrentPathBufferIndex],
                    BounceHits,
                    Geometry,
                    Request,
                    BounceIndex,
                    ShadedStates,
                    ReflectionPathBuffers[NextPathBufferIndex]);

                GatherEnergyFieldFromShadowResults(ShadedStates, ShadowOcclusionHits, ShadowRayPathIndices, Request, OutResult);
            }

            if (BounceIndex < Request.MaxReflectionBounces - 1)
            {
                CurrentPathBufferIndex = NextPathBufferIndex;
                ReflectionPathBuffers[1 - CurrentPathBufferIndex].Reset();
            }
        }

        return true;
    }

    bool SimulateIndirectEnergyFieldWithHardwareRayTracing(
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        FUERayTracingAudioEnergyFieldTraceResult& OutResult)
    {
        if (!Request.Scene)
        {
            return false;
        }

        const TArray<FUERayTracingAudioGeometryExport> Geometry = Request.Scene->GetStaticGeometry();
        bool bSucceeded = false;

        ENQUEUE_RENDER_COMMAND(UERayTracingAudioSimulateIndirectEnergyField)(
            [&Geometry, &Request, &OutResult, &bSucceeded](FRHICommandListImmediate& RHICmdList)
            {
                bSucceeded = SimulateIndirectEnergyFieldWithHardwareRayTracing_RenderThread(RHICmdList, Geometry, Request, OutResult);
            });

        FlushRenderingCommands();
        return bSucceeded;
    }
#endif
}

bool FUERayTracingAudioRayTracingDevice::IsRayTracingAvailable() const
{
    return GRHISupportsRayTracing && GRHISupportsRayTracingShaders;
}

bool FUERayTracingAudioRayTracingDevice::TraceDirectPath(const FUERayTracingAudioTraceRequest& Request, FUERayTracingAudioTraceHit& OutHit) const
{
    OutHit = FUERayTracingAudioTraceHit();

    TArray<bool> HitResults;
    TArray<FUERayTracingAudioRay> Rays;
    Rays.Add(FUERayTracingAudioRay{ Request.Start, Request.End });
    if (!TraceRays(Request, Rays, HitResults) || HitResults.Num() == 0 || !HitResults[0])
    {
        return false;
    }

    OutHit.bHit = true;
    OutHit.Location = Request.End;
    OutHit.Distance = FVector::Distance(Request.Start, Request.End);
    return true;
}

bool FUERayTracingAudioRayTracingDevice::TraceRays(
    const FUERayTracingAudioTraceRequest& Request,
    const TArray<FUERayTracingAudioRay>& Rays,
    TArray<bool>& OutHits) const
{
    if (Rays.IsEmpty())
    {
        OutHits.Reset();
        return true;
    }

#if RHI_RAYTRACING
    if (IsRayTracingAvailable() && Request.Scene && !Request.Scene->IsEmpty())
    {
        if (TraceRaysWithHardwareRayTracing(Request, Rays, OutHits))
        {
            if (!GHasLoggedHardwareRayTracingPath)
            {
                UE_LOG(LogUERayTracingAudioSDK, Display, TEXT("UERayTracingAudioSDK uses hardware ray tracing for direct sound visibility queries."));
                GHasLoggedHardwareRayTracingPath = true;
            }
            return true;
        }
    }
#endif

    if (!GHasLoggedPhysicsFallbackPath)
    {
        UE_LOG(LogUERayTracingAudioSDK, Warning, TEXT("UERayTracingAudioSDK falls back to physics line traces for direct sound visibility queries."));
        GHasLoggedPhysicsFallbackPath = true;
    }

    return TraceRaysWithPhysics(Request, Rays, OutHits);
}

bool FUERayTracingAudioRayTracingDevice::TraceDetailedRays(
    const FUERayTracingAudioTraceRequest& Request,
    const TArray<FUERayTracingAudioRay>& Rays,
    TArray<FUERayTracingAudioDetailedTraceHit>& OutHits) const
{
    if (Rays.IsEmpty())
    {
        OutHits.Reset();
        return true;
    }

#if RHI_RAYTRACING
    if (IsRayTracingAvailable() && Request.Scene && !Request.Scene->IsEmpty())
    {
        if (TraceDetailedRaysWithHardwareRayTracing(Request, Rays, OutHits))
        {
            if (!GHasLoggedIndirectHardwareRayTracingPath)
            {
                UE_LOG(LogUERayTracingAudioSDK, Display, TEXT("UERayTracingAudioSDK uses hardware ray tracing for indirect sound path queries."));
                GHasLoggedIndirectHardwareRayTracingPath = true;
            }
            return true;
        }
    }
#endif

    if (!GHasLoggedIndirectCpuFallbackPath)
    {
        UE_LOG(LogUERayTracingAudioSDK, Warning, TEXT("UERayTracingAudioSDK falls back to CPU acoustic scene queries for indirect sound path tracing."));
        GHasLoggedIndirectCpuFallbackPath = true;
    }

    return false;
}

bool FUERayTracingAudioRayTracingDevice::SimulateIndirectEnergyField(
    const FUERayTracingAudioEnergyFieldTraceRequest& Request,
    FUERayTracingAudioEnergyFieldTraceResult& OutResult) const
{
#if RHI_RAYTRACING
    if (IsRayTracingAvailable() && Request.Scene && !Request.Scene->IsEmpty())
    {
        return SimulateIndirectEnergyFieldWithHardwareRayTracing(Request, OutResult);
    }
#endif

    OutResult = FUERayTracingAudioEnergyFieldTraceResult();
    return false;
}
