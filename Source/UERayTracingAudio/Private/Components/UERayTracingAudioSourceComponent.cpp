#include "Components/UERayTracingAudioSourceComponent.h"

#include "GameFramework/Actor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "UERayTracingAudioModule.h"

UUERayTracingAudioSourceComponent::UUERayTracingAudioSourceComponent()
    : bEnableDirectSound(true)
    , OccludedGain(0.2f)
    , SourceRadiusCm(30.0f)
    , NumOcclusionSamples(8)
    , bUseVolumetricOcclusion(true)
    , AirAbsorptionPerMeter(0.0002f, 0.0006f, 0.0012f)
    , bIsOccluded(false)
    , DistanceAttenuation(1.0f)
    , DirectVisibility(1.0f)
    , OverallGain(1.0f)
{
    PrimaryComponentTick.bCanEverTick = true;
}

void UUERayTracingAudioSourceComponent::BeginPlay()
{
    Super::BeginPlay();
    FUERayTracingAudioModule::GetManager().AddSource(this);
}

void UUERayTracingAudioSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    FUERayTracingAudioModule::GetManager().RemoveSource(this);
    Super::EndPlay(EndPlayReason);
}

void UUERayTracingAudioSourceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bEnableDirectSound)
    {
        DirectSoundResult = FUERayTracingAudioDirectSimulationResult();
        bIsOccluded = false;
        DistanceAttenuation = 1.0f;
        DirectVisibility = 1.0f;
        OverallGain = 1.0f;
        return;
    }

    DirectSoundResult = FUERayTracingAudioModule::GetManager().SimulateSource(this);
    bIsOccluded = DirectSoundResult.bIsOccluded;
    DistanceAttenuation = DirectSoundResult.DistanceAttenuation;
    DirectVisibility = DirectSoundResult.DirectVisibility;
    OverallGain = DirectSoundResult.OverallGain;
}

FVector UUERayTracingAudioSourceComponent::GetSourceLocation() const
{
    return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

FVector UUERayTracingAudioSourceComponent::GetSourceForward() const
{
    return GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
}

float UUERayTracingAudioSourceComponent::GetOccludedGain() const
{
    return OccludedGain;
}

float UUERayTracingAudioSourceComponent::GetSourceRadiusCm() const
{
    return SourceRadiusCm;
}

int32 UUERayTracingAudioSourceComponent::GetNumOcclusionSamples() const
{
    return NumOcclusionSamples;
}

bool UUERayTracingAudioSourceComponent::ShouldUseVolumetricOcclusion() const
{
    return bUseVolumetricOcclusion;
}

FVector UUERayTracingAudioSourceComponent::GetAirAbsorptionPerMeter() const
{
    return AirAbsorptionPerMeter;
}

const FUERayTracingAudioDirectSimulationResult& UUERayTracingAudioSourceComponent::GetDirectSoundResult() const
{
    return DirectSoundResult;
}

float UUERayTracingAudioSourceComponent::GetCurrentOverallGain() const
{
    return DirectSoundResult.OverallGain;
}
