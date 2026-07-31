#pragma once

#include "CoreMinimal.h"

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioGeometryExport
{
    FTransform Transform;
    FBox Bounds;
    FVector Extent;
    FVector Absorption;
    FVector Transmission = FVector::ZeroVector;
    float Scattering = 0.35f;
    bool bVisibleForDirectSound = true;
    bool bUseStaticMeshTriangles = false;
    bool bVerticesAreLocalSpace = false;
    FString StaticMeshCacheKey;
    int32 StaticMeshLODIndex = 0;
    uint32 StaticMeshContentHash = 0;
    TArray<FVector> Vertices;
    TArray<uint32> Indices;

    bool HasTriangleMesh() const
    {
        return Vertices.Num() > 0 && Indices.Num() >= 3;
    }

    bool HasCachedStaticMeshSource() const
    {
        return bUseStaticMeshTriangles && !StaticMeshCacheKey.IsEmpty() && HasTriangleMesh();
    }

    FString GetRayTracingGeometryCacheKey() const
    {
        if (!HasCachedStaticMeshSource())
        {
            return FString();
        }

        return FString::Printf(
            TEXT("%s|LOD=%d|V=%d|I=%d|H=%08x"),
            *StaticMeshCacheKey,
            StaticMeshLODIndex,
            Vertices.Num(),
            Indices.Num(),
            StaticMeshContentHash);
    }

    FVector GetVertexWorldPosition(int32 VertexIndex) const
    {
        if (!Vertices.IsValidIndex(VertexIndex))
        {
            return FVector::ZeroVector;
        }

        return bVerticesAreLocalSpace
            ? Transform.TransformPosition(Vertices[VertexIndex])
            : Vertices[VertexIndex];
    }
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioScene
{
public:
    FUERayTracingAudioScene();

    void SetStaticGeometry(TArray<FUERayTracingAudioGeometryExport>&& InGeometry);
    const TArray<FUERayTracingAudioGeometryExport>& GetStaticGeometry() const;
    int32 GetVersion() const;
    uint64 GetCacheKey() const;
    bool IsEmpty() const;

private:
    TArray<FUERayTracingAudioGeometryExport> StaticGeometry;
    uint64 SceneIdentity = 0;
    int32 Version = 0;
};
