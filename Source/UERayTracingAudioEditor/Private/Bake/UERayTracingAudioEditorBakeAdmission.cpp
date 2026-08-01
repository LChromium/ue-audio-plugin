#include "Bake/UERayTracingAudioEditorBakeAdmission.h"

#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Engine/World.h"
#include "Sound/SoundWave.h"

bool FUERayTracingAudioEditorBakeAdmission::Validate(
    UWorld* World,
    UUERayTracingAudioSourceComponent* Source,
    UUERayTracingAudioListenerComponent* Listener,
    USoundWave* InputSoundWave,
    FString& OutError)
{
    OutError.Reset();
    if (!IsValid(World) || !IsValid(Source))
    {
        OutError = TEXT(
            "Select an Actor that has "
            "UERayTracingAudioSourceComponent.");
        return false;
    }
    if (!IsValid(Listener))
    {
        OutError = TEXT(
            "No UERayTracingAudioListenerComponent exists in the "
            "current editor world.");
        return false;
    }
    if (Source->GetWorld() != World
        || Listener->GetWorld() != World)
    {
        OutError = TEXT(
            "Bake Source and Listener must belong to the current "
            "editor world.");
        return false;
    }
    if (!IsValid(InputSoundWave))
    {
        return true;
    }

    TArray<uint8> ValidationPcm;
    uint32 ValidationSampleRate = 0;
    uint16 ValidationChannels = 0;
    if (!InputSoundWave->GetImportedSoundWaveData(
            ValidationPcm,
            ValidationSampleRate,
            ValidationChannels)
        || ValidationPcm.IsEmpty()
        || ValidationPcm.Num() % sizeof(int16) != 0
        || ValidationSampleRate == 0
        || ValidationChannels == 0)
    {
        OutError = TEXT(
            "The selected SoundWave has no readable imported PCM16 "
            "data; A/B bake was not started.");
        return false;
    }
    return true;
}
