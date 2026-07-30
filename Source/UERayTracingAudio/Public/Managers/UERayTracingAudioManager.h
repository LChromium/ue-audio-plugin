#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"

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
    explicit FUERayTracingAudioManager(
        const FUERayTracingAudioContextSettings& ContextSettings = {});
    ~FUERayTracingAudioManager();
    FUERayTracingAudioManager(const FUERayTracingAudioManager&) = delete;
    FUERayTracingAudioManager& operator=(
        const FUERayTracingAudioManager&) = delete;

    void AddSource(UUERayTracingAudioSourceComponent* Source);
    void RemoveSource(UUERayTracingAudioSourceComponent* Source);

    void AddListener(UUERayTracingAudioListenerComponent* Listener);
    void RemoveListener(UUERayTracingAudioListenerComponent* Listener);

    void AddGeometry(UUERayTracingAudioGeometryComponent* Geometry);
    void RemoveGeometry(UUERayTracingAudioGeometryComponent* Geometry);

    void MarkSceneDirty(UWorld* World);
    void RebuildScene(UWorld* World);

    UUERayTracingAudioListenerComponent* GetCurrentListener(
        const UWorld* World) const;

    FUERayTracingAudioDirectSimulationResult SimulateDirectSource(
        UUERayTracingAudioSourceComponent* Source);
    FUERayTracingAudioIndirectSimulationResult SimulateIndirectSource(
        UUERayTracingAudioSourceComponent* Source);

    const FUERayTracingAudioScene& GetScene(UWorld* World);
    FString GetCurrentSceneSignature(UWorld* World);
    const FUERayTracingAudioRayTracingDevice& GetRayTracingDevice() const;

private:
    struct FWorldAcousticState
    {
        FUERayTracingAudioScene Scene;
        FString SceneSignature;
        bool bSceneDirty = true;
    };

    bool BuildDirectSimulationInput(
        UUERayTracingAudioSourceComponent* Source,
        FUERayTracingAudioDirectSimulationInput& OutInput);
    bool BuildIndirectSimulationInput(
        UUERayTracingAudioSourceComponent* Source,
        FUERayTracingAudioIndirectSimulationInput& OutInput);
    FString BuildSceneSignature(const FUERayTracingAudioScene& Scene) const;
    FWorldAcousticState& GetOrCreateWorldAcousticState(UWorld* World);
    void RemoveDeadWorldState();
    bool TickWorldStateCleanup(float DeltaTime);

    FUERayTracingAudioContext Context;
    FUERayTracingAudioRayTracingDevice RayTracingDevice;
    FUERayTracingAudioSimulator Simulator;
    TSet<TWeakObjectPtr<UUERayTracingAudioSourceComponent>> Sources;
    TMap<
        TWeakObjectPtr<UWorld>,
        TWeakObjectPtr<UUERayTracingAudioListenerComponent>> ListenersByWorld;
    TMap<
        TWeakObjectPtr<UWorld>,
        TUniquePtr<FWorldAcousticState>> AcousticStatesByWorld;
    TSet<TWeakObjectPtr<UUERayTracingAudioGeometryComponent>> GeometryComponents;
    FTSTicker::FDelegateHandle WorldStateTickerHandle;
};
