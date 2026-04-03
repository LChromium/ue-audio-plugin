#include "Components/UERayTracingAudioGeometryComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "StaticMeshResources.h"
#include "UERayTracingAudioModule.h"

UUERayTracingAudioGeometryComponent::UUERayTracingAudioGeometryComponent()
    : bExportToAcousticScene(true)
    , bAffectsDirectSound(true)
    , ExportMode(EUERayTracingAudioGeometryExportMode::BoundingBox)
    , Absorption(0.2f, 0.3f, 0.4f)
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UUERayTracingAudioGeometryComponent::BeginPlay()
{
    Super::BeginPlay();
    FUERayTracingAudioModule::GetManager().AddGeometry(this);
}

void UUERayTracingAudioGeometryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    FUERayTracingAudioModule::GetManager().RemoveGeometry(this);
    Super::EndPlay(EndPlayReason);
}

bool UUERayTracingAudioGeometryComponent::BuildGeometryExport(FUERayTracingAudioGeometryExport& OutGeometryExport) const
{
    if (!bExportToAcousticScene || !GetOwner())
    {
        return false;
    }

    UPrimitiveComponent* PrimitiveComponent = GetOwner()->FindComponentByClass<UPrimitiveComponent>();
    if (!IsValid(PrimitiveComponent))
    {
        return false;
    }

    OutGeometryExport.Transform = PrimitiveComponent->GetComponentTransform();
    OutGeometryExport.Bounds = PrimitiveComponent->Bounds.GetBox();
    OutGeometryExport.Extent = PrimitiveComponent->Bounds.BoxExtent;
    OutGeometryExport.Absorption = Absorption;
    OutGeometryExport.bVisibleForDirectSound = bAffectsDirectSound;

    if (ExportMode == EUERayTracingAudioGeometryExportMode::StaticMeshTriangles)
    {
        const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(PrimitiveComponent);
        const UStaticMesh* StaticMesh = StaticMeshComponent ? StaticMeshComponent->GetStaticMesh() : nullptr;
        const FStaticMeshRenderData* RenderData = StaticMesh ? StaticMesh->GetRenderData() : nullptr;

        if (RenderData && RenderData->LODResources.Num() > 0)
        {
            const FStaticMeshLODResources& LODResources = RenderData->LODResources[0];
            if (LODResources.GetNumVertices() > 0
                && LODResources.GetNumTriangles() > 0
                && LODResources.VertexBuffers.PositionVertexBuffer.GetAllowCPUAccess()
                && LODResources.IndexBuffer.GetAllowCPUAccess())
            {
                const FTransform ComponentTransform = StaticMeshComponent->GetComponentTransform();
                const FPositionVertexBuffer& PositionVertexBuffer = LODResources.VertexBuffers.PositionVertexBuffer;
                const FIndexArrayView Indices = LODResources.IndexBuffer.GetArrayView();

                OutGeometryExport.bUseStaticMeshTriangles = true;
                OutGeometryExport.Vertices.Reset();
                OutGeometryExport.Indices.Reset();
                OutGeometryExport.Vertices.Reserve(LODResources.GetNumVertices());
                OutGeometryExport.Indices.Reserve(LODResources.GetNumTriangles() * 3);

                for (int32 VertexIndex = 0; VertexIndex < LODResources.GetNumVertices(); ++VertexIndex)
                {
                    const FVector LocalPosition = FVector(PositionVertexBuffer.VertexPosition(VertexIndex));
                    OutGeometryExport.Vertices.Add(ComponentTransform.TransformPosition(LocalPosition));
                }

                for (const FStaticMeshSection& Section : LODResources.Sections)
                {
                    for (uint32 TriangleIndex = 0; TriangleIndex < Section.NumTriangles; ++TriangleIndex)
                    {
                        const uint32 BaseIndex = Section.FirstIndex + (TriangleIndex * 3);
                        OutGeometryExport.Indices.Add(Indices[BaseIndex + 0]);
                        OutGeometryExport.Indices.Add(Indices[BaseIndex + 2]);
                        OutGeometryExport.Indices.Add(Indices[BaseIndex + 1]);
                    }
                }

                if (OutGeometryExport.HasTriangleMesh())
                {
                    UE_LOG(
                        LogUERayTracingAudio,
                        Display,
                        TEXT("Geometry export uses StaticMeshTriangles for actor '%s' (%d vertices, %d triangles)."),
                        *GetOwner()->GetName(),
                        OutGeometryExport.Vertices.Num(),
                        OutGeometryExport.Indices.Num() / 3);
                    return true;
                }
            }
        }

        UE_LOG(
            LogUERayTracingAudio,
            Warning,
            TEXT("Geometry export requested StaticMeshTriangles for actor '%s' but CPU mesh data was unavailable. Falling back to BoundingBox."),
            *GetOwner()->GetName());
    }

    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("Geometry export uses BoundingBox for actor '%s'."),
        *GetOwner()->GetName());
    return true;
}
