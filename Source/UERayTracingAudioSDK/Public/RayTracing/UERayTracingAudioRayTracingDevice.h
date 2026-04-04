#pragma once

#include "CoreMinimal.h"

class UWorld;
class AActor;
class UPrimitiveComponent;
class FUERayTracingAudioScene;

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioTraceRequest
{
    UWorld* World = nullptr;
    const FUERayTracingAudioScene* Scene = nullptr;
    FVector Start = FVector::ZeroVector;
    FVector End = FVector::ZeroVector;
    const AActor* IgnoredActor = nullptr;
    const AActor* SecondaryIgnoredActor = nullptr;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioRay
{
    FVector Start = FVector::ZeroVector;
    FVector End = FVector::ZeroVector;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioTraceHit
{
    bool bHit = false;
    FVector Location = FVector::ZeroVector;
    FVector Normal = FVector::UpVector;
    float Distance = 0.0f;
    TWeakObjectPtr<const UPrimitiveComponent> Primitive;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioDetailedTraceHit
{
    bool bHit = false;
    FVector Location = FVector::ZeroVector;
    FVector Normal = FVector::UpVector;
    float Distance = 0.0f;
    int32 GeometryIndex = INDEX_NONE;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioEnergyFieldTraceRequest
{
    UWorld* World = nullptr;
    const FUERayTracingAudioScene* Scene = nullptr;
    FVector ListenerLocation = FVector::ZeroVector;
    FVector ListenerForward = FVector::ForwardVector;
    FVector SourceLocation = FVector::ZeroVector;
    FVector AirAbsorptionPerMeter = FVector(0.0002f, 0.0006f, 0.0012f);
    const AActor* ListenerActor = nullptr;
    const AActor* SourceActor = nullptr;
    int32 NumReflectionRays = 64;
    int32 MaxReflectionBounces = 3;
    int32 NumDelayBins = 96;
    float DurationSeconds = 1.0f;
    float ReferenceDistance = 100.0f;
    float SpeedOfSound = 34300.0f;
    float MaxTraceDistance = 100000.0f;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioEnergyFieldTraceResult
{
    TArray<FVector> DelayBinEnergy;
    int32 NumValidContributions = 0;
    float EarliestArrivalSeconds = 0.0f;
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioRayTracingDevice
{
public:
    bool IsRayTracingAvailable() const;
    bool TraceDirectPath(const FUERayTracingAudioTraceRequest& Request, FUERayTracingAudioTraceHit& OutHit) const;
    bool TraceRays(const FUERayTracingAudioTraceRequest& Request, const TArray<FUERayTracingAudioRay>& Rays, TArray<bool>& OutHits) const;
    bool TraceDetailedRays(
        const FUERayTracingAudioTraceRequest& Request,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<FUERayTracingAudioDetailedTraceHit>& OutHits) const;
    bool SimulateIndirectEnergyField(
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        FUERayTracingAudioEnergyFieldTraceResult& OutResult) const;
};
