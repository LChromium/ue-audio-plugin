#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"

#include "API/UERayTracingAudioContext.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "Bake/UERayTracingAudioBakeJob.h"
#include "RayTracing/UERayTracingAudioRayTracingDevice.h"
#include "Scene/UERayTracingAudioScene.h"
#include "Simulation/UERayTracingAudioSimulator.h"

class UUERayTracingAudioGeometryComponent;
class UUERayTracingAudioListenerComponent;
class UUERayTracingAudioSourceComponent;
class UWorld;

struct UERAYTRACINGAUDIO_API FUERayTracingAudioSourceSimulationResult
{
    FUERayTracingAudioDirectSimulationResult DirectResult;
    FUERayTracingAudioIndirectSimulationResult IndirectResult;
    bool bHasDirectResult = false;
    bool bHasIndirectResult = false;
    uint64 DirectGeneration = 0;
    uint64 IndirectGeneration = 0;
    uint64 Generation = 0;
};

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

    FUERayTracingAudioDirectSimulationResult SimulateDirectSource(UUERayTracingAudioSourceComponent* Source);
    FUERayTracingAudioIndirectSimulationResult SimulateIndirectSource(UUERayTracingAudioSourceComponent* Source);

    void RequestSourceSimulation(
        UUERayTracingAudioSourceComponent* Source,
        bool bRequestDirect,
        bool bRequestIndirect);
    bool GetLatestSourceSimulation(
        const UUERayTracingAudioSourceComponent* Source,
        FUERayTracingAudioSourceSimulationResult& OutResult) const;

    const FUERayTracingAudioScene& GetScene(UWorld* World);
    FString GetCurrentSceneSignature(UWorld* World);
    const FUERayTracingAudioRayTracingDevice& GetRayTracingDevice() const;
    FUERayTracingAudioSimulationSnapshotRegistry& GetSnapshotRegistry();
    TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> GetSnapshotRegistrySharedRef();

    TSharedPtr<FUERayTracingAudioBakeJob> StartImpulseResponseBake(
        UUERayTracingAudioSourceComponent* Source,
        UUERayTracingAudioListenerComponent* Listener,
        const FUERayTracingAudioBakeSettings& Settings,
        bool bCaptureCpuReference = false);

private:
    struct FWorldAcousticState
    {
        FUERayTracingAudioScene Scene;
        FString SceneSignature;
        bool bSceneDirty = true;
    };

    struct FSourceSimulationState
    {
        FUERayTracingAudioSourceSimulationResult LatestResult;
        FUERayTracingAudioDirectSimulationInput PendingDirectInput;
        FUERayTracingAudioDirectSimulationQuery PendingDirectQuery;
        TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> DirectQuery;
        FUERayTracingAudioIndirectSimulationInput PendingIndirectInput;
        TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe> IndirectQuery;
        bool bDirectRequested = false;
        bool bIndirectRequested = false;
        bool bQueued = false;
        double FirstQueuedTimeSeconds = 0.0;
    };

    bool BuildDirectSimulationInput(
        UUERayTracingAudioSourceComponent* Source,
        FUERayTracingAudioDirectSimulationInput& OutInput);
    bool BuildIndirectSimulationInput(
        UUERayTracingAudioSourceComponent* Source,
        FUERayTracingAudioIndirectSimulationInput& OutInput);
    void PollCompletedDirectQueries();
    void PollCompletedIndirectQueries();
    void PollBakeJobs();
    bool RebuildSceneForBake(UWorld* World);
    FString BuildSceneSignature(const FUERayTracingAudioScene& Scene) const;
    FWorldAcousticState& GetOrCreateWorldAcousticState(UWorld* World);
    void RemoveDeadWorldState();
    bool TickSimulationQueue(float DeltaTime);

    FUERayTracingAudioContext Context;
    FUERayTracingAudioRayTracingDevice RayTracingDevice;
    FUERayTracingAudioSimulator Simulator;
    TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry;
    TSet<TWeakObjectPtr<UUERayTracingAudioSourceComponent>> Sources;
    TMap<
        TWeakObjectPtr<UWorld>,
        TWeakObjectPtr<UUERayTracingAudioListenerComponent>> ListenersByWorld;
    TMap<
        TWeakObjectPtr<UWorld>,
        TUniquePtr<FWorldAcousticState>> AcousticStatesByWorld;
    TSet<TWeakObjectPtr<UUERayTracingAudioGeometryComponent>> GeometryComponents;
    TMap<TWeakObjectPtr<UUERayTracingAudioSourceComponent>, FSourceSimulationState> SourceSimulationStates;
    TArray<TWeakObjectPtr<UUERayTracingAudioSourceComponent>> PendingSimulationSources;
    TArray<TSharedPtr<FUERayTracingAudioBakeJob>> ActiveBakeJobs;
    FTSTicker::FDelegateHandle SimulationTickerHandle;
    uint64 NextSimulationGeneration = 0;
};
