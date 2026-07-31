#pragma once

#include "CoreMinimal.h"

#include "Assets/UERayTracingAudioImpulseResponseAsset.h"
#include "RayTracing/UERayTracingAudioRayTracingDevice.h"
#include "Simulation/UERayTracingAudioSimulator.h"

enum class EUERayTracingAudioBakeJobState : uint8
{
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

struct UERAYTRACINGAUDIO_API FUERayTracingAudioBakeResult
{
    FSoftObjectPath SourceWorld;
    int32 SceneVersion = 0;
    FString SceneSignature;
    FVector SourceLocation = FVector::ZeroVector;
    FVector ListenerLocation = FVector::ZeroVector;
    FUERayTracingAudioBakeSettings BakeSettings;
    EUERayTracingAudioImpulseResponseChannelFormat ChannelFormat = EUERayTracingAudioImpulseResponseChannelFormat::Mono;
    int32 NumChannels = 1;
    float BinDurationSeconds = 0.0f;
    TArray<float> Samples;
    FUERayTracingAudioDirectSimulationResult DirectResult;
    int32 IndirectValidPathCount = 0;
    float IndirectGain = 0.0f;
    float EarlyReflectionGain = 0.0f;
    float LateReverbGain = 0.0f;
    float EarliestArrivalSeconds = 0.0f;
    float AverageDelaySeconds = 0.0f;
    FVector ReverbTimes = FVector::ZeroVector;
    FVector DominantArrivalDirection = FVector::ZeroVector;
    float DirectionalEnergyRatio = 0.0f;
    int32 DirectionalBinCount = 0;
    double ImpulseResponseEnergy = 0.0;
    bool bHasCpuReference = false;
    int32 CpuReferenceValidPathCount = 0;
    float CpuReferenceIndirectGain = 0.0f;
    float CpuReferenceEarlyReflectionGain = 0.0f;
    float CpuReferenceLateReverbGain = 0.0f;
    FVector CpuReferenceDominantArrivalDirection = FVector::ZeroVector;
    float CpuReferenceDirectionalEnergyRatio = 0.0f;
    int32 CpuReferenceDirectionalBinCount = 0;
    double CpuReferenceImpulseResponseEnergy = 0.0;
    bool bUsedHardwareRayTracing = false;
};

class UERAYTRACINGAUDIO_API FUERayTracingAudioBakeJob
{
public:
    void Cancel();

    EUERayTracingAudioBakeJobState GetState() const;
    float GetProgress() const;
    const FString& GetStatusText() const;
    const FString& GetError() const;
    bool GetResult(FUERayTracingAudioBakeResult& OutResult) const;

private:
    friend class FUERayTracingAudioManager;

    void SetFailed(FString InError);

    EUERayTracingAudioBakeJobState State = EUERayTracingAudioBakeJobState::Pending;
    float Progress = 0.0f;
    FString StatusText = TEXT("Pending");
    FString Error;
    FUERayTracingAudioIndirectSimulationInput SimulationInput;
    FUERayTracingAudioDirectSimulationInput DirectSimulationInput;
    FUERayTracingAudioDirectSimulationQuery DirectSimulationQuery;
    FUERayTracingAudioBakeResult Result;
    TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe> Query;
    TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe> DirectQuery;
    bool bCaptureCpuReference = false;
    bool bDirectCompleted = false;
    bool bIndirectCompleted = false;
};
