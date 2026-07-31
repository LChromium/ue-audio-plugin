#pragma once

#include "CoreMinimal.h"
#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"

class AActor;
class APawn;
class FUERayTracingAudioValidationSourceBufferListener;
class UAudioComponent;
class FUERayTracingAudioBakeJob;
class FUERayTracingAudioManager;
class UUERayTracingAudioListenerComponent;
class UUERayTracingAudioSourceComponent;
class UUERayTracingAudioValidationSoundProxy;
class UUERayTracingAudioValidationSoundWave;
class UStaticMeshComponent;
class UWorld;
enum class EUERayTracingAudioIndirectDataSource : uint8;

class FUERayTracingAudioRuntimeValidation
{
public:
    void Start();
    void Stop();

private:
    struct FScenarioState
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AActor> CameraActor;
        TWeakObjectPtr<APawn> InteractivePawn;
        TWeakObjectPtr<UStaticMeshComponent> ListenerMarker;
        TWeakObjectPtr<UUERayTracingAudioSourceComponent> Source;
        TWeakObjectPtr<UUERayTracingAudioListenerComponent> Listener;
        TWeakObjectPtr<UUERayTracingAudioValidationSoundProxy> PrimarySoundProxy;
        TWeakObjectPtr<UUERayTracingAudioValidationSoundWave> PrimaryPlayback;
        TWeakObjectPtr<UAudioComponent> ReferenceAudioComponent;
        TSharedPtr<
            FUERayTracingAudioValidationSourceBufferListener,
            ESPMode::ThreadSafe> PrimarySourceBufferListener;
        TArray<TWeakObjectPtr<UUERayTracingAudioSourceComponent>> Sources;
        TArray<TWeakObjectPtr<UAudioComponent>> AudioComponents;
        TSharedPtr<FUERayTracingAudioBakeJob> BakeJob;
        TSharedPtr<FUERayTracingAudioBakeJob> DataSourceBakeJob;
        TArray<float> FirstBakeSamples;
        FString DirectPreset = TEXT("soft_occluded");
        FUERayTracingAudioDataSourceAudioStats RealtimeAudioStats;
        FUERayTracingAudioDataSourceAudioStats BakedAudioStats;
        FUERayTracingAudioDataSourceAudioStats HybridAudioStats;
        double StartTimeSeconds = 0.0;
        double AudioStartTimeSeconds = 0.0;
        double BakeStartTimeSeconds = 0.0;
        double DataSourcePhaseStartTimeSeconds = 0.0;
        double InteractiveSmokePhaseStartTimeSeconds = 0.0;
        double FirstBakeEnergy = 0.0;
        float FirstBakeBinDurationSeconds = 0.0f;
        FVector FixedListenerLocation = FVector::ZeroVector;
        FVector InteractiveSmokeStartPawnLocation = FVector::ZeroVector;
        FRotator InteractiveStartRotation = FRotator::ZeroRotator;
        float InteractiveSmokeMovementDistanceCm = 0.0f;
        float InteractiveSmokeMaxListenerCameraErrorCm = 0.0f;
        int32 SourceCount = 0;
        int32 GeometryCount = 0;
        int32 TriangleCount = 0;
        int32 MaxDirectBatchSources = 0;
        int32 MaxIndirectBatchSources = 0;
        int32 MutedForeignAudioComponentCount = 0;
        int32 ABPlaybackRestartCount = 0;
        int32 BakePassIndex = 0;
        int32 DataSourceValidationPhase = 0;
        int32 InteractiveSmokePhase = 0;
        int32 BakedKernelCount = 0;
        int32 RealtimeKernelCount = 0;
        int32 HybridKernelCount = 0;
        bool bAllAudioChainsStarted = false;
        bool bABPlaybackStarted = false;
        bool bAcousticStartupFailed = false;
        bool bReferenceAudioStarted = false;
        bool bRenderedABEnabled = false;
        bool bRenderedPlaybackReady = false;
        bool bCameraActivated = false;
        bool bInteractiveRequested = false;
        bool bInteractiveMode = false;
        bool bInteractiveReadyLogged = false;
        bool bInteractiveSmokeEnabled = false;
        bool bInteractiveSmokeLogged = false;
        bool bInteractiveSmokeSawRealtime = false;
        bool bInteractiveSmokeSawBaked = false;
        bool bInteractiveSmokeSawHybrid = false;
        bool bInteractiveSmokeSawRenderedAB = false;
        bool bInteractiveSmokeSawReferenceAB = false;
        bool bResultLogged = false;
        bool bDataSourceDiagnosticsArmed = false;
        bool bDataSourceValidationLogged = false;
        bool bDataSourceValidationPassed = false;
        bool bStereoBakedIrObserved = false;
        bool bPerformanceProfile = false;
        bool bBakeRepeatabilityEnabled = false;
        bool bBakeRepeatabilityLogged = false;
    };

    void CreateScenario(UWorld* World);
    bool Tick(float DeltaTime);
    void TickDataSourceValidation(FScenarioState& State, FUERayTracingAudioManager& Manager);
    void TickBakeRepeatability(FScenarioState& State, FUERayTracingAudioManager& Manager);
    void TickInteractiveControls(FScenarioState& State);
    void TickInteractiveSmoke(FScenarioState& State);
    bool SetInteractiveMode(FScenarioState& State, bool bEnabled);
    bool PlaceInteractiveViewAtBakedOrigin(
        FScenarioState& State,
        class APlayerController& PlayerController,
        APawn& Pawn);
    void SetInteractiveDataSource(
        FScenarioState& State,
        EUERayTracingAudioIndirectDataSource DataSource);
    bool EnsureSynchronizedABPlayback(
        FScenarioState& State,
        const TCHAR* Reason,
        bool bForceRestart);
    void SetRenderedABMode(FScenarioState& State, bool bRenderedEnabled);
    int32 MuteForeignWorldAudio(FScenarioState& State);
    int32 CountPlayingForeignWorldAudio(const FScenarioState& State) const;

    FDelegateHandle WorldInitializationHandle;
    FTSTicker::FDelegateHandle TickerHandle;
    TSet<TWeakObjectPtr<UWorld>> InitializedWorlds;
    TArray<FScenarioState> Scenarios;
};
