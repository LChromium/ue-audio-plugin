#pragma once

#include "CoreMinimal.h"

#include "API/UERayTracingAudioContext.h"
#include "RayTracing/UERayTracingAudioRayTracingDevice.h"

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioDirectSimulationInput
{
    FVector ListenerLocation = FVector::ZeroVector;
    FVector SourceLocation = FVector::ZeroVector;
    FVector SourceForward = FVector::ForwardVector;
    float OccludedGain = 0.2f;
    float SourceRadiusCm = 30.0f;
    int32 NumOcclusionSamples = 8;
    bool bUseVolumetricOcclusion = true;
    FVector AirAbsorptionPerMeter = FVector(0.0002f, 0.0006f, 0.0012f);
    UWorld* World = nullptr;
    const class FUERayTracingAudioScene* Scene = nullptr;
    const AActor* ListenerActor = nullptr;
    const AActor* SourceActor = nullptr;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioDirectSimulationResult
{
    bool bHasListener = false;
    bool bIsOccluded = false;
    bool bRayTracingAvailable = false;
    float DistanceCm = 0.0f;
    float DistanceAttenuation = 1.0f;
    float DirectVisibility = 1.0f;
    float Occlusion = 1.0f;
    FVector AirAbsorption = FVector::OneVector;
    float OverallGain = 1.0f;
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioSimulator
{
public:
    explicit FUERayTracingAudioSimulator(const FUERayTracingAudioContext& InContext);

    FUERayTracingAudioDirectSimulationResult SimulateDirectSound(
        const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
        const FUERayTracingAudioDirectSimulationInput& Input) const;

private:
    const FUERayTracingAudioContext& Context;
};
