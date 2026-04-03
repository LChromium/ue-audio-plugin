#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"
#include "UERayTracingAudioSpatializationSettings.generated.h"

UCLASS()
class UERAYTRACINGAUDIO_API UUERayTracingAudioSpatializationSettings : public USpatializationPluginSourceSettingsBase
{
    GENERATED_BODY()

public:
    UUERayTracingAudioSpatializationSettings();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Spatialization)
    bool bEnableDirectPathSpatialization;
};
