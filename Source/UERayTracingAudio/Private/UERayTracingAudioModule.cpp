#include "UERayTracingAudioModule.h"

#include "Audio/UERayTracingAudioOcclusion.h"
#include "Audio/UERayTracingAudioSpatialization.h"
#include "Features/IModularFeatures.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogUERayTracingAudio);

IMPLEMENT_MODULE(FUERayTracingAudioModule, UERayTracingAudio)

FUERayTracingAudioModule::FUERayTracingAudioModule() = default;

FUERayTracingAudioModule::~FUERayTracingAudioModule() = default;

FUERayTracingAudioModule& FUERayTracingAudioModule::Get()
{
    return FModuleManager::GetModuleChecked<FUERayTracingAudioModule>("UERayTracingAudio");
}

FUERayTracingAudioManager& FUERayTracingAudioModule::GetManager()
{
    return *Get().Manager;
}

void FUERayTracingAudioModule::StartupModule()
{
    Manager = MakeShared<FUERayTracingAudioManager>();
    OcclusionPluginFactory = MakeUnique<FUERayTracingAudioOcclusionPluginFactory>();
    SpatializationPluginFactory = MakeUnique<FUERayTracingAudioSpatializationPluginFactory>();

    IModularFeatures::Get().RegisterModularFeature(FUERayTracingAudioOcclusionPluginFactory::GetModularFeatureName(), OcclusionPluginFactory.Get());
    IModularFeatures::Get().RegisterModularFeature(FUERayTracingAudioSpatializationPluginFactory::GetModularFeatureName(), SpatializationPluginFactory.Get());
}

void FUERayTracingAudioModule::ShutdownModule()
{
    if (SpatializationPluginFactory)
    {
        IModularFeatures::Get().UnregisterModularFeature(FUERayTracingAudioSpatializationPluginFactory::GetModularFeatureName(), SpatializationPluginFactory.Get());
        SpatializationPluginFactory.Reset();
    }

    if (OcclusionPluginFactory)
    {
        IModularFeatures::Get().UnregisterModularFeature(FUERayTracingAudioOcclusionPluginFactory::GetModularFeatureName(), OcclusionPluginFactory.Get());
        OcclusionPluginFactory.Reset();
    }

    AudioDevices.Reset();
    Manager.Reset();
}

void FUERayTracingAudioModule::RegisterAudioDevice(FAudioDevice* AudioDevice)
{
    if (!AudioDevices.Contains(AudioDevice))
    {
        AudioDevices.Add(AudioDevice);
    }
}

void FUERayTracingAudioModule::UnregisterAudioDevice(FAudioDevice* AudioDevice)
{
    AudioDevices.Remove(AudioDevice);
}
