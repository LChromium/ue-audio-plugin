#pragma once

#include "CoreMinimal.h"

#include "API/UERayTracingAudioContext.h"
#include "RayTracing/UERayTracingAudioRayTracingDevice.h"

enum class EUERayTracingAudioIndirectEffectType : uint8
{
    Convolution = 0,
    Parametric = 1,
    Hybrid = 2
};

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

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioIndirectSimulationInput
{
    FVector ListenerLocation = FVector::ZeroVector;
    FVector ListenerForward = FVector::ForwardVector;
    FVector SourceLocation = FVector::ZeroVector;
    FVector SourceForward = FVector::ForwardVector;
    float SourceRadiusCm = 30.0f;
    int32 NumReflectionRays = 64;
    int32 MaxReflectionBounces = 2;
    float DurationSeconds = 1.0f;
    int32 MaxEarlyReflectionTaps = 16;
    float HybridTransitionRatio = 0.35f;
    FVector AirAbsorptionPerMeter = FVector(0.0002f, 0.0006f, 0.0012f);
    UWorld* World = nullptr;
    const class FUERayTracingAudioScene* Scene = nullptr;
    const AActor* ListenerActor = nullptr;
    const AActor* SourceActor = nullptr;
    EUERayTracingAudioIndirectEffectType EffectType = EUERayTracingAudioIndirectEffectType::Convolution;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioIndirectSimulationResult
{
    bool bHasListener = false;
    bool bHasValidPaths = false;
    bool bUsedHybrid = false;
    bool bUsedParametricTail = false;
    int32 NumValidPaths = 0;
    float IndirectGain = 0.0f;
    float EarlyReflectionGain = 0.0f;
    float LateReverbGain = 0.0f;
    float AverageDelaySeconds = 0.0f;
    float HybridTransitionSeconds = 0.0f;
    FVector ReverbTimes = FVector::ZeroVector;
    TArray<float> EarlyReflectionDelaySeconds;
    TArray<float> EarlyReflectionGains;
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioSimulator
{
public:
    explicit FUERayTracingAudioSimulator(const FUERayTracingAudioContext& InContext);

    FUERayTracingAudioDirectSimulationResult SimulateDirectSound(
        const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
        const FUERayTracingAudioDirectSimulationInput& Input) const;

    FUERayTracingAudioIndirectSimulationResult SimulateIndirectSound(
        const FUERayTracingAudioRayTracingDevice& RayTracingDevice,
        const FUERayTracingAudioIndirectSimulationInput& Input) const;

private:
    const FUERayTracingAudioContext& Context;
};
