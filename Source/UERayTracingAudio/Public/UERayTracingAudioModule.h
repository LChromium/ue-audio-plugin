#pragma once

#include "CoreMinimal.h"
#include "IAudioExtensionPlugin.h"
#include "Modules/ModuleManager.h"
#include "Audio/UERayTracingAudioConvolution.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUERayTracingAudio, Log, All);

class FUERayTracingAudioManager;
class FUERayTracingAudioOcclusionPluginFactory;
class FUERayTracingAudioSpatializationPluginFactory;
class FUERayTracingAudioIndirectAudioBridge;
class FUERayTracingAudioRuntimeValidation;

class UERAYTRACINGAUDIO_API FUERayTracingAudioModule : public IModuleInterface
{
public:
    FUERayTracingAudioModule();
    ~FUERayTracingAudioModule();

    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
    virtual bool SupportsDynamicReloading() override
    {
        return false;
    }

    void RegisterAudioDevice(FAudioDevice* AudioDevice);
    void UnregisterAudioDevice(FAudioDevice* AudioDevice);
    TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> GetOrCreateIndirectAudioBridge(
        FAudioDevice* AudioDevice);
    void PublishConvolutionTargets(
        FAudioDevice* AudioDevice,
        uint64 AudioComponentId,
        const FUERayTracingAudioConvolutionRevisions& Revisions,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& BakedLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& BakedRight,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& RealtimeLeft,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr& RealtimeRight);
    void RemoveConvolutionTargets(uint64 AudioComponentId);
    void ServiceIndirectAudioBridges();

    static FUERayTracingAudioModule& Get();
    static FUERayTracingAudioManager& GetManager();

private:
    TSharedPtr<FUERayTracingAudioManager> Manager;
    FCriticalSection AudioBridgeMutex;
    TMap<FAudioDevice*, TWeakPtr<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>> AudioBridges;
    FVector2f AirAbsorptionCrossoversHz =
        FVector2f(500.0f, 4000.0f);
    TUniquePtr<FUERayTracingAudioOcclusionPluginFactory> OcclusionPluginFactory;
    TUniquePtr<FUERayTracingAudioSpatializationPluginFactory> SpatializationPluginFactory;
    TUniquePtr<FUERayTracingAudioRuntimeValidation> RuntimeValidation;
};
