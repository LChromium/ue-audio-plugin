#include "Managers/UERayTracingAudioManager.h"

#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Engine/World.h"

FUERayTracingAudioManager::FUERayTracingAudioManager()
    : Simulator(Context)
{
}

void FUERayTracingAudioManager::AddSource(UUERayTracingAudioSourceComponent* Source)
{
    Sources.Add(Source);
}

void FUERayTracingAudioManager::RemoveSource(UUERayTracingAudioSourceComponent* Source)
{
    Sources.Remove(Source);
}

void FUERayTracingAudioManager::AddListener(UUERayTracingAudioListenerComponent* Listener)
{
    Listeners.Add(Listener);
}

void FUERayTracingAudioManager::RemoveListener(UUERayTracingAudioListenerComponent* Listener)
{
    Listeners.Remove(Listener);
}

void FUERayTracingAudioManager::AddGeometry(UUERayTracingAudioGeometryComponent* Geometry)
{
    GeometryComponents.Add(Geometry);
    bSceneDirty = true;
}

void FUERayTracingAudioManager::RemoveGeometry(UUERayTracingAudioGeometryComponent* Geometry)
{
    GeometryComponents.Remove(Geometry);
    bSceneDirty = true;
}

void FUERayTracingAudioManager::MarkSceneDirty()
{
    bSceneDirty = true;
}

void FUERayTracingAudioManager::RebuildScene(UWorld* World)
{
    if (!bSceneDirty)
    {
        return;
    }

    TArray<FUERayTracingAudioGeometryExport> GeometryExports;
    GeometryExports.Reserve(GeometryComponents.Num());

    for (const TWeakObjectPtr<UUERayTracingAudioGeometryComponent>& GeometryComponent : GeometryComponents)
    {
        if (!GeometryComponent.IsValid())
        {
            continue;
        }

        FUERayTracingAudioGeometryExport GeometryExport;
        if (GeometryComponent->BuildGeometryExport(GeometryExport))
        {
            GeometryExports.Add(MoveTemp(GeometryExport));
        }
    }

    Scene.SetStaticGeometry(MoveTemp(GeometryExports));
    bSceneDirty = false;
}

UUERayTracingAudioListenerComponent* FUERayTracingAudioManager::GetCurrentListener() const
{
    for (const TWeakObjectPtr<UUERayTracingAudioListenerComponent>& Listener : Listeners)
    {
        if (Listener.IsValid())
        {
            return Listener.Get();
        }
    }

    return nullptr;
}

FUERayTracingAudioDirectSimulationResult FUERayTracingAudioManager::SimulateSource(UUERayTracingAudioSourceComponent* Source)
{
    FUERayTracingAudioDirectSimulationResult Result;

    if (!IsValid(Source))
    {
        return Result;
    }

    UWorld* World = Source->GetWorld();
    UUERayTracingAudioListenerComponent* Listener = GetCurrentListener();
    if (!IsValid(World) || !IsValid(Listener))
    {
        return Result;
    }

    RebuildScene(World);

    FUERayTracingAudioDirectSimulationInput Input;
    Input.World = World;
    Input.Scene = &Scene;
    Input.ListenerLocation = Listener->GetListenerLocation();
    Input.ListenerActor = Listener->GetOwner();
    Input.SourceLocation = Source->GetSourceLocation();
    Input.SourceForward = Source->GetSourceForward();
    Input.SourceActor = Source->GetOwner();
    Input.OccludedGain = Source->GetOccludedGain();
    Input.SourceRadiusCm = Source->GetSourceRadiusCm();
    Input.NumOcclusionSamples = Source->GetNumOcclusionSamples();
    Input.bUseVolumetricOcclusion = Source->ShouldUseVolumetricOcclusion();
    Input.AirAbsorptionPerMeter = Source->GetAirAbsorptionPerMeter();

    return Simulator.SimulateDirectSound(RayTracingDevice, Input);
}

const FUERayTracingAudioScene& FUERayTracingAudioManager::GetScene() const
{
    return Scene;
}

const FUERayTracingAudioRayTracingDevice& FUERayTracingAudioManager::GetRayTracingDevice() const
{
    return RayTracingDevice;
}
