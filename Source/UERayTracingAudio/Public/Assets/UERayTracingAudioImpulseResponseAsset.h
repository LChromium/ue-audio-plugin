#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"

#include "UERayTracingAudioImpulseResponseAsset.generated.h"

UENUM(BlueprintType)
enum class EUERayTracingAudioImpulseResponseChannelFormat : uint8
{
    Mono UMETA(DisplayName = "Mono"),
    Stereo UMETA(DisplayName = "Stereo"),
    FirstOrderAmbisonics UMETA(DisplayName = "First Order Ambisonics")
};

USTRUCT(BlueprintType)
struct UERAYTRACINGAUDIO_API FUERayTracingAudioBakeSettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Bake, meta = (ClampMin = "1", ClampMax = "1048576"))
    int32 NumRays = 4096;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Bake, meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxBounces = 8;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Bake, meta = (ClampMin = "0.05", ClampMax = "30.0"))
    float DurationSeconds = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Bake, meta = (ClampMin = "8000", ClampMax = "192000"))
    int32 SampleRate = 48000;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Bake)
    bool bRequireHardwareRayTracing = true;
};

UCLASS(BlueprintType)
class UERAYTRACINGAUDIO_API UUERayTracingAudioImpulseResponseAsset : public UObject
{
    GENERATED_BODY()

public:
    static constexpr int32 CurrentAssetVersion = 2;

    virtual void PostLoad() override;

    void Initialize(
        const FSoftObjectPath& InSourceWorld,
        int32 InSceneVersion,
        FString InSceneSignature,
        const FVector& InSourceLocation,
        const FVector& InListenerLocation,
        const FUERayTracingAudioBakeSettings& InBakeSettings,
        EUERayTracingAudioImpulseResponseChannelFormat InChannelFormat,
        int32 InNumChannels,
        float InBinDurationSeconds,
        TArray<float>&& InSamples);

    UFUNCTION(BlueprintPure, Category = "Ray Tracing Audio|Impulse Response")
    bool HasValidData() const;

    UFUNCTION(BlueprintPure, Category = "Ray Tracing Audio|Impulse Response")
    int32 GetNumFrames() const;

    UFUNCTION(BlueprintPure, Category = "Ray Tracing Audio|Impulse Response")
    float GetDurationSeconds() const;

    bool Validate(FString& OutError) const;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Metadata)
    int32 AssetVersion = CurrentAssetVersion;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Metadata)
    FGuid BakeId;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Metadata)
    FDateTime BakedAtUtc;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Scene)
    FSoftObjectPath SourceWorld;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Scene)
    int32 SceneVersion = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Scene)
    FString SceneSignature;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Scene)
    bool bHasPlacementMetadata = false;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Scene)
    FVector SourceLocation = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Scene)
    FVector ListenerLocation = FVector::ZeroVector;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Bake)
    FUERayTracingAudioBakeSettings BakeSettings;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Audio)
    EUERayTracingAudioImpulseResponseChannelFormat ChannelFormat = EUERayTracingAudioImpulseResponseChannelFormat::Mono;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Audio)
    int32 NumChannels = 1;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Audio)
    float BinDurationSeconds = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = Audio)
    TArray<float> Samples;
};
