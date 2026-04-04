#pragma once

#include "CoreMinimal.h"

#include "API/UERayTracingAudioContext.h"
#include "RayTracing/UERayTracingAudioRayTracingDevice.h"
#include "Scene/UERayTracingAudioScene.h"
#include "Simulation/UERayTracingAudioSimulator.h"

class UUERayTracingAudioGeometryComponent;
class UUERayTracingAudioListenerComponent;
class UUERayTracingAudioSourceComponent;
class UWorld;

class UERAYTRACINGAUDIO_API FUERayTracingAudioManager
{
public:
    FUERayTracingAudioManager();

    void AddSource(UUERayTracingAudioSourceComponent* Source);
    void RemoveSource(UUERayTracingAudioSourceComponent* Source);

    void AddListener(UUERayTracingAudioListenerComponent* Listener);
    void RemoveListener(UUERayTracingAudioListenerComponent* Listener);

    void AddGeometry(UUERayTracingAudioGeometryComponent* Geometry);
    void RemoveGeometry(UUERayTracingAudioGeometryComponent* Geometry);

    void MarkSceneDirty();
    void RebuildScene(UWorld* World);

    UUERayTracingAudioListenerComponent* GetCurrentListener() const;

    FUERayTracingAudioDirectSimulationResult SimulateDirectSource(UUERayTracingAudioSourceComponent* Source);
    FUERayTracingAudioIndirectSimulationResult SimulateIndirectSource(UUERayTracingAudioSourceComponent* Source);

    const FUERayTracingAudioScene& GetScene() const;
    const FUERayTracingAudioRayTracingDevice& GetRayTracingDevice() const;

private:
    FUERayTracingAudioContext Context;
    FUERayTracingAudioScene Scene;
    FUERayTracingAudioRayTracingDevice RayTracingDevice;
    FUERayTracingAudioSimulator Simulator;
    TSet<TWeakObjectPtr<UUERayTracingAudioSourceComponent>> Sources;
    TSet<TWeakObjectPtr<UUERayTracingAudioListenerComponent>> Listeners;
    TSet<TWeakObjectPtr<UUERayTracingAudioGeometryComponent>> GeometryComponents;
    bool bSceneDirty = true;
};
