#include "Components/UERayTracingAudioListenerComponent.h"

#include "GameFramework/Actor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "UERayTracingAudioModule.h"

TWeakObjectPtr<UUERayTracingAudioListenerComponent> UUERayTracingAudioListenerComponent::CurrentListener;

UUERayTracingAudioListenerComponent::UUERayTracingAudioListenerComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UUERayTracingAudioListenerComponent::BeginPlay()
{
    Super::BeginPlay();
    CurrentListener = this;
    FUERayTracingAudioModule::GetManager().AddListener(this);
}

void UUERayTracingAudioListenerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    FUERayTracingAudioModule::GetManager().RemoveListener(this);

    if (CurrentListener.Get() == this)
    {
        CurrentListener = nullptr;
    }

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

UUERayTracingAudioListenerComponent* UUERayTracingAudioListenerComponent::GetCurrentListener()
{
    return CurrentListener.Get();
}
