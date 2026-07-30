#include "Components/UERayTracingAudioListenerComponent.h"

#include "GameFramework/Actor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "UERayTracingAudioModule.h"

UUERayTracingAudioListenerComponent::UUERayTracingAudioListenerComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UUERayTracingAudioListenerComponent::BeginPlay()
{
    Super::BeginPlay();
    FUERayTracingAudioModule::GetManager().AddListener(this);
}

void UUERayTracingAudioListenerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    FUERayTracingAudioModule::GetManager().RemoveListener(this);
    Super::EndPlay(EndPlayReason);
}

FVector UUERayTracingAudioListenerComponent::GetListenerLocation() const
{
    return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

FVector UUERayTracingAudioListenerComponent::GetListenerForward() const
{
    return GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
}
