#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UERayTracingAudioListenerComponent.generated.h"

UCLASS(ClassGroup = (UERayTracingAudio), meta = (BlueprintSpawnableComponent))
class UERAYTRACINGAUDIO_API UUERayTracingAudioListenerComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UUERayTracingAudioListenerComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    FVector GetListenerLocation() const;
    FVector GetListenerForward() const;
};
