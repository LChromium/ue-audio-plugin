#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUERayTracingAudio, Log, All);

class FUERayTracingAudioManager;
class FUERayTracingAudioOcclusionPluginFactory;
class FUERayTracingAudioSpatializationPluginFactory;

class UERAYTRACINGAUDIO_API FUERayTracingAudioModule : public IModuleInterface
{
public:
    FUERayTracingAudioModule();
    ~FUERayTracingAudioModule();

    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    void RegisterAudioDevice(FAudioDevice* AudioDevice);
    void UnregisterAudioDevice(FAudioDevice* AudioDevice);

    static FUERayTracingAudioModule& Get();
    static FUERayTracingAudioManager& GetManager();

private:
    TSharedPtr<FUERayTracingAudioManager> Manager;
    TArray<FAudioDevice*> AudioDevices;
    TUniquePtr<FUERayTracingAudioOcclusionPluginFactory> OcclusionPluginFactory;
    TUniquePtr<FUERayTracingAudioSpatializationPluginFactory> SpatializationPluginFactory;
};
