#include "RayTracing/UERayTracingAudioRayTracingDevice.h"

#include "BuiltInRayTracingShaders.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PipelineStateCache.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHIResourceUtils.h"
#include "RayTracingDefinitions.h"
#include "RayTracingInstanceBufferUtil.h"
#include "RayTracingPayloadType.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "Scene/UERayTracingAudioScene.h"
#include "ShaderParameterStruct.h"
#include "Simulation/UERayTracingAudioEnergyFieldShaders.h"
#include "Misc/ScopeLock.h"
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
    // Each path is normalized by NumReflectionRays before GPU atomic accumulation.
    // 1e6 rounded valid high-ray-count, multi-bounce contributions down to zero.
    // 1e8 preserves the shared 1e-8 minimum while the worst-case 64-bounce
    // harmonic sum remains well below the uint32 accumulation limit.
    constexpr uint32 EnergyQuantizationScale = 100000000u;
    constexpr float RayTracingMinimumPathEnergy = 1e-8f;

    bool GHasLoggedHardwareRayTracingPath = false;
    bool GHasLoggedPhysicsFallbackPath = false;
    bool GHasLoggedIndirectHardwareRayTracingPath = false;
    bool GHasLoggedIndirectCpuFallbackPath = false;
    uint64 GStaticMeshBLASCacheSerial = 0;

    constexpr uint64 StaticMeshBLASCacheMaxUnusedSerials = 240;
    constexpr int32 StaticMeshBLASCacheMaxEntries = 128;

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

    struct FCachedStaticMeshRayTracingGeometry
    {
        FBufferRHIRef VertexBuffer;
        FBufferRHIRef IndexBuffer;
        FRayTracingGeometryRHIRef RayTracingGeometry;
        uint32 NumPrimitives = 0;
        uint32 NumVertices = 0;
        uint32 NumIndices = 0;
        uint64 LastUsedSerial = 0;
    };

    TMap<FString, FCachedStaticMeshRayTracingGeometry> GStaticMeshBLASCache;

    struct FHardwareReflectionPathState
    {
        FVector RayOrigin = FVector::ZeroVector;
        FVector RayDirection = FVector::ForwardVector;
        FVector ListenerDirection = FVector::ForwardVector;
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

    FVector GenerateMaterialBounceDirection(
        const FVector& IncomingDirection,
        const FVector& SurfaceNormal,
        float Scattering,
        int32 SampleIndex)
    {
        const FVector SpecularDirection = IncomingDirection.MirrorByVector(SurfaceNormal).GetSafeNormal();
        FVector DiffuseDirection = RayTracingDeviceGenerateSphereDirectionSample(SampleIndex);
        if (FVector::DotProduct(DiffuseDirection, SurfaceNormal) < 0.0f)
        {
            DiffuseDirection *= -1.0f;
        }
        DiffuseDirection = (DiffuseDirection + SurfaceNormal).GetSafeNormal();
        return FMath::Lerp(SpecularDirection, DiffuseDirection, FMath::Clamp(Scattering, 0.0f, 1.0f)).GetSafeNormal();
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
            Path.ListenerDirection = Path.RayDirection;
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

            const FUERayTracingAudioGeometryExport& HitGeometry = Geometry[Hit.GeometryIndex];
            const FVector Reflection = FVector::OneVector - HitGeometry.Absorption - HitGeometry.Transmission;

            ShadedState.bHasHit = true;
            ShadedState.GeometryIndex = Hit.GeometryIndex;
            ShadedState.NextPath = ActivePaths[PathIndex];
            ShadedState.NextPath.TravelDistance += Hit.Distance;
            ShadedState.NextPath.Throughput.X *= FMath::Clamp(Reflection.X, 0.0f, 1.0f);
            ShadedState.NextPath.Throughput.Y *= FMath::Clamp(Reflection.Y, 0.0f, 1.0f);
            ShadedState.NextPath.Throughput.Z *= FMath::Clamp(Reflection.Z, 0.0f, 1.0f);
            ShadedState.NextPath.RayOrigin = Hit.Location + (Hit.Normal * 1.0f);
            ShadedState.NextPath.RayDirection = GenerateMaterialBounceDirection(
                ActivePaths[PathIndex].RayDirection,
                Hit.Normal,
                HitGeometry.Scattering,
                PathIndex + (BounceIndex * FMath::Max(ActivePaths.Num(), 1)));

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
            if (MonoEnergy <= RayTracingMinimumPathEnergy)
            {
                continue;
            }

            const int32 DelayBinIndex = FMath::Clamp(
                FMath::FloorToInt(DelaySeconds / DelayBinDurationSeconds),
                0,
                OutResult.DelayBinEnergy.Num() - 1);
            OutResult.DelayBinEnergy[DelayBinIndex] += BandGain;
            if (OutResult.DelayBinDirection.IsValidIndex(DelayBinIndex))
            {
                OutResult.DelayBinDirection[DelayBinIndex] += ShadedState.NextPath.ListenerDirection * MonoEnergy;
            }
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
            Path.ListenerDirection = Path.RayDirection;
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
        TArray<FVector>& OutDirectionBins,
        int32& OutContributionCount)
    {
        OutNextPaths.Reset();
        OutEnergyBins.Init(FVector::ZeroVector, FMath::Max(Request.NumDelayBins, 1));
        OutDirectionBins.Init(FVector::ZeroVector, OutEnergyBins.Num());
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
            const int32 ContributionBinIndex = FMath::RoundToInt(Output.ThroughputAndPadding.W) - 1;
            if (OutDirectionBins.IsValidIndex(ContributionBinIndex)
                && Output.ListenerDirectionAndContribution.W > 0.0f)
            {
                const FVector ListenerDirection(
                    Output.ListenerDirectionAndContribution.X,
                    Output.ListenerDirectionAndContribution.Y,
                    Output.ListenerDirectionAndContribution.Z);
                OutDirectionBins[ContributionBinIndex] += ListenerDirection.GetSafeNormal()
                    * Output.ListenerDirectionAndContribution.W;
            }
            if (Output.NextDirectionAndAlive.W < 0.5f)
            {
                continue;
            }

            FHardwareReflectionPathState& NextPath = OutNextPaths.AddDefaulted_GetRef();
            NextPath.RayOrigin = FVector(Output.NextOriginAndTravelDistance.X, Output.NextOriginAndTravelDistance.Y, Output.NextOriginAndTravelDistance.Z);
            NextPath.TravelDistance = Output.NextOriginAndTravelDistance.W;
            NextPath.RayDirection = FVector(Output.NextDirectionAndAlive.X, Output.NextDirectionAndAlive.Y, Output.NextDirectionAndAlive.Z).GetSafeNormal();
            NextPath.ListenerDirection = FVector(
                Output.ListenerDirectionAndContribution.X,
                Output.ListenerDirectionAndContribution.Y,
                Output.ListenerDirectionAndContribution.Z).GetSafeNormal();
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

    void AppendTriangleGeometry(
        const FUERayTracingAudioGeometryExport& GeometryExport,
        bool bOutputWorldSpace,
        TArray<FVector3f>& OutVertices,
        TArray<uint32>& OutIndices)
    {
        const uint32 BaseVertex = static_cast<uint32>(OutVertices.Num());
        OutVertices.Reserve(OutVertices.Num() + GeometryExport.Vertices.Num());
        OutIndices.Reserve(OutIndices.Num() + GeometryExport.Indices.Num());

        for (int32 VertexIndex = 0; VertexIndex < GeometryExport.Vertices.Num(); ++VertexIndex)
        {
            const FVector Vertex = bOutputWorldSpace
                ? GeometryExport.GetVertexWorldPosition(VertexIndex)
                : GeometryExport.Vertices[VertexIndex];
            OutVertices.Add(FVector3f(Vertex));
        }

        for (uint32 Index : GeometryExport.Indices)
        {
            OutIndices.Add(BaseVertex + Index);
        }
    }

    void BuildGeometryArrays(
        const FUERayTracingAudioGeometryExport& GeometryExport,
        bool bOutputWorldSpace,
        TArray<FVector3f>& OutVertices,
        TArray<uint32>& OutIndices)
    {
        OutVertices.Reset();
        OutIndices.Reset();

        if (GeometryExport.bUseStaticMeshTriangles && GeometryExport.HasTriangleMesh())
        {
            AppendTriangleGeometry(GeometryExport, bOutputWorldSpace, OutVertices, OutIndices);
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
        BuildGeometryArrays(GeometryExport, true, Vertices, Indices);

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

    bool CreateRayTracingGeometryFromArrays(
        FRHICommandListImmediate& RHICmdList,
        const TCHAR* DebugName,
        const TArray<FVector3f>& Vertices,
        const TArray<uint32>& Indices,
        FBufferRHIRef& OutVertexBuffer,
        FBufferRHIRef& OutIndexBuffer,
        FRayTracingGeometryRHIRef& OutRayTracingGeometry)
    {
        if (Vertices.IsEmpty() || Indices.Num() < 3)
        {
            return false;
        }

        OutVertexBuffer = UE::RHIResourceUtils::CreateVertexBufferFromArray(
            RHICmdList,
            DebugName,
            EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource,
            MakeConstArrayView(Vertices));

        OutIndexBuffer = UE::RHIResourceUtils::CreateIndexBufferFromArray(
            RHICmdList,
            DebugName,
            EBufferUsageFlags::Static | EBufferUsageFlags::ShaderResource,
            MakeConstArrayView(Indices));

        FRayTracingGeometryInitializer GeometryInitializer;
        GeometryInitializer.DebugName = FName(DebugName);
        GeometryInitializer.IndexBuffer = OutIndexBuffer;
        GeometryInitializer.GeometryType = RTGT_Triangles;
        GeometryInitializer.bFastBuild = false;

        FRayTracingGeometrySegment Segment;
        Segment.VertexBuffer = OutVertexBuffer;
        Segment.VertexBufferStride = sizeof(FVector3f);
        Segment.NumPrimitives = Indices.Num() / 3;
        Segment.MaxVertices = Vertices.Num();
        GeometryInitializer.Segments.Add(Segment);
        GeometryInitializer.TotalPrimitiveCount = Segment.NumPrimitives;

        OutRayTracingGeometry = RHICmdList.CreateRayTracingGeometry(GeometryInitializer);
        RHICmdList.BuildAccelerationStructure(OutRayTracingGeometry);
        return OutRayTracingGeometry.IsValid();
    }

    void TrimStaticMeshBLASCache()
    {
        for (auto CacheIt = GStaticMeshBLASCache.CreateIterator(); CacheIt; ++CacheIt)
        {
            if (GStaticMeshBLASCacheSerial > CacheIt.Value().LastUsedSerial
                && GStaticMeshBLASCacheSerial - CacheIt.Value().LastUsedSerial > StaticMeshBLASCacheMaxUnusedSerials)
            {
                CacheIt.RemoveCurrent();
            }
        }

        while (GStaticMeshBLASCache.Num() > StaticMeshBLASCacheMaxEntries)
        {
            FString OldestKey;
            uint64 OldestSerial = MAX_uint64;
            for (const TPair<FString, FCachedStaticMeshRayTracingGeometry>& Pair : GStaticMeshBLASCache)
            {
                if (Pair.Value.LastUsedSerial < OldestSerial)
                {
                    OldestKey = Pair.Key;
                    OldestSerial = Pair.Value.LastUsedSerial;
                }
            }
            if (OldestKey.IsEmpty())
            {
                break;
            }
            GStaticMeshBLASCache.Remove(OldestKey);
        }
    }

    bool GetOrCreateCachedStaticMeshRayTracingGeometry(
        FRHICommandListImmediate& RHICmdList,
        const FUERayTracingAudioGeometryExport& GeometryExport,
        FRayTracingGeometryRHIRef& OutRayTracingGeometry)
    {
        const FString CacheKey = GeometryExport.GetRayTracingGeometryCacheKey();
        if (CacheKey.IsEmpty())
        {
            return false;
        }

        if (FCachedStaticMeshRayTracingGeometry* CachedGeometry = GStaticMeshBLASCache.Find(CacheKey))
        {
            if (CachedGeometry->RayTracingGeometry.IsValid()
                && CachedGeometry->VertexBuffer.IsValid()
                && CachedGeometry->IndexBuffer.IsValid()
                && CachedGeometry->NumVertices == static_cast<uint32>(GeometryExport.Vertices.Num())
                && CachedGeometry->NumIndices == static_cast<uint32>(GeometryExport.Indices.Num()))
            {
                CachedGeometry->LastUsedSerial = GStaticMeshBLASCacheSerial;
                OutRayTracingGeometry = CachedGeometry->RayTracingGeometry;
                return true;
            }

            GStaticMeshBLASCache.Remove(CacheKey);
        }

        TArray<FVector3f> LocalVertices;
        TArray<uint32> LocalIndices;
        BuildGeometryArrays(GeometryExport, false, LocalVertices, LocalIndices);
        if (LocalIndices.IsEmpty())
        {
            return false;
        }

        FCachedStaticMeshRayTracingGeometry NewCacheEntry;
        if (!CreateRayTracingGeometryFromArrays(
            RHICmdList,
            TEXT("UERayTracingAudioStaticMeshBLAS"),
            LocalVertices,
            LocalIndices,
            NewCacheEntry.VertexBuffer,
            NewCacheEntry.IndexBuffer,
            NewCacheEntry.RayTracingGeometry))
        {
            return false;
        }

        NewCacheEntry.NumPrimitives = LocalIndices.Num() / 3;
        NewCacheEntry.NumVertices = static_cast<uint32>(LocalVertices.Num());
        NewCacheEntry.NumIndices = static_cast<uint32>(LocalIndices.Num());
        NewCacheEntry.LastUsedSerial = GStaticMeshBLASCacheSerial;
        OutRayTracingGeometry = NewCacheEntry.RayTracingGeometry;
        GStaticMeshBLASCache.Add(CacheKey, MoveTemp(NewCacheEntry));
        TrimStaticMeshBLASCache();
        return true;
    }

    bool BuildRayTracingSceneFromGeometry(
        FRHICommandListImmediate& RHICmdList,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const EUERayTracingAudioGeometryUsage Usage,
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

        ++GStaticMeshBLASCacheSerial;
        TrimStaticMeshBLASCache();

        TArray<FVector3f> Vertices;
        TArray<uint32> Indices;

        for (int32 GeometryIndex = 0; GeometryIndex < Geometry.Num(); ++GeometryIndex)
        {
            const FUERayTracingAudioGeometryExport& GeometryExport = Geometry[GeometryIndex];
            if (!GeometryExport.IsVisibleForUsage(Usage))
            {
                continue;
            }
            if (!GeometryExport.HasBuildableGeometry())
            {
                return false;
            }

            FRayTracingGeometryRHIRef RayTracingGeometry;

            // Static-mesh snapshots contain only immutable values. The Render Thread
            // therefore uses the plugin-managed BLAS cache and never dereferences a
            // UStaticMesh (or any other UObject) to borrow renderer-owned resources.
            const bool bUsingCachedStaticMeshGeometry = GeometryExport.HasCachedStaticMeshSource()
                && GetOrCreateCachedStaticMeshRayTracingGeometry(RHICmdList, GeometryExport, RayTracingGeometry);
            if (!bUsingCachedStaticMeshGeometry)
            {
                BuildGeometryArrays(GeometryExport, !GeometryExport.bVerticesAreLocalSpace, Vertices, Indices);
                if (Indices.IsEmpty())
                {
                    continue;
                }

                FBufferRHIRef VertexBuffer;
                FBufferRHIRef IndexBuffer;
                if (!CreateRayTracingGeometryFromArrays(
                    RHICmdList,
                    TEXT("UERayTracingAudioBLAS"),
                    Vertices,
                    Indices,
                    VertexBuffer,
                    IndexBuffer,
                    RayTracingGeometry))
                {
                    continue;
                }

                OutVertexBuffers.Add(VertexBuffer);
                OutIndexBuffers.Add(IndexBuffer);
            }

            OutRayTracingGeometries.Add(RayTracingGeometry);

            // Cached static-mesh geometry is local-space; BoundingBox and fallback
            // triangle arrays are emitted in world-space and use identity.
            const bool bNeedsLocalToWorldTransform =
                bUsingCachedStaticMeshGeometry && GeometryExport.bVerticesAreLocalSpace;
            const FMatrix InstanceTransform = bNeedsLocalToWorldTransform
                ? GeometryExport.Transform.ToMatrixWithScale()
                : FMatrix::Identity;
            const int32 TransformIndex = InstanceTransforms.Add(InstanceTransform);
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
            // DXR needs a TLAS to execute a ray dispatch even when the logical
            // acoustic scene is empty. Keep this sentinel out of Geometry and
            // give it an all-zero instance mask so no plugin ray can intersect
            // it. This preserves an exact zero-energy scene while proving the
            // Direct and Indirect results came through a real RHI dispatch.
            Vertices = {
                FVector3f(0.0f, 0.0f, 0.0f),
                FVector3f(1.0f, 0.0f, 0.0f),
                FVector3f(0.0f, 1.0f, 0.0f),
            };
            Indices = { 0u, 1u, 2u };

            FBufferRHIRef SentinelVertexBuffer;
            FBufferRHIRef SentinelIndexBuffer;
            FRayTracingGeometryRHIRef SentinelRayTracingGeometry;
            if (!CreateRayTracingGeometryFromArrays(
                RHICmdList,
                TEXT("UERayTracingAudioEmptySceneSentinelBLAS"),
                Vertices,
                Indices,
                SentinelVertexBuffer,
                SentinelIndexBuffer,
                SentinelRayTracingGeometry))
            {
                return false;
            }

            OutVertexBuffers.Add(SentinelVertexBuffer);
            OutIndexBuffers.Add(SentinelIndexBuffer);
            OutRayTracingGeometries.Add(SentinelRayTracingGeometry);
            const int32 TransformIndex = InstanceTransforms.Add(FMatrix::Identity);
            FRayTracingGeometryInstance& SentinelInstance =
                Instances.AddDefaulted_GetRef();
            SentinelInstance.GeometryRHI = SentinelRayTracingGeometry;
            SentinelInstance.NumTransforms = 1;
            SentinelInstance.Transforms =
                MakeArrayView(&InstanceTransforms[TransformIndex], 1);
            SentinelInstance.InstanceContributionToHitGroupIndex = 0;
            SentinelInstance.DefaultUserData = 0u;
            SentinelInstance.Mask = 0x00;
            OutInstanceToGeometryIndex.Add(INDEX_NONE);
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

    struct FCachedRayTracingAudioSceneResources
    {
        ~FCachedRayTracingAudioSceneResources()
        {
            InstanceBuffer.Release();
            HitGroupContributionsBuffer.Release();
        }

        TArray<FBufferRHIRef> VertexBuffers;
        TArray<FBufferRHIRef> IndexBuffers;
        TArray<FRayTracingGeometryRHIRef> RayTracingGeometries;
        FRayTracingSceneRHIRef RayTracingScene;
        FBufferRHIRef SceneBuffer;
        FBufferRHIRef ScratchBuffer;
        FRWBufferStructured InstanceBuffer;
        FRWBufferStructured HitGroupContributionsBuffer;
        FShaderResourceViewRHIRef SceneView;
        TArray<int32> InstanceToGeometryIndex;
        uint64 LastUsedSerial = 0;
    };

    TMap<uint64, TSharedPtr<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe>> GSceneTLASCache;
    uint64 GSceneTLASCacheSerial = 0;
    constexpr int32 SceneTLASCacheMaxEntries = 4;
    constexpr uint64 SceneTLASCacheMaxUnusedSerials = 240;

    uint64 GetSceneTLASCacheKey(
        const uint64 SceneCacheKey,
        const EUERayTracingAudioGeometryUsage Usage)
    {
        if (SceneCacheKey == 0)
        {
            return 0;
        }

        constexpr uint64 DirectUsageSalt = 0xD1EC7A5B4C3D291Full;
        constexpr uint64 IndirectUsageSalt = 0x9E3779B97F4A7C15ull;
        return Usage == EUERayTracingAudioGeometryUsage::Direct
            ? (SceneCacheKey ^ DirectUsageSalt)
            : (SceneCacheKey ^ IndirectUsageSalt);
    }

    void TrimSceneTLASCache_RenderThread()
    {
        check(IsInRenderingThread());
        while (GSceneTLASCache.Num() > SceneTLASCacheMaxEntries)
        {
            uint64 OldestKey = 0;
            uint64 OldestSerial = MAX_uint64;
            for (const TPair<uint64, TSharedPtr<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe>>& Pair : GSceneTLASCache)
            {
                if (Pair.Value.IsValid() && Pair.Value->LastUsedSerial < OldestSerial)
                {
                    OldestKey = Pair.Key;
                    OldestSerial = Pair.Value->LastUsedSerial;
                }
            }
            if (OldestSerial == MAX_uint64)
            {
                break;
            }
            GSceneTLASCache.Remove(OldestKey);
        }

        for (auto It = GSceneTLASCache.CreateIterator(); It; ++It)
        {
            if (!It.Value().IsValid()
                || (GSceneTLASCacheSerial > It.Value()->LastUsedSerial
                    && GSceneTLASCacheSerial - It.Value()->LastUsedSerial > SceneTLASCacheMaxUnusedSerials))
            {
                It.RemoveCurrent();
            }
        }
    }

    TSharedPtr<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe> GetOrBuildSceneTLAS_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        uint64 SceneCacheKey,
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const EUERayTracingAudioGeometryUsage Usage)
    {
        check(IsInRenderingThread());
        ++GSceneTLASCacheSerial;
        const uint64 TLASCacheKey = GetSceneTLASCacheKey(
            SceneCacheKey,
            Usage);

        if (TLASCacheKey != 0)
        {
            if (TSharedPtr<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe>* Existing = GSceneTLASCache.Find(TLASCacheKey))
            {
                if (Existing->IsValid())
                {
                    (*Existing)->LastUsedSerial = GSceneTLASCacheSerial;
                    return *Existing;
                }
            }
        }

        TSharedPtr<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe> Resources =
            MakeShared<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe>();
        FRHIShaderResourceView* SceneView = nullptr;
        uint32 NumGeometrySegments = 0;
        if (!BuildRayTracingSceneFromGeometry(
            RHICmdList,
            Geometry,
            Usage,
            SceneView,
            Resources->RayTracingScene,
            Resources->VertexBuffers,
            Resources->IndexBuffers,
            Resources->RayTracingGeometries,
            Resources->SceneBuffer,
            Resources->ScratchBuffer,
            Resources->InstanceBuffer,
            Resources->HitGroupContributionsBuffer,
            NumGeometrySegments,
            Resources->InstanceToGeometryIndex)
            || !SceneView
            || NumGeometrySegments == 0)
        {
            return nullptr;
        }

        Resources->SceneView = SceneView;
        Resources->LastUsedSerial = GSceneTLASCacheSerial;
        if (TLASCacheKey != 0)
        {
            GSceneTLASCache.Add(TLASCacheKey, Resources);
            TrimSceneTLASCache_RenderThread();
        }
        return Resources;
    }

    bool DispatchOcclusionReadback_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FRHIShaderResourceView* SceneView,
        const TArray<FUERayTracingAudioRay>& Rays,
        TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe>& OutReadback)
    {
        if (!SceneView || Rays.IsEmpty())
        {
            return false;
        }

        TArray<FUERayTracingAudioBasicRay> RayData;
        BuildBasicRayBufferData(Rays, RayData);
        FBufferRHIRef RayBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
            RHICmdList,
            TEXT("UERayTracingAudioAsyncRayBuffer"),
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
            FRHIBufferCreateDesc::CreateStructured<uint32>(TEXT("UERayTracingAudioAsyncResultBuffer"), RayData.Num())
            .AddUsage(EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy)
            .SetInitialState(ERHIAccess::UAVMask);
        FBufferRHIRef ResultBuffer = RHICmdList.CreateBuffer(ResultBufferCreateDesc);
        FUnorderedAccessViewRHIRef ResultBufferView = RHICmdList.CreateUnorderedAccessView(
            ResultBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(uint32))
                .SetNumElements(RayData.Num()));

        DispatchOcclusionRays(RHICmdList, SceneView, RayBufferView, ResultBufferView, RayData.Num());
        RHICmdList.Transition(FRHITransitionInfo(ResultBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
        OutReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("UERayTracingAudioOcclusionReadback"));
        OutReadback->EnqueueCopy(RHICmdList, ResultBuffer, sizeof(uint32) * RayData.Num());
        return true;
    }

    bool DispatchDetailedReadback_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        FRHIShaderResourceView* SceneView,
        int32 NumGeometryInstances,
        const TArray<FUERayTracingAudioRay>& Rays,
        TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe>& OutReadback)
    {
        if (!SceneView || Rays.IsEmpty())
        {
            return false;
        }

        TArray<FUERayTracingAudioDetailedRay> RayData;
        BuildDetailedRayBufferData(Rays, RayData);
        FBufferRHIRef RayBuffer = UE::RHIResourceUtils::CreateBufferFromArray(
            RHICmdList,
            TEXT("UERayTracingAudioAsyncIndirectRayBuffer"),
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
            FRHIBufferCreateDesc::CreateStructured<FUERayTracingAudioDetailedTraceResult>(
                TEXT("UERayTracingAudioAsyncIndirectResultBuffer"),
                RayData.Num())
            .AddUsage(EBufferUsageFlags::Static | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::SourceCopy)
            .SetInitialState(ERHIAccess::UAVMask);
        FBufferRHIRef ResultBuffer = RHICmdList.CreateBuffer(ResultBufferCreateDesc);
        FUnorderedAccessViewRHIRef ResultBufferView = RHICmdList.CreateUnorderedAccessView(
            ResultBuffer,
            FRHIViewDesc::CreateBufferUAV()
                .SetType(FRHIViewDesc::EBufferType::Structured)
                .SetStride(sizeof(FUERayTracingAudioDetailedTraceResult))
                .SetNumElements(RayData.Num()));

        DispatchDetailedRays(
            RHICmdList,
            SceneView,
            RayBufferView,
            ResultBufferView,
            RayData.Num(),
            static_cast<uint32>(FMath::Max(NumGeometryInstances, 1)));
        RHICmdList.Transition(FRHITransitionInfo(ResultBuffer, ERHIAccess::UAVCompute, ERHIAccess::CopySrc));
        OutReadback = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("UERayTracingAudioDetailedReadback"));
        OutReadback->EnqueueCopy(
            RHICmdList,
            ResultBuffer,
            sizeof(FUERayTracingAudioDetailedTraceResult) * RayData.Num());
        return true;
    }

    void DecodeDetailedTraceResults(
        const TArray<FUERayTracingAudioGeometryExport>& Geometry,
        const TArray<int32>& InstanceToGeometryIndex,
        const TArray<FUERayTracingAudioRay>& Rays,
        const FUERayTracingAudioDetailedTraceResult* Results,
        TArray<FUERayTracingAudioDetailedTraceHit>& OutHits)
    {
        OutHits.Init(FUERayTracingAudioDetailedTraceHit(), Rays.Num());
        if (!Results)
        {
            return;
        }

        for (int32 Index = 0; Index < Rays.Num(); ++Index)
        {
            const FUERayTracingAudioDetailedTraceResult& Result = Results[Index];
            FUERayTracingAudioDetailedTraceHit& OutHit = OutHits[Index];
            OutHit.bHit = Result.HitT >= 0.0f
                && InstanceToGeometryIndex.IsValidIndex(static_cast<int32>(Result.InstanceIndex))
                && Geometry.IsValidIndex(InstanceToGeometryIndex[Result.InstanceIndex]);
            OutHit.Distance = OutHit.bHit ? Result.HitT : 0.0f;
            OutHit.GeometryIndex = OutHit.bHit ? InstanceToGeometryIndex[Result.InstanceIndex] : INDEX_NONE;
            const FVector Direction = (Rays[Index].End - Rays[Index].Start).GetSafeNormal();
            OutHit.Location = Rays[Index].Start + (Direction * OutHit.Distance);
            OutHit.Normal = FVector::UpVector;

            if (OutHit.bHit)
            {
                FVector A;
                FVector B;
                FVector C;
                if (GetPrimitiveTriangle(Geometry[OutHit.GeometryIndex], Result.PrimitiveIndex, A, B, C))
                {
                    OutHit.Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
                    if (FVector::DotProduct(OutHit.Normal, Direction) > 0.0f)
                    {
                        OutHit.Normal *= -1.0f;
                    }
                }
            }
        }
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
            EUERayTracingAudioGeometryUsage::Direct,
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
            EUERayTracingAudioGeometryUsage::Indirect,
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
        OutResult.DelayBinDirection.Init(FVector::ZeroVector, OutResult.DelayBinEnergy.Num());

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
            EUERayTracingAudioGeometryUsage::Indirect,
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
                const FUERayTracingAudioGeometryExport* HitGeometry = (bHasHit && Geometry.IsValidIndex(BounceHits[PathIndex].GeometryIndex))
                    ? &Geometry[BounceHits[PathIndex].GeometryIndex]
                    : nullptr;
                const FVector Absorption = HitGeometry ? HitGeometry->Absorption : FVector::ZeroVector;
                const FVector Transmission = HitGeometry ? HitGeometry->Transmission : FVector::ZeroVector;
                const float Scattering = HitGeometry ? HitGeometry->Scattering : 0.0f;

                FShadeAndGatherPathInputGPU& PathInput = PathInputs.AddDefaulted_GetRef();
                PathInput.RayOriginAndTravelDistance = FVector4f(FVector3f(Path.RayOrigin), Path.TravelDistance);
                PathInput.RayDirectionAndBounceIndex = FVector4f(FVector3f(Path.RayDirection), static_cast<float>(BounceIndex));
                PathInput.ThroughputAndHitDistance = FVector4f(FVector3f(Path.Throughput), bHasHit ? BounceHits[PathIndex].Distance : 0.0f);
                PathInput.HitLocationAndHitValid = FVector4f(bHasHit ? FVector3f(BounceHits[PathIndex].Location) : FVector3f::ZeroVector, bHasHit ? 1.0f : 0.0f);
                PathInput.HitNormalAndOccluded = FVector4f(bHasHit ? FVector3f(BounceHits[PathIndex].Normal) : FVector3f::ZeroVector, bOccluded ? 1.0f : 0.0f);
                PathInput.AbsorptionAndPadding = FVector4f(FVector3f(Absorption), 0.0f);
                PathInput.TransmissionAndScattering = FVector4f(FVector3f(Transmission), Scattering);
                PathInput.ListenerDirectionAndPadding = FVector4f(FVector3f(Path.ListenerDirection), 0.0f);
            }

            TArray<FVector> BounceEnergyBins;
            TArray<FVector> BounceDirectionBins;
            int32 BounceContributionCount = 0;
            if (DispatchShadeAndGatherOnGPU_RenderThread(
                RHICmdList,
                PathInputs,
                Request,
                ReflectionPathBuffers[NextPathBufferIndex],
                BounceEnergyBins,
                BounceDirectionBins,
                BounceContributionCount))
            {
                for (int32 BinIndex = 0; BinIndex < OutResult.DelayBinEnergy.Num() && BinIndex < BounceEnergyBins.Num(); ++BinIndex)
                {
                    OutResult.DelayBinEnergy[BinIndex] += BounceEnergyBins[BinIndex];
                    if (OutResult.DelayBinDirection.IsValidIndex(BinIndex)
                        && BounceDirectionBins.IsValidIndex(BinIndex))
                    {
                        OutResult.DelayBinDirection[BinIndex] += BounceDirectionBins[BinIndex];
                    }
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

struct FUERayTracingAudioAsyncRayQuery::FReadbackState
{
    struct FQuerySegment
    {
        TWeakPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> Query;
        int32 RayOffset = 0;
        int32 NumRays = 0;
    };

    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> Readback;
    TSharedPtr<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe> SceneResources;
    TArray<FQuerySegment> Segments;
    TAtomic<bool> bPollQueued = false;
    int32 NumCombinedRays = 0;
};

struct FUERayTracingAudioAsyncEnergyFieldQuery::FReadbackState
{
    enum class EPhase : uint8
    {
        DetailedHit,
        ShadowVisibility
    };

    struct FItem
    {
        TWeakPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe> Query;
        FUERayTracingAudioEnergyFieldTraceRequest Request;
        FUERayTracingAudioEnergyFieldTraceResult Result;

        TArray<FHardwareReflectionPathState> ActivePaths;
        TArray<FHardwareReflectionPathState> NextPaths;
        TArray<FHardwareShadedBounceState> ShadedStates;
        TArray<FUERayTracingAudioRay> BounceRays;
        TArray<FUERayTracingAudioDetailedTraceHit> BounceHits;
        TArray<FUERayTracingAudioRay> ShadowRays;
        TArray<int32> ShadowRayStateIndices;
        int32 ReadbackOffset = 0;
        int32 ReadbackCount = 0;
        int32 BounceIndex = 0;
    };

    TArray<FUERayTracingAudioGeometryExport> Geometry;
    TSharedPtr<FCachedRayTracingAudioSceneResources, ESPMode::ThreadSafe> SceneResources;
    TArray<FItem> Items;
    TArray<FUERayTracingAudioRay> CombinedRays;
    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> Readback;
    TAtomic<bool> bPollQueued = false;
    EPhase Phase = EPhase::DetailedHit;
};

bool FUERayTracingAudioRayTracingDevice::IsRayTracingAvailable() const
{
    return GRHISupportsRayTracing && GRHISupportsRayTracingShaders;
}

FUERayTracingAudioAsyncEnergyFieldQuery::
    ~FUERayTracingAudioAsyncEnergyFieldQuery()
{
    RetireReadbackState();
}

bool FUERayTracingAudioAsyncEnergyFieldQuery::IsCancelled() const
{
    return bCancelled.Load();
}

bool FUERayTracingAudioAsyncEnergyFieldQuery::
    PublishReadbackState_RenderThread(
        const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State)
{
    check(IsInRenderingThread());
    FScopeLock Lock(&ReadbackStateMutex);
    if (bCancelled.Load() || !State.IsValid())
    {
        return false;
    }
    ReadbackState = State;
    bReadbackSubmitted.Store(true);
    return true;
}

TSharedPtr<
    FUERayTracingAudioAsyncEnergyFieldQuery::FReadbackState,
    ESPMode::ThreadSafe>
FUERayTracingAudioAsyncEnergyFieldQuery::
    GetPublishedReadbackState() const
{
    FScopeLock Lock(&ReadbackStateMutex);
    return ReadbackState;
}

void FUERayTracingAudioAsyncEnergyFieldQuery::RetireReadbackState()
{
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> State;
    {
        FScopeLock Lock(&ReadbackStateMutex);
        State = MoveTemp(ReadbackState);
    }
    if (!State.IsValid() || IsInRenderingThread())
    {
        return;
    }
    ENQUEUE_RENDER_COMMAND(UERayTracingAudioRetireIndirectReadback)(
        [State = MoveTemp(State)](
            FRHICommandListImmediate&) mutable
        {
            State.Reset();
        });
}

void FUERayTracingAudioAsyncEnergyFieldQuery::
    MarkHardwareRayTracingUsed()
{
    bUsedHardwareRayTracing.Store(true);
}

bool FUERayTracingAudioAsyncEnergyFieldQuery::
    WasHardwareRayTracingUsed() const
{
    return bUsedHardwareRayTracing.Load();
}

void FUERayTracingAudioAsyncEnergyFieldQuery::Cancel()
{
    {
        FScopeLock Lock(&ResultMutex);
        if (!bComplete.Load())
        {
            bCancelled.Store(true);
            bSucceeded = false;
            Result = FUERayTracingAudioEnergyFieldTraceResult();
            bComplete.Store(true);
        }
    }
    RetireReadbackState();
}

bool FUERayTracingAudioAsyncEnergyFieldQuery::IsComplete() const
{
    if (bComplete.Load())
    {
        return true;
    }

    // Publication is immutable: observe the release flag before reading the
    // mutex-protected shared state, and never let the render thread reset the
    // query member after publication.
    if (!bReadbackSubmitted.Load())
    {
        return false;
    }
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> State =
        GetPublishedReadbackState();
    if (State.IsValid() && !State->bPollQueued.Exchange(true))
    {
        TSharedRef<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe> Query =
            const_cast<FUERayTracingAudioAsyncEnergyFieldQuery*>(this)->AsShared();
        ENQUEUE_RENDER_COMMAND(UERayTracingAudioPollIndirectReadback)(
            [Query, State](FRHICommandListImmediate& RHICmdList)
            {
                Query->PollHardwareReadback_RenderThread(
                    RHICmdList,
                    State);
            });
    }

    return false;
}

bool FUERayTracingAudioAsyncEnergyFieldQuery::ConsumeResult(
    bool& bOutSucceeded,
    FUERayTracingAudioEnergyFieldTraceResult& OutResult)
{
    if (!bComplete.Load())
    {
        return false;
    }

    FScopeLock Lock(&ResultMutex);
    if (bConsumed)
    {
        return false;
    }

    bConsumed = true;
    bOutSucceeded = bSucceeded;
    OutResult = MoveTemp(Result);
    RetireReadbackState();
    return true;
}

void FUERayTracingAudioAsyncEnergyFieldQuery::Complete(
    bool bInSucceeded,
    FUERayTracingAudioEnergyFieldTraceResult&& InResult)
{
    FScopeLock Lock(&ResultMutex);
    if (bComplete.Load() || bCancelled.Load())
    {
        return;
    }
    bSucceeded = bInSucceeded;
    Result = MoveTemp(InResult);
    bComplete.Store(true);
}

void FUERayTracingAudioAsyncEnergyFieldQuery::BeginHardwareBatchReadback_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    TArray<FUERayTracingAudioGeometryExport>&& Geometry,
    TArray<FUERayTracingAudioEnergyFieldTraceRequest>&& Requests,
    TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>>&& Queries,
    uint64 SceneCacheKey)
{
#if RHI_RAYTRACING
    check(IsInRenderingThread());

    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> State = MakeShared<FReadbackState, ESPMode::ThreadSafe>();
    State->Geometry = MoveTemp(Geometry);

    if (Requests.Num() != Queries.Num())
    {
        for (const TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>& Query : Queries)
        {
            if (Query.IsValid())
            {
                Query->Complete(false, FUERayTracingAudioEnergyFieldTraceResult());
            }
        }
        return;
    }

    State->Items.Reserve(Requests.Num());
    for (int32 RequestIndex = 0; RequestIndex < Requests.Num(); ++RequestIndex)
    {
        const TSharedPtr<
            FUERayTracingAudioAsyncEnergyFieldQuery,
            ESPMode::ThreadSafe>& Query = Queries[RequestIndex];
        if (!Query.IsValid() || Query->IsCancelled())
        {
            continue;
        }

        FReadbackState::FItem Item;
        Item.Query = Query;
        Item.Request = MoveTemp(Requests[RequestIndex]);
        Item.Result.DelayBinEnergy.Init(FVector::ZeroVector, FMath::Max(Item.Request.NumDelayBins, 1));
        Item.Result.DelayBinDirection.Init(FVector::ZeroVector, Item.Result.DelayBinEnergy.Num());

        if (Item.Request.NumReflectionRays <= 0
            || Item.Request.MaxReflectionBounces <= 0
            || Item.Request.DurationSeconds <= 0.0f)
        {
            Query->Complete(true, MoveTemp(Item.Result));
            continue;
        }

        GenerateListenerReflectionPaths(Item.Request, Item.ActivePaths);
        BuildBounceRaysFromReflectionPaths(Item.ActivePaths, Item.Request.MaxTraceDistance, Item.BounceRays);
        State->Items.Add(MoveTemp(Item));
    }

    if (State->Items.IsEmpty())
    {
        return;
    }

    State->SceneResources = GetOrBuildSceneTLAS_RenderThread(
        RHICmdList,
        SceneCacheKey,
        State->Geometry,
        EUERayTracingAudioGeometryUsage::Indirect);
    if (!State->SceneResources.IsValid())
    {
        for (FReadbackState::FItem& Item : State->Items)
        {
            if (const TSharedPtr<
                    FUERayTracingAudioAsyncEnergyFieldQuery,
                    ESPMode::ThreadSafe> Query = Item.Query.Pin())
            {
                Query->Complete(false, MoveTemp(Item.Result));
            }
        }
        return;
    }

    State->CombinedRays.Reset();
    for (FReadbackState::FItem& Item : State->Items)
    {
        Item.ReadbackOffset = State->CombinedRays.Num();
        Item.ReadbackCount = Item.BounceRays.Num();
        State->CombinedRays.Append(Item.BounceRays);
    }
    if (!DispatchDetailedReadback_RenderThread(
        RHICmdList,
        State->SceneResources->SceneView,
        State->SceneResources->InstanceToGeometryIndex.Num(),
        State->CombinedRays,
        State->Readback))
    {
        for (FReadbackState::FItem& Item : State->Items)
        {
            if (const TSharedPtr<
                    FUERayTracingAudioAsyncEnergyFieldQuery,
                    ESPMode::ThreadSafe> Query = Item.Query.Pin())
            {
                Query->Complete(false, MoveTemp(Item.Result));
            }
        }
        return;
    }

    for (FReadbackState::FItem& Item : State->Items)
    {
        if (const TSharedPtr<
                FUERayTracingAudioAsyncEnergyFieldQuery,
                ESPMode::ThreadSafe> Query = Item.Query.Pin())
        {
            Query->MarkHardwareRayTracingUsed();
            Query->PublishReadbackState_RenderThread(State);
        }
    }
#else
    for (const TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>& Query : Queries)
    {
        if (Query.IsValid())
        {
            Query->Complete(false, FUERayTracingAudioEnergyFieldTraceResult());
        }
    }
#endif
}

void FUERayTracingAudioAsyncEnergyFieldQuery::PollHardwareReadback_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State)
{
#if RHI_RAYTRACING
    check(IsInRenderingThread());
    if (!State.IsValid() || !State->Readback.IsValid())
    {
        Complete(false, FUERayTracingAudioEnergyFieldTraceResult());
        return;
    }

    if (!State->Readback->IsReady())
    {
        State->bPollQueued.Store(false);
        return;
    }

    auto FailAll = [&State]()
    {
        for (FReadbackState::FItem& Item : State->Items)
        {
            if (const TSharedPtr<
                    FUERayTracingAudioAsyncEnergyFieldQuery,
                    ESPMode::ThreadSafe> Query = Item.Query.Pin())
            {
                Query->Complete(false, MoveTemp(Item.Result));
            }
        }
        State->Readback.Reset();
        State->Items.Reset();
    };

    auto DispatchCombinedDetailed = [&State, &RHICmdList]() -> bool
    {
        State->CombinedRays.Reset();
        for (FReadbackState::FItem& Item : State->Items)
        {
            Item.ReadbackOffset = State->CombinedRays.Num();
            Item.ReadbackCount = Item.BounceRays.Num();
            State->CombinedRays.Append(Item.BounceRays);
        }
        State->Phase = FReadbackState::EPhase::DetailedHit;
        return !State->CombinedRays.IsEmpty()
            && DispatchDetailedReadback_RenderThread(
            RHICmdList,
            State->SceneResources->SceneView,
            State->SceneResources->InstanceToGeometryIndex.Num(),
            State->CombinedRays,
            State->Readback);
    };

    auto CompleteOrDispatchNextBounce = [&State, &DispatchCombinedDetailed, &FailAll]()
    {
        for (FReadbackState::FItem& Item : State->Items)
        {
            const TSharedPtr<
                FUERayTracingAudioAsyncEnergyFieldQuery,
                ESPMode::ThreadSafe> Query = Item.Query.Pin();
            ++Item.BounceIndex;
            if (!Query.IsValid() || Query->IsCancelled())
            {
                continue;
            }
            if (Item.BounceIndex >= Item.Request.MaxReflectionBounces
                || Item.NextPaths.IsEmpty())
            {
                Query->Complete(true, MoveTemp(Item.Result));
                continue;
            }

            Item.ActivePaths = MoveTemp(Item.NextPaths);
            Item.ShadedStates.Reset();
            Item.BounceHits.Reset();
            Item.ShadowRays.Reset();
            Item.ShadowRayStateIndices.Reset();
            BuildBounceRaysFromReflectionPaths(
                Item.ActivePaths,
                Item.Request.MaxTraceDistance,
                Item.BounceRays);
        }

        State->Items.RemoveAllSwap(
            [](const FReadbackState::FItem& Item)
            {
                const TSharedPtr<
                    FUERayTracingAudioAsyncEnergyFieldQuery,
                    ESPMode::ThreadSafe> Query = Item.Query.Pin();
                return !Query.IsValid()
                    || Query->IsCancelled()
                    || Query->IsComplete();
            },
            EAllowShrinking::No);

        if (State->Items.IsEmpty())
        {
            State->Readback.Reset();
            State->CombinedRays.Reset();
            return;
        }

        if (!DispatchCombinedDetailed())
        {
            FailAll();
            return;
        }
        State->bPollQueued.Store(false);
    };

    if (State->Phase == FReadbackState::EPhase::DetailedHit)
    {
        const uint32 NumBytes = sizeof(FUERayTracingAudioDetailedTraceResult) * State->CombinedRays.Num();
        const FUERayTracingAudioDetailedTraceResult* Results =
            static_cast<const FUERayTracingAudioDetailedTraceResult*>(State->Readback->Lock(NumBytes));
        if (!Results)
        {
            FailAll();
            return;
        }

        for (FReadbackState::FItem& Item : State->Items)
        {
            DecodeDetailedTraceResults(
                State->Geometry,
                State->SceneResources->InstanceToGeometryIndex,
                Item.BounceRays,
                Results + Item.ReadbackOffset,
                Item.BounceHits);
        }
        State->Readback->Unlock();
        State->Readback.Reset();

        State->CombinedRays.Reset();
        for (FReadbackState::FItem& Item : State->Items)
        {
            ShadeAndBounceHardwareReflectionPaths(
                Item.ActivePaths,
                Item.BounceHits,
                State->Geometry,
                Item.Request,
                Item.BounceIndex,
                Item.ShadedStates,
                Item.NextPaths);
            BuildShadowRaysFromShadedStates(
                Item.ShadedStates,
                Item.ShadowRays,
                Item.ShadowRayStateIndices);
            Item.ReadbackOffset = State->CombinedRays.Num();
            Item.ReadbackCount = Item.ShadowRays.Num();
            State->CombinedRays.Append(Item.ShadowRays);
        }

        if (State->CombinedRays.IsEmpty())
        {
            for (FReadbackState::FItem& Item : State->Items)
            {
                GatherEnergyFieldFromShadowResults(
                    Item.ShadedStates,
                    TArray<bool>(),
                    Item.ShadowRayStateIndices,
                    Item.Request,
                    Item.Result);
            }
            CompleteOrDispatchNextBounce();
            return;
        }

        State->Phase = FReadbackState::EPhase::ShadowVisibility;
        if (!DispatchOcclusionReadback_RenderThread(
            RHICmdList,
            State->SceneResources->SceneView,
            State->CombinedRays,
            State->Readback))
        {
            FailAll();
            return;
        }
        State->bPollQueued.Store(false);
        return;
    }

    const uint32 NumBytes = sizeof(uint32) * State->CombinedRays.Num();
    const uint32* Results = static_cast<const uint32*>(State->Readback->Lock(NumBytes));
    if (!Results)
    {
        FailAll();
        return;
    }

    for (FReadbackState::FItem& Item : State->Items)
    {
        TArray<bool> ShadowHits;
        ShadowHits.Init(false, Item.ReadbackCount);
        for (int32 Index = 0; Index < ShadowHits.Num(); ++Index)
        {
            ShadowHits[Index] = Results[Item.ReadbackOffset + Index] != 0;
        }
        GatherEnergyFieldFromShadowResults(
            Item.ShadedStates,
            ShadowHits,
            Item.ShadowRayStateIndices,
            Item.Request,
            Item.Result);
    }
    State->Readback->Unlock();
    State->Readback.Reset();
    CompleteOrDispatchNextBounce();
#else
    Complete(false, FUERayTracingAudioEnergyFieldTraceResult());
#endif
}

FUERayTracingAudioAsyncRayQuery::~FUERayTracingAudioAsyncRayQuery()
{
    RetireReadbackState();
}

bool FUERayTracingAudioAsyncRayQuery::IsCancelled() const
{
    return bCancelled.Load();
}

bool FUERayTracingAudioAsyncRayQuery::
    PublishReadbackState_RenderThread(
        const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State)
{
    check(IsInRenderingThread());
    FScopeLock Lock(&ReadbackStateMutex);
    if (bCancelled.Load() || !State.IsValid())
    {
        return false;
    }
    ReadbackState = State;
    bReadbackSubmitted.Store(true);
    return true;
}

TSharedPtr<FUERayTracingAudioAsyncRayQuery::FReadbackState, ESPMode::ThreadSafe>
FUERayTracingAudioAsyncRayQuery::GetPublishedReadbackState() const
{
    FScopeLock Lock(&ReadbackStateMutex);
    return ReadbackState;
}

void FUERayTracingAudioAsyncRayQuery::RetireReadbackState()
{
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> State;
    {
        FScopeLock Lock(&ReadbackStateMutex);
        State = MoveTemp(ReadbackState);
    }
    if (!State.IsValid() || IsInRenderingThread())
    {
        return;
    }
    ENQUEUE_RENDER_COMMAND(UERayTracingAudioRetireDirectReadback)(
        [State = MoveTemp(State)](
            FRHICommandListImmediate&) mutable
        {
            State.Reset();
        });
}

void FUERayTracingAudioAsyncRayQuery::MarkHardwareRayTracingUsed()
{
    bUsedHardwareRayTracing.Store(true);
}

bool FUERayTracingAudioAsyncRayQuery::WasHardwareRayTracingUsed() const
{
    return bUsedHardwareRayTracing.Load();
}

void FUERayTracingAudioAsyncRayQuery::Cancel()
{
    {
        FScopeLock Lock(&ResultMutex);
        if (!bComplete.Load())
        {
            bCancelled.Store(true);
            bSucceeded = false;
            Hits.Reset();
            bComplete.Store(true);
        }
    }
    RetireReadbackState();
}

bool FUERayTracingAudioAsyncRayQuery::IsComplete() const
{
    if (bComplete.Load())
    {
        return true;
    }

    if (!bReadbackSubmitted.Load())
    {
        return false;
    }
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> State =
        GetPublishedReadbackState();
    if (State.IsValid() && !State->bPollQueued.Exchange(true))
    {
        TSharedRef<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> Query =
            const_cast<FUERayTracingAudioAsyncRayQuery*>(this)->AsShared();
        ENQUEUE_RENDER_COMMAND(UERayTracingAudioPollDirectReadback)(
            [Query, State](FRHICommandListImmediate& RHICmdList)
            {
                Query->PollHardwareReadback_RenderThread(
                    RHICmdList,
                    State);
            });
    }

    return false;
}

bool FUERayTracingAudioAsyncRayQuery::ConsumeResult(bool& bOutSucceeded, TArray<bool>& OutHits)
{
    if (!bComplete.Load())
    {
        return false;
    }

    FScopeLock Lock(&ResultMutex);
    if (bConsumed)
    {
        return false;
    }

    bConsumed = true;
    bOutSucceeded = bSucceeded;
    OutHits = MoveTemp(Hits);
    RetireReadbackState();
    return true;
}

void FUERayTracingAudioAsyncRayQuery::Complete(bool bInSucceeded, TArray<bool>&& InHits)
{
    FScopeLock Lock(&ResultMutex);
    if (bComplete.Load() || bCancelled.Load())
    {
        return;
    }
    bSucceeded = bInSucceeded;
    Hits = MoveTemp(InHits);
    bComplete.Store(true);
}

void FUERayTracingAudioAsyncRayQuery::BeginHardwareBatchReadback_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    TArray<FUERayTracingAudioGeometryExport>&& Geometry,
    TArray<FUERayTracingAudioRay>&& CombinedRays,
    TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>>&& Queries,
    TArray<int32>&& RayCounts,
    uint64 SceneCacheKey)
{
#if RHI_RAYTRACING
    check(IsInRenderingThread());
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> State = MakeShared<FReadbackState, ESPMode::ThreadSafe>();

    if (Queries.Num() != RayCounts.Num() || Queries.IsEmpty())
    {
        for (const TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>& Query : Queries)
        {
            if (Query.IsValid())
            {
                Query->Complete(false, TArray<bool>());
            }
        }
        return;
    }

    int32 OriginalRayOffset = 0;
    TArray<FUERayTracingAudioRay> ActiveCombinedRays;
    ActiveCombinedRays.Reserve(CombinedRays.Num());
    State->Segments.Reserve(Queries.Num());
    for (int32 QueryIndex = 0;
        QueryIndex < Queries.Num();
        ++QueryIndex)
    {
        const int32 NumQueryRays = RayCounts[QueryIndex];
        if (NumQueryRays <= 0
            || OriginalRayOffset > CombinedRays.Num() - NumQueryRays)
        {
            for (const TSharedPtr<
                    FUERayTracingAudioAsyncRayQuery,
                    ESPMode::ThreadSafe>& Query : Queries)
            {
                if (Query.IsValid())
                {
                    Query->Complete(false, TArray<bool>());
                }
            }
            return;
        }

        const TSharedPtr<
            FUERayTracingAudioAsyncRayQuery,
            ESPMode::ThreadSafe>& Query = Queries[QueryIndex];
        if (Query.IsValid() && !Query->IsCancelled())
        {
            FReadbackState::FQuerySegment& Segment =
                State->Segments.AddDefaulted_GetRef();
            Segment.Query = Query;
            Segment.RayOffset = ActiveCombinedRays.Num();
            Segment.NumRays = NumQueryRays;
            ActiveCombinedRays.Append(
                CombinedRays.GetData() + OriginalRayOffset,
                NumQueryRays);
        }
        OriginalRayOffset += NumQueryRays;
    }
    if (OriginalRayOffset != CombinedRays.Num())
    {
        for (const TSharedPtr<
                FUERayTracingAudioAsyncRayQuery,
                ESPMode::ThreadSafe>& Query : Queries)
        {
            if (Query.IsValid())
            {
                Query->Complete(false, TArray<bool>());
            }
        }
        return;
    }
    if (State->Segments.IsEmpty())
    {
        return;
    }
    CombinedRays = MoveTemp(ActiveCombinedRays);
    State->NumCombinedRays = CombinedRays.Num();

    State->SceneResources = GetOrBuildSceneTLAS_RenderThread(
        RHICmdList,
        SceneCacheKey,
        Geometry,
        EUERayTracingAudioGeometryUsage::Direct);
    if (!State->SceneResources.IsValid())
    {
        for (const TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>& Query : Queries)
        {
            if (Query.IsValid())
            {
                Query->Complete(false, TArray<bool>());
            }
        }
        return;
    }

    const bool bDispatched = DispatchOcclusionReadback_RenderThread(
        RHICmdList,
        State->SceneResources->SceneView,
        CombinedRays,
        State->Readback);
    if (!bDispatched)
    {
        for (const TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>& Query : Queries)
        {
            if (Query.IsValid())
            {
                Query->Complete(false, TArray<bool>());
            }
        }
        return;
    }

    for (FReadbackState::FQuerySegment& Segment : State->Segments)
    {
        if (const TSharedPtr<
                FUERayTracingAudioAsyncRayQuery,
                ESPMode::ThreadSafe> Query = Segment.Query.Pin())
        {
            Query->MarkHardwareRayTracingUsed();
            Query->PublishReadbackState_RenderThread(State);
        }
    }
#else
    for (const TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>& Query : Queries)
    {
        if (Query.IsValid())
        {
            Query->Complete(false, TArray<bool>());
        }
    }
#endif
}

void FUERayTracingAudioAsyncRayQuery::PollHardwareReadback_RenderThread(
    FRHICommandListImmediate& RHICmdList,
    const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State)
{
#if RHI_RAYTRACING
    check(IsInRenderingThread());
    if (!State.IsValid() || !State->Readback.IsValid())
    {
        Complete(false, TArray<bool>());
        return;
    }
    if (!State->Readback->IsReady())
    {
        State->bPollQueued.Store(false);
        return;
    }

    const uint32 NumBytes = sizeof(uint32) * State->NumCombinedRays;
    const uint32* Results = static_cast<const uint32*>(State->Readback->Lock(NumBytes));
    if (!Results)
    {
        for (FReadbackState::FQuerySegment& Segment : State->Segments)
        {
            if (const TSharedPtr<
                    FUERayTracingAudioAsyncRayQuery,
                    ESPMode::ThreadSafe> Query = Segment.Query.Pin())
            {
                Query->Complete(false, TArray<bool>());
            }
        }
        State->Segments.Reset();
        return;
    }

    for (FReadbackState::FQuerySegment& Segment : State->Segments)
    {
        TArray<bool> OutHits;
        OutHits.Init(false, Segment.NumRays);
        for (int32 Index = 0; Index < Segment.NumRays; ++Index)
        {
            OutHits[Index] = Results[Segment.RayOffset + Index] != 0;
        }
        if (const TSharedPtr<
                FUERayTracingAudioAsyncRayQuery,
                ESPMode::ThreadSafe> Query = Segment.Query.Pin())
        {
            Query->Complete(true, MoveTemp(OutHits));
        }
    }
    State->Readback->Unlock();
    State->Readback.Reset();
    State->Segments.Reset();
#else
    Complete(false, TArray<bool>());
#endif
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
    TArray<bool>& OutHits,
    bool* bOutUsedHardwareRayTracing) const
{
    if (bOutUsedHardwareRayTracing)
    {
        *bOutUsedHardwareRayTracing = false;
    }
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
            if (bOutUsedHardwareRayTracing)
            {
                *bOutUsedHardwareRayTracing = true;
            }
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

TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>
FUERayTracingAudioRayTracingDevice::SubmitRays(
    const FUERayTracingAudioTraceRequest& Request,
    const TArray<FUERayTracingAudioRay>& Rays) const
{
    TArray<TArray<FUERayTracingAudioRay>> RayBatches;
    RayBatches.Add(Rays);
    TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>> Queries =
        SubmitRaysBatch(Request, RayBatches);
    return Queries.IsEmpty() ? nullptr : Queries[0];
}

TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>>
FUERayTracingAudioRayTracingDevice::SubmitRaysBatch(
    const FUERayTracingAudioTraceRequest& Request,
    const TArray<TArray<FUERayTracingAudioRay>>& RayBatches) const
{
    TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>> Queries;
    Queries.Reserve(RayBatches.Num());

    TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>> ActiveQueries;
    TArray<int32> RayCounts;
    TArray<FUERayTracingAudioRay> CombinedRays;
    for (const TArray<FUERayTracingAudioRay>& Rays : RayBatches)
    {
        TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> Query =
            MakeShared<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>();
        Queries.Add(Query);

        if (Rays.IsEmpty())
        {
            Query->Complete(true, TArray<bool>());
            continue;
        }

        ActiveQueries.Add(Query);
        RayCounts.Add(Rays.Num());
        CombinedRays.Append(Rays);
    }

    if (ActiveQueries.IsEmpty())
    {
        return Queries;
    }

#if RHI_RAYTRACING
    if (IsRayTracingAvailable() && Request.Scene)
    {
        TArray<FUERayTracingAudioGeometryExport> Geometry = Request.Scene->GetStaticGeometry();
        const uint64 SceneCacheKey = Request.SceneCacheKey != 0
            ? Request.SceneCacheKey
            : Request.Scene->GetCacheKey();

        ENQUEUE_RENDER_COMMAND(UERayTracingAudioSubmitRayBatch)(
            [Coordinator = ActiveQueries[0], Geometry = MoveTemp(Geometry), CombinedRays = MoveTemp(CombinedRays), ActiveQueries = MoveTemp(ActiveQueries), RayCounts = MoveTemp(RayCounts), SceneCacheKey](FRHICommandListImmediate& RHICmdList) mutable
            {
                Coordinator->BeginHardwareBatchReadback_RenderThread(
                    RHICmdList,
                    MoveTemp(Geometry),
                    MoveTemp(CombinedRays),
                    MoveTemp(ActiveQueries),
                    MoveTemp(RayCounts),
                    SceneCacheKey);
            });

        if (!GHasLoggedHardwareRayTracingPath)
        {
            UE_LOG(LogUERayTracingAudioSDK, Display, TEXT("UERayTracingAudioSDK submits direct sound visibility queries asynchronously to the render thread."));
            GHasLoggedHardwareRayTracingPath = true;
        }

        return Queries;
    }
#endif

    for (const TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>& Query : ActiveQueries)
    {
        Query->Complete(false, TArray<bool>());
    }
    return Queries;
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

TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>
FUERayTracingAudioRayTracingDevice::SubmitIndirectEnergyField(
    const FUERayTracingAudioEnergyFieldTraceRequest& Request) const
{
    TArray<FUERayTracingAudioEnergyFieldTraceRequest> Requests;
    Requests.Add(Request);
    TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>> Queries =
        SubmitIndirectEnergyFieldBatch(Requests);
    return Queries.IsEmpty() ? nullptr : Queries[0];
}

TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>>
FUERayTracingAudioRayTracingDevice::SubmitIndirectEnergyFieldBatch(
    const TArray<FUERayTracingAudioEnergyFieldTraceRequest>& Requests) const
{
    TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>> Queries;
    Queries.Reserve(Requests.Num());
    for (int32 RequestIndex = 0; RequestIndex < Requests.Num(); ++RequestIndex)
    {
        Queries.Add(MakeShared<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>());
    }

    if (Requests.IsEmpty())
    {
        return Queries;
    }

#if RHI_RAYTRACING
    if (IsRayTracingAvailable())
    {
        const FUERayTracingAudioScene* CommonScene = nullptr;
        uint64 CommonSceneCacheKey = 0;
        TArray<FUERayTracingAudioEnergyFieldTraceRequest> ActiveRequests;
        TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>> ActiveQueries;
        ActiveRequests.Reserve(Requests.Num());
        ActiveQueries.Reserve(Requests.Num());

        for (int32 RequestIndex = 0; RequestIndex < Requests.Num(); ++RequestIndex)
        {
            const FUERayTracingAudioEnergyFieldTraceRequest& Request = Requests[RequestIndex];
            if (!Request.Scene)
            {
                Queries[RequestIndex]->Complete(false, FUERayTracingAudioEnergyFieldTraceResult());
                continue;
            }

            const uint64 SceneCacheKey = Request.SceneCacheKey != 0
                ? Request.SceneCacheKey
                : Request.Scene->GetCacheKey();
            if (!CommonScene)
            {
                CommonScene = Request.Scene;
                CommonSceneCacheKey = SceneCacheKey;
            }
            if (SceneCacheKey != CommonSceneCacheKey)
            {
                Queries[RequestIndex]->Complete(false, FUERayTracingAudioEnergyFieldTraceResult());
                continue;
            }

            FUERayTracingAudioEnergyFieldTraceRequest& RequestCopy = ActiveRequests.Add_GetRef(Request);
            RequestCopy.SceneCacheKey = SceneCacheKey;
            RequestCopy.Scene = nullptr;
            RequestCopy.World = nullptr;
            RequestCopy.ListenerActor = nullptr;
            RequestCopy.SourceActor = nullptr;
            ActiveQueries.Add(Queries[RequestIndex]);
        }

        if (CommonScene && !ActiveQueries.IsEmpty())
        {
            TArray<FUERayTracingAudioGeometryExport> Geometry = CommonScene->GetStaticGeometry();
            ENQUEUE_RENDER_COMMAND(UERayTracingAudioSubmitIndirectEnergyFieldBatch)(
                [Coordinator = ActiveQueries[0], Geometry = MoveTemp(Geometry), ActiveRequests = MoveTemp(ActiveRequests), ActiveQueries = MoveTemp(ActiveQueries), CommonSceneCacheKey](FRHICommandListImmediate& RHICmdList) mutable
                {
                    Coordinator->BeginHardwareBatchReadback_RenderThread(
                        RHICmdList,
                        MoveTemp(Geometry),
                        MoveTemp(ActiveRequests),
                        MoveTemp(ActiveQueries),
                        CommonSceneCacheKey);
                });

            if (!GHasLoggedIndirectHardwareRayTracingPath)
            {
                UE_LOG(LogUERayTracingAudioSDK, Display, TEXT("UERayTracingAudioSDK submits indirect sound energy-field queries asynchronously to the render thread."));
                GHasLoggedIndirectHardwareRayTracingPath = true;
            }
        }

        return Queries;
    }
#endif

    for (const TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>& Query : Queries)
    {
        Query->Complete(false, FUERayTracingAudioEnergyFieldTraceResult());
    }
    return Queries;
}
