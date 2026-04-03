#include "RayTracing/UERayTracingAudioRayTracingDevice.h"

#include "Components/PrimitiveComponent.h"
#include "BuiltInRayTracingShaders.h"
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
#endif

namespace
{
    bool GHasLoggedHardwareRayTracingPath = false;
    bool GHasLoggedPhysicsFallbackPath = false;

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
    struct FUERayTracingAudioBasicRay
    {
        float Origin[3];
        uint32 Mask;
        float Direction[3];
        float TFar;
    };

    struct FUERayTracingAudioRayTracingPipeline
    {
        FRayTracingPipelineState* PipelineState = nullptr;
        FShaderBindingTableRHIRef ShaderBindingTable;
        TShaderRef<FUERayTracingAudioOcclusionRGS> RayGenerationShader;
        TShaderRef<FUERayTracingAudioIntersectionCHS> HitGroupShader;
    };

    FUERayTracingAudioRayTracingPipeline CreateRayTracingPipeline(FRHICommandListImmediate& RHICmdList)
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

        FUERayTracingAudioRayTracingPipeline Result;
        Result.PipelineState = PipelineStateCache::GetAndOrCreateRayTracingPipelineState(RHICmdList, PipelineInitializer);
        Result.ShaderBindingTable = RHICmdList.CreateRayTracingShaderBindingTable(ShaderBindingTableInitializer);
        Result.RayGenerationShader = RayGenerationShader;
        Result.HitGroupShader = HitGroupShader;
        return Result;
    }

    void DispatchOcclusionRays(
        FRHICommandListImmediate& RHICmdList,
        FRHIShaderResourceView* SceneView,
        FRHIShaderResourceView* RayBufferView,
        FRHIUnorderedAccessView* ResultBufferView,
        uint32 NumRays)
    {
        FUERayTracingAudioRayTracingPipeline Pipeline = CreateRayTracingPipeline(RHICmdList);
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

        TArray<FVector3f> Vertices;
        TArray<uint32> Indices;

        for (const FUERayTracingAudioGeometryExport& GeometryExport : Geometry)
        {
            if (!GeometryExport.bVisibleForDirectSound || !GeometryExport.Bounds.IsValid)
            {
                continue;
            }

            if (GeometryExport.bUseStaticMeshTriangles && GeometryExport.HasTriangleMesh())
            {
                AppendTriangleGeometry(GeometryExport, Vertices, Indices);
            }
            else
            {
                AppendBoxGeometry(GeometryExport.Bounds, Vertices, Indices);
            }
        }

        if (Indices.IsEmpty())
        {
            return true;
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

        TArray<FUERayTracingAudioBasicRay> RayData;
        RayData.Reserve(Rays.Num());

        for (const FUERayTracingAudioRay& Ray : Rays)
        {
            const FVector Delta = Ray.End - Ray.Start;
            const float Distance = Delta.Length();
            const FVector Direction = (Distance > UE_SMALL_NUMBER) ? (Delta / Distance) : FVector::ForwardVector;

            FUERayTracingAudioBasicRay ShaderRay;
            ShaderRay.Origin[0] = Ray.Start.X;
            ShaderRay.Origin[1] = Ray.Start.Y;
            ShaderRay.Origin[2] = Ray.Start.Z;
            ShaderRay.Mask = 0xFFFFFFFF;
            ShaderRay.Direction[0] = Direction.X;
            ShaderRay.Direction[1] = Direction.Y;
            ShaderRay.Direction[2] = Direction.Z;
            ShaderRay.TFar = Distance;
            RayData.Add(ShaderRay);
        }

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

        FRayTracingGeometryInstance Instance;
        Instance.GeometryRHI = RayTracingGeometry;
        Instance.NumTransforms = 1;
        Instance.Transforms = MakeArrayView(&FMatrix::Identity, 1);
        Instance.InstanceContributionToHitGroupIndex = 0;

        FRayTracingGeometryInstance Instances[] = { Instance };
        FRayTracingInstanceBufferBuilder InstanceBufferBuilder;
        InstanceBufferBuilder.Init(Instances, FVector::ZeroVector);

        FRayTracingSceneInitializer SceneInitializer;
        SceneInitializer.DebugName = FName(TEXT("UERayTracingAudioTLAS"));
        SceneInitializer.MaxNumInstances = InstanceBufferBuilder.GetMaxNumInstances();
        SceneInitializer.BuildFlags = ERayTracingAccelerationStructureFlags::FastTrace;

        FRayTracingSceneRHIRef RayTracingScene = RHICreateRayTracingScene(MoveTemp(SceneInitializer));
        const FRayTracingSceneInitializer& RayTracingSceneInitializer = RayTracingScene->GetInitializer();
        const FRayTracingAccelerationStructureSize SceneSizeInfo = RHICalcRayTracingSceneSize(RayTracingSceneInitializer);

        const FRHIBufferCreateDesc SceneBufferCreateDesc =
            FRHIBufferCreateDesc::Create(TEXT("UERayTracingAudioSceneBuffer"), static_cast<uint32>(SceneSizeInfo.ResultSize), 0, EBufferUsageFlags::AccelerationStructure)
            .SetInitialState(ERHIAccess::BVHWrite);
        FBufferRHIRef SceneBuffer = RHICmdList.CreateBuffer(SceneBufferCreateDesc);

        const FRHIBufferCreateDesc ScratchBufferCreateDesc =
            FRHIBufferCreateDesc::Create(TEXT("UERayTracingAudioScratchBuffer"), static_cast<uint32>(SceneSizeInfo.BuildScratchSize), GRHIRayTracingScratchBufferAlignment, EBufferUsageFlags::UnorderedAccess)
            .SetInitialState(ERHIAccess::UAVCompute);
        FBufferRHIRef ScratchBuffer = RHICmdList.CreateBuffer(ScratchBufferCreateDesc);

        FRWBufferStructured InstanceBuffer;
        InstanceBuffer.Initialize(RHICmdList, TEXT("UERayTracingAudioInstanceBuffer"), GRHIRayTracingInstanceDescriptorSize, RayTracingSceneInitializer.MaxNumInstances);

        FRWBufferStructured HitGroupContributionsBuffer;
        if (GRHIGlobals.RayTracing.RequiresSeparateHitGroupContributionsBuffer)
        {
            HitGroupContributionsBuffer.Initialize(RHICmdList, TEXT("UERayTracingAudioHitGroupContributions"), sizeof(uint32), RayTracingSceneInitializer.MaxNumInstances);
        }

        InstanceBufferBuilder.FillRayTracingInstanceUploadBuffer(RHICmdList);
        InstanceBufferBuilder.FillAccelerationStructureAddressesBuffer(RHICmdList);

        InstanceBufferBuilder.BuildRayTracingInstanceBuffer(
            RHICmdList,
            nullptr,
            nullptr,
            InstanceBuffer.UAV,
            GRHIGlobals.RayTracing.RequiresSeparateHitGroupContributionsBuffer ? HitGroupContributionsBuffer.UAV : nullptr,
            RayTracingSceneInitializer.MaxNumInstances,
            false,
            nullptr,
            0,
            nullptr);

        RHICmdList.BindAccelerationStructureMemory(RayTracingScene, SceneBuffer, 0);
        RHICmdList.BuildAccelerationStructure(RayTracingGeometry);

        FRayTracingSceneBuildParams BuildParams;
        BuildParams.Scene = RayTracingScene;
        BuildParams.ScratchBuffer = ScratchBuffer;
        BuildParams.ScratchBufferOffset = 0;
        BuildParams.InstanceBuffer = InstanceBuffer.Buffer;
        BuildParams.InstanceBufferOffset = 0;
        BuildParams.ReferencedGeometries = InstanceBufferBuilder.GetReferencedGeometries();
        BuildParams.NumInstances = RayTracingSceneInitializer.MaxNumInstances;

        if (GRHIGlobals.RayTracing.RequiresSeparateHitGroupContributionsBuffer)
        {
            BuildParams.HitGroupContributionsBuffer = HitGroupContributionsBuffer.Buffer;
            BuildParams.HitGroupContributionsBufferOffset = 0;
        }

        RHICmdList.Transition(FRHITransitionInfo(InstanceBuffer.Buffer, ERHIAccess::UAVMask, ERHIAccess::SRVCompute));
        RHICmdList.BuildAccelerationStructure(BuildParams);
        RHICmdList.Transition(FRHITransitionInfo(RayTracingScene.GetReference(), ERHIAccess::BVHWrite, ERHIAccess::BVHRead));

        FShaderResourceViewInitializer SceneViewInitializer(SceneBuffer, RayTracingScene, 0);
        FShaderResourceViewRHIRef SceneView = RHICmdList.CreateShaderResourceView(SceneViewInitializer);
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
    Rays.Add(FUERayTracingAudioRay{Request.Start, Request.End});
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
                UE_LOG(
                    LogUERayTracingAudioSDK,
                    Display,
                    TEXT("UERayTracingAudioSDK uses hardware ray tracing for direct sound visibility queries."));
                GHasLoggedHardwareRayTracingPath = true;
            }
            return true;
        }
    }
#endif

    if (!GHasLoggedPhysicsFallbackPath)
    {
        UE_LOG(
            LogUERayTracingAudioSDK,
            Warning,
            TEXT("UERayTracingAudioSDK falls back to physics line traces for direct sound visibility queries."));
        GHasLoggedPhysicsFallbackPath = true;
    }
    return TraceRaysWithPhysics(Request, Rays, OutHits);
}
