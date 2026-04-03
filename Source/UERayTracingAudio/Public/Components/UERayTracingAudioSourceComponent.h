#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Simulation/UERayTracingAudioSimulator.h"
#include "UERayTracingAudioSourceComponent.generated.h"

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
    const FUERayTracingAudioDirectSimulationResult& GetDirectSoundResult() const;
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

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    bool bIsOccluded;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    float DistanceAttenuation;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    float DirectVisibility;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = DirectSound)
    float OverallGain;

private:
    FUERayTracingAudioDirectSimulationResult DirectSoundResult;
};
