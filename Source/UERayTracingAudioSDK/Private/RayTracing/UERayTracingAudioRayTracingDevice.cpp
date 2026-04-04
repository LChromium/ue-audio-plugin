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
#include "RenderUtils.h"
#include "Scene/UERayTracingAudioScene.h"
#include "ShaderParameterStruct.h"
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

        DispatchDetailedRays(RHICmdList, SceneView, RayBufferView, ResultBufferView, RayData.Num(), NumGeometrySegments);
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
