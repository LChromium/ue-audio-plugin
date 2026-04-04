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
    FVector AirAbsorptionPerMeter;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound)
    bool bEnableIndirectSound;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound)
    EUERayTracingAudioIndirectMode IndirectMode;

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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = IndirectSound, meta = (ClampMin = "0.0", ClampMax = "1.0"))
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

private:
    FUERayTracingAudioDirectSimulationResult DirectSoundResult;
    FUERayTracingAudioIndirectSimulationResult IndirectSoundResult;
};
