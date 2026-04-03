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

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioRayTracingDevice
{
public:
    bool IsRayTracingAvailable() const;
    bool TraceDirectPath(const FUERayTracingAudioTraceRequest& Request, FUERayTracingAudioTraceHit& OutHit) const;
    bool TraceRays(const FUERayTracingAudioTraceRequest& Request, const TArray<FUERayTracingAudioRay>& Rays, TArray<bool>& OutHits) const;
};
