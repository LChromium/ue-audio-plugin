#pragma once

#include "CoreMinimal.h"

enum class EUERayTracingAudioGeometryUsage : uint8
{
    Direct,
    Indirect
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioGeometryExport
{
    FTransform Transform;
    FBox Bounds;
    FVector Extent;
    FVector Absorption;
    FVector Transmission = FVector::ZeroVector;
    float Scattering = 0.35f;
    bool bVisibleForDirectSound = true;
    bool bVisibleForIndirectSound = true;
    bool bUseStaticMeshTriangles = false;
    bool bVerticesAreLocalSpace = false;
    FString StaticMeshCacheKey;
    int32 StaticMeshLODIndex = 0;
    uint32 StaticMeshContentHash = 0;
    TArray<FVector> Vertices;
    TArray<uint32> Indices;

    bool HasTriangleMesh() const
    {
        if (Vertices.Num() <= 0 || Indices.Num() < 3 || (Indices.Num() % 3) != 0)
        {
            return false;
        }

        for (const uint32 Index : Indices)
        {
            if (!Vertices.IsValidIndex(static_cast<int32>(Index)))
            {
                return false;
            }
        }

        return true;
    }

    bool HasBuildableGeometry() const
    {
        return HasTriangleMesh() || Bounds.IsValid;
    }

    bool IsVisibleForUsage(const EUERayTracingAudioGeometryUsage Usage) const
    {
        return Usage == EUERayTracingAudioGeometryUsage::Direct
            ? bVisibleForDirectSound
            : bVisibleForIndirectSound;
    }

    bool IsBuildableForUsage(const EUERayTracingAudioGeometryUsage Usage) const
    {
        return IsVisibleForUsage(Usage) && HasBuildableGeometry();
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
    bool HasInvalidGeometryForUsage(EUERayTracingAudioGeometryUsage Usage) const;

private:
    TArray<FUERayTracingAudioGeometryExport> StaticGeometry;
    uint64 SceneIdentity = 0;
    int32 Version = 0;
};
