#include "Scene/UERayTracingAudioScene.h"

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

bool FUERayTracingAudioScene::IsEmpty() const
{
    return StaticGeometry.IsEmpty();
}
