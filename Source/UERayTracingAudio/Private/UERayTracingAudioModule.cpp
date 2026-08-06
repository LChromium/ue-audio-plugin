#include "UERayTracingAudioModule.h"

#include "Audio/UERayTracingAudioOcclusion.h"
#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Audio/UERayTracingAudioSpatialization.h"
#include "Features/IModularFeatures.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Modules/ModuleManager.h"
#include "Settings/UERayTracingAudioProjectSettings.h"
#if WITH_UERAYTRACINGAUDIO_VALIDATION
#include "Validation/UERayTracingAudioRuntimeValidation.h"
#endif

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
    constexpr float ConfigurationValidationSampleRate = 48000.0f;
    const UUERayTracingAudioProjectSettings* ProjectSettings =
        GetDefault<UUERayTracingAudioProjectSettings>();
    const FUERayTracingAudioContextSettings ContextSettings =
        ProjectSettings->GetValidatedContextSettings();
    AirAbsorptionCrossoversHz =
        ProjectSettings->GetValidatedAirAbsorptionCrossoversHz(
            ConfigurationValidationSampleRate);
    Manager = MakeShared<FUERayTracingAudioManager>(ContextSettings);
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("Acoustic physics on Game Thread: reference=%.2f cm maximum=%.2f cm speed=%.2f cm/s configured crossovers=%.2f/%.2f Hz (runtime audio-device Nyquist clamp applied on Initialize)."),
        ContextSettings.ReferenceDistanceCm,
        ContextSettings.MaxDistanceCm,
        ContextSettings.SpeedOfSoundCmPerSecond,
        AirAbsorptionCrossoversHz.X,
        AirAbsorptionCrossoversHz.Y);
    OcclusionPluginFactory =
        MakeUnique<FUERayTracingAudioOcclusionPluginFactory>(
            AirAbsorptionCrossoversHz);
    SpatializationPluginFactory = MakeUnique<FUERayTracingAudioSpatializationPluginFactory>();

    IModularFeatures::Get().RegisterModularFeature(FUERayTracingAudioOcclusionPluginFactory::GetModularFeatureName(), OcclusionPluginFactory.Get());
    IModularFeatures::Get().RegisterModularFeature(FUERayTracingAudioSpatializationPluginFactory::GetModularFeatureName(), SpatializationPluginFactory.Get());
#if WITH_UERAYTRACINGAUDIO_VALIDATION
    RuntimeValidation = MakeUnique<FUERayTracingAudioRuntimeValidation>();
    RuntimeValidation->Start();
#endif
    UE_LOG(LogUERayTracingAudio, Display, TEXT("UERayTracingAudio runtime module initialized."));
}

void FUERayTracingAudioModule::ShutdownModule()
{
#if WITH_UERAYTRACINGAUDIO_VALIDATION
    if (RuntimeValidation)
    {
        RuntimeValidation->Stop();
        RuntimeValidation.Reset();
    }
#endif

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

    {
        FScopeLock Lock(&AudioBridgeMutex);
        AudioBridges.Reset();
    }
    if (Manager)
    {
        Manager->GetSnapshotRegistry().Reset();
    }
    Manager.Reset();
}

void FUERayTracingAudioModule::RegisterAudioDevice(FAudioDevice* AudioDevice)
{
    (void)AudioDevice;
}

void FUERayTracingAudioModule::UnregisterAudioDevice(FAudioDevice* AudioDevice)
{
    FScopeLock Lock(&AudioBridgeMutex);
    AudioBridges.Remove(AudioDevice);
}

TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>
FUERayTracingAudioModule::GetOrCreateIndirectAudioBridge(FAudioDevice* AudioDevice)
{
    FScopeLock Lock(&AudioBridgeMutex);
    TWeakPtr<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>& WeakBridge =
        AudioBridges.FindOrAdd(AudioDevice);
    TSharedPtr<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> Bridge = WeakBridge.Pin();
    if (!Bridge.IsValid())
    {
        Bridge = MakeShared<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe>();
        WeakBridge = Bridge;
    }
    return Bridge.ToSharedRef();
}

void FUERayTracingAudioModule::PublishConvolutionTargets(
    FAudioDevice* AudioDevice,
    const uint64 AudioComponentId,
    const FUERayTracingAudioConvolutionRevisions& InRevisions,
    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        BakedLeft,
    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        BakedRight,
    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        RealtimeLeft,
    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        RealtimeRight)
{
    TSharedPtr<
        FUERayTracingAudioIndirectAudioBridge,
        ESPMode::ThreadSafe> Bridge;
    {
        FScopeLock Lock(&AudioBridgeMutex);
        if (const TWeakPtr<
                FUERayTracingAudioIndirectAudioBridge,
                ESPMode::ThreadSafe>* WeakBridge =
            AudioBridges.Find(AudioDevice))
        {
            Bridge = WeakBridge->Pin();
        }
    }
    if (!Bridge)
    {
        return;
    }

    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        EffectiveBakedRight =
            BakedRight ? BakedRight : BakedLeft;
    const FUERayTracingAudioConvolutionKernel::FKernelPtr&
        EffectiveRealtimeRight =
            RealtimeRight
                ? RealtimeRight
                : RealtimeLeft;
    FUERayTracingAudioConvolutionRevisions EffectiveRevisions =
        InRevisions;
    if (!BakedRight
        && EffectiveRevisions.BakedRight == 0)
    {
        EffectiveRevisions.BakedRight =
            EffectiveRevisions.BakedLeft;
    }
    if (!RealtimeRight
        && EffectiveRevisions.RealtimeRight == 0)
    {
        EffectiveRevisions.RealtimeRight =
            EffectiveRevisions.RealtimeLeft;
    }
    Bridge->PublishConvolutionTargets(
        AudioComponentId,
        EffectiveRevisions,
        BakedLeft,
        EffectiveBakedRight,
        RealtimeLeft,
        EffectiveRealtimeRight);
}

void FUERayTracingAudioModule::RemoveConvolutionTargets(
    const uint64 AudioComponentId)
{
    TArray<
        TSharedPtr<
            FUERayTracingAudioIndirectAudioBridge,
            ESPMode::ThreadSafe>> Bridges;
    {
        FScopeLock Lock(&AudioBridgeMutex);
        for (auto Iterator =
                AudioBridges.CreateIterator();
            Iterator;
            ++Iterator)
        {
            TSharedPtr<
                FUERayTracingAudioIndirectAudioBridge,
                ESPMode::ThreadSafe> Bridge =
                    Iterator.Value().Pin();
            if (Bridge)
            {
                Bridges.Add(MoveTemp(Bridge));
            }
            else
            {
                Iterator.RemoveCurrent();
            }
        }
    }
    for (const TSharedPtr<
            FUERayTracingAudioIndirectAudioBridge,
            ESPMode::ThreadSafe>& Bridge : Bridges)
    {
        Bridge->RemoveConvolutionTargets(
            AudioComponentId);
    }
}

void FUERayTracingAudioModule::ServiceIndirectAudioBridges()
{
    TArray<
        TSharedPtr<
            FUERayTracingAudioIndirectAudioBridge,
            ESPMode::ThreadSafe>> Bridges;
    {
        FScopeLock Lock(&AudioBridgeMutex);
        for (auto Iterator =
                AudioBridges.CreateIterator();
            Iterator;
            ++Iterator)
        {
            TSharedPtr<
                FUERayTracingAudioIndirectAudioBridge,
                ESPMode::ThreadSafe> Bridge =
                    Iterator.Value().Pin();
            if (Bridge)
            {
                Bridges.Add(MoveTemp(Bridge));
            }
            else
            {
                Iterator.RemoveCurrent();
            }
        }
    }
    for (const TSharedPtr<
            FUERayTracingAudioIndirectAudioBridge,
            ESPMode::ThreadSafe>& Bridge : Bridges)
    {
        Bridge->ServiceConvolutionGameThread();
    }
}
