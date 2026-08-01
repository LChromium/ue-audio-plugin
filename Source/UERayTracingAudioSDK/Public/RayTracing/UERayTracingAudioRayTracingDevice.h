#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UWorld;
class AActor;
class UPrimitiveComponent;
class FUERayTracingAudioScene;
class FRHICommandListImmediate;
struct FUERayTracingAudioGeometryExport;

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioTraceRequest
{
    UWorld* World = nullptr;
    const FUERayTracingAudioScene* Scene = nullptr;
    uint64 SceneCacheKey = 0;
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

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioAsyncRayQuery
    : public TSharedFromThis<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>
{
public:
    ~FUERayTracingAudioAsyncRayQuery();
    bool IsComplete() const;
    bool ConsumeResult(bool& bOutSucceeded, TArray<bool>& OutHits);
    void Cancel();
    bool WasHardwareRayTracingUsed() const;

private:
    friend class FUERayTracingAudioRayTracingDevice;

    struct FReadbackState;

    bool IsCancelled() const;
    bool PublishReadbackState_RenderThread(
        const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State);
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe>
        GetPublishedReadbackState() const;
    void RetireReadbackState();
    void MarkHardwareRayTracingUsed();
    void Complete(bool bInSucceeded, TArray<bool>&& InHits);
    void BeginHardwareBatchReadback_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        TArray<FUERayTracingAudioGeometryExport>&& Geometry,
        TArray<FUERayTracingAudioRay>&& CombinedRays,
        TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>>&& Queries,
        TArray<int32>&& RayCounts,
        uint64 SceneCacheKey);
    void PollHardwareReadback_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State);

    FCriticalSection ResultMutex;
    mutable FCriticalSection ReadbackStateMutex;
    TAtomic<bool> bComplete = false;
    TAtomic<bool> bReadbackSubmitted = false;
    TAtomic<bool> bCancelled = false;
    TAtomic<bool> bUsedHardwareRayTracing = false;
    bool bSucceeded = false;
    bool bConsumed = false;
    TArray<bool> Hits;
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> ReadbackState;
};

struct UERAYTRACINGAUDIOSDK_API FUERayTracingAudioEnergyFieldTraceRequest
{
    UWorld* World = nullptr;
    const FUERayTracingAudioScene* Scene = nullptr;
    uint64 SceneCacheKey = 0;
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
    TArray<FVector> DelayBinDirection;
    int32 NumValidContributions = 0;
    float EarliestArrivalSeconds = 0.0f;
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioAsyncEnergyFieldQuery
    : public TSharedFromThis<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>
{
public:
    ~FUERayTracingAudioAsyncEnergyFieldQuery();
    bool IsComplete() const;
    bool ConsumeResult(bool& bOutSucceeded, FUERayTracingAudioEnergyFieldTraceResult& OutResult);
    void Cancel();
    bool WasHardwareRayTracingUsed() const;

private:
    friend class FUERayTracingAudioRayTracingDevice;

    struct FReadbackState;

    bool IsCancelled() const;
    bool PublishReadbackState_RenderThread(
        const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State);
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe>
        GetPublishedReadbackState() const;
    void RetireReadbackState();
    void MarkHardwareRayTracingUsed();
    void Complete(bool bInSucceeded, FUERayTracingAudioEnergyFieldTraceResult&& InResult);
    void BeginHardwareBatchReadback_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        TArray<FUERayTracingAudioGeometryExport>&& Geometry,
        TArray<FUERayTracingAudioEnergyFieldTraceRequest>&& Requests,
        TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>>&& Queries,
        uint64 SceneCacheKey);
    void PollHardwareReadback_RenderThread(
        FRHICommandListImmediate& RHICmdList,
        const TSharedPtr<FReadbackState, ESPMode::ThreadSafe>& State);

    FCriticalSection ResultMutex;
    mutable FCriticalSection ReadbackStateMutex;
    TAtomic<bool> bComplete = false;
    TAtomic<bool> bReadbackSubmitted = false;
    TAtomic<bool> bCancelled = false;
    TAtomic<bool> bUsedHardwareRayTracing = false;
    bool bSucceeded = false;
    bool bConsumed = false;
    FUERayTracingAudioEnergyFieldTraceResult Result;
    TSharedPtr<FReadbackState, ESPMode::ThreadSafe> ReadbackState;
};

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioRayTracingDevice
{
public:
    bool IsRayTracingAvailable() const;
    bool TraceDirectPath(const FUERayTracingAudioTraceRequest& Request, FUERayTracingAudioTraceHit& OutHit) const;
    bool TraceRays(
        const FUERayTracingAudioTraceRequest& Request,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<bool>& OutHits,
        bool* bOutUsedHardwareRayTracing = nullptr) const;
    bool TraceDetailedRays(
        const FUERayTracingAudioTraceRequest& Request,
        const TArray<FUERayTracingAudioRay>& Rays,
        TArray<FUERayTracingAudioDetailedTraceHit>& OutHits) const;
    TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> SubmitRays(
        const FUERayTracingAudioTraceRequest& Request,
        const TArray<FUERayTracingAudioRay>& Rays) const;
    // All ray groups share Request.Scene. Returned handles are index-aligned
    // with RayBatches; the implementation merges the RHI dispatch/readback.
    TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>> SubmitRaysBatch(
        const FUERayTracingAudioTraceRequest& Request,
        const TArray<TArray<FUERayTracingAudioRay>>& RayBatches) const;
    bool SimulateIndirectEnergyField(
        const FUERayTracingAudioEnergyFieldTraceRequest& Request,
        FUERayTracingAudioEnergyFieldTraceResult& OutResult) const;
    TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe> SubmitIndirectEnergyField(
        const FUERayTracingAudioEnergyFieldTraceRequest& Request) const;
    // Requests must reference the same immutable scene version. Each active
    // bounce/shadow phase is merged into one RHI dispatch and split back into
    // index-aligned query handles.
    TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>> SubmitIndirectEnergyFieldBatch(
        const TArray<FUERayTracingAudioEnergyFieldTraceRequest>& Requests) const;
};
