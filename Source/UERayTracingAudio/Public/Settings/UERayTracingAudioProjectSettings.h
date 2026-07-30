#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "API/UERayTracingAudioContext.h"

#include "UERayTracingAudioProjectSettings.generated.h"

UCLASS(Config=Engine, DefaultConfig, meta=(DisplayName="UE Ray Tracing Audio"))
class UERAYTRACINGAUDIO_API UUERayTracingAudioProjectSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UUERayTracingAudioProjectSettings();

    UPROPERTY(
        Config,
        EditAnywhere,
        Category="Acoustic Physics",
        meta=(ClampMin="1.0", UIMin="1.0", Units="cm", ConfigRestartRequired=true))
    float ReferenceDistanceCm;

    UPROPERTY(
        Config,
        EditAnywhere,
        Category="Acoustic Physics",
        meta=(ClampMin="1.0", UIMin="1.0", Units="cm", ConfigRestartRequired=true))
    float MaxDistanceCm;

    UPROPERTY(
        Config,
        EditAnywhere,
        Category="Acoustic Physics",
        meta=(ClampMin="1.0", UIMin="1.0", Units="cm/s", ConfigRestartRequired=true))
    float SpeedOfSoundCmPerSecond;

    UPROPERTY(
        Config,
        EditAnywhere,
        Category="Air Absorption",
        meta=(ClampMin="20.0", UIMin="20.0", Units="Hz", ConfigRestartRequired=true))
    float AirAbsorptionLowMidCrossoverHz;

    UPROPERTY(
        Config,
        EditAnywhere,
        Category="Air Absorption",
        meta=(ClampMin="20.0", UIMin="20.0", Units="Hz", ConfigRestartRequired=true))
    float AirAbsorptionMidHighCrossoverHz;

    FUERayTracingAudioContextSettings GetValidatedContextSettings() const;
    FVector2f GetValidatedAirAbsorptionCrossoversHz(float SampleRate) const;

    virtual FName GetCategoryName() const override;
};
