#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"
#include "UERayTracingAudioOcclusionSettings.generated.h"

UCLASS()
class UERAYTRACINGAUDIO_API UUERayTracingAudioOcclusionSettings : public UOcclusionPluginSourceSettingsBase
{
    GENERATED_BODY()

public:
    UUERayTracingAudioOcclusionSettings();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound)
    bool bApplyDistanceAttenuation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound)
    bool bApplyAirAbsorption;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = DirectSound)
    bool bApplyOcclusion;
};
