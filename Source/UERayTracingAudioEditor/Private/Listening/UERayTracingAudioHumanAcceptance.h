#pragma once

#include "CoreMinimal.h"

struct FUERayTracingAudioHumanAcceptanceRequest
{
    FString RecordedAtUtc;
    FString TargetListeningDevice;
    FString ListeningNotes;
    FString LastPreviewedMode;
    bool bHumanListeningPassed = false;
    bool bPreviewedReference = false;
    bool bPreviewedDirect = false;
    bool bPreviewedWet = false;
    bool bPreviewedFull = false;
    bool bRecognizableDirectConfirmed = false;
    bool bAudibleWetFullDifferenceConfirmed = false;
    bool bMovingOcclusionContinuityConfirmed = false;
    bool bModeSwitchingContinuityConfirmed = false;
    bool bEnvironmentDifferenceConfirmed = false;
};

class FUERayTracingAudioHumanAcceptance
{
public:
    static constexpr int32 SchemaVersion = 3;

    static bool BuildRecordJson(
        const FString& ComparisonManifestFilename,
        const FString& ComparisonManifestJson,
        const FUERayTracingAudioHumanAcceptanceRequest& Request,
        FString& OutRecordJson,
        FString& OutError);

    static bool SaveRecord(
        const FString& ComparisonManifestFilename,
        const FUERayTracingAudioHumanAcceptanceRequest& Request,
        FString& OutRecordFilename,
        FString& OutError);
};
