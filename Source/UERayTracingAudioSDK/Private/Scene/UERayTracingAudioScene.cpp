#include "Scene/UERayTracingAudioScene.h"

namespace
{
    uint64 GNextRayTracingAudioSceneIdentity = 0;
}

FUERayTracingAudioScene::FUERayTracingAudioScene()
    : SceneIdentity(++GNextRayTracingAudioSceneIdentity)
{
}

void FUERayTracingAudioScene::SetStaticGeometry(TArray<FUERayTracingAudioGeometryExport>&& InGeometry)
{
    StaticGeometry = MoveTemp(InGeometry);
    ++Version;
}

const TArray<FUERayTracingAudioGeometryExport>& FUERayTracingAudioScene::GetStaticGeometry() const
{
    return StaticGeometry;
}

int32 FUERayTracingAudioScene::GetVersion() const
{
    return Version;
}

uint64 FUERayTracingAudioScene::GetCacheKey() const
{
    return (SceneIdentity << 32) ^ static_cast<uint32>(Version);
}

bool FUERayTracingAudioScene::IsEmpty() const
{
    return StaticGeometry.IsEmpty();
}

bool FUERayTracingAudioScene::HasInvalidGeometryForUsage(
    const EUERayTracingAudioGeometryUsage Usage) const
{
    for (const FUERayTracingAudioGeometryExport& Geometry : StaticGeometry)
    {
        if (Geometry.IsVisibleForUsage(Usage)
            && !Geometry.HasBuildableGeometry())
        {
            return true;
        }
    }

    return false;
}
