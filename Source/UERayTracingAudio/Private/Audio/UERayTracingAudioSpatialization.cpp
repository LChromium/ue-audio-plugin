#include "Audio/UERayTracingAudioSpatialization.h"

#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Settings/UERayTracingAudioSpatializationSettings.h"
#include "UERayTracingAudioModule.h"

FUERayTracingAudioSpatializationPlugin::FUERayTracingAudioSpatializationPlugin(
    TSharedRef<FUERayTracingAudioIndirectAudioBridge, ESPMode::ThreadSafe> InIndirectAudioBridge,
    FAudioDevice* InOwningDevice)
    : IndirectAudioBridge(MoveTemp(InIndirectAudioBridge))
    , OwningDevice(InOwningDevice)
{
}

void FUERayTracingAudioSpatializationPlugin::Initialize(const FAudioPluginInitializationParams InitializationParams)
{
    // UE 5.7 allocates exactly two output channels for an internal spatialization plugin;
    // the mixer maps that stereo result to the physical device layout afterwards.
    NumOutputChannels = 2;
    SampleRate = FMath::Max(static_cast<int32>(InitializationParams.SampleRate), 8000);
    Sources.Reset();
    Sources.SetNum(InitializationParams.NumSources);
    IndirectAudioBridge->Initialize(
        static_cast<int32>(InitializationParams.NumSources),
        static_cast<int32>(InitializationParams.BufferLength),
        SampleRate);
    bInitialized = true;
    bShutdown = false;
}

void FUERayTracingAudioSpatializationPlugin::Shutdown()
{
    if (bShutdown)
    {
        return;
    }
    bShutdown = true;
    bInitialized = false;
    Sources.Reset();
    if (OwningDevice)
    {
        FUERayTracingAudioModule::Get().
            UnregisterAudioDevice(OwningDevice);
        OwningDevice = nullptr;
    }
}

bool FUERayTracingAudioSpatializationPlugin::IsSpatializationEffectInitialized() const
{
    return bInitialized;
}

void FUERayTracingAudioSpatializationPlugin::OnInitSource(const uint32 SourceId, const FName& AudioComponentUserId, USpatializationPluginSourceSettingsBase* InSettings)
{
    if (!Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        return;
    }

    FUERayTracingAudioSpatializationSource& Source = Sources[SourceId];
    Source = FUERayTracingAudioSpatializationSource();
    const UUERayTracingAudioSpatializationSettings* Settings = Cast<UUERayTracingAudioSpatializationSettings>(InSettings);
    Source.bEnableSpatialization = Settings ? Settings->bEnableDirectPathSpatialization : true;
    Source.PanningStrength = Settings ? FMath::Clamp(Settings->PanningStrength, 0.0f, 1.0f) : 1.0f;
    Source.PreviousLeftGain = UE_INV_SQRT_2;
    Source.PreviousRightGain = UE_INV_SQRT_2;
}

void FUERayTracingAudioSpatializationPlugin::OnInitSource(
    const uint32 SourceId,
    const FName& AudioComponentUserId,
    const uint32 NumChannels,
    USpatializationPluginSourceSettingsBase* InSettings)
{
    OnInitSource(SourceId, AudioComponentUserId, InSettings);
    if (Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        Sources[SourceId].NumInputChannels = FMath::Max(static_cast<int32>(NumChannels), 1);
    }
}

void FUERayTracingAudioSpatializationPlugin::OnReleaseSource(const uint32 SourceId)
{
    if (Sources.IsValidIndex(static_cast<int32>(SourceId)))
    {
        Sources[SourceId] = FUERayTracingAudioSpatializationSource();
        IndirectAudioBridge->ClearSource(static_cast<int32>(SourceId));
    }
}

void FUERayTracingAudioSpatializationPlugin::CalculateStereoGains(
    const FSpatializationParams* SpatializationParams,
    const bool bEnableSpatialization,
    const float PanningStrength,
    float& OutLeftGain,
    float& OutRightGain)
{
    float Pan = 0.0f;
    if (bEnableSpatialization && SpatializationParams)
    {
        const FVector Direction = SpatializationParams->EmitterPosition.GetSafeNormal();
        const float SpatializedAmount = 1.0f - FMath::Clamp(SpatializationParams->NonSpatializedAmount, 0.0f, 1.0f);
        Pan = FMath::Clamp(Direction.Y, -1.0f, 1.0f)
            * FMath::Clamp(PanningStrength, 0.0f, 1.0f)
            * SpatializedAmount;
    }

    const float PanAngle = (Pan + 1.0f) * (PI * 0.25f);
    OutLeftGain = FMath::Cos(PanAngle);
    OutRightGain = FMath::Sin(PanAngle);
}

void FUERayTracingAudioSpatializationPlugin::ProcessAudio(const FAudioPluginSourceInputData& InputData, FAudioPluginSourceOutputData& OutputData)
{
    FUERayTracingAudioAudioDiagnostics::
        RecordHardRealtimeCallback();
    if (!InputData.AudioBuffer
        || InputData.NumChannels <= 0
        || !Sources.IsValidIndex(InputData.SourceId))
    {
        if (!OutputData.AudioBuffer.IsEmpty())
        {
            FMemory::Memzero(
                OutputData.AudioBuffer.GetData(),
                OutputData.AudioBuffer.Num() * sizeof(float));
        }
        return;
    }

    const int32 NumInputFrames = InputData.AudioBuffer->Num() / InputData.NumChannels;
    const int32 NumOutputFrames = OutputData.AudioBuffer.Num() / NumOutputChannels;
    const int32 NumFrames = FMath::Min(NumInputFrames, NumOutputFrames);
    if (NumFrames <= 0)
    {
        return;
    }

    FUERayTracingAudioSpatializationSource& Source = Sources[InputData.SourceId];
    TArrayView<const FVector2f> StereoWetFrames;
    bool bHasDataSource = false;
    EUERayTracingAudioRuntimeDataSource DataSource =
        EUERayTracingAudioRuntimeDataSource::Realtime;
    IndirectAudioBridge->Consume(
        InputData.SourceId,
        InputData.AudioComponentId,
        StereoWetFrames,
        bHasDataSource,
        DataSource);
    if (Source.NumInputChannels != InputData.NumChannels)
    {
        Source.NumInputChannels = InputData.NumChannels;
        Source.PreviousLeftGain = UE_INV_SQRT_2;
        Source.PreviousRightGain = UE_INV_SQRT_2;
    }
    float TargetLeftGain = UE_INV_SQRT_2;
    float TargetRightGain = UE_INV_SQRT_2;
    CalculateStereoGains(
        InputData.SpatializationParams,
        Source.bEnableSpatialization,
        Source.PanningStrength,
        TargetLeftGain,
        TargetRightGain);

    for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
    {
        const int32 InputSampleIndex = FrameIndex * InputData.NumChannels;
        const int32 OutputSampleIndex = FrameIndex * NumOutputChannels;
        const bool bPreserveNonSpatializedLayout = !Source.bEnableSpatialization
            && InputData.NumChannels > 1;
        if (bPreserveNonSpatializedLayout)
        {
            for (int32 OutputChannelIndex = 0; OutputChannelIndex < NumOutputChannels; ++OutputChannelIndex)
            {
                OutputData.AudioBuffer[OutputSampleIndex + OutputChannelIndex] =
                    OutputChannelIndex < InputData.NumChannels
                        ? (*InputData.AudioBuffer)[InputSampleIndex + OutputChannelIndex]
                        : 0.0f;
            }
            continue;
        }

        float MonoSample = 0.0f;
        for (int32 ChannelIndex = 0; ChannelIndex < InputData.NumChannels; ++ChannelIndex)
        {
            MonoSample += (*InputData.AudioBuffer)[InputSampleIndex + ChannelIndex];
        }
        MonoSample /= static_cast<float>(InputData.NumChannels);
        const FVector2f IndirectWet = StereoWetFrames.IsValidIndex(FrameIndex)
            ? StereoWetFrames[FrameIndex]
            : FVector2f::ZeroVector;
        const float CollapsedIndirectWet = 0.5f * (IndirectWet.X + IndirectWet.Y);
        const float DirectMonoSample = MonoSample - CollapsedIndirectWet;

        const float Alpha = static_cast<float>(FrameIndex + 1) / static_cast<float>(NumFrames);
        const float LeftGain = FMath::Lerp(Source.PreviousLeftGain, TargetLeftGain, Alpha);
        const float RightGain = FMath::Lerp(Source.PreviousRightGain, TargetRightGain, Alpha);
        if (NumOutputChannels == 1)
        {
            OutputData.AudioBuffer[OutputSampleIndex] = DirectMonoSample + CollapsedIndirectWet;
        }
        else
        {
            OutputData.AudioBuffer[OutputSampleIndex] = (DirectMonoSample * LeftGain) + IndirectWet.X;
            OutputData.AudioBuffer[OutputSampleIndex + 1] = (DirectMonoSample * RightGain) + IndirectWet.Y;
        }
    }

    const int32 NumProcessedSamples = NumFrames * NumOutputChannels;
    for (int32 SampleIndex = NumProcessedSamples;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        OutputData.AudioBuffer[SampleIndex] = 0.0f;
    }

    float PeakAbsoluteOutput = 0.0f;
    uint64 OverUnitOutputSampleCount = 0;
    uint64 NonFiniteOutputSampleCount = 0;
    for (int32 SampleIndex = 0;
        SampleIndex < NumProcessedSamples;
        ++SampleIndex)
    {
        float& OutputSample = OutputData.AudioBuffer[SampleIndex];
        if (!FMath::IsFinite(OutputSample))
        {
            OutputSample = 0.0f;
            ++NonFiniteOutputSampleCount;
            continue;
        }

        const float AbsoluteOutput = FMath::Abs(OutputSample);
        PeakAbsoluteOutput = FMath::Max(
            PeakAbsoluteOutput,
            AbsoluteOutput);
        if (AbsoluteOutput > 1.0f)
        {
            ++OverUnitOutputSampleCount;
        }
    }
    if (bHasDataSource)
    {
        FUERayTracingAudioAudioDiagnostics::RecordFinalOutput(
            DataSource,
            InputData.AudioComponentId,
            PeakAbsoluteOutput,
            OverUnitOutputSampleCount,
            NonFiniteOutputSampleCount);
    }

    Source.PreviousLeftGain = TargetLeftGain;
    Source.PreviousRightGain = TargetRightGain;
}

FString FUERayTracingAudioSpatializationPluginFactory::GetDisplayName()
{
    return TEXT("UE Ray Tracing Audio Spatialization");
}

bool FUERayTracingAudioSpatializationPluginFactory::SupportsPlatform(const FString& PlatformName)
{
    return PlatformName == TEXT("Windows") || PlatformName == TEXT("Linux") || PlatformName == TEXT("Mac");
}

UClass* FUERayTracingAudioSpatializationPluginFactory::GetCustomSpatializationSettingsClass() const
{
    return UUERayTracingAudioSpatializationSettings::StaticClass();
}

TAudioSpatializationPtr FUERayTracingAudioSpatializationPluginFactory::CreateNewSpatializationPlugin(FAudioDevice* OwningDevice)
{
    FUERayTracingAudioModule::Get().RegisterAudioDevice(OwningDevice);
    FUERayTracingAudioModule& Module = FUERayTracingAudioModule::Get();
    return TAudioSpatializationPtr(new FUERayTracingAudioSpatializationPlugin(
        Module.GetOrCreateIndirectAudioBridge(OwningDevice),
        OwningDevice));
}
