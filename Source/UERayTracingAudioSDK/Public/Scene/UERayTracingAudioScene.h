#pragma once

#include "CoreMinimal.h"

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioGeometryExport
{
    FTransform Transform;
    FBox Bounds;
    FVector Extent;
    FVector Absorption;
    bool bVisibleForDirectSound = true;
    bool bUseStaticMeshTriangles = false;
    TArray<FVector> Vertices;
    TArray<uint32> Indices;

    bool HasTriangleMesh() const
    {
        return Vertices.Num() > 0 && Indices.Num() >= 3;
    }
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioScene
{
public:
    void SetStaticGeometry(TArray<FUERayTracingAudioGeometryExport>&& InGeometry);
    const TArray<FUERayTracingAudioGeometryExport>& GetStaticGeometry() const;
    int32 GetVersion() const;
    bool IsEmpty() const;

private:
    TArray<FUERayTracingAudioGeometryExport> StaticGeometry;
    int32 Version = 0;
};
