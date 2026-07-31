#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Simulation/UERayTracingAudioSimulator.h"
#include "UERayTracingAudioSourceComponent.generated.h"

UENUM(BlueprintType)
enum class EUERayTracingAudioIndirectMode : uint8
{
    MinimalConvolution UMETA(DisplayName = "Minimal Convolution"),
    ParametricReverb UMETA(DisplayName = "Parametric Reverb"),
    HybridReverb UMETA(DisplayName = "Hybrid Reverb")
};

UENUM(BlueprintType)
enum class EUERayTracingAudioIndirectDataSource : uint8
{
    Realtime UMETA(DisplayName = "Realtime"),
    Baked UMETA(DisplayName = "Baked IR"),
    Hybrid UMETA(DisplayName = "Baked IR + Realtime Tail")
};

UENUM(BlueprintType)
enum class EUERayTracingAudioBakedAssetStatus : uint8
{
    NotRequired UMETA(DisplayName = "Not Required"),
    MissingAsset UMETA(DisplayName = "Missing Asset"),
    InvalidAsset UMETA(DisplayName = "Invalid Asset"),
    WorldMismatch UMETA(DisplayName = "World Mismatch"),
    MissingListener UMETA(DisplayName = "Missing Listener"),
    MissingPlacementMetadata UMETA(DisplayName = "Legacy Asset Requires Rebake"),
    StaleScene UMETA(DisplayName = "Stale Scene or Materials"),
    StalePlacement UMETA(DisplayName = "Stale Source or Listener Placement"),
    StaleAllowed UMETA(DisplayName = "Stale Asset Allowed"),
    Ready UMETA(DisplayName = "Ready")
};

class UUERayTracingAudioImpulseResponseAsset;
struct FUERayTracingAudioSourceSimulationResult;

UCLASS(ClassGroup = (UERayTracingAudio), meta = (BlueprintSpawnableComponent))
class UERAYTRACINGAUDIO_API UUERayTracingAudioSourceComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UUERayTracingAudioSourceComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    FVector GetSourceLocation() const;
    FVector GetSourceForward() const;
    float GetOccludedGain() const;
    float GetSourceRadiusCm() const;
    int32 GetNumOcclusionSamples() const;
    bool ShouldUseVolumetricOcclusion() const;
    bool ShouldUseHardOcclusion() const;
    FVector GetAirAbsorptionPerMeter() const;
    EUERayTracingAudioIndirectMode GetIndirectMode() const;
    int32 GetNumReflectionRays() const;
    int32 GetMaxReflectionBounces() const;
    float GetIndirectDurationSeconds() const;
    int32 GetMaxEarlyReflectionTaps() const;
    float GetHybridTransitionRatio() const;
    float GetIndirectMix() const;
    const FUERayTracingAudioDirectSimulationResult& GetDirectSoundResult() const;
    const FUERayTracingAudioIndirectSimulationResult& GetIndirectSoundResult() const;
    float GetCurrentOverallGain() const;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound)
    bool bEnableDirectSound;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound, meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float OccludedGain;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound, meta = (ClampMin = "0.0"))
    float SourceRadiusCm;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound, meta = (ClampMin = "1", ClampMax = "128"))
    int32 NumOcclusionSamples;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound)
    bool bUseVolumetricOcclusion;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound)
    bool bHardOcclusion;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound)
    FVector AirAbsorptionPerMeter;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound)
    bool bEnableIndirectSound;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound)
    EUERayTracingAudioIndirectMode IndirectMode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound)
    EUERayTracingAudioIndirectDataSource IndirectDataSource;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound)
    TObjectPtr<UUERayTracingAudioImpulseResponseAsset> BakedImpulseResponseAsset;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (AdvancedDisplay))
    bool bAllowStaleBakedAsset;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (ClampMin = "0.0", AdvancedDisplay))
    float BakedPlacementToleranceCm;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (ClampMin = "1", ClampMax = "4096"))
    int32 NumReflectionRays;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (ClampMin = "1", ClampMax = "8"))
    int32 MaxReflectionBounces;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (ClampMin = "0.05", ClampMax = "4.0"))
    float IndirectDurationSeconds;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxEarlyReflectionTaps;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (ClampMin = "0.05", ClampMax = "0.95"))
    float HybridTransitionRatio;

    // Wet send multiplier. Direct is rendered on an independent path, so
    // values above 1.0 are intentional makeup gain rather than a dry/wet ratio.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (DisplayName = "Wet Send", ClampMin = "0.0", ClampMax = "4.0"))
    float IndirectMix;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    bool bIsOccluded;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    float DistanceAttenuation;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    float DirectVisibility;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    float OverallGain;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    bool bHasIndirectPath;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    int32 NumValidReflectionPaths;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    float IndirectGain;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    float EarlyReflectionGain;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    float LateReverbGain;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    float AverageReflectionDelaySeconds;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    FVector ReverbTimes;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    EUERayTracingAudioBakedAssetStatus BakedAssetStatus;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = IndirectSound)
    FString BakedAssetStatusMessage;

private:
    void PublishSimulationSnapshot();
    void RemoveSimulationSnapshots();
    void RefreshBakedConvolutionKernel();
    void RefreshRealtimeConvolutionKernel(const FUERayTracingAudioSourceSimulationResult& LatestSimulation);
    void ResetRealtimeConvolutionKernel();
    void ResetBakedRuntimeTail();
    void ApplyRuntimeTailFallback();
    void ApplyBakedOnlyResult();
    void SetBakedAssetStatus(EUERayTracingAudioBakedAssetStatus NewStatus, FString NewMessage);

    FUERayTracingAudioDirectSimulationResult DirectSoundResult;
    FUERayTracingAudioIndirectSimulationResult IndirectSoundResult;
    TArray<uint64> PublishedAudioComponentIds;
    TWeakObjectPtr<UUERayTracingAudioImpulseResponseAsset> CachedBakedAsset;
    FGuid CachedBakedAssetId;
    int32 CachedBakedSampleRate = 0;
    float CachedBakedHybridTransitionSeconds = 0.0f;
    bool bBakedRuntimeUsesParametricTail = false;
    float BakedRuntimeLateReverbGain = 0.0f;
    float BakedRuntimeParametricDelaySeconds = 0.0f;
    FVector BakedRuntimeReverbTimes = FVector::ZeroVector;
    TSharedPtr<const class FUERayTracingAudioConvolutionKernel, ESPMode::ThreadSafe> BakedConvolutionKernel;
    TSharedPtr<const class FUERayTracingAudioConvolutionKernel, ESPMode::ThreadSafe> BakedConvolutionKernelRight;
    TSharedPtr<const class FUERayTracingAudioConvolutionKernel, ESPMode::ThreadSafe> RealtimeConvolutionKernelLeft;
    TSharedPtr<const class FUERayTracingAudioConvolutionKernel, ESPMode::ThreadSafe> RealtimeConvolutionKernelRight;
    uint64 CachedRealtimeIndirectGeneration = 0;
    int32 CachedRealtimeSampleRate = 0;
    bool bCachedRealtimeUsesHybridTail = false;
    float CachedRealtimeHybridTransitionSeconds = 0.0f;
    bool bRealtimeRuntimeUsesParametricTail = false;
    float RealtimeRuntimeLateReverbGain = 0.0f;
    float RealtimeRuntimeParametricDelaySeconds = 0.0f;
    FVector RealtimeRuntimeReverbTimes = FVector::ZeroVector;
    const class FUERayTracingAudioConvolutionKernel*
        PublishedBakedKernelIdentity = nullptr;
    const class FUERayTracingAudioConvolutionKernel*
        PublishedBakedRightKernelIdentity = nullptr;
    const class FUERayTracingAudioConvolutionKernel*
        PublishedRealtimeLeftKernelIdentity = nullptr;
    const class FUERayTracingAudioConvolutionKernel*
        PublishedRealtimeRightKernelIdentity = nullptr;
    uint64 BakedLeftConvolutionRevision = 0;
    uint64 BakedRightConvolutionRevision = 0;
    uint64 RealtimeLeftConvolutionRevision = 0;
    uint64 RealtimeRightConvolutionRevision = 0;
    uint64 ConvolutionRevision = 0;
    uint64 SnapshotGeneration = 0;
};
