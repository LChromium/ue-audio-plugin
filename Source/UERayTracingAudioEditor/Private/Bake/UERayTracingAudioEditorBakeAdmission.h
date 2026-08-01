#pragma once

#include "CoreMinimal.h"

class UWorld;
class USoundWave;
class UUERayTracingAudioListenerComponent;
class UUERayTracingAudioSourceComponent;

struct FUERayTracingAudioEditorBakeAdmission
{
    static bool Validate(
        UWorld* World,
        UUERayTracingAudioSourceComponent* Source,
        UUERayTracingAudioListenerComponent* Listener,
        USoundWave* InputSoundWave,
        FString& OutError);
};
