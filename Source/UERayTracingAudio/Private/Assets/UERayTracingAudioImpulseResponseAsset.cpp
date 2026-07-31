#include "Assets/UERayTracingAudioImpulseResponseAsset.h"

void UUERayTracingAudioImpulseResponseAsset::PostLoad()
{
    Super::PostLoad();

    if (AssetVersion == 1)
    {
        // Version 1 did not persist the source/listener placement used by the bake.
        // Keep the audio loadable, but force stale detection to request a rebake.
        bHasPlacementMetadata = false;
        if (!BakeId.IsValid())
        {
            BakeId = FGuid::NewGuid();
        }
        if (BinDurationSeconds <= 0.0f && BakeSettings.SampleRate > 0)
        {
            BinDurationSeconds = 1.0f / static_cast<float>(BakeSettings.SampleRate);
        }
        AssetVersion = CurrentAssetVersion;
    }
}

void UUERayTracingAudioImpulseResponseAsset::Initialize(
    const FSoftObjectPath& InSourceWorld,
    int32 InSceneVersion,
    FString InSceneSignature,
    const FVector& InSourceLocation,
    const FVector& InListenerLocation,
    const FUERayTracingAudioBakeSettings& InBakeSettings,
    EUERayTracingAudioImpulseResponseChannelFormat InChannelFormat,
    int32 InNumChannels,
    float InBinDurationSeconds,
    TArray<float>&& InSamples)
{
    AssetVersion = CurrentAssetVersion;
    BakeId = FGuid::NewGuid();
    BakedAtUtc = FDateTime::UtcNow();
    SourceWorld = InSourceWorld;
    SceneVersion = FMath::Max(InSceneVersion, 0);
    SceneSignature = MoveTemp(InSceneSignature);
    bHasPlacementMetadata = true;
    SourceLocation = InSourceLocation;
    ListenerLocation = InListenerLocation;
    BakeSettings = InBakeSettings;
    BakeSettings.NumRays = FMath::Max(BakeSettings.NumRays, 1);
    BakeSettings.MaxBounces = FMath::Max(BakeSettings.MaxBounces, 1);
    BakeSettings.DurationSeconds = FMath::Max(BakeSettings.DurationSeconds, 0.05f);
    BakeSettings.SampleRate = FMath::Clamp(BakeSettings.SampleRate, 8000, 192000);
    ChannelFormat = InChannelFormat;
    NumChannels = FMath::Max(InNumChannels, 1);
    BinDurationSeconds = FMath::Max(InBinDurationSeconds, 0.0f);
    Samples = MoveTemp(InSamples);

    for (float& Sample : Samples)
    {
        Sample = FMath::IsFinite(Sample) ? FMath::Clamp(Sample, -1.0f, 1.0f) : 0.0f;
    }
}

bool UUERayTracingAudioImpulseResponseAsset::HasValidData() const
{
    FString Error;
    return Validate(Error);
}

int32 UUERayTracingAudioImpulseResponseAsset::GetNumFrames() const
{
    return NumChannels > 0 ? Samples.Num() / NumChannels : 0;
}

float UUERayTracingAudioImpulseResponseAsset::GetDurationSeconds() const
{
    return BakeSettings.SampleRate > 0
        ? static_cast<float>(GetNumFrames()) / static_cast<float>(BakeSettings.SampleRate)
        : 0.0f;
}

bool UUERayTracingAudioImpulseResponseAsset::Validate(FString& OutError) const
{
    if (AssetVersion <= 0 || AssetVersion > CurrentAssetVersion)
    {
        OutError = FString::Printf(TEXT("Unsupported impulse response asset version: %d."), AssetVersion);
        return false;
    }
    if (!BakeId.IsValid())
    {
        OutError = TEXT("Impulse response asset has no valid Bake ID.");
        return false;
    }
    if (SourceWorld.IsNull() || SceneSignature.IsEmpty())
    {
        OutError = TEXT("Impulse response asset is missing its source world or scene signature.");
        return false;
    }
    if (BakeSettings.SampleRate < 8000 || BakeSettings.SampleRate > 192000)
    {
        OutError = TEXT("Sample rate is outside the supported 8 kHz to 192 kHz range.");
        return false;
    }
    if (NumChannels <= 0 || Samples.IsEmpty() || Samples.Num() % NumChannels != 0)
    {
        OutError = TEXT("Impulse response samples do not match the channel layout.");
        return false;
    }
    const int32 ExpectedChannels = ChannelFormat == EUERayTracingAudioImpulseResponseChannelFormat::Mono
        ? 1
        : (ChannelFormat == EUERayTracingAudioImpulseResponseChannelFormat::Stereo ? 2 : 4);
    if (NumChannels != ExpectedChannels)
    {
        OutError = TEXT("Impulse response channel count does not match its channel format.");
        return false;
    }
    if (BinDurationSeconds <= 0.0f || !FMath::IsFinite(BinDurationSeconds))
    {
        OutError = TEXT("Impulse response bin duration is invalid.");
        return false;
    }
    for (const float Sample : Samples)
    {
        if (!FMath::IsFinite(Sample))
        {
            OutError = TEXT("Impulse response contains a non-finite sample.");
            return false;
        }
    }

    OutError.Reset();
    return true;
}
