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
    , bEnableIndirectSound(true)
    , IndirectMode(EUERayTracingAudioIndirectMode::MinimalConvolution)
    , NumReflectionRays(64)
    , MaxReflectionBounces(2)
    , IndirectDurationSeconds(1.0f)
    , MaxEarlyReflectionTaps(16)
    , HybridTransitionRatio(0.35f)
    , IndirectMix(0.3f)
    , bIsOccluded(false)
    , DistanceAttenuation(1.0f)
    , DirectVisibility(1.0f)
    , OverallGain(1.0f)
    , bHasIndirectPath(false)
    , NumValidReflectionPaths(0)
    , IndirectGain(0.0f)
    , EarlyReflectionGain(0.0f)
    , LateReverbGain(0.0f)
    , AverageReflectionDelaySeconds(0.0f)
    , ReverbTimes(FVector::ZeroVector)
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
    }
    else
    {
        DirectSoundResult = FUERayTracingAudioModule::GetManager().SimulateDirectSource(this);
        bIsOccluded = DirectSoundResult.bIsOccluded;
        DistanceAttenuation = DirectSoundResult.DistanceAttenuation;
        DirectVisibility = DirectSoundResult.DirectVisibility;
        OverallGain = DirectSoundResult.OverallGain;
    }

    if (!bEnableIndirectSound)
    {
        IndirectSoundResult = FUERayTracingAudioIndirectSimulationResult();
        bHasIndirectPath = false;
        NumValidReflectionPaths = 0;
        IndirectGain = 0.0f;
        EarlyReflectionGain = 0.0f;
        LateReverbGain = 0.0f;
        AverageReflectionDelaySeconds = 0.0f;
        ReverbTimes = FVector::ZeroVector;
        return;
    }

    IndirectSoundResult = FUERayTracingAudioModule::GetManager().SimulateIndirectSource(this);
    bHasIndirectPath = IndirectSoundResult.bHasValidPaths;
    NumValidReflectionPaths = IndirectSoundResult.NumValidPaths;
    IndirectGain = IndirectSoundResult.IndirectGain;
    EarlyReflectionGain = IndirectSoundResult.EarlyReflectionGain;
    LateReverbGain = IndirectSoundResult.LateReverbGain;
    AverageReflectionDelaySeconds = IndirectSoundResult.AverageDelaySeconds;
    ReverbTimes = IndirectSoundResult.ReverbTimes;
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

EUERayTracingAudioIndirectMode UUERayTracingAudioSourceComponent::GetIndirectMode() const
{
    return IndirectMode;
}

int32 UUERayTracingAudioSourceComponent::GetNumReflectionRays() const
{
    return NumReflectionRays;
}

int32 UUERayTracingAudioSourceComponent::GetMaxReflectionBounces() const
{
    return MaxReflectionBounces;
}

float UUERayTracingAudioSourceComponent::GetIndirectDurationSeconds() const
{
    return IndirectDurationSeconds;
}

int32 UUERayTracingAudioSourceComponent::GetMaxEarlyReflectionTaps() const
{
    return MaxEarlyReflectionTaps;
}

float UUERayTracingAudioSourceComponent::GetHybridTransitionRatio() const
{
    return HybridTransitionRatio;
}

float UUERayTracingAudioSourceComponent::GetIndirectMix() const
{
    return IndirectMix;
}

const FUERayTracingAudioDirectSimulationResult& UUERayTracingAudioSourceComponent::GetDirectSoundResult() const
{
    return DirectSoundResult;
}

const FUERayTracingAudioIndirectSimulationResult& UUERayTracingAudioSourceComponent::GetIndirectSoundResult() const
{
    return IndirectSoundResult;
}

float UUERayTracingAudioSourceComponent::GetCurrentOverallGain() const
{
    return DirectSoundResult.OverallGain;
}
