#pragma once

#include "CoreMinimal.h"

struct FUERayTracingAudioOfflineRenderRequest
{
    TArray<int16> InputInterleavedPcm;
    int32 InputSampleRate = 0;
    int32 NumChannels = 0;
    TArray<float> ImpulseResponse;
    int32 ImpulseResponseNumChannels = 1;
    int32 OutputSampleRate = 0;
    float DirectGain = 1.0f;
    float WetMix = 1.0f;
    FString OutputDirectory;
    FString OutputFilenamePrefix;
    FString InputAssetPath;
    FString SourceActorPath;
    FString ListenerActorPath;
    FString SceneSignature;
    FString DirectPreset = TEXT("custom");
    FString ReflectionEnvironment = TEXT("enclosed");
    float DirectDistanceCm = 0.0f;
    float DirectVisibility = 1.0f;
    float DirectOcclusion = 1.0f;
    float DirectDistanceAttenuation = 1.0f;
    float DirectAirAbsorptionAverage = 1.0f;
    int32 ReflectionRayCount = 0;
    int32 ReflectionBounceCount = 0;
    int32 HardwareIndirectValidPaths = 0;
    float HardwareIndirectGain = 0.0f;
    float HardwareEarlyReflectionGain = 0.0f;
    float HardwareLateReverbGain = 0.0f;
    float HardwareEarliestArrivalSeconds = 0.0f;
    float HardwareAverageDelaySeconds = 0.0f;
    FVector HardwareReverbTimes = FVector::ZeroVector;
    FVector HardwareDominantArrivalDirection = FVector::ZeroVector;
    float HardwareDirectionalEnergyRatio = 0.0f;
    int32 HardwareDirectionalBinCount = 0;
    double HardwareImpulseResponseEnergy = 0.0;
    bool bHasCpuReference = false;
    int32 CpuReferenceIndirectValidPaths = 0;
    float CpuReferenceIndirectGain = 0.0f;
    float CpuReferenceEarlyReflectionGain = 0.0f;
    float CpuReferenceLateReverbGain = 0.0f;
    FVector CpuReferenceDominantArrivalDirection = FVector::ZeroVector;
    float CpuReferenceDirectionalEnergyRatio = 0.0f;
    int32 CpuReferenceDirectionalBinCount = 0;
    double CpuReferenceImpulseResponseEnergy = 0.0;
    bool bUsedHardwareRayTracing = false;
};

struct FUERayTracingAudioOfflineRenderResult
{
    bool bSucceeded = false;
    FString Error;
    FString ReferenceWaveFilename;
    FString DirectWaveFilename;
    FString WetWaveFilename;
    FString FullWaveFilename;
    FString ManifestFilename;
    int32 SampleRate = 0;
    int32 NumChannels = 0;
    int32 ImpulseResponseNumChannels = 1;
    int32 ImpulseResponseNumFrames = 0;
    double ImpulseResponseDurationSeconds = 0.0;
    int32 NumFrames = 0;
    float DurationSeconds = 0.0f;
    float DirectGain = 1.0f;
    float WetMix = 0.0f;
    FString DirectPreset;
    FString ReflectionEnvironment;
    float DirectDistanceCm = 0.0f;
    float DirectVisibility = 1.0f;
    float DirectOcclusion = 1.0f;
    float DirectDistanceAttenuation = 1.0f;
    float DirectAirAbsorptionAverage = 1.0f;
    int32 ReflectionRayCount = 0;
    int32 ReflectionBounceCount = 0;
    int32 HardwareIndirectValidPaths = 0;
    float HardwareIndirectGain = 0.0f;
    float HardwareEarlyReflectionGain = 0.0f;
    float HardwareLateReverbGain = 0.0f;
    float HardwareEarliestArrivalSeconds = 0.0f;
    float HardwareAverageDelaySeconds = 0.0f;
    FVector HardwareReverbTimes = FVector::ZeroVector;
    FVector HardwareDominantArrivalDirection = FVector::ZeroVector;
    float HardwareDirectionalEnergyRatio = 0.0f;
    int32 HardwareDirectionalBinCount = 0;
    double HardwareImpulseResponseEnergy = 0.0;
    bool bHasCpuReference = false;
    int32 CpuReferenceIndirectValidPaths = 0;
    float CpuReferenceIndirectGain = 0.0f;
    float CpuReferenceEarlyReflectionGain = 0.0f;
    float CpuReferenceLateReverbGain = 0.0f;
    FVector CpuReferenceDominantArrivalDirection = FVector::ZeroVector;
    float CpuReferenceDirectionalEnergyRatio = 0.0f;
    int32 CpuReferenceDirectionalBinCount = 0;
    double CpuReferenceImpulseResponseEnergy = 0.0;
    float CommonOutputScale = 1.0f;
    float PeakBeforeScale = 0.0f;
    float ReferenceRms = 0.0f;
    float DirectRms = 0.0f;
    float WetRms = 0.0f;
    float FullRms = 0.0f;
    float DirectDryCorrelation = 0.0f;
    float FullDryCorrelation = 0.0f;
    float WetDryCorrelation = 0.0f;
    float DirectToReferenceRmsRatio = 0.0f;
    float WetToReferenceRmsRatio = 0.0f;
    float FullToReferenceRmsRatio = 0.0f;
    float DirectWetNormalizedDifference = 0.0f;
    float WetStereoNormalizedDifference = 0.0f;
    bool bDirectionalWetIsDistinct = false;
    float PostScalePeak = 0.0f;
    int64 ClippedSampleCount = 0;
    int32 DirectActiveWindowCount = 0;
    int32 DirectDropoutWindowCount = 0;
    float DirectModelResidualRms = 0.0f;
    float FullMixResidualRms = 0.0f;
    float MaxDirectDiscontinuityResidual = 0.0f;
    bool bSamplesFinite = false;
    bool bDirectDropoutCheckApplicable = false;
    bool bAudioSafetyChecksPassed = false;
    bool bDirectSemanticsPassed = false;
    bool bModesAreDistinct = false;
    bool bRecommendedInputDuration = false;
    bool bAutomaticChecksPassed = false;
    bool bUsedHardwareRayTracing = false;
};

class FUERayTracingAudioOfflineRenderer
{
public:
    static FUERayTracingAudioOfflineRenderResult RenderComparisonToWaveFiles(
        FUERayTracingAudioOfflineRenderRequest&& Request);
};
