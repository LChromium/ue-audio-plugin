#include "Validation/UERayTracingAudioRuntimeValidation.h"

#if WITH_UERAYTRACINGAUDIO_VALIDATION

#include "Validation/UERayTracingAudioValidationSoundWave.h"

#include "Assets/UERayTracingAudioImpulseResponseAsset.h"
#include "Audio.h"
#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "Bake/UERayTracingAudioBakeJob.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Settings/UERayTracingAudioOcclusionSettings.h"
#include "Settings/UERayTracingAudioSpatializationSettings.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundWaveProcedural.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UERayTracingAudioModule.h"

class FUERayTracingAudioValidationSourceBufferListener final
    : public ISourceBufferListener
{
public:
    virtual void OnNewBuffer(const FOnNewBufferParams& Params) override
    {
        ++BufferCount;
        float PeakAbsoluteSample = 0.0f;
        for (int32 SampleIndex = 0;
            Params.AudioData && SampleIndex < Params.NumSamples;
            ++SampleIndex)
        {
            if (FMath::IsFinite(Params.AudioData[SampleIndex]))
            {
                PeakAbsoluteSample = FMath::Max(
                    PeakAbsoluteSample,
                    FMath::Abs(Params.AudioData[SampleIndex]));
            }
        }
        if (PeakAbsoluteSample > 1.0e-8f)
        {
            ++NonSilentBufferCount;
        }
    }

    virtual void OnSourceReleased(const int32) override
    {
    }

    uint64 GetBufferCount() const { return BufferCount.Load(); }
    uint64 GetNonSilentBufferCount() const
    {
        return NonSilentBufferCount.Load();
    }

private:
    TAtomic<uint64> BufferCount { 0 };
    TAtomic<uint64> NonSilentBufferCount { 0 };
};

namespace
{
    constexpr int32 ValidationSampleRate = 48000;
    constexpr int32 ValidationToneSeconds = 30;
    constexpr int32 ValidationPrebufferSeconds = 4;
    constexpr int32 ValidationBakeSampleRate = 8000;
    constexpr int32 ValidationBakeRays = 1024;
    constexpr int32 ValidationBakeBounces = 4;
    constexpr float ValidationBakeDurationSeconds = 0.25f;
    constexpr double ValidationBakeRelativeTolerance = 0.05;
    constexpr int32 ValidationToneCycleSamples = 1024;
    constexpr double ValidationDataSourceTimeoutSeconds = 15.0;
    constexpr double ValidationDataSourceSettleSeconds = 0.25;
    constexpr double ValidationAcousticStartupTimeoutSeconds = 30.0;
    constexpr double ValidationABLoopObservationSeconds = 17.0;
    constexpr uint64 ValidationDataSourceMinimumBufferCount = 24;
    constexpr uint64 ValidationMinimumWetPresenceNumerator = 4;
    constexpr uint64 ValidationMinimumWetPresenceDenominator = 5;
    constexpr float ValidationMinimumWetToInputRmsRatio =
        FUERayTracingAudioAudioDiagnostics::AudibleWetToInputRmsRatio;
    constexpr float ValidationPrimaryVolume = 0.4f;
    constexpr float ValidationActiveFadeLevel = 1.0f;
    // Keep an inaudible non-zero fader level so both decoders stay active and
    // sample position remains continuous across A/B switches.
    constexpr float ValidationInactiveFadeLevel = 0.00025f;
    constexpr double ValidationDirectSweepClearHoldSeconds = 0.5;
    constexpr double ValidationDirectSweepTraversalSeconds = 3.0;
    constexpr double ValidationDirectSweepOccludedHoldSeconds = 0.5;
    constexpr double ValidationDirectSweepReturnHoldSeconds = 0.5;
    constexpr double ValidationDirectSweepTimeoutSeconds = 15.0;
    constexpr double ValidationDirectSweepRestoreTimeoutSeconds = 5.0;
    constexpr float ValidationDirectSweepSoftOccludedGain = 0.35f;

    const TCHAR* GetDirectSweepPhaseName(
        const EUERayTracingAudioDirectSweepPhase Phase)
    {
        switch (Phase)
        {
        case EUERayTracingAudioDirectSweepPhase::ClearHold:
            return TEXT("CLEAR");
        case EUERayTracingAudioDirectSweepPhase::EnteringWall:
            return TEXT("ENTERING WALL");
        case EUERayTracingAudioDirectSweepPhase::OccludedHold:
            return TEXT("OCCLUDED");
        case EUERayTracingAudioDirectSweepPhase::Returning:
            return TEXT("RETURNING");
        case EUERayTracingAudioDirectSweepPhase::Restoring:
            return TEXT("RESTORING");
        case EUERayTracingAudioDirectSweepPhase::Complete:
            return TEXT("COMPLETE");
        case EUERayTracingAudioDirectSweepPhase::Failed:
            return TEXT("FAILED");
        default:
            return TEXT("IDLE");
        }
    }

    FString GetValidationDirectPreset()
    {
        FString DirectPreset = TEXT("soft_occluded");
        FParse::Value(
            FCommandLine::Get(),
            TEXT("UERayTracingAudioValidationDirectPreset="),
            DirectPreset);
        DirectPreset.ToLowerInline();
        if (DirectPreset == TEXT("soft"))
        {
            return TEXT("soft_occluded");
        }
        if (DirectPreset == TEXT("hard"))
        {
            return TEXT("hard_occluded");
        }
        if (DirectPreset == TEXT("clear")
            || DirectPreset == TEXT("soft_occluded")
            || DirectPreset == TEXT("hard_occluded"))
        {
            return DirectPreset;
        }

        UE_LOG(
            LogUERayTracingAudio,
            Warning,
            TEXT("Unknown validation direct preset '%s'; using soft_occluded."),
            *DirectPreset);
        return TEXT("soft_occluded");
    }

    const TCHAR* GetDataSourceName(const EUERayTracingAudioIndirectDataSource DataSource)
    {
        switch (DataSource)
        {
        case EUERayTracingAudioIndirectDataSource::Baked:
            return TEXT("BAKED IR");
        case EUERayTracingAudioIndirectDataSource::Hybrid:
            return TEXT("HYBRID IR");
        default:
            return TEXT("REALTIME IR");
        }
    }

    const TCHAR* GetBakedAssetStatusName(const EUERayTracingAudioBakedAssetStatus Status)
    {
        switch (Status)
        {
        case EUERayTracingAudioBakedAssetStatus::Ready:
            return TEXT("READY");
        case EUERayTracingAudioBakedAssetStatus::StaleAllowed:
            return TEXT("STALE ALLOWED");
        case EUERayTracingAudioBakedAssetStatus::StalePlacement:
            return TEXT("STALE PLACEMENT");
        case EUERayTracingAudioBakedAssetStatus::StaleScene:
            return TEXT("STALE SCENE");
        case EUERayTracingAudioBakedAssetStatus::MissingAsset:
            return TEXT("MISSING");
        case EUERayTracingAudioBakedAssetStatus::NotRequired:
            return TEXT("NOT REQUIRED");
        default:
            return TEXT("INVALID");
        }
    }

    FUERayTracingAudioBakeSettings MakeValidationBakeSettings()
    {
        FUERayTracingAudioBakeSettings Settings;
        Settings.NumRays = ValidationBakeRays;
        Settings.MaxBounces = ValidationBakeBounces;
        Settings.DurationSeconds = ValidationBakeDurationSeconds;
        Settings.SampleRate = ValidationBakeSampleRate;
        Settings.bRequireHardwareRayTracing = true;
        return Settings;
    }

    double CalculateBakeEnergy(const TArray<float>& Samples)
    {
        double Energy = 0.0;
        for (const float Sample : Samples)
        {
            Energy += static_cast<double>(Sample) * static_cast<double>(Sample);
        }
        return Energy;
    }

    bool AreBakeSamplesFinite(const TArray<float>& Samples)
    {
        for (const float Sample : Samples)
        {
            if (!FMath::IsFinite(Sample))
            {
                return false;
            }
        }
        return true;
    }

    AActor* SpawnValidationActor(UWorld& World, const TCHAR* BaseName, const FVector& Location)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
        if (!Actor)
        {
            return nullptr;
        }

        Actor->Tags.AddUnique(FName(BaseName));

        USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("ValidationRoot"));
        Actor->AddInstanceComponent(Root);
        Actor->SetRootComponent(Root);
        Root->RegisterComponent();
        Actor->SetActorLocation(Location);
        return Actor;
    }

    UUERayTracingAudioGeometryComponent* AddBoxGeometry(
        UWorld& World,
        UStaticMesh& CubeMesh,
        const TCHAR* Name,
        const FVector& Location,
        const FVector& Scale,
        const FVector& Absorption,
        const float Scattering)
    {
        AActor* Actor = SpawnValidationActor(World, Name, Location);
        if (!Actor)
        {
            return nullptr;
        }

        UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Actor, TEXT("ValidationMesh"));
        Actor->AddInstanceComponent(Mesh);
        Mesh->SetupAttachment(Actor->GetRootComponent());
        Mesh->SetStaticMesh(&CubeMesh);
        Mesh->SetRelativeScale3D(Scale);
        Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        Mesh->SetCollisionObjectType(ECC_WorldStatic);
        Mesh->SetCollisionResponseToAllChannels(ECR_Block);
        Mesh->SetVisibility(true, true);
        Mesh->RegisterComponent();

        UUERayTracingAudioGeometryComponent* Geometry =
            NewObject<UUERayTracingAudioGeometryComponent>(Actor, TEXT("ValidationAcousticGeometry"));
        Actor->AddInstanceComponent(Geometry);
        Geometry->ExportMode = EUERayTracingAudioGeometryExportMode::BoundingBox;
        Geometry->Absorption = Absorption;
        Geometry->Transmission = FVector::ZeroVector;
        Geometry->Scattering = Scattering;
        Geometry->RegisterComponent();
        return Geometry;
    }

    UStaticMeshComponent* AddVisualMarker(
        AActor& Actor,
        UStaticMesh& MarkerMesh,
        UMaterialInterface& MarkerMaterial,
        const TCHAR* ComponentName,
        const FVector& Scale,
        const FLinearColor& Color)
    {
        UStaticMeshComponent* Marker = NewObject<UStaticMeshComponent>(&Actor, ComponentName);
        if (!Marker)
        {
            return nullptr;
        }
        Actor.AddInstanceComponent(Marker);
        Marker->SetupAttachment(Actor.GetRootComponent());
        Marker->SetStaticMesh(&MarkerMesh);
        UMaterialInstanceDynamic* DynamicMaterial =
            UMaterialInstanceDynamic::Create(&MarkerMaterial, Marker);
        if (DynamicMaterial)
        {
            DynamicMaterial->SetVectorParameterValue(TEXT("Color"), Color);
            Marker->SetMaterial(0, DynamicMaterial);
        }
        Marker->SetRelativeScale3D(Scale);
        Marker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Marker->SetVisibility(true, true);
        Marker->RegisterComponent();
        return Marker;
    }

    void SpawnValidationLight(
        UWorld& World,
        const FVector& Location,
        const FLinearColor& Color,
        float Intensity,
        float Radius)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        APointLight* Light = World.SpawnActor<APointLight>(
            APointLight::StaticClass(),
            FTransform(FRotator::ZeroRotator, Location),
            SpawnParameters);
        if (!Light || !Light->PointLightComponent)
        {
            return;
        }

        Light->Tags.AddUnique(FName(TEXT("VRTA_ValidationLight")));
        Light->PointLightComponent->SetIntensity(Intensity);
        Light->PointLightComponent->SetAttenuationRadius(Radius);
        Light->PointLightComponent->SetLightColor(Color);
        Light->PointLightComponent->SetCastShadows(true);
    }

    void InitializeValidationTone(UUERayTracingAudioValidationSoundWave& Tone)
    {
        TArray<float> Samples;
        Samples.SetNumUninitialized(ValidationSampleRate * ValidationToneSeconds);
        constexpr int32 BurstLengthSamples = 256;
        constexpr double Amplitude = 0.95;
        for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
        {
            const int32 BurstSampleIndex = SampleIndex % ValidationToneCycleSamples;
            if (BurstSampleIndex >= BurstLengthSamples)
            {
                Samples[SampleIndex] = 0;
                continue;
            }

            const double TimeSeconds = static_cast<double>(BurstSampleIndex)
                / static_cast<double>(ValidationSampleRate);
            const double BurstAlpha = static_cast<double>(BurstSampleIndex)
                / static_cast<double>(BurstLengthSamples - 1);
            const double Envelope = FMath::Square(FMath::Sin(PI * BurstAlpha));
            const double Signal = (
                FMath::Sin(2.0 * PI * 220.0 * TimeSeconds)
                + FMath::Sin(2.0 * PI * 660.0 * TimeSeconds)
                + FMath::Sin(2.0 * PI * 1320.0 * TimeSeconds)) / 3.0;
            Samples[SampleIndex] = static_cast<float>(Amplitude * Envelope * Signal);
        }
        Tone.InitializeSamples(
            MoveTemp(Samples),
            ValidationSampleRate * ValidationPrebufferSeconds);
    }

    UUERayTracingAudioValidationSoundWave* BuildValidationTone(UObject& Outer)
    {
        UUERayTracingAudioValidationSoundWave* Tone =
            NewObject<UUERayTracingAudioValidationSoundWave>(&Outer);
        Tone->NumChannels = 1;
        Tone->SetSampleRate(ValidationSampleRate);
        Tone->Duration = INDEFINITELY_LOOPING_DURATION;
        Tone->bLooping = true;
        Tone->bEnableBaseSubmix = true;
        InitializeValidationTone(*Tone);
        return Tone;
    }

    bool InspectImportedSound(
        const USoundWave& ImportedSound,
        int32& OutSampleRate,
        int32& OutNumChannels,
        int32& OutNumFrames,
        float& OutPeakAbsoluteSample)
    {
        OutSampleRate = 0;
        OutNumChannels = 0;
        OutNumFrames = 0;
        OutPeakAbsoluteSample = 0.0f;

#if WITH_EDITOR
        TArray<uint8> ImportedPcm;
        uint32 ImportedSampleRate = 0;
        uint16 ImportedNumChannels = 0;
        if (!ImportedSound.GetImportedSoundWaveData(
                ImportedPcm,
                ImportedSampleRate,
                ImportedNumChannels)
            || ImportedSampleRate == 0
            || ImportedNumChannels == 0
            || ImportedPcm.Num() < static_cast<int32>(sizeof(int16)))
        {
            return false;
        }

        const int32 ImportedSampleCount = ImportedPcm.Num() / sizeof(int16);
        const int32 AlignedSampleCount =
            ImportedSampleCount - (ImportedSampleCount % ImportedNumChannels);
        if (AlignedSampleCount <= 0)
        {
            return false;
        }

        const int16* ImportedSamples =
            reinterpret_cast<const int16*>(ImportedPcm.GetData());
        const int32 ImportedFrameCount =
            AlignedSampleCount / ImportedNumChannels;
        for (int32 FrameIndex = 0; FrameIndex < ImportedFrameCount; ++FrameIndex)
        {
            const float Sample = static_cast<float>(
                ImportedSamples[FrameIndex * ImportedNumChannels]) / 32768.0f;
            OutPeakAbsoluteSample = FMath::Max(OutPeakAbsoluteSample, FMath::Abs(Sample));
        }
        if (OutPeakAbsoluteSample <= 1.0e-8f)
        {
            return false;
        }

        OutSampleRate = static_cast<int32>(ImportedSampleRate);
        OutNumChannels = ImportedNumChannels;
        OutNumFrames = ImportedFrameCount;
        return true;
#else
        return false;
#endif
    }

    UAudioComponent* AddValidationAudio(
        AActor& SourceActor,
        USoundBase& Sound,
        const float VolumeMultiplier)
    {
        UAudioComponent* Audio = NewObject<UAudioComponent>(&SourceActor);
        SourceActor.AddInstanceComponent(Audio);
        Audio->SetupAttachment(SourceActor.GetRootComponent());
        Audio->bAutoActivate = false;
        Audio->bAutoDestroy = false;
        Audio->bAllowSpatialization = true;
        Audio->bOverrideAttenuation = true;
        Audio->bAlwaysPlay = true;
        Audio->bShouldRemainActiveIfDropped = true;
        Audio->bIsUISound = false;
        Audio->bOverridePriority = true;
        Audio->Priority = 100.0f;
        Audio->SetVolumeMultiplier(VolumeMultiplier);

        FSoundAttenuationSettings Attenuation;
        Attenuation.bAttenuate = false;
        Attenuation.bSpatialize = true;
        Attenuation.bEnableOcclusion = true;
        Attenuation.bEnableReverbSend = false;
        Attenuation.bEnableSendToAudioLink = false;
        Attenuation.PluginSettings.SpatializationPluginSettingsArray.Add(
            NewObject<UUERayTracingAudioSpatializationSettings>(Audio));
        Attenuation.PluginSettings.OcclusionPluginSettingsArray.Add(
            NewObject<UUERayTracingAudioOcclusionSettings>(Audio));
        Audio->SetAttenuationOverrides(Attenuation);
        Audio->SetSound(&Sound);
        Audio->RegisterComponent();
        return Audio;
    }

    UAudioComponent* AddValidationReferenceAudio(
        AActor& Owner,
        USoundBase& Sound)
    {
        UAudioComponent* Audio = NewObject<UAudioComponent>(&Owner);
        Owner.AddInstanceComponent(Audio);
        Audio->SetupAttachment(Owner.GetRootComponent());
        Audio->bAutoActivate = false;
        Audio->bAutoDestroy = false;
        Audio->bAllowSpatialization = false;
        Audio->bOverrideAttenuation = true;
        Audio->bAlwaysPlay = true;
        Audio->bShouldRemainActiveIfDropped = true;
        Audio->bIsUISound = false;
        Audio->bOverridePriority = true;
        Audio->Priority = 100.0f;
        // Keep the rendered and reference paths at the same base level.
        // SetRenderedABMode owns the crossfade; multiplying a second 0.4/0.0001
        // level here made the reference path effectively inaudible.
        Audio->SetVolumeMultiplier(ValidationPrimaryVolume);

        FSoundAttenuationSettings Attenuation;
        Attenuation.bAttenuate = false;
        Attenuation.bSpatialize = false;
        Attenuation.bEnableOcclusion = false;
        Attenuation.bEnableReverbSend = false;
        Attenuation.bEnableSendToAudioLink = false;
        Audio->SetAttenuationOverrides(Attenuation);
        Audio->SetSound(&Sound);
        Audio->RegisterComponent();
        return Audio;
    }
}

void FUERayTracingAudioRuntimeValidation::Start()
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("UERayTracingAudioValidationScenario")))
    {
        return;
    }

    bValidationEnabled = true;
    FUERayTracingAudioAudioDiagnostics::ResetHardRealtime();
    WorldInitializationHandle = FWorldDelegates::OnPostWorldInitialization.AddLambda(
        [this](UWorld* World, const UWorld::InitializationValues)
        {
            CreateScenario(World);
        });
    WorldBeginTearDownHandle =
        FWorldDelegates::OnWorldBeginTearDown.AddRaw(
            this,
            &FUERayTracingAudioRuntimeValidation::
                HandleWorldBeginTearDown);
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FUERayTracingAudioRuntimeValidation::Tick));
    UE_LOG(LogUERayTracingAudio, Display, TEXT("UERayTracingAudio validation harness enabled."));
}

void FUERayTracingAudioRuntimeValidation::Stop()
{
    if (!bValidationEnabled)
    {
        return;
    }

    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
    if (WorldInitializationHandle.IsValid())
    {
        FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitializationHandle);
        WorldInitializationHandle.Reset();
    }
    if (WorldBeginTearDownHandle.IsValid())
    {
        FWorldDelegates::OnWorldBeginTearDown.Remove(
            WorldBeginTearDownHandle);
        WorldBeginTearDownHandle.Reset();
    }

    for (FScenarioState& State : Scenarios)
    {
        if (IsDirectSweepActive(State))
        {
            AbortDirectSweepImmediately(
                State,
                TEXT("validation stopped"));
        }
        if (UWorld* World = State.World.Get();
            IsValid(World)
            && State.ActorDestroyedHandle.IsValid())
        {
            World->RemoveOnActorDestroyedHandler(
                State.ActorDestroyedHandle);
            State.ActorDestroyedHandle.Reset();
        }
        if (State.BakeJob.IsValid())
        {
            State.BakeJob->Cancel();
            State.BakeJob.Reset();
        }
        if (State.DataSourceBakeJob.IsValid())
        {
            State.DataSourceBakeJob->Cancel();
            State.DataSourceBakeJob.Reset();
        }
    }

    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(0);
    ActiveDirectSweepWorld.Reset();
    InitializedWorlds.Reset();
    Scenarios.Reset();
    bValidationOwnerAssigned = false;
    bValidationEnabled = false;
}

bool FUERayTracingAudioRuntimeValidation::
    ClaimValidationOwnership()
{
    if (bValidationOwnerAssigned)
    {
        return false;
    }
    bValidationOwnerAssigned = true;
    return true;
}

void FUERayTracingAudioRuntimeValidation::CreateScenario(UWorld* World)
{
    if (!IsValid(World)
        || !World->IsGameWorld()
        || World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/"))
        || InitializedWorlds.Contains(World))
    {
        return;
    }
    InitializedWorlds.Add(World);
    const FString DirectPreset = GetValidationDirectPreset();
    const bool bClearDirectPreset = DirectPreset == TEXT("clear");
    const bool bHardDirectPreset =
        DirectPreset == TEXT("hard_occluded");
    const FVector ListenerLocation(-100.0, 0.0, 180.0);
    const FVector PrimarySourceLocation = bClearDirectPreset
        ? FVector(-100.0, 200.0, 180.0)
        : FVector(100.0, 0.0, 180.0);

    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    UMaterialInterface* MarkerMaterial = LoadObject<UMaterialInterface>(
        nullptr,
        TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!IsValid(CubeMesh) || !IsValid(SphereMesh) || !IsValid(MarkerMaterial))
    {
        UE_LOG(
            LogUERayTracingAudio,
            Error,
            TEXT("UERayTracingAudio validation scenario could not load its engine meshes or marker material."));
        return;
    }

    struct FBoxDefinition
    {
        const TCHAR* Name;
        FVector Location;
        FVector Scale;
        FVector Absorption;
        float Scattering;
    };
    const FBoxDefinition Boxes[] =
    {
        { TEXT("VRTA_Floor"), FVector(0.0, 0.0, 0.0), FVector(10.0, 10.0, 0.2), FVector(0.15, 0.2, 0.25), 0.3f },
        { TEXT("VRTA_Ceiling"), FVector(0.0, 0.0, 400.0), FVector(10.0, 10.0, 0.2), FVector(0.35, 0.45, 0.55), 0.6f },
        { TEXT("VRTA_WallNorth"), FVector(0.0, 500.0, 200.0), FVector(10.0, 0.2, 4.0), FVector(0.2, 0.3, 0.4), 0.4f },
        { TEXT("VRTA_WallSouth"), FVector(0.0, -500.0, 200.0), FVector(10.0, 0.2, 4.0), FVector(0.2, 0.3, 0.4), 0.4f },
        { TEXT("VRTA_WallEast"), FVector(500.0, 0.0, 200.0), FVector(0.2, 10.0, 4.0), FVector(0.12, 0.18, 0.24), 0.25f },
        { TEXT("VRTA_WallWest"), FVector(-500.0, 0.0, 200.0), FVector(0.2, 10.0, 4.0), FVector(0.12, 0.18, 0.24), 0.25f },
        { TEXT("VRTA_OcclusionWall"), FVector(0.0, 0.0, 150.0), FVector(0.3, 4.0, 3.0), FVector(0.08, 0.12, 0.2), 0.2f }
    };

    int32 GeometryCount = 0;
    for (const FBoxDefinition& Box : Boxes)
    {
        GeometryCount += AddBoxGeometry(
            *World,
            *CubeMesh,
            Box.Name,
            Box.Location,
            Box.Scale,
            Box.Absorption,
            Box.Scattering) != nullptr ? 1 : 0;
    }

    const FVector ValidationCameraLocation(-420.0, -420.0, 260.0);
    const FVector ValidationCameraTarget(40.0, 0.0, 145.0);
    FActorSpawnParameters CameraSpawnParameters;
    CameraSpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ACameraActor* CameraActor = World->SpawnActor<ACameraActor>(
        ACameraActor::StaticClass(),
        FTransform(
            (ValidationCameraTarget - ValidationCameraLocation).Rotation(),
            ValidationCameraLocation),
        CameraSpawnParameters);
    if (!CameraActor || !CameraActor->GetCameraComponent())
    {
        UE_LOG(LogUERayTracingAudio, Error, TEXT("UERayTracingAudio validation scenario could not spawn its visible-scene camera."));
        return;
    }
    CameraActor->Tags.AddUnique(FName(TEXT("VRTA_ValidationCamera")));
    CameraActor->GetCameraComponent()->SetFieldOfView(82.0f);

    SpawnValidationLight(
        *World,
        FVector(-160.0, -120.0, 330.0),
        FLinearColor(1.0f, 0.82f, 0.58f),
        12000.0f,
        1400.0f);

    AActor* ListenerActor = SpawnValidationActor(
        *World,
        TEXT("VRTA_Listener"),
        ListenerLocation);
    if (!ListenerActor)
    {
        UE_LOG(LogUERayTracingAudio, Error, TEXT("UERayTracingAudio validation scenario could not spawn the listener actor."));
        return;
    }

    UUERayTracingAudioListenerComponent* Listener =
        NewObject<UUERayTracingAudioListenerComponent>(ListenerActor, TEXT("ValidationListener"));
    ListenerActor->AddInstanceComponent(Listener);
    Listener->RegisterComponent();
    UStaticMeshComponent* ListenerMarker = AddVisualMarker(
        *ListenerActor,
        *SphereMesh,
        *MarkerMaterial,
        TEXT("ValidationListenerMarker"),
        FVector(0.32f),
        FLinearColor(0.02f, 0.2f, 1.0f));
    SpawnValidationLight(
        *World,
        ListenerActor->GetActorLocation() + FVector(0.0, 0.0, 35.0),
        FLinearColor(0.1f, 0.45f, 1.0f),
        1800.0f,
        320.0f);

    int32 RequestedSourceCount = 4;
    FParse::Value(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioValidationSourceCount="),
        RequestedSourceCount);
    RequestedSourceCount = FMath::Clamp(RequestedSourceCount, 2, 32);
    IConsoleVariable* DirectProceduralRendering =
        IConsoleManager::Get().FindConsoleVariable(
            TEXT("au.DirectProceduralRendering"));
    IConsoleVariable* PerSourceResampling =
        IConsoleManager::Get().FindConsoleVariable(
            TEXT("au.PerSourceResampling"));
    IConsoleVariable* ForceAudioLink =
        IConsoleManager::Get().FindConsoleVariable(
            TEXT("au.AudioLink.ForceOnAllSource"));
    if (DirectProceduralRendering)
    {
        DirectProceduralRendering->Set(1, ECVF_SetByCode);
    }
    if (PerSourceResampling)
    {
        PerSourceResampling->Set(1, ECVF_SetByCode);
    }
    if (ForceAudioLink)
    {
        ForceAudioLink->Set(0, ECVF_SetByCode);
    }
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio validation source rendering: direct_procedural=%d per_source_resampling=%d force_audiolink=%d."),
        DirectProceduralRendering ? DirectProceduralRendering->GetInt() : -1,
        PerSourceResampling ? PerSourceResampling->GetInt() : -1,
        ForceAudioLink ? ForceAudioLink->GetInt() : -1);
    TArray<FVector> SourceLocations;
    SourceLocations.Reserve(RequestedSourceCount);
    SourceLocations.Add(PrimarySourceLocation);
    for (int32 SourceIndex = 1; SourceIndex < RequestedSourceCount; ++SourceIndex)
    {
        const float Alpha = static_cast<float>(SourceIndex - 1)
            / static_cast<float>(FMath::Max(RequestedSourceCount - 1, 1));
        const float Angle = Alpha * 2.0f * PI;
        SourceLocations.Add(FVector(
            270.0f + (45.0f * FMath::Cos(Angle * 3.0f)),
            320.0f * FMath::Sin(Angle),
            100.0f + (60.0f * static_cast<float>(SourceIndex % 4))));
    }

    USoundWave* PrimaryProjectSoundWave = LoadObject<USoundWave>(
        nullptr,
        TEXT("/Game/FirstPerson/Audio/MarchingBand.MarchingBand"));
    if (IsValid(PrimaryProjectSoundWave))
    {
        // Interactive A/B validation can run indefinitely. Keep the rendered
        // proxy and the original reference alive from the same looping wave.
        PrimaryProjectSoundWave->bLooping = true;
    }
    int32 PrimaryPlaybackSampleRate = 0;
    int32 PrimaryPlaybackNumChannels = 0;
    int32 PrimaryPlaybackNumFrames = 0;
    float PrimaryPlaybackPeakAbsoluteSample = 0.0f;
    const bool bPrimaryInputInspected = IsValid(PrimaryProjectSoundWave)
        && InspectImportedSound(
            *PrimaryProjectSoundWave,
            PrimaryPlaybackSampleRate,
            PrimaryPlaybackNumChannels,
            PrimaryPlaybackNumFrames,
            PrimaryPlaybackPeakAbsoluteSample);
    UUERayTracingAudioValidationSoundProxy* PrimarySoundProxy = nullptr;
    UUERayTracingAudioValidationSoundWave* PrimaryPlayback = nullptr;
    USoundBase* PrimaryValidationSound = nullptr;
    TSharedPtr<
        FUERayTracingAudioValidationSourceBufferListener,
        ESPMode::ThreadSafe> PrimarySourceBufferListener;
    UUERayTracingAudioSourceComponent* PrimarySource = nullptr;
    TArray<TWeakObjectPtr<UUERayTracingAudioSourceComponent>> Sources;
    Sources.Reserve(RequestedSourceCount);
    TArray<TWeakObjectPtr<UAudioComponent>> AudioComponents;
    AudioComponents.Reserve(RequestedSourceCount);
    int32 SourceCount = 0;
    for (int32 SourceIndex = 0; SourceIndex < SourceLocations.Num(); ++SourceIndex)
    {
        const FString SourceActorName = FString::Printf(TEXT("VRTA_Source_%d"), SourceIndex);
        AActor* SourceActor = SpawnValidationActor(
            *World,
            *SourceActorName,
            SourceLocations[SourceIndex]);
        if (!SourceActor)
        {
            continue;
        }

        UUERayTracingAudioSourceComponent* Source =
            NewObject<UUERayTracingAudioSourceComponent>(SourceActor, TEXT("ValidationSource"));
        SourceActor->AddInstanceComponent(Source);
        // A soft wall should remain clearly audible in the validation scene.
        // At the shared 2 m distance this produces approximately 0.175 linear
        // Direct gain; Hard Occlusion remains the explicit silence case.
        Source->OccludedGain = SourceIndex == 0 ? 0.35f : 0.2f;
        Source->bHardOcclusion =
            SourceIndex == 0 && bHardDirectPreset;
        Source->NumOcclusionSamples = 8;
        Source->NumReflectionRays = SourceIndex == 0 ? 256 : 96;
        Source->MaxReflectionBounces = SourceIndex == 0 ? 4 : 2;
        Source->IndirectDurationSeconds = SourceIndex == 0 ? 1.5f : 0.75f;
        Source->IndirectMode = EUERayTracingAudioIndirectMode::HybridReverb;
        Source->SetIndirectDataSource(
            EUERayTracingAudioIndirectDataSource::Realtime);
        Source->IndirectMix = SourceIndex == 0 ? 1.75f : 0.4f;
        Source->RegisterComponent();
        AddVisualMarker(
            *SourceActor,
            *SphereMesh,
            *MarkerMaterial,
            TEXT("ValidationSourceMarker"),
            SourceIndex == 0 ? FVector(0.38f) : FVector(0.22f),
            SourceIndex == 0
                ? FLinearColor(1.0f, 0.08f, 0.005f)
                : FLinearColor(1.0f, 0.65f, 0.08f));
        if (SourceIndex == 0)
        {
            SpawnValidationLight(
                *World,
                SourceActor->GetActorLocation() + FVector(0.0, 0.0, 35.0),
                FLinearColor(1.0f, 0.22f, 0.05f),
                2200.0f,
                360.0f);
        }
        Sources.Add(Source);
        USoundBase* ValidationSound = nullptr;
        if (SourceIndex == 0 && bPrimaryInputInspected)
        {
            UUERayTracingAudioValidationSoundProxy* SoundProxy =
                NewObject<UUERayTracingAudioValidationSoundProxy>(SourceActor);
            SoundProxy->Initialize(*PrimaryProjectSoundWave);
            PrimarySoundProxy = SoundProxy;
            ValidationSound = SoundProxy;
        }
        else
        {
            ValidationSound = BuildValidationTone(*SourceActor);
        }
        if (!IsValid(ValidationSound))
        {
            UE_LOG(
                LogUERayTracingAudio,
                Error,
                TEXT("UERayTracingAudio validation could not create deterministic playback from the real project SoundWave."));
            return;
        }
        if (SourceIndex == 0)
        {
            PrimaryValidationSound = ValidationSound;
        }
        if (UAudioComponent* Audio = AddValidationAudio(
            *SourceActor,
            *ValidationSound,
            SourceIndex == 0 ? ValidationPrimaryVolume : 0.02f))
        {
            if (SourceIndex == 0)
            {
                PrimarySourceBufferListener = MakeShared<
                    FUERayTracingAudioValidationSourceBufferListener,
                    ESPMode::ThreadSafe>();
                Audio->SetSourceBufferListener(
                    PrimarySourceBufferListener,
                    false);
            }
            AudioComponents.Add(Audio);
        }
        ++SourceCount;

        if (!PrimarySource)
        {
            PrimarySource = Source;
        }
    }

    if (!PrimarySource
        || SourceCount < 2
        || AudioComponents.Num() != SourceCount
        || !IsValid(PrimaryValidationSound))
    {
        UE_LOG(LogUERayTracingAudio, Error, TEXT("UERayTracingAudio validation scenario could not spawn enough source actors for RHI batch validation."));
        return;
    }

    USoundBase* ReferenceSound = IsValid(PrimaryProjectSoundWave)
        ? static_cast<USoundBase*>(PrimaryProjectSoundWave)
        : PrimaryValidationSound;
    UAudioComponent* ReferenceAudio = AddValidationReferenceAudio(
        *ListenerActor,
        *ReferenceSound);
    if (!IsValid(ReferenceAudio))
    {
        UE_LOG(
            LogUERayTracingAudio,
            Error,
            TEXT("UERayTracingAudio validation scenario could not create its unrendered reference playback."));
        return;
    }

    FScenarioState& State = Scenarios.AddDefaulted_GetRef();
    State.bValidationOwner = ClaimValidationOwnership();
    State.World = World;
    State.CameraActor = CameraActor;
    State.ListenerMarker = ListenerMarker;
    State.Source = PrimarySource;
    State.Listener = Listener;
    State.PrimarySoundProxy = PrimarySoundProxy;
    State.PrimaryPlayback = PrimaryPlayback;
    State.ReferenceAudioComponent = ReferenceAudio;
    State.PrimarySourceBufferListener = PrimarySourceBufferListener;
    State.DirectPreset = DirectPreset;
    State.Sources = MoveTemp(Sources);
    State.AudioComponents = MoveTemp(AudioComponents);
    State.StartTimeSeconds = FPlatformTime::Seconds();
    State.FixedListenerLocation = ListenerActor->GetActorLocation();
    State.InteractiveStartRotation = FRotator(0.0f, 0.0f, 0.0f);
    State.SourceCount = SourceCount;
    State.GeometryCount = GeometryCount;
    State.TriangleCount = GeometryCount * 12;
    State.bPerformanceProfile = FParse::Param(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioPerformanceProfile"));
    State.bBakeRepeatabilityEnabled = FParse::Param(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioValidationBakeRepeatability"));
    State.bDirectSweepAutomaticRequested =
        State.bValidationOwner
        && FParse::Param(
            FCommandLine::Get(),
            TEXT("UERayTracingAudioValidationDirectSweep"));
    State.bInteractiveSmokeEnabled = FParse::Param(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioInteractiveSmoke"));
    State.bInteractiveRequested = State.bInteractiveSmokeEnabled || FParse::Param(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioInteractiveValidation"));
    State.ActorDestroyedHandle =
        World->AddOnActorDestroyedHandler(
            FOnActorDestroyed::FDelegate::CreateRaw(
                this,
                &FUERayTracingAudioRuntimeValidation::
                    HandleActorDestroyed));
    if (UAudioComponent* PrimaryAudio =
            State.AudioComponents.IsValidIndex(0)
                ? State.AudioComponents[0].Get()
                : nullptr;
        State.bValidationOwner && IsValid(PrimaryAudio))
    {
        FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
            PrimaryAudio->GetAudioComponentID());
    }

    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio validation scenario ready: scene=1 sources=%d listeners=1 geometry=%d triangles=%d direct_preset=%s primary_distance_cm=%.3f direct_rays_per_source=8 primary_indirect_rays=256 primary_bounces=4 primary_duration=1.500 primary_wet_send=1.750."),
        State.SourceCount,
        State.GeometryCount,
        State.TriangleCount,
        *State.DirectPreset,
        FVector::Distance(ListenerLocation, PrimarySourceLocation));
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio validation primary input: real_soundwave=%d asset=\"%s\" playback=project_soundwave_proxy sample_rate=%d channels=%d frames=%d peak=%.6f."),
        bPrimaryInputInspected ? 1 : 0,
        IsValid(PrimaryProjectSoundWave)
            ? *PrimaryProjectSoundWave->GetPathName()
            : TEXT("generated validation fallback"),
        PrimaryPlaybackSampleRate,
        PrimaryPlaybackNumChannels,
        PrimaryPlaybackNumFrames,
        PrimaryPlaybackPeakAbsoluteSample);

    if (State.bPerformanceProfile)
    {
        CSV_EVENT_GLOBAL(TEXT("UERayTracingAudioPerformanceStart"));
    }
}

bool FUERayTracingAudioRuntimeValidation::IsDirectSweepActive(
    const FScenarioState& State) const
{
    return State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::ClearHold
        || State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::EnteringWall
        || State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::OccludedHold
        || State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::Returning
        || State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::Restoring;
}

bool FUERayTracingAudioRuntimeValidation::StartDirectSweep(
    FScenarioState& State,
    FUERayTracingAudioManager& Manager,
    const bool bAutomatic)
{
    if (!State.bValidationOwner
        || IsDirectSweepActive(State)
        || ActiveDirectSweepWorld.IsValid())
    {
        UE_LOG(
            LogUERayTracingAudio,
            Warning,
            TEXT("UERayTracingAudio direct sweep start rejected: non_reentrant=1 phase=%s."),
            GetDirectSweepPhaseName(State.DirectSweepPhase));
        return false;
    }

    UWorld* World = State.World.Get();
    UUERayTracingAudioSourceComponent* Source = State.Source.Get();
    UUERayTracingAudioListenerComponent* Listener =
        State.Listener.Get();
    UAudioComponent* Audio = State.AudioComponents.IsValidIndex(0)
        ? State.AudioComponents[0].Get()
        : nullptr;
    AActor* SourceActor = IsValid(Source)
        ? Source->GetOwner()
        : nullptr;
    AActor* ListenerActor = IsValid(Listener)
        ? Listener->GetOwner()
        : nullptr;
    if (!IsValid(World)
        || !IsValid(Source)
        || !IsValid(Listener)
        || !IsValid(Audio)
        || !IsValid(SourceActor)
        || !IsValid(ListenerActor))
    {
        UE_LOG(
            LogUERayTracingAudio,
            Error,
            TEXT("UERayTracingAudio direct sweep start rejected: source, listener, audio, or owner is invalid."));
        return false;
    }

    FUERayTracingAudioSourceSimulationResult LatestResult;
    const bool bHasLatestDirect =
        Manager.GetLatestSourceSimulation(
            Source,
            LatestResult)
        && LatestResult.bHasDirectResult
        && LatestResult.DirectGeneration != 0;

    State.DirectSweepMetrics.Reset();
    State.DirectSweepAudioStats =
        FUERayTracingAudioDirectAudioStats();
    State.DirectSweepLatestResult =
        FUERayTracingAudioDirectSimulationResult();
    State.DirectSweepFailureReason.Reset();
    State.DirectSweepSavedSourceTransform =
        SourceActor->GetActorTransform();
    State.bDirectSweepSavedHardOcclusion =
        Source->bHardOcclusion;
    State.DirectSweepSavedOccludedGain =
        Source->OccludedGain;
    State.DirectSweepSavedIndirectMix =
        Source->IndirectMix;
    State.DirectSweepSavedDataSource =
        Source->IndirectDataSource;
    State.DirectSweepRestoredDistanceCm =
        FVector::Distance(
            State.DirectSweepSavedSourceTransform.
                GetLocation(),
            State.FixedListenerLocation);
    State.DirectSweepStartTimeSeconds =
        FPlatformTime::Seconds();
    State.DirectSweepPhaseStartTimeSeconds =
        State.DirectSweepStartTimeSeconds;
    State.DirectSweepGenerationFloor =
        bHasLatestDirect
            ? LatestResult.DirectGeneration
            : 0;
    State.DirectSweepPendingGenerationDiscardCount = 1;
    State.DirectSweepPhase =
        EUERayTracingAudioDirectSweepPhase::ClearHold;
    State.bDirectSweepWasAutomatic = bAutomatic;
    State.bDirectSweepStateSaved = true;
    State.bDirectSweepRestoreApplied = false;
    State.bDirectSweepRestored = false;
    State.bDirectSweepHardwareObserved = false;
    State.bDirectSweepHardwareOnly = true;
    State.bDirectSweepMotionSucceeded = false;
    State.bDirectSweepSummaryLogged = false;
    State.bDirectSweepWarmupComplete = false;
    State.bDirectSweepAudioStatsCaptured = false;
    if (bAutomatic)
    {
        State.bDirectSweepAutomaticStarted = true;
        State.bDirectSweepAutomaticTerminal = false;
    }

    ListenerActor->SetActorLocation(
        State.FixedListenerLocation,
        false,
        nullptr,
        ETeleportType::TeleportPhysics);
    SourceActor->SetActorLocation(
        FUERayTracingAudioDirectSweepTrajectory::Evaluate(
            State.FixedListenerLocation,
            0.0f),
        false,
        nullptr,
        ETeleportType::TeleportPhysics);
    Source->bHardOcclusion = false;
    Source->OccludedGain =
        ValidationDirectSweepSoftOccludedGain;
    Source->IndirectMix = 0.0f;
    Source->SetIndirectDataSource(
        EUERayTracingAudioIndirectDataSource::Realtime);

    FUERayTracingAudioAudioDiagnostics::
        SetTargetAudioComponentId(
            Audio->GetAudioComponentID());
    FUERayTracingAudioAudioDiagnostics::ResetDirect();
    ActiveDirectSweepWorld = World;

    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio direct sweep started: automatic=%d radius_cm=200.000 clear_hold_seconds=0.500 traversal_seconds=3.000 occluded_hold_seconds=0.500 return_hold_seconds=0.500."),
        bAutomatic ? 1 : 0);
    return true;
}

void FUERayTracingAudioRuntimeValidation::
    BeginDirectSweepRestore(
        FScenarioState& State,
        FUERayTracingAudioManager& Manager,
        const bool bMotionSucceeded,
        const FString& FailureReason)
{
    if (!IsDirectSweepActive(State)
        || State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::Restoring)
    {
        return;
    }

    State.bDirectSweepMotionSucceeded =
        bMotionSucceeded;
    State.DirectSweepFailureReason = FailureReason;
    if (!State.bDirectSweepAudioStatsCaptured)
    {
        State.DirectSweepAudioStats =
            FUERayTracingAudioAudioDiagnostics::
                ReadDirect();
        State.bDirectSweepAudioStatsCaptured = true;
    }

    UUERayTracingAudioSourceComponent* Source =
        State.Source.Get();
    UUERayTracingAudioListenerComponent* Listener =
        State.Listener.Get();
    AActor* SourceActor = IsValid(Source)
        ? Source->GetOwner()
        : nullptr;
    AActor* ListenerActor = IsValid(Listener)
        ? Listener->GetOwner()
        : nullptr;
    if (State.bDirectSweepStateSaved
        && !State.bDirectSweepRestoreApplied)
    {
        State.bDirectSweepRestoreApplied = true;
        if (IsValid(Source) && IsValid(SourceActor))
        {
            SourceActor->SetActorTransform(
                State.DirectSweepSavedSourceTransform,
                false,
                nullptr,
                ETeleportType::TeleportPhysics);
            Source->bHardOcclusion =
                State.bDirectSweepSavedHardOcclusion;
            Source->OccludedGain =
                State.DirectSweepSavedOccludedGain;
            Source->IndirectMix =
                State.DirectSweepSavedIndirectMix;
            Source->SetIndirectDataSource(
                State.DirectSweepSavedDataSource);
        }
        if (IsValid(ListenerActor))
        {
            ListenerActor->SetActorLocation(
                State.FixedListenerLocation,
                false,
                nullptr,
                ETeleportType::TeleportPhysics);
        }
    }

    FUERayTracingAudioSourceSimulationResult LatestResult;
    if (IsValid(Source)
        && Manager.GetLatestSourceSimulation(
            Source,
            LatestResult)
        && LatestResult.bHasDirectResult)
    {
        State.DirectSweepGenerationFloor =
            FMath::Max(
                State.DirectSweepGenerationFloor,
                LatestResult.DirectGeneration);
    }
    State.DirectSweepPendingGenerationDiscardCount = 1;
    State.DirectSweepPhase =
        EUERayTracingAudioDirectSweepPhase::Restoring;
    State.DirectSweepPhaseStartTimeSeconds =
        FPlatformTime::Seconds();

    if (!IsValid(Source)
        || !IsValid(SourceActor)
        || !IsValid(ListenerActor))
    {
        FinishDirectSweep(State, false);
    }
}

void FUERayTracingAudioRuntimeValidation::FinishDirectSweep(
    FScenarioState& State,
    const bool bRestored)
{
    if (State.bDirectSweepSummaryLogged)
    {
        return;
    }

    State.bDirectSweepRestored = bRestored;
    const FUERayTracingAudioHardRealtimeStats
        HardRealtimeStats =
            FUERayTracingAudioAudioDiagnostics::
                ReadHardRealtime();
    const bool bHardRealtimePassed =
        HardRealtimeStats.AudioCallbackCount > 0
        && HardRealtimeStats.CallbackCapacityMissCount == 0
        && HardRealtimeStats.
            ConvolutionPrepareCapacityDropCount == 0;
    const bool bHardwarePassed =
        State.bDirectSweepHardwareObserved
        && State.bDirectSweepHardwareOnly;
    const bool bPassed =
        State.bDirectSweepMotionSucceeded
        && State.DirectSweepMetrics.Passes(
            State.DirectSweepAudioStats,
            bHardwarePassed,
            bRestored)
        && bHardRealtimePassed;
    State.DirectSweepPhase =
        bPassed
            ? EUERayTracingAudioDirectSweepPhase::Complete
            : EUERayTracingAudioDirectSweepPhase::Failed;
    State.bDirectSweepSummaryLogged = true;
    if (State.bDirectSweepWasAutomatic)
    {
        State.bDirectSweepAutomaticTerminal = true;
    }

    if (!bPassed
        && !State.DirectSweepFailureReason.IsEmpty())
    {
        UE_LOG(
            LogUERayTracingAudio,
            Error,
            TEXT("UERayTracingAudio direct sweep failure: reason=\"%s\" callbacks=%llu callback_capacity_misses=%llu convolution_prepare_drops=%llu."),
            *State.DirectSweepFailureReason,
            HardRealtimeStats.AudioCallbackCount,
            HardRealtimeStats.CallbackCapacityMissCount,
            HardRealtimeStats.
                ConvolutionPrepareCapacityDropCount);
    }

    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio direct sweep: passed=%d generations=%d distance_min_cm=%.3f distance_max_cm=%.3f visibility_min=%.6f visibility_max=%.6f gain_min=%.6f gain_max=%.6f max_gain_step=%.8f direct_dropouts=%llu restored=%d hardware=%d"),
        bPassed ? 1 : 0,
        State.DirectSweepMetrics.GetGenerationCount(),
        State.DirectSweepMetrics.GetDistanceMinCm(),
        State.DirectSweepMetrics.GetDistanceMaxCm(),
        State.DirectSweepMetrics.GetVisibilityMin(),
        State.DirectSweepMetrics.GetVisibilityMax(),
        State.DirectSweepMetrics.GetGainMin(),
        State.DirectSweepMetrics.GetGainMax(),
        State.DirectSweepAudioStats.MaxBandGainStep,
        State.DirectSweepAudioStats.
            MaxConsecutiveSilentDirectBufferCount,
        bRestored ? 1 : 0,
        bHardwarePassed ? 1 : 0);

    if (ActiveDirectSweepWorld == State.World)
    {
        ActiveDirectSweepWorld.Reset();
    }
}

void FUERayTracingAudioRuntimeValidation::
    FailAutomaticDirectSweepStart(
        FScenarioState& State,
        const FString& FailureReason)
{
    if (State.bDirectSweepAutomaticStarted
        || State.bDirectSweepSummaryLogged)
    {
        return;
    }

    State.DirectSweepMetrics.Reset();
    State.DirectSweepAudioStats =
        FUERayTracingAudioDirectAudioStats();
    State.DirectSweepLatestResult =
        FUERayTracingAudioDirectSimulationResult();
    State.DirectSweepFailureReason = FailureReason;
    State.bDirectSweepAutomaticStarted = true;
    State.bDirectSweepAutomaticTerminal = false;
    State.bDirectSweepWasAutomatic = true;
    State.bDirectSweepStateSaved = false;
    State.bDirectSweepRestoreApplied = false;
    State.bDirectSweepRestored = true;
    State.bDirectSweepHardwareObserved = false;
    State.bDirectSweepHardwareOnly = false;
    State.bDirectSweepMotionSucceeded = false;
    State.bDirectSweepSummaryLogged = false;
    State.bDirectSweepWarmupComplete = false;
    State.bDirectSweepAudioStatsCaptured = false;
    FinishDirectSweep(State, true);
}

void FUERayTracingAudioRuntimeValidation::
    AbortDirectSweepImmediately(
        FScenarioState& State,
        const FString& FailureReason)
{
    if (!IsDirectSweepActive(State))
    {
        return;
    }

    FUERayTracingAudioManager& Manager =
        FUERayTracingAudioModule::GetManager();
    BeginDirectSweepRestore(
        State,
        Manager,
        false,
        FailureReason);
    FinishDirectSweep(State, false);
}

void FUERayTracingAudioRuntimeValidation::
    HandleWorldBeginTearDown(UWorld* World)
{
    if (!World)
    {
        return;
    }

    for (int32 ScenarioIndex = Scenarios.Num() - 1;
        ScenarioIndex >= 0;
        --ScenarioIndex)
    {
        FScenarioState& State = Scenarios[ScenarioIndex];
        if (State.World.Get() != World)
        {
            continue;
        }
        if (IsDirectSweepActive(State))
        {
            AbortDirectSweepImmediately(
                State,
                TEXT("world began teardown"));
        }
        if (State.BakeJob.IsValid())
        {
            State.BakeJob->Cancel();
            State.BakeJob.Reset();
        }
        if (State.DataSourceBakeJob.IsValid())
        {
            State.DataSourceBakeJob->Cancel();
            State.DataSourceBakeJob.Reset();
        }
        if (State.ActorDestroyedHandle.IsValid())
        {
            World->RemoveOnActorDestroyedHandler(
                State.ActorDestroyedHandle);
            State.ActorDestroyedHandle.Reset();
        }
        if (State.bValidationOwner)
        {
            FUERayTracingAudioAudioDiagnostics::
                SetTargetAudioComponentId(0);
            bValidationOwnerAssigned = false;
        }
        Scenarios.RemoveAt(
            ScenarioIndex,
            1,
            EAllowShrinking::No);
    }
    InitializedWorlds.Remove(World);
    if (ActiveDirectSweepWorld.Get() == World)
    {
        ActiveDirectSweepWorld.Reset();
    }
}

void FUERayTracingAudioRuntimeValidation::HandleActorDestroyed(
    AActor* Actor)
{
    if (!Actor)
    {
        return;
    }

    for (FScenarioState& State : Scenarios)
    {
        UUERayTracingAudioSourceComponent* Source =
            State.Source.Get();
        UUERayTracingAudioListenerComponent* Listener =
            State.Listener.Get();
        if (IsDirectSweepActive(State)
            && ((IsValid(Source)
                    && Source->GetOwner() == Actor)
                || (IsValid(Listener)
                    && Listener->GetOwner() == Actor)))
        {
            AbortDirectSweepImmediately(
                State,
                TEXT("validation Source or Listener actor was destroyed"));
        }
    }
}

void FUERayTracingAudioRuntimeValidation::TickDirectSweep(
    FScenarioState& State,
    FUERayTracingAudioManager& Manager,
    const FUERayTracingAudioSourceSimulationResult*
        LatestResult)
{
    if (!IsDirectSweepActive(State))
    {
        return;
    }

    UWorld* World = State.World.Get();
    UUERayTracingAudioSourceComponent* Source =
        State.Source.Get();
    UUERayTracingAudioListenerComponent* Listener =
        State.Listener.Get();
    AActor* SourceActor = IsValid(Source)
        ? Source->GetOwner()
        : nullptr;
    AActor* ListenerActor = IsValid(Listener)
        ? Listener->GetOwner()
        : nullptr;
    if (!IsValid(World)
        || !IsValid(Source)
        || !IsValid(SourceActor)
        || !IsValid(ListenerActor))
    {
        AbortDirectSweepImmediately(
            State,
            TEXT("validation Source, Listener, or World became invalid"));
        return;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    ListenerActor->SetActorLocation(
        State.FixedListenerLocation,
        false,
        nullptr,
        ETeleportType::TeleportPhysics);

    if (State.DirectSweepPhase
        == EUERayTracingAudioDirectSweepPhase::Restoring)
    {
        if (LatestResult
            && LatestResult->bHasDirectResult
            && LatestResult->DirectGeneration
                > State.DirectSweepGenerationFloor)
        {
            State.DirectSweepGenerationFloor =
                LatestResult->DirectGeneration;
            if (State.DirectSweepPendingGenerationDiscardCount > 0)
            {
                --State.
                    DirectSweepPendingGenerationDiscardCount;
            }
            else
            {
                const bool bSourceRestored =
                    State.bDirectSweepRestoreApplied
                    && SourceActor->GetActorTransform().Equals(
                        State.DirectSweepSavedSourceTransform,
                        0.01f)
                    && Source->bHardOcclusion
                        == State.
                            bDirectSweepSavedHardOcclusion
                    && FMath::IsNearlyEqual(
                        Source->OccludedGain,
                        State.
                            DirectSweepSavedOccludedGain,
                        UE_KINDA_SMALL_NUMBER)
                    && FMath::IsNearlyEqual(
                        Source->IndirectMix,
                        State.
                            DirectSweepSavedIndirectMix,
                        UE_KINDA_SMALL_NUMBER)
                    && Source->IndirectDataSource
                        == State.
                            DirectSweepSavedDataSource;
                const bool bListenerRestored =
                    ListenerActor->GetActorLocation().Equals(
                        State.FixedListenerLocation,
                        0.01f);
                const bool bRestoredGenerationAtLocation =
                    LatestResult->DirectResult.bHasListener
                    && FMath::IsNearlyEqual(
                        LatestResult->DirectResult.DistanceCm,
                        State.
                            DirectSweepRestoredDistanceCm,
                        2.0f);
                FinishDirectSweep(
                    State,
                    bSourceRestored
                        && bListenerRestored
                        && bRestoredGenerationAtLocation);
                return;
            }
        }

        if (NowSeconds
                - State.DirectSweepPhaseStartTimeSeconds
            > ValidationDirectSweepRestoreTimeoutSeconds)
        {
            State.DirectSweepFailureReason =
                State.DirectSweepFailureReason.IsEmpty()
                    ? TEXT("post-restore Direct generation timed out")
                    : State.DirectSweepFailureReason
                        + TEXT("; post-restore Direct generation timed out");
            FinishDirectSweep(State, false);
        }
        return;
    }

    const double PhaseElapsedSeconds =
        NowSeconds
        - State.DirectSweepPhaseStartTimeSeconds;
    switch (State.DirectSweepPhase)
    {
    case EUERayTracingAudioDirectSweepPhase::ClearHold:
        SourceActor->SetActorLocation(
            FUERayTracingAudioDirectSweepTrajectory::
                Evaluate(
                    State.FixedListenerLocation,
                    0.0f),
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        break;
    case EUERayTracingAudioDirectSweepPhase::EnteringWall:
        SourceActor->SetActorLocation(
            FUERayTracingAudioDirectSweepTrajectory::
                Evaluate(
                    State.FixedListenerLocation,
                    static_cast<float>(
                        PhaseElapsedSeconds
                        / ValidationDirectSweepTraversalSeconds)),
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        break;
    case EUERayTracingAudioDirectSweepPhase::OccludedHold:
        SourceActor->SetActorLocation(
            FUERayTracingAudioDirectSweepTrajectory::
                Evaluate(
                    State.FixedListenerLocation,
                    1.0f),
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        break;
    case EUERayTracingAudioDirectSweepPhase::Returning:
        SourceActor->SetActorLocation(
            FUERayTracingAudioDirectSweepTrajectory::
                Evaluate(
                    State.FixedListenerLocation,
                    1.0f
                    - static_cast<float>(
                        FMath::Min(
                            PhaseElapsedSeconds
                                / ValidationDirectSweepTraversalSeconds,
                            1.0))),
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        break;
    default:
        break;
    }

    if (LatestResult
        && LatestResult->bHasDirectResult
        && LatestResult->DirectGeneration
            > State.DirectSweepGenerationFloor)
    {
        State.DirectSweepGenerationFloor =
            LatestResult->DirectGeneration;
        State.DirectSweepLatestResult =
            LatestResult->DirectResult;
        if (State.DirectSweepPendingGenerationDiscardCount > 0)
        {
            --State.
                DirectSweepPendingGenerationDiscardCount;
            if (State.
                    DirectSweepPendingGenerationDiscardCount
                == 0)
            {
                State.bDirectSweepWarmupComplete = true;
                State.DirectSweepPhaseStartTimeSeconds =
                    NowSeconds;
                FUERayTracingAudioAudioDiagnostics::
                    ResetDirect();
            }
        }
        else
        {
            const bool bGenerationUsedHardware =
                LatestResult->DirectResult.
                    bUsedHardwareRayTracing;
            State.bDirectSweepHardwareObserved |=
                bGenerationUsedHardware;
            State.bDirectSweepHardwareOnly &=
                bGenerationUsedHardware;
            State.DirectSweepMetrics.Observe(
                LatestResult->DirectGeneration,
                LatestResult->DirectResult);
        }
    }

    if (!State.bDirectSweepWarmupComplete)
    {
        if (NowSeconds - State.DirectSweepStartTimeSeconds
            > ValidationDirectSweepTimeoutSeconds)
        {
            BeginDirectSweepRestore(
                State,
                Manager,
                false,
                TEXT("Direct sweep warmup timed out"));
        }
        return;
    }

    const double ActivePhaseElapsedSeconds =
        NowSeconds
        - State.DirectSweepPhaseStartTimeSeconds;
    if (State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::ClearHold
        && ActivePhaseElapsedSeconds
            >= ValidationDirectSweepClearHoldSeconds)
    {
        State.DirectSweepPhase =
            EUERayTracingAudioDirectSweepPhase::EnteringWall;
        State.DirectSweepPhaseStartTimeSeconds =
            NowSeconds;
    }
    else if (State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::EnteringWall
        && ActivePhaseElapsedSeconds
            >= ValidationDirectSweepTraversalSeconds)
    {
        State.DirectSweepPhase =
            EUERayTracingAudioDirectSweepPhase::OccludedHold;
        State.DirectSweepPhaseStartTimeSeconds =
            NowSeconds;
    }
    else if (State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::OccludedHold
        && ActivePhaseElapsedSeconds
            >= ValidationDirectSweepOccludedHoldSeconds)
    {
        State.DirectSweepPhase =
            EUERayTracingAudioDirectSweepPhase::Returning;
        State.DirectSweepPhaseStartTimeSeconds =
            NowSeconds;
    }
    else if (State.DirectSweepPhase
            == EUERayTracingAudioDirectSweepPhase::Returning
        && ActivePhaseElapsedSeconds
            >= ValidationDirectSweepTraversalSeconds
                + ValidationDirectSweepReturnHoldSeconds)
    {
        BeginDirectSweepRestore(
            State,
            Manager,
            true,
            FString());
        return;
    }

    if (NowSeconds - State.DirectSweepStartTimeSeconds
        > ValidationDirectSweepTimeoutSeconds)
    {
        BeginDirectSweepRestore(
            State,
            Manager,
            false,
            TEXT("Direct sweep motion timed out"));
    }
}

bool FUERayTracingAudioRuntimeValidation::Tick(const float)
{
    FUERayTracingAudioManager& Manager = FUERayTracingAudioModule::GetManager();
    for (FScenarioState& State : Scenarios)
    {
        UWorld* World = State.World.Get();
        UUERayTracingAudioSourceComponent* Source = State.Source.Get();
        if (!IsValid(World) || !IsValid(Source))
        {
            if (IsDirectSweepActive(State))
            {
                AbortDirectSweepImmediately(
                    State,
                    TEXT("validation Source or World became invalid"));
            }
            continue;
        }

        if (!State.bCameraActivated)
        {
            AActor* CameraActor = State.CameraActor.Get();
            APlayerController* PlayerController = World->GetFirstPlayerController();
            if (IsValid(CameraActor) && IsValid(PlayerController))
            {
                APawn* Pawn = PlayerController->GetPawn();
                UUERayTracingAudioListenerComponent* Listener = State.Listener.Get();
                AActor* ListenerActor = IsValid(Listener)
                    ? Listener->GetOwner()
                    : nullptr;
                if (IsValid(Pawn) && IsValid(ListenerActor))
                {
                    const FVector PawnViewLocation = Pawn->GetPawnViewLocation();
                    const float ViewOffsetZ =
                        PawnViewLocation.Z - Pawn->GetActorLocation().Z;
                    constexpr float ValidationFloorTopZ = 10.0f;
                    float StandingViewZ = State.FixedListenerLocation.Z;
                    if (const ACharacter* Character = Cast<ACharacter>(Pawn))
                    {
                        if (const UCapsuleComponent* Capsule =
                            Character->GetCapsuleComponent())
                        {
                            StandingViewZ =
                                ValidationFloorTopZ
                                + Capsule->GetScaledCapsuleHalfHeight()
                                + ((UCharacterMovementComponent::MIN_FLOOR_DIST
                                    + UCharacterMovementComponent::MAX_FLOOR_DIST)
                                    * 0.5f)
                                + ViewOffsetZ;
                        }
                    }
                    State.FixedListenerLocation.Z = StandingViewZ;
                    ListenerActor->SetActorLocation(
                        State.FixedListenerLocation,
                        false,
                        nullptr,
                        ETeleportType::TeleportPhysics);
                    UE_LOG(
                        LogUERayTracingAudio,
                        Display,
                        TEXT("UERayTracingAudio interactive origin calibrated before tracing: listener=(%.3f,%.3f,%.3f) map_pawn_view_z=%.3f view_offset_z=%.3f."),
                        State.FixedListenerLocation.X,
                        State.FixedListenerLocation.Y,
                        State.FixedListenerLocation.Z,
                        PawnViewLocation.Z,
                        ViewOffsetZ);
                }
                PlayerController->SetViewTarget(CameraActor);
                State.bCameraActivated = true;
                UE_LOG(
                    LogUERayTracingAudio,
                    Display,
                    TEXT("UERayTracingAudio validation visible scene ready: camera=1 visible_geometry=%d listener_marker=1 source_markers=%d lighting=1."),
                    State.GeometryCount,
                    State.SourceCount);
            }
        }

        TickInteractiveControls(State);

        if (GEngine
            && State.bCameraActivated
            && State.bValidationOwner)
        {
            const TCHAR* DataSourceStatus = TEXT("WAITING");
            if (State.bDataSourceValidationLogged)
            {
                DataSourceStatus = State.bDataSourceValidationPassed
                    ? TEXT("PASSED")
                    : TEXT("FAILED");
            }
            else if (State.DataSourceValidationPhase == 1)
            {
                DataSourceStatus = TEXT("BAKING");
            }
            else if (State.DataSourceValidationPhase == 2)
            {
                DataSourceStatus = TEXT("BAKED");
            }
            else if (State.DataSourceValidationPhase == 3)
            {
                DataSourceStatus = TEXT("REALTIME");
            }
            else if (State.DataSourceValidationPhase == 4)
            {
                DataSourceStatus = TEXT("HYBRID");
            }
            GEngine->AddOnScreenDebugMessage(
                0x554552415544494FULL,
                0.25f,
                State.bDataSourceValidationPassed ? FColor::Green : FColor::Cyan,
                FString::Printf(
                    TEXT("UE RAY TRACING AUDIO - HARDWARE VALIDATION\nVisible acoustic room | Blue: Listener | Orange: Primary Source | Direct preset: %s\nSources: %d | Geometry: %d | Direct/Indirect RHI: %s | IR Modes: %s\nDIRECT SWEEP: %s | Distance: %.3f cm | Visibility: %.6f | Direct gain: %.6f | Air bands: %.6f / %.6f / %.6f\nCURRENT MODE: %s | A/B PLAYBACK: %s | Baked asset: %s | View: %s\nF3: Original/Rendered A/B | F6: Direct Sweep | F1: Realtime | F2: Baked | F5: Hybrid | F4: Bake origin | F8: View"),
                    *State.DirectPreset,
                    State.SourceCount,
                    State.GeometryCount,
                    State.bResultLogged ? TEXT("READY") : TEXT("TRACING"),
                    DataSourceStatus,
                    GetDirectSweepPhaseName(
                        State.DirectSweepPhase),
                    State.DirectSweepLatestResult.DistanceCm,
                    State.DirectSweepLatestResult.
                        DirectVisibility,
                    State.DirectSweepLatestResult.OverallGain,
                    State.DirectSweepLatestResult.
                        AirAbsorption.X,
                    State.DirectSweepLatestResult.
                        AirAbsorption.Y,
                    State.DirectSweepLatestResult.
                        AirAbsorption.Z,
                    GetDataSourceName(Source->IndirectDataSource),
                    State.bRenderedABEnabled
                        ? TEXT("RENDERED DIRECT+WET")
                        : TEXT("ORIGINAL UNRENDERED"),
                    GetBakedAssetStatusName(Source->BakedAssetStatus),
                    State.bInteractiveMode
                        ? TEXT("INTERACTIVE - WASD + MOUSE")
                        : TEXT("FIXED AUTOMATION CAMERA")));
        }

        // Start Original and Rendered together from sample zero, but keep the
        // rendered side effectively muted until a valid Direct/Indirect result
        // exists. Original is therefore available during hardware warmup while
        // the unattenuated renderer default can never become the audible path.
        if (!State.bABPlaybackStarted
            && World->HasBegunPlay())
        {
            if (State.AudioStartTimeSeconds <= 0.0)
            {
                State.AudioStartTimeSeconds = FPlatformTime::Seconds();
            }
            State.MutedForeignAudioComponentCount =
                MuteForeignWorldAudio(State);
            const bool bABPlaybackStarted =
                EnsureSynchronizedABPlayback(
                    State,
                    State.bResultLogged
                        ? TEXT("initial hardware-ready start")
                        : TEXT("initial Original playback during hardware warmup"),
                    true);
            int32 StartedAudioComponents = bABPlaybackStarted ? 1 : 0;
            for (int32 AudioIndex = 1;
                AudioIndex < State.AudioComponents.Num();
                ++AudioIndex)
            {
                if (UAudioComponent* Audio =
                    State.AudioComponents[AudioIndex].Get())
                {
                    Audio->Play();
                    if (Audio->IsPlaying())
                    {
                        ++StartedAudioComponents;
                    }
                }
            }
            State.bAllAudioChainsStarted =
                bABPlaybackStarted
                && StartedAudioComponents == State.SourceCount;
            UE_LOG(
                LogUERayTracingAudio,
                Display,
                TEXT("UERayTracingAudio validation tones started through %d configured audio plugin chains; ab_playback_started=%d all_chains_started=%d unrendered_reference=%d adjacent_ab_start_commands=1 muted_foreign_audio=%d."),
                StartedAudioComponents,
                bABPlaybackStarted ? 1 : 0,
                State.bAllAudioChainsStarted ? 1 : 0,
                State.bReferenceAudioStarted ? 1 : 0,
                State.MutedForeignAudioComponentCount);
        }
        else if (State.bABPlaybackStarted)
        {
            if (!EnsureSynchronizedABPlayback(
                    State,
                    TEXT("runtime playback health check"),
                    false))
            {
                State.bAllAudioChainsStarted = false;
            }
        }

        FUERayTracingAudioSourceSimulationResult LatestResult;
        const bool bHasLatestResult =
            Manager.GetLatestSourceSimulation(
                Source,
                LatestResult);
        const double ScenarioElapsedSeconds =
            FPlatformTime::Seconds() - State.StartTimeSeconds;
        if (FUERayTracingAudioDirectSweepPolicy::
                ShouldStartAutomatic(
                    State.bDirectSweepAutomaticRequested,
                    State.bDirectSweepAutomaticStarted,
                    State.bValidationOwner,
                    State.bResultLogged,
                    bHasLatestResult
                        && LatestResult.bHasDirectResult,
                    bHasLatestResult
                        ? LatestResult.DirectGeneration
                        : 0,
                    bHasLatestResult
                        && LatestResult.DirectResult.
                            bUsedHardwareRayTracing))
        {
            if (!StartDirectSweep(
                    State,
                    Manager,
                    true))
            {
                FailAutomaticDirectSweepStart(
                    State,
                    TEXT("automatic sweep start was rejected after hardware Direct became ready"));
            }
        }
        else if (FUERayTracingAudioDirectSweepPolicy::
                HasHardwareWaitTimedOut(
                    State.bDirectSweepAutomaticRequested,
                    State.bDirectSweepAutomaticStarted,
                    State.bValidationOwner,
                    ScenarioElapsedSeconds,
                    ValidationAcousticStartupTimeoutSeconds))
        {
            FailAutomaticDirectSweepStart(
                State,
                TEXT("hardware Direct result timed out before automatic sweep"));
        }
        if (IsDirectSweepActive(State))
        {
            TickDirectSweep(
                State,
                Manager,
                bHasLatestResult
                    ? &LatestResult
                    : nullptr);
        }

        if (!State.bResultLogged)
        {
        if (const IConsoleVariable* DirectBatchSources =
            IConsoleManager::Get().FindConsoleVariable(TEXT("au.UERayTracingAudio.Stats.DirectBatchSources")))
        {
            State.MaxDirectBatchSources = FMath::Max(State.MaxDirectBatchSources, DirectBatchSources->GetInt());
        }
        if (const IConsoleVariable* IndirectBatchSources =
            IConsoleManager::Get().FindConsoleVariable(TEXT("au.UERayTracingAudio.Stats.IndirectBatchSources")))
        {
            State.MaxIndirectBatchSources = FMath::Max(State.MaxIndirectBatchSources, IndirectBatchSources->GetInt());
        }

        if (bHasLatestResult
            && LatestResult.bHasDirectResult
            && LatestResult.bHasIndirectResult
            && LatestResult.DirectResult.
                bUsedHardwareRayTracing
            && LatestResult.IndirectResult.
                bUsedHardwareRayTracing)
        {
            const FUERayTracingAudioSourceSimulationResult&
                Result = LatestResult;
            const FUERayTracingAudioIndirectSimulationResult CpuReference =
                Manager.SimulateIndirectSource(Source);
            const float PathRelativeDelta = static_cast<float>(FMath::Abs(
                Result.IndirectResult.NumValidPaths - CpuReference.NumValidPaths))
                / static_cast<float>(FMath::Max(
                    FMath::Max(Result.IndirectResult.NumValidPaths, CpuReference.NumValidPaths),
                    1));
            const float GainRelativeDelta = FMath::Abs(
                Result.IndirectResult.IndirectGain - CpuReference.IndirectGain)
                / FMath::Max(
                    FMath::Max(Result.IndirectResult.IndirectGain, CpuReference.IndirectGain),
                    UE_KINDA_SMALL_NUMBER);
            const double ElapsedMilliseconds =
                (FPlatformTime::Seconds() - State.StartTimeSeconds) * 1000.0;
            UE_LOG(
                LogUERayTracingAudio,
                Display,
                TEXT("UERayTracingAudio validation result: scene=1 sources=%d listeners=1 geometry=%d triangles=%d direct_preset=%s direct_batch_sources=%d indirect_batch_sources=%d direct_ready=1 distance_cm=%.3f distance_attenuation=%.6f visibility=%.4f occlusion=%.4f overall_gain=%.6f indirect_ready=1 valid_paths=%d indirect_gain=%.6f early_gain=%.6f late_gain=%.6f parametric_eq=(%.4f,%.4f,%.4f) reverb_times=(%.4f,%.4f,%.4f) wet_send=%.3f elapsed_ms=%.2f."),
                State.SourceCount,
                State.GeometryCount,
                State.TriangleCount,
                *State.DirectPreset,
                State.MaxDirectBatchSources,
                State.MaxIndirectBatchSources,
                Result.DirectResult.DistanceCm,
                Result.DirectResult.DistanceAttenuation,
                Result.DirectResult.DirectVisibility,
                Result.DirectResult.Occlusion,
                Result.DirectResult.OverallGain,
                Result.IndirectResult.NumValidPaths,
                Result.IndirectResult.IndirectGain,
                Result.IndirectResult.EarlyReflectionGain,
                Result.IndirectResult.LateReverbGain,
                Result.IndirectResult.ParametricEq.X,
                Result.IndirectResult.ParametricEq.Y,
                Result.IndirectResult.ParametricEq.Z,
                Result.IndirectResult.ReverbTimes.X,
                Result.IndirectResult.ReverbTimes.Y,
                Result.IndirectResult.ReverbTimes.Z,
                Source->IndirectMix,
                ElapsedMilliseconds);
            UE_LOG(
                LogUERayTracingAudio,
                Display,
                TEXT("UERayTracingAudio validation CPU reference: hardware_paths=%d cpu_paths=%d path_relative_delta=%.4f hardware_gain=%.6f cpu_gain=%.6f gain_relative_delta=%.4f."),
                Result.IndirectResult.NumValidPaths,
                CpuReference.NumValidPaths,
                PathRelativeDelta,
                Result.IndirectResult.IndirectGain,
                CpuReference.IndirectGain,
                GainRelativeDelta);
            State.bResultLogged = true;
        }
        }

        if (!State.bResultLogged
            && !State.bAcousticStartupFailed
            && FPlatformTime::Seconds() - State.StartTimeSeconds
                > ValidationAcousticStartupTimeoutSeconds)
        {
            State.bAcousticStartupFailed = true;
            State.bDataSourceValidationLogged = true;
            State.bDataSourceValidationPassed = false;
            State.bRenderedABEnabled = false;
            UE_LOG(
                LogUERayTracingAudio,
                Error,
                TEXT("UERayTracingAudio validation acoustic startup: passed=0 timeout_seconds=%.1f direct_ready=0 indirect_ready=0 fallback_playback=original; manual interaction and F3 remain available."),
                ValidationAcousticStartupTimeoutSeconds);
        }

        // The Manager can receive a GPU result one CoreTicker frame before the
        // SourceComponent publishes it to the audio snapshot registry. Only
        // expose Rendered after the exact audio component has a valid Direct,
        // Indirect, and stereo realtime IR snapshot; otherwise one unity-gain
        // frame can leak through at the start of the crossfade.
        if (State.bResultLogged && !State.bRenderedPlaybackReady)
        {
            UAudioComponent* PrimaryAudio =
                State.AudioComponents.IsValidIndex(0)
                    ? State.AudioComponents[0].Get()
                    : nullptr;
            const FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr
                AudioSnapshot = IsValid(PrimaryAudio)
                    ? Manager.GetSnapshotRegistry().Read(
                        PrimaryAudio->GetAudioComponentID())
                    : FUERayTracingAudioSimulationSnapshotRegistry::
                        FSnapshotPtr();
            const bool bAudioSnapshotReady =
                AudioSnapshot.IsValid()
                && AudioSnapshot->DataSource
                    == EUERayTracingAudioRuntimeDataSource::Realtime
                && AudioSnapshot->DirectResult.bHasListener
                && AudioSnapshot->IndirectResult.bHasListener
                && AudioSnapshot->RealtimeConvolutionKernelLeft.IsValid()
                && AudioSnapshot->RealtimeConvolutionKernelRight.IsValid();
            if (bAudioSnapshotReady)
            {
                State.bRenderedPlaybackReady = true;
                UE_LOG(
                    LogUERayTracingAudio,
                    Display,
                    TEXT("UERayTracingAudio rendered playback ready: audio_snapshot=1 direct=1 indirect=1 realtime_left=1 realtime_right=1 auto_enable=%d."),
                    State.bAcousticStartupFailed ? 0 : 1);
                if (!State.bAcousticStartupFailed)
                {
                    SetRenderedABMode(State, true);
                }
            }
        }

        if (State.bValidationOwner
            && State.bResultLogged
            && !State.bDataSourceValidationLogged
            && (!State.bDirectSweepAutomaticRequested
                || State.bDirectSweepAutomaticTerminal))
        {
            TickDataSourceValidation(State, Manager);
        }
        else if (State.bBakeRepeatabilityEnabled
            && State.bValidationOwner
            && State.bDataSourceValidationPassed
            && !State.bBakeRepeatabilityLogged)
        {
            TickBakeRepeatability(State, Manager);
        }
    }
    return true;
}

bool FUERayTracingAudioRuntimeValidation::PlaceInteractiveViewAtBakedOrigin(
    FScenarioState& State,
    APlayerController& PlayerController,
    APawn& Pawn)
{
    PlayerController.SetControlRotation(State.InteractiveStartRotation);
    PlayerController.SetViewTarget(&Pawn);

    const FVector CurrentViewLocation = Pawn.GetPawnViewLocation();
    const FVector TargetPawnLocation =
        Pawn.GetActorLocation()
        + (State.FixedListenerLocation - CurrentViewLocation);
    bool bMoved = Pawn.SetActorLocationAndRotation(
        TargetPawnLocation,
        State.InteractiveStartRotation,
        false,
        nullptr,
        ETeleportType::TeleportPhysics);
    PlayerController.SetControlRotation(State.InteractiveStartRotation);

    // Character view offsets can settle after the first teleport. Apply one
    // correction using the post-teleport view so F4 lands on the baked ear
    // position rather than merely placing the capsule origin approximately.
    const FVector CorrectedPawnLocation =
        Pawn.GetActorLocation()
        + (State.FixedListenerLocation - Pawn.GetPawnViewLocation());
    bMoved = Pawn.SetActorLocation(
        CorrectedPawnLocation,
        false,
        nullptr,
        ETeleportType::TeleportPhysics)
        && bMoved;
    Pawn.ConsumeMovementInputVector();
    if (UPawnMovementComponent* MovementComponent =
        Pawn.GetMovementComponent())
    {
        MovementComponent->StopMovementImmediately();
    }
    return bMoved;
}

void FUERayTracingAudioRuntimeValidation::SetInteractiveDataSource(
    FScenarioState& State,
    const EUERayTracingAudioIndirectDataSource DataSource)
{
    UUERayTracingAudioSourceComponent* PrimarySource = State.Source.Get();
    if (!IsValid(PrimarySource))
    {
        return;
    }

    PrimarySource->SetIndirectDataSource(DataSource);
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio interactive rendering mode changed: mode=%s."),
        GetDataSourceName(DataSource));
}

bool FUERayTracingAudioRuntimeValidation::EnsureSynchronizedABPlayback(
    FScenarioState& State,
    const TCHAR* Reason,
    const bool bForceRestart)
{
    UAudioComponent* RenderedAudio = State.AudioComponents.IsValidIndex(0)
        ? State.AudioComponents[0].Get()
        : nullptr;
    UAudioComponent* ReferenceAudio = State.ReferenceAudioComponent.Get();
    if (!IsValid(RenderedAudio) || !IsValid(ReferenceAudio))
    {
        State.bReferenceAudioStarted = false;
        State.bABPlaybackStarted = false;
        return false;
    }

    const bool bRenderedWasPlaying = RenderedAudio->IsPlaying();
    const bool bReferenceWasPlaying = ReferenceAudio->IsPlaying();
    if (!bForceRestart && bRenderedWasPlaying && bReferenceWasPlaying)
    {
        State.bReferenceAudioStarted = true;
        State.bABPlaybackStarted = true;
        return true;
    }

    // Restart both sides together from sample zero. Restarting only the failed
    // component would invalidate the same-position Original/Rendered A/B.
    RenderedAudio->Stop();
    ReferenceAudio->Stop();
    RenderedAudio->FadeIn(
        0.0f,
        State.bRenderedABEnabled
            ? ValidationActiveFadeLevel
            : ValidationInactiveFadeLevel,
        0.0f,
        EAudioFaderCurve::Linear);
    ReferenceAudio->FadeIn(
        0.0f,
        State.bRenderedABEnabled
            ? ValidationInactiveFadeLevel
            : ValidationActiveFadeLevel,
        0.0f,
        EAudioFaderCurve::Linear);

    const bool bRenderedStarted = RenderedAudio->IsPlaying();
    const bool bReferenceStarted = ReferenceAudio->IsPlaying();
    State.bReferenceAudioStarted = bReferenceStarted;
    State.bABPlaybackStarted = bRenderedStarted && bReferenceStarted;
    if (!bForceRestart)
    {
        ++State.ABPlaybackRestartCount;
    }
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio synchronized A/B playback: reason=\"%s\" force=%d rendered_was_playing=%d reference_was_playing=%d rendered_playing=%d reference_playing=%d restart_count=%d sample_start=0."),
        Reason ? Reason : TEXT("unspecified"),
        bForceRestart ? 1 : 0,
        bRenderedWasPlaying ? 1 : 0,
        bReferenceWasPlaying ? 1 : 0,
        bRenderedStarted ? 1 : 0,
        bReferenceStarted ? 1 : 0,
        State.ABPlaybackRestartCount);
    return bRenderedStarted && bReferenceStarted;
}

void FUERayTracingAudioRuntimeValidation::SetRenderedABMode(
    FScenarioState& State,
    const bool bRenderedEnabled)
{
    if (bRenderedEnabled && !State.bRenderedPlaybackReady)
    {
        return;
    }

    UAudioComponent* RenderedAudio = State.AudioComponents.IsValidIndex(0)
        ? State.AudioComponents[0].Get()
        : nullptr;
    UAudioComponent* ReferenceAudio = State.ReferenceAudioComponent.Get();
    if (!IsValid(RenderedAudio) || !IsValid(ReferenceAudio))
    {
        return;
    }

    const bool bNeedsSynchronizedRestart =
        !RenderedAudio->IsPlaying()
        || !ReferenceAudio->IsPlaying();
    State.bRenderedABEnabled = bRenderedEnabled;
    if (bNeedsSynchronizedRestart
        && !EnsureSynchronizedABPlayback(
            State,
            TEXT("A/B mode switch recovered a stopped component"),
            false))
    {
        return;
    }

    constexpr float CrossfadeSeconds = 0.05f;
    const float RenderedFadeTarget = bRenderedEnabled
        ? ValidationActiveFadeLevel
        : ValidationInactiveFadeLevel;
    const float ReferenceFadeTarget = bRenderedEnabled
        ? ValidationInactiveFadeLevel
        : ValidationActiveFadeLevel;
    RenderedAudio->AdjustVolume(
        CrossfadeSeconds,
        RenderedFadeTarget);
    ReferenceAudio->AdjustVolume(
        CrossfadeSeconds,
        ReferenceFadeTarget);
    const bool bBaseLevelsMatched = FMath::IsNearlyEqual(
        RenderedAudio->VolumeMultiplier,
        ReferenceAudio->VolumeMultiplier,
        UE_KINDA_SMALL_NUMBER);
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio interactive audio A/B changed: playback=%s rendered_playing=%d reference_playing=%d crossfade_ms=50 rendered_base=%.3f reference_base=%.3f rendered_fade_target=%.6f reference_fade_target=%.6f base_levels_matched=%d."),
        bRenderedEnabled
            ? TEXT("RENDERED DIRECT+WET")
            : TEXT("ORIGINAL UNRENDERED"),
        RenderedAudio->IsPlaying() ? 1 : 0,
        ReferenceAudio->IsPlaying() ? 1 : 0,
        RenderedAudio->VolumeMultiplier,
        ReferenceAudio->VolumeMultiplier,
        RenderedFadeTarget,
        ReferenceFadeTarget,
        bBaseLevelsMatched ? 1 : 0);
}

int32 FUERayTracingAudioRuntimeValidation::MuteForeignWorldAudio(
    FScenarioState& State)
{
    UWorld* World = State.World.Get();
    if (!IsValid(World))
    {
        return 0;
    }

    int32 MutedCount = 0;
    for (TObjectIterator<UAudioComponent> It; It; ++It)
    {
        UAudioComponent* Audio = *It;
        if (!IsValid(Audio)
            || Audio->IsTemplate()
            || Audio->GetWorld() != World)
        {
            continue;
        }

        bool bValidationAudio = Audio == State.ReferenceAudioComponent.Get();
        for (const TWeakObjectPtr<UAudioComponent>& WeakAudio
            : State.AudioComponents)
        {
            if (Audio == WeakAudio.Get())
            {
                bValidationAudio = true;
                break;
            }
        }
        if (bValidationAudio)
        {
            continue;
        }

        // SetAutoActivate() refuses registered components. This validation-only
        // runtime instance must not restart and mask the controlled A/B pair.
        Audio->bAutoActivate = false;
        Audio->SetVolumeMultiplier(0.0f);
        Audio->Stop();
        ++MutedCount;
    }
    return MutedCount;
}

int32 FUERayTracingAudioRuntimeValidation::CountPlayingForeignWorldAudio(
    const FScenarioState& State) const
{
    const UWorld* World = State.World.Get();
    if (!IsValid(World))
    {
        return 0;
    }

    int32 PlayingCount = 0;
    for (TObjectIterator<UAudioComponent> It; It; ++It)
    {
        const UAudioComponent* Audio = *It;
        if (!IsValid(Audio)
            || Audio->IsTemplate()
            || Audio->GetWorld() != World
            || !Audio->IsPlaying())
        {
            continue;
        }

        bool bValidationAudio = Audio == State.ReferenceAudioComponent.Get();
        for (const TWeakObjectPtr<UAudioComponent>& WeakAudio
            : State.AudioComponents)
        {
            if (Audio == WeakAudio.Get())
            {
                bValidationAudio = true;
                break;
            }
        }
        if (!bValidationAudio)
        {
            ++PlayingCount;
        }
    }
    return PlayingCount;
}

bool FUERayTracingAudioRuntimeValidation::SetInteractiveMode(
    FScenarioState& State,
    const bool bEnabled)
{
    UWorld* World = State.World.Get();
    UUERayTracingAudioListenerComponent* Listener = State.Listener.Get();
    AActor* CameraActor = State.CameraActor.Get();
    APlayerController* PlayerController = IsValid(World)
        ? World->GetFirstPlayerController()
        : nullptr;
    APawn* Pawn = IsValid(PlayerController)
        ? PlayerController->GetPawn()
        : nullptr;
    if (!IsValid(World)
        || !IsValid(Listener)
        || !IsValid(CameraActor)
        || !IsValid(PlayerController)
        || (bEnabled && !IsValid(Pawn)))
    {
        return false;
    }

    AActor* ListenerActor = Listener->GetOwner();
    if (!IsValid(ListenerActor))
    {
        return false;
    }

    if (bEnabled)
    {
        if (!PlaceInteractiveViewAtBakedOrigin(State, *PlayerController, *Pawn))
        {
            return false;
        }
        State.InteractivePawn = Pawn;
        for (int32 AudioIndex = 1; AudioIndex < State.AudioComponents.Num(); ++AudioIndex)
        {
            if (UAudioComponent* SecondaryAudio = State.AudioComponents[AudioIndex].Get())
            {
                SecondaryAudio->AdjustVolume(0.05f, 0.0f);
            }
        }
        SetRenderedABMode(State, State.bRenderedABEnabled);
        if (UStaticMeshComponent* Marker = State.ListenerMarker.Get())
        {
            Marker->SetVisibility(false, true);
        }
    }
    else
    {
        PlayerController->SetViewTarget(CameraActor);
        ListenerActor->SetActorLocationAndRotation(
            State.FixedListenerLocation,
            State.InteractiveStartRotation,
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
        State.InteractivePawn.Reset();
        for (int32 AudioIndex = 1; AudioIndex < State.AudioComponents.Num(); ++AudioIndex)
        {
            if (UAudioComponent* SecondaryAudio = State.AudioComponents[AudioIndex].Get())
            {
                SecondaryAudio->AdjustVolume(0.05f, 0.02f);
            }
        }
        SetRenderedABMode(State, State.bRenderedABEnabled);
        if (UStaticMeshComponent* Marker = State.ListenerMarker.Get())
        {
            Marker->SetVisibility(true, true);
        }
    }

    State.bInteractiveMode = bEnabled;
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio interactive validation mode: enabled=%d listener_follows_player=%d controls=\"WASD+Mouse,F1=Realtime,F2=Baked,F3=RenderedAB,F4=Origin,F5=Hybrid,F6=DirectSweep,F8=View\"."),
        bEnabled ? 1 : 0,
        bEnabled ? 1 : 0);
    return true;
}

void FUERayTracingAudioRuntimeValidation::TickInteractiveControls(FScenarioState& State)
{
    UWorld* World = State.World.Get();
    UUERayTracingAudioSourceComponent* PrimarySource = State.Source.Get();
    APlayerController* PlayerController = IsValid(World)
        ? World->GetFirstPlayerController()
        : nullptr;
    if (!IsValid(World) || !IsValid(PrimarySource) || !IsValid(PlayerController))
    {
        return;
    }

    const bool bInteractiveReady =
        State.bDataSourceValidationLogged
        && (!State.bBakeRepeatabilityEnabled
            || !State.bDataSourceValidationPassed
            || State.bBakeRepeatabilityLogged);
    if (bInteractiveReady && !State.bInteractiveReadyLogged)
    {
        State.bInteractiveReadyLogged = true;
        UE_LOG(
            LogUERayTracingAudio,
            Display,
            TEXT("UERayTracingAudio interactive validation ready: fixed_camera=1 interactive_available=1 acoustic_gate_passed=%d mode_hotkeys=F1,F2,F5 ab_hotkey=F3 direct_sweep_hotkey=F6 origin_hotkey=F4 view_hotkey=F8."),
            State.bDataSourceValidationPassed ? 1 : 0);
    }

    if (State.bInteractiveRequested && bInteractiveReady && !State.bInteractiveMode)
    {
        SetInteractiveMode(State, true);
    }

    // Original/Rendered comparison is a playback control, not a movement or
    // hardware-pass privilege. Keep F3 usable while the fixed-camera acoustic
    // gate is running or after it reports a failure.
    if (PlayerController->WasInputKeyJustPressed(EKeys::F3))
    {
        if (State.bABPlaybackStarted)
        {
            const bool bEnableRendered = !State.bRenderedABEnabled;
            if (bEnableRendered && !State.bRenderedPlaybackReady)
            {
                UE_LOG(
                    LogUERayTracingAudio,
                    Warning,
                    TEXT("UERayTracingAudio F3 rendered playback is waiting for a valid Direct/Indirect stereo-IR audio snapshot; Original remains audible."));
            }
            else
            {
                if (bEnableRendered && State.bDataSourceValidationLogged)
                {
                    // After the gate, F3 is the stable
                    // Original/Realtime-rendered comparison.
                    SetInteractiveDataSource(
                        State,
                        EUERayTracingAudioIndirectDataSource::Realtime);
                }
                SetRenderedABMode(State, bEnableRendered);
            }
        }
        else
        {
            UE_LOG(
                LogUERayTracingAudio,
                Warning,
            TEXT("UERayTracingAudio F3 A/B is waiting for synchronized audio playback to start."));
        }
    }

    if (PlayerController->WasInputKeyJustPressed(EKeys::F6))
    {
        if (IsDirectSweepActive(State))
        {
            UE_LOG(
                LogUERayTracingAudio,
                Warning,
                TEXT("UERayTracingAudio F6 direct sweep rejected: non_reentrant=1 phase=%s."),
                GetDirectSweepPhaseName(
                    State.DirectSweepPhase));
        }
        else if (!bInteractiveReady
            || !State.bInteractiveMode)
        {
            UE_LOG(
                LogUERayTracingAudio,
                Warning,
                TEXT("UERayTracingAudio F6 direct sweep requires interactive validation after the hardware IR mode gate."));
        }
        else if (!State.bValidationOwner)
        {
            UE_LOG(
                LogUERayTracingAudio,
                Warning,
                TEXT("UERayTracingAudio F6 direct sweep is reserved for the process validation-owner World."));
        }
        else if (ActiveDirectSweepWorld.IsValid()
            && ActiveDirectSweepWorld != State.World)
        {
            UE_LOG(
                LogUERayTracingAudio,
                Warning,
                TEXT("UERayTracingAudio F6 direct sweep rejected: another World owns the active sweep."));
        }
        else
        {
            APawn* Pawn = State.InteractivePawn.Get();
            FUERayTracingAudioManager& Manager =
                FUERayTracingAudioModule::GetManager();
            FUERayTracingAudioSourceSimulationResult
                LatestResult;
            UUERayTracingAudioListenerComponent* Listener =
                State.Listener.Get();
            UAudioComponent* PrimaryAudio =
                State.AudioComponents.IsValidIndex(0)
                    ? State.AudioComponents[0].Get()
                    : nullptr;
            AActor* SourceActor =
                PrimarySource->GetOwner();
            AActor* ListenerActor = IsValid(Listener)
                ? Listener->GetOwner()
                : nullptr;
            const bool bSweepDependenciesReady =
                IsValid(SourceActor)
                && IsValid(Listener)
                && IsValid(ListenerActor)
                && IsValid(PrimaryAudio);
            const bool bHardwareDirectReady =
                Manager.GetLatestSourceSimulation(
                    PrimarySource,
                    LatestResult)
                && LatestResult.bHasDirectResult
                && LatestResult.DirectGeneration != 0
                && LatestResult.DirectResult.
                    bUsedHardwareRayTracing;
            if (!IsValid(Pawn)
                || !bSweepDependenciesReady
                || !bHardwareDirectReady)
            {
                UE_LOG(
                    LogUERayTracingAudio,
                    Warning,
                    TEXT("UERayTracingAudio F6 direct sweep is waiting for valid Source/Listener/Audio actors, an interactive Pawn, and a hardware Direct generation."));
            }
            else if (PlaceInteractiveViewAtBakedOrigin(
                    State,
                    *PlayerController,
                    *Pawn))
            {
                StartDirectSweep(
                    State,
                    Manager,
                    false);
            }
        }
    }

    // The Direct sweep owns Listener position and temporary Source settings.
    // Keep F3 available for audible comparison, but reject movement/view/mode
    // controls until exact-once restoration reaches a fresh Direct generation.
    if (IsDirectSweepActive(State))
    {
        return;
    }

    if (PlayerController->WasInputKeyJustPressed(EKeys::F8))
    {
        if (bInteractiveReady)
        {
            State.bInteractiveRequested = !State.bInteractiveMode;
            SetInteractiveMode(State, !State.bInteractiveMode);
        }
        else
        {
            UE_LOG(
                LogUERayTracingAudio,
                Warning,
                TEXT("UERayTracingAudio interactive validation is waiting for the hardware IR mode gate."));
        }
    }

    if (!State.bInteractiveMode)
    {
        return;
    }

    if (PlayerController->WasInputKeyJustPressed(EKeys::F1))
    {
        SetInteractiveDataSource(
            State,
            EUERayTracingAudioIndirectDataSource::Realtime);
    }
    else if (PlayerController->WasInputKeyJustPressed(EKeys::F2))
    {
        SetInteractiveDataSource(
            State,
            EUERayTracingAudioIndirectDataSource::Baked);
    }
    else if (PlayerController->WasInputKeyJustPressed(EKeys::F5))
    {
        SetInteractiveDataSource(
            State,
            EUERayTracingAudioIndirectDataSource::Hybrid);
    }

    APawn* Pawn = State.InteractivePawn.Get();
    if (PlayerController->WasInputKeyJustPressed(EKeys::F4) && IsValid(Pawn))
    {
        if (PlaceInteractiveViewAtBakedOrigin(State, *PlayerController, *Pawn))
        {
            UE_LOG(
                LogUERayTracingAudio,
                Display,
                TEXT("UERayTracingAudio interactive listener returned to the baked IR origin."));
        }
    }

    UUERayTracingAudioListenerComponent* Listener = State.Listener.Get();
    AActor* ListenerActor = IsValid(Listener) ? Listener->GetOwner() : nullptr;
    APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager;
    if (IsValid(ListenerActor) && IsValid(CameraManager))
    {
        ListenerActor->SetActorLocationAndRotation(
            CameraManager->GetCameraLocation(),
            CameraManager->GetCameraRotation(),
            false,
            nullptr,
            ETeleportType::TeleportPhysics);
    }

    TickInteractiveSmoke(State);
}

void FUERayTracingAudioRuntimeValidation::TickInteractiveSmoke(
    FScenarioState& State)
{
    if (!State.bInteractiveSmokeEnabled
        || State.bInteractiveSmokeLogged
        || !State.bInteractiveMode)
    {
        return;
    }

    UWorld* World = State.World.Get();
    UUERayTracingAudioSourceComponent* PrimarySource = State.Source.Get();
    UUERayTracingAudioListenerComponent* Listener = State.Listener.Get();
    APlayerController* PlayerController = IsValid(World)
        ? World->GetFirstPlayerController()
        : nullptr;
    APawn* Pawn = State.InteractivePawn.Get();
    AActor* ListenerActor = IsValid(Listener) ? Listener->GetOwner() : nullptr;
    APlayerCameraManager* CameraManager = IsValid(PlayerController)
        ? PlayerController->PlayerCameraManager
        : nullptr;
    if (!IsValid(World)
        || !IsValid(PrimarySource)
        || !IsValid(PlayerController)
        || !IsValid(Pawn)
        || !IsValid(ListenerActor)
        || !IsValid(CameraManager))
    {
        return;
    }

    const double NowSeconds = FPlatformTime::Seconds();
    State.InteractiveSmokeMaxListenerCameraErrorCm = FMath::Max(
        State.InteractiveSmokeMaxListenerCameraErrorCm,
        FVector::Distance(
            ListenerActor->GetActorLocation(),
            CameraManager->GetCameraLocation()));

    if (State.InteractiveSmokePhase == 0)
    {
        State.InteractiveSmokeStartPawnLocation = Pawn->GetActorLocation();
        State.InteractiveSmokePhaseStartTimeSeconds = NowSeconds;
        SetInteractiveDataSource(
            State,
            EUERayTracingAudioIndirectDataSource::Realtime);
        State.bInteractiveSmokeSawRealtime =
            PrimarySource->IndirectDataSource
            == EUERayTracingAudioIndirectDataSource::Realtime;
        State.InteractiveSmokePhase = 1;
        return;
    }

    const double PhaseElapsedSeconds =
        NowSeconds - State.InteractiveSmokePhaseStartTimeSeconds;
    if (State.InteractiveSmokePhase == 1)
    {
        Pawn->AddMovementInput(Pawn->GetActorForwardVector(), 1.0f);
        if (PhaseElapsedSeconds < 1.0)
        {
            return;
        }

        State.InteractiveSmokeMovementDistanceCm = FVector::Distance(
            Pawn->GetActorLocation(),
            State.InteractiveSmokeStartPawnLocation);
        SetInteractiveDataSource(
            State,
            EUERayTracingAudioIndirectDataSource::Baked);
        State.bInteractiveSmokeSawBaked =
            PrimarySource->IndirectDataSource
            == EUERayTracingAudioIndirectDataSource::Baked;
        State.InteractiveSmokePhase = 2;
        State.InteractiveSmokePhaseStartTimeSeconds = NowSeconds;
        return;
    }

    if (State.InteractiveSmokePhase == 2 && PhaseElapsedSeconds >= 0.25)
    {
        SetInteractiveDataSource(
            State,
            EUERayTracingAudioIndirectDataSource::Hybrid);
        State.bInteractiveSmokeSawHybrid =
            PrimarySource->IndirectDataSource
            == EUERayTracingAudioIndirectDataSource::Hybrid;
        State.InteractiveSmokePhase = 3;
        State.InteractiveSmokePhaseStartTimeSeconds = NowSeconds;
        return;
    }

    if (State.InteractiveSmokePhase == 3 && PhaseElapsedSeconds >= 0.25)
    {
        SetRenderedABMode(State, false);
        State.bInteractiveSmokeSawReferenceAB = !State.bRenderedABEnabled;
        State.InteractiveSmokePhase = 4;
        State.InteractiveSmokePhaseStartTimeSeconds = NowSeconds;
        return;
    }

    if (State.InteractiveSmokePhase == 4 && PhaseElapsedSeconds >= 0.25)
    {
        if (State.bRenderedPlaybackReady)
        {
            SetRenderedABMode(State, true);
        }
        State.bInteractiveSmokeSawRenderedAB =
            State.bRenderedPlaybackReady
            && State.bRenderedABEnabled;
        PlaceInteractiveViewAtBakedOrigin(
            State,
            *PlayerController,
            *Pawn);
        State.InteractiveSmokePhase = 5;
        State.InteractiveSmokePhaseStartTimeSeconds = NowSeconds;
        return;
    }

    if (State.InteractiveSmokePhase != 5
        || PhaseElapsedSeconds < ValidationABLoopObservationSeconds)
    {
        return;
    }

    const float OriginErrorCm = FVector::Distance(
        Pawn->GetPawnViewLocation(),
        State.FixedListenerLocation);
    const bool bFixedView =
        SetInteractiveMode(State, false)
        && PlayerController->GetViewTarget() == State.CameraActor.Get();
    const bool bInteractiveView =
        SetInteractiveMode(State, true)
        && PlayerController->GetViewTarget() == Pawn;
    const UAudioComponent* PrimaryAudio =
        State.AudioComponents.IsValidIndex(0)
        ? State.AudioComponents[0].Get()
        : nullptr;
    const bool bAudioPlaying =
        IsValid(PrimaryAudio)
        && PrimaryAudio->IsPlaying();
    const UAudioComponent* ReferenceAudio = State.ReferenceAudioComponent.Get();
    const bool bReferencePlaying =
        IsValid(ReferenceAudio)
        && ReferenceAudio->IsPlaying();
    const bool bABBaseLevelsMatched =
        IsValid(PrimaryAudio)
        && IsValid(ReferenceAudio)
        && FMath::IsNearlyEqual(
            PrimaryAudio->VolumeMultiplier,
            ReferenceAudio->VolumeMultiplier,
            UE_KINDA_SMALL_NUMBER);
    const int32 ForeignAudioPlayingCount =
        CountPlayingForeignWorldAudio(State);
    const bool bPassed =
        State.InteractiveSmokeMovementDistanceCm >= 25.0f
        && State.InteractiveSmokeMaxListenerCameraErrorCm <= 1.0f
        && OriginErrorCm <= 1.0f
        && State.bInteractiveSmokeSawRealtime
        && State.bInteractiveSmokeSawBaked
        && State.bInteractiveSmokeSawHybrid
        && State.bInteractiveSmokeSawRenderedAB
        && State.bInteractiveSmokeSawReferenceAB
        && bFixedView
        && bInteractiveView
        && bAudioPlaying
        && bReferencePlaying
        && bABBaseLevelsMatched
        && State.ABPlaybackRestartCount == 0
        && ForeignAudioPlayingCount == 0;
    State.bInteractiveSmokeLogged = true;
    UE_LOG(
        LogUERayTracingAudio,
        Display,
        TEXT("UERayTracingAudio interactive smoke: passed=%d moved_cm=%.3f listener_camera_error_cm=%.3f origin_error_cm=%.3f realtime=%d baked=%d hybrid=%d rendered_ab=%d reference_ab=%d fixed_view=%d interactive_view=%d audio_playing=%d reference_playing=%d ab_base_levels_matched=%d ab_restart_count=%d foreign_audio_playing=%d muted_foreign_audio=%d."),
        bPassed ? 1 : 0,
        State.InteractiveSmokeMovementDistanceCm,
        State.InteractiveSmokeMaxListenerCameraErrorCm,
        OriginErrorCm,
        State.bInteractiveSmokeSawRealtime ? 1 : 0,
        State.bInteractiveSmokeSawBaked ? 1 : 0,
        State.bInteractiveSmokeSawHybrid ? 1 : 0,
        State.bInteractiveSmokeSawRenderedAB ? 1 : 0,
        State.bInteractiveSmokeSawReferenceAB ? 1 : 0,
        bFixedView ? 1 : 0,
        bInteractiveView ? 1 : 0,
        bAudioPlaying ? 1 : 0,
        bReferencePlaying ? 1 : 0,
        bABBaseLevelsMatched ? 1 : 0,
        State.ABPlaybackRestartCount,
        ForeignAudioPlayingCount,
        State.MutedForeignAudioComponentCount);
}

void FUERayTracingAudioRuntimeValidation::TickDataSourceValidation(
    FScenarioState& State,
    FUERayTracingAudioManager& Manager)
{
    UUERayTracingAudioSourceComponent* Source = State.Source.Get();
    UUERayTracingAudioListenerComponent* Listener = State.Listener.Get();
    UAudioComponent* Audio = State.AudioComponents.IsValidIndex(0)
        ? State.AudioComponents[0].Get()
        : nullptr;
    const double NowSeconds = FPlatformTime::Seconds();

    auto LogResult = [&State, &Manager, Audio, Source](
        const bool bPassed,
        const FString& Reason)
    {
        const FUERayTracingAudioHardRealtimeStats
            HardRealtimeStats =
                FUERayTracingAudioAudioDiagnostics::
                    ReadHardRealtime();
        const bool bHardRealtimePassed =
            HardRealtimeStats.AudioCallbackCount > 0
            && HardRealtimeStats.CallbackCapacityMissCount == 0
            && HardRealtimeStats.
                ConvolutionPrepareCapacityDropCount == 0;
        const bool bEffectivePassed =
            bPassed && bHardRealtimePassed;
        const FString EffectiveReason =
            bHardRealtimePassed
            ? Reason
            : FString::Printf(
                TEXT("%s; hard realtime callbacks=%llu capacity_misses=%llu prepare_drops=%llu"),
                *Reason,
                HardRealtimeStats.AudioCallbackCount,
                HardRealtimeStats.CallbackCapacityMissCount,
                HardRealtimeStats.
                    ConvolutionPrepareCapacityDropCount);
        const uint64 NonFiniteCount =
            State.BakedAudioStats.NonFiniteSampleCount
            + State.RealtimeAudioStats.NonFiniteSampleCount
            + State.HybridAudioStats.NonFiniteSampleCount;
        const UUERayTracingAudioValidationSoundWave* PrimaryPlayback =
            State.PrimaryPlayback.Get();
        const UUERayTracingAudioValidationSoundProxy* PrimarySoundProxy =
            State.PrimarySoundProxy.Get();
        const TSharedPtr<
            FUERayTracingAudioValidationSourceBufferListener,
            ESPMode::ThreadSafe> SourceBufferListener =
                State.PrimarySourceBufferListener;
        const FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr
            FinalSnapshot = IsValid(Audio)
                ? Manager.GetSnapshotRegistry().Read(
                    Audio->GetAudioComponentID())
                : FUERayTracingAudioSimulationSnapshotRegistry::
                    FSnapshotPtr();
        UE_LOG(
            LogUERayTracingAudio,
            Display,
            TEXT("UERayTracingAudio hard realtime: passed=%d callbacks=%llu callback_capacity_misses=%llu convolution_prepare_drops=%llu."),
            bHardRealtimePassed ? 1 : 0,
            HardRealtimeStats.AudioCallbackCount,
            HardRealtimeStats.CallbackCapacityMissCount,
            HardRealtimeStats.
                ConvolutionPrepareCapacityDropCount);
        UE_LOG(
            LogUERayTracingAudio,
            Display,
            TEXT("UERayTracingAudio validation audio pipeline: proxy_parses=%llu proxy_waves=%llu proxy_audiolink_overrides=%llu max_actual_volume=%.6f last_volume=%.6f last_multiplier=%.6f last_distance=%.6f last_occlusion=%.6f generator_callbacks=%llu generator_non_silent=%llu pre_distance_buffers=%llu pre_distance_non_silent=%llu."),
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetParseCount()
                : 0,
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetParsedWaveCount()
                : 0,
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetAudioLinkOverrideCount()
                : 0,
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetMaxActualVolume()
                : 0.0f,
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetLastVolume()
                : 0.0f,
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetLastVolumeMultiplier()
                : 0.0f,
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetLastDistanceAttenuation()
                : 0.0f,
            IsValid(PrimarySoundProxy)
                ? PrimarySoundProxy->GetLastOcclusionAttenuation()
                : 0.0f,
            IsValid(PrimaryPlayback)
                ? PrimaryPlayback->GetGeneratorCallbackCount()
                : 0,
            IsValid(PrimaryPlayback)
                ? PrimaryPlayback->GetNonSilentGeneratorCallbackCount()
                : 0,
            SourceBufferListener.IsValid()
                ? SourceBufferListener->GetBufferCount()
                : 0,
            SourceBufferListener.IsValid()
                ? SourceBufferListener->GetNonSilentBufferCount()
                : 0);
        UE_LOG(
            LogUERayTracingAudio,
            Display,
            TEXT("UERayTracingAudio validation data sources: passed=%d baked_buffers=%llu baked_input_non_silent=%llu baked_non_silent=%llu baked_rms_measured=%llu baked_audible_wet=%llu baked_max_inaudible_run=%llu baked_wet_present=%llu baked_max_silent_run=%llu baked_input_rms=%.6f baked_wet_rms=%.6f baked_wet_input_rms_ratio=%.6f baked_integrated_wet_input_rms_ratio=%.6f baked_full_peak=%.6f baked_over_unit=%llu realtime_buffers=%llu realtime_input_non_silent=%llu realtime_non_silent=%llu realtime_rms_measured=%llu realtime_audible_wet=%llu realtime_max_inaudible_run=%llu realtime_wet_present=%llu realtime_max_silent_run=%llu realtime_input_rms=%.6f realtime_wet_rms=%.6f realtime_wet_input_rms_ratio=%.6f realtime_integrated_wet_input_rms_ratio=%.6f realtime_full_peak=%.6f realtime_over_unit=%llu hybrid_buffers=%llu hybrid_input_non_silent=%llu hybrid_non_silent=%llu hybrid_rms_measured=%llu hybrid_audible_wet=%llu hybrid_max_inaudible_run=%llu hybrid_wet_present=%llu hybrid_max_silent_run=%llu hybrid_input_rms=%.6f hybrid_wet_rms=%.6f hybrid_wet_input_rms_ratio=%.6f hybrid_integrated_wet_input_rms_ratio=%.6f hybrid_full_peak=%.6f hybrid_over_unit=%llu minimum_wet_input_rms_ratio=%.6f minimum_wet_presence_fraction=0.800 non_finite=%llu stereo_ir=%d baked_kernels=%d realtime_kernels=%d hybrid_kernels=%d audio_playing=%d interactive_hybrid_reverb=%d parametric_tail=%d reason=\"%s\"."),
            bEffectivePassed ? 1 : 0,
            State.BakedAudioStats.BufferCount,
            State.BakedAudioStats.NonSilentInputBufferCount,
            State.BakedAudioStats.NonSilentBufferCount,
            State.BakedAudioStats.RmsMeasuredBufferCount,
            State.BakedAudioStats.AudibleWetBufferCount,
            State.BakedAudioStats.MaxConsecutiveInaudibleWetBufferCount,
            State.BakedAudioStats.WetPresentInputBufferCount,
            State.BakedAudioStats.MaxConsecutiveSilentWetBufferCount,
            State.BakedAudioStats.MaxInputRms,
            State.BakedAudioStats.MaxWetRms,
            State.BakedAudioStats.MaxWetToInputRmsRatio,
            State.BakedAudioStats.IntegratedWetToInputRmsRatio,
            State.BakedAudioStats.MaxOutputPeak,
            State.BakedAudioStats.OverUnitOutputSampleCount,
            State.RealtimeAudioStats.BufferCount,
            State.RealtimeAudioStats.NonSilentInputBufferCount,
            State.RealtimeAudioStats.NonSilentBufferCount,
            State.RealtimeAudioStats.RmsMeasuredBufferCount,
            State.RealtimeAudioStats.AudibleWetBufferCount,
            State.RealtimeAudioStats.MaxConsecutiveInaudibleWetBufferCount,
            State.RealtimeAudioStats.WetPresentInputBufferCount,
            State.RealtimeAudioStats.MaxConsecutiveSilentWetBufferCount,
            State.RealtimeAudioStats.MaxInputRms,
            State.RealtimeAudioStats.MaxWetRms,
            State.RealtimeAudioStats.MaxWetToInputRmsRatio,
            State.RealtimeAudioStats.IntegratedWetToInputRmsRatio,
            State.RealtimeAudioStats.MaxOutputPeak,
            State.RealtimeAudioStats.OverUnitOutputSampleCount,
            State.HybridAudioStats.BufferCount,
            State.HybridAudioStats.NonSilentInputBufferCount,
            State.HybridAudioStats.NonSilentBufferCount,
            State.HybridAudioStats.RmsMeasuredBufferCount,
            State.HybridAudioStats.AudibleWetBufferCount,
            State.HybridAudioStats.MaxConsecutiveInaudibleWetBufferCount,
            State.HybridAudioStats.WetPresentInputBufferCount,
            State.HybridAudioStats.MaxConsecutiveSilentWetBufferCount,
            State.HybridAudioStats.MaxInputRms,
            State.HybridAudioStats.MaxWetRms,
            State.HybridAudioStats.MaxWetToInputRmsRatio,
            State.HybridAudioStats.IntegratedWetToInputRmsRatio,
            State.HybridAudioStats.MaxOutputPeak,
            State.HybridAudioStats.OverUnitOutputSampleCount,
            ValidationMinimumWetToInputRmsRatio,
            NonFiniteCount,
            State.bStereoBakedIrObserved ? 1 : 0,
            State.BakedKernelCount,
            State.RealtimeKernelCount,
            State.HybridKernelCount,
            IsValid(Audio) && Audio->IsPlaying() ? 1 : 0,
            IsValid(Source)
                && Source->IndirectMode
                    == EUERayTracingAudioIndirectMode::HybridReverb
                ? 1
                : 0,
            FinalSnapshot.IsValid()
                && FinalSnapshot->IndirectResult.bUsedParametricTail
                ? 1
                : 0,
            *EffectiveReason);
        State.bDataSourceValidationLogged = true;
        State.bDataSourceValidationPassed =
            bEffectivePassed;
        State.DataSourceBakeJob.Reset();
    };

    auto FailValidation = [&LogResult](const FString& Reason)
    {
        LogResult(false, Reason);
    };

    if (!IsValid(Source) || !IsValid(Listener) || !IsValid(Audio))
    {
        FailValidation(TEXT("source, listener, or primary audio component became invalid"));
        return;
    }
    if (!State.bABPlaybackStarted || !Audio->IsPlaying())
    {
        if (State.AudioStartTimeSeconds > 0.0
            && NowSeconds - State.AudioStartTimeSeconds
                > ValidationDataSourceTimeoutSeconds)
        {
            FailValidation(TEXT("primary validation SoundWave did not remain in active playback"));
        }
        return;
    }

    if (State.DataSourceValidationPhase == 0)
    {
        for (int32 AudioIndex = 1;
            !State.bPerformanceProfile && AudioIndex < State.AudioComponents.Num();
            ++AudioIndex)
        {
            if (UAudioComponent* SecondaryAudio = State.AudioComponents[AudioIndex].Get())
            {
                SecondaryAudio->Stop();
            }
        }
        if (!State.bPerformanceProfile && State.AudioComponents.Num() > 1)
        {
            State.bAllAudioChainsStarted = false;
        }
        State.DataSourceBakeJob = Manager.StartImpulseResponseBake(
            Source,
            Listener,
            MakeValidationBakeSettings());
        State.DataSourceValidationPhase = 1;
        State.DataSourcePhaseStartTimeSeconds = NowSeconds;
        UE_LOG(
            LogUERayTracingAudio,
            Display,
            TEXT("UERayTracingAudio validation data-source bake started during audible playback: rays=%d bounces=%d duration=%.3f sample_rate=%d."),
            ValidationBakeRays,
            ValidationBakeBounces,
            ValidationBakeDurationSeconds,
            ValidationBakeSampleRate);
        return;
    }

    if (NowSeconds - State.DataSourcePhaseStartTimeSeconds > ValidationDataSourceTimeoutSeconds)
    {
        FailValidation(FString::Printf(
            TEXT("phase %d exceeded %.1f seconds"),
            State.DataSourceValidationPhase,
            ValidationDataSourceTimeoutSeconds));
        return;
    }

    if (State.DataSourceValidationPhase == 1)
    {
        if (!State.DataSourceBakeJob.IsValid())
        {
            FailValidation(TEXT("hardware Bake job was not created"));
            return;
        }
        const EUERayTracingAudioBakeJobState JobState = State.DataSourceBakeJob->GetState();
        if (JobState == EUERayTracingAudioBakeJobState::Pending
            || JobState == EUERayTracingAudioBakeJobState::Running)
        {
            return;
        }
        if (JobState != EUERayTracingAudioBakeJobState::Completed)
        {
            FailValidation(FString::Printf(
                TEXT("hardware Bake failed: %s"),
                *State.DataSourceBakeJob->GetError()));
            return;
        }

        FUERayTracingAudioBakeResult Result;
        if (!State.DataSourceBakeJob->GetResult(Result))
        {
            FailValidation(TEXT("completed hardware Bake did not expose an IR result"));
            return;
        }
        const int32 NumChannels = FMath::Max(Result.NumChannels, 1);
        const int32 NumFrames = Result.Samples.Num() / NumChannels;
        const int32 ExpectedFrames = FMath::RoundToInt(
            ValidationBakeDurationSeconds * static_cast<float>(ValidationBakeSampleRate));
        const float DurationSeconds = Result.BinDurationSeconds * static_cast<float>(NumFrames);
        if (!Result.bUsedHardwareRayTracing
            || Result.ChannelFormat != EUERayTracingAudioImpulseResponseChannelFormat::Stereo
            || Result.NumChannels != 2
            || Result.Samples.Num() != ExpectedFrames * Result.NumChannels
            || !AreBakeSamplesFinite(Result.Samples)
            || !FMath::IsFinite(Result.BinDurationSeconds)
            || Result.BinDurationSeconds <= 0.0f
            || !FMath::IsNearlyEqual(DurationSeconds, ValidationBakeDurationSeconds, 1.0e-4f)
            || CalculateBakeEnergy(Result.Samples) <= UE_DOUBLE_SMALL_NUMBER)
        {
            FailValidation(FString::Printf(
                TEXT("hardware Bake returned an invalid stereo IR: hardware=%d channels=%d samples=%d expected=%d duration=%.6f"),
                Result.bUsedHardwareRayTracing ? 1 : 0,
                Result.NumChannels,
                Result.Samples.Num(),
                ExpectedFrames * 2,
                DurationSeconds));
            return;
        }

        UUERayTracingAudioImpulseResponseAsset* RuntimeAsset =
            NewObject<UUERayTracingAudioImpulseResponseAsset>(Source);
        RuntimeAsset->Initialize(
            Result.SourceWorld,
            Result.SceneVersion,
            MoveTemp(Result.SceneSignature),
            Result.SourceLocation,
            Result.ListenerLocation,
            Result.BakeSettings,
            Result.ChannelFormat,
            Result.NumChannels,
            Result.BinDurationSeconds,
            MoveTemp(Result.Samples));
        Source->SetBakedImpulseResponseAsset(RuntimeAsset);
        Source->IndirectMode = EUERayTracingAudioIndirectMode::MinimalConvolution;
        Source->SetIndirectDataSource(
            EUERayTracingAudioIndirectDataSource::Baked);
        State.DataSourceBakeJob.Reset();
        State.DataSourceValidationPhase = 2;
        State.DataSourcePhaseStartTimeSeconds = NowSeconds;
        State.bDataSourceDiagnosticsArmed = false;
        return;
    }

    const FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr Snapshot =
        Manager.GetSnapshotRegistry().Read(Audio->GetAudioComponentID());
    const bool bBakedKernelsValid = Snapshot.IsValid()
        && Snapshot->BakedConvolutionKernel.IsValid()
        && Snapshot->BakedConvolutionKernelRight.IsValid();
    const bool bRealtimeKernelsValid = Snapshot.IsValid()
        && Snapshot->RealtimeConvolutionKernelLeft.IsValid()
        && Snapshot->RealtimeConvolutionKernelRight.IsValid();
    const double PhaseElapsedSeconds = NowSeconds - State.DataSourcePhaseStartTimeSeconds;

    auto ObserveMode = [
        &State,
        NowSeconds,
        PhaseElapsedSeconds](
            const EUERayTracingAudioRuntimeDataSource DataSource,
            FUERayTracingAudioDataSourceAudioStats& OutStats) -> bool
    {
        if (!State.bDataSourceDiagnosticsArmed)
        {
            if (PhaseElapsedSeconds < ValidationDataSourceSettleSeconds)
            {
                return false;
            }
            FUERayTracingAudioAudioDiagnostics::Reset(DataSource);
            State.bDataSourceDiagnosticsArmed = true;
            State.DataSourcePhaseStartTimeSeconds = NowSeconds;
            return false;
        }

        OutStats = FUERayTracingAudioAudioDiagnostics::Read(DataSource);
        const bool bWetPresentForMostMeasuredBuffers =
            OutStats.RmsMeasuredBufferCount
                >= ValidationDataSourceMinimumBufferCount
            && OutStats.WetPresentInputBufferCount
                    * ValidationMinimumWetPresenceDenominator
                >= OutStats.RmsMeasuredBufferCount
                    * ValidationMinimumWetPresenceNumerator;
        const bool bNoLongSilentRun =
            OutStats.MaxConsecutiveSilentWetBufferCount
                    * ValidationMinimumWetPresenceDenominator
                <= OutStats.RmsMeasuredBufferCount;
        return OutStats.BufferCount >= ValidationDataSourceMinimumBufferCount
            && OutStats.NonSilentInputBufferCount > 0
            && OutStats.NonSilentBufferCount > 0
            && bWetPresentForMostMeasuredBuffers
            && bNoLongSilentRun
            && OutStats.IntegratedWetToInputRmsRatio
                >= ValidationMinimumWetToInputRmsRatio
            && OutStats.MaxWetToInputRmsRatio
                >= ValidationMinimumWetToInputRmsRatio
            && OutStats.MaxOutputPeak <= 1.0f + UE_KINDA_SMALL_NUMBER
            && OutStats.OverUnitOutputSampleCount == 0
            && OutStats.NonFiniteSampleCount == 0;
    };

    if (State.DataSourceValidationPhase == 2)
    {
        if (Snapshot.IsValid()
            && Snapshot->DataSource
                == EUERayTracingAudioRuntimeDataSource::Baked)
        {
            State.BakedKernelCount =
                (Snapshot->BakedConvolutionKernel.IsValid() ? 1 : 0)
                + (Snapshot->BakedConvolutionKernelRight.IsValid() ? 1 : 0);
            State.bStereoBakedIrObserved = State.BakedKernelCount == 2;
        }
        const bool bSnapshotReady = Snapshot.IsValid()
            && Snapshot->DataSource == EUERayTracingAudioRuntimeDataSource::Baked
            && bBakedKernelsValid
            && !bRealtimeKernelsValid
            && Source->BakedAssetStatus == EUERayTracingAudioBakedAssetStatus::Ready;
        if (bSnapshotReady
            && ObserveMode(
                EUERayTracingAudioRuntimeDataSource::Baked,
                State.BakedAudioStats))
        {
            // Baked convolution intentionally has no parametric tail. Restore
            // HybridReverb before validating realtime so reflected energy and
            // the late parametric tail remain continuously audible.
            Source->IndirectMode =
                EUERayTracingAudioIndirectMode::HybridReverb;
            Source->SetIndirectDataSource(
                EUERayTracingAudioIndirectDataSource::Realtime);
            State.DataSourceValidationPhase = 3;
            State.DataSourcePhaseStartTimeSeconds = NowSeconds;
            State.bDataSourceDiagnosticsArmed = false;
        }
        return;
    }

    if (State.DataSourceValidationPhase == 3)
    {
        if (Snapshot.IsValid()
            && Snapshot->DataSource
                == EUERayTracingAudioRuntimeDataSource::Realtime)
        {
            State.RealtimeKernelCount =
                (Snapshot->RealtimeConvolutionKernelLeft.IsValid() ? 1 : 0)
                + (Snapshot->RealtimeConvolutionKernelRight.IsValid() ? 1 : 0);
        }
        const bool bSnapshotReady = Snapshot.IsValid()
            && Snapshot->DataSource == EUERayTracingAudioRuntimeDataSource::Realtime
            && !bBakedKernelsValid
            && bRealtimeKernelsValid
            && Snapshot->IndirectResult.bUsedParametricTail;
        if (bSnapshotReady
            && ObserveMode(
                EUERayTracingAudioRuntimeDataSource::Realtime,
                State.RealtimeAudioStats))
        {
            Source->SetIndirectDataSource(
                EUERayTracingAudioIndirectDataSource::Hybrid);
            State.DataSourceValidationPhase = 4;
            State.DataSourcePhaseStartTimeSeconds = NowSeconds;
            State.bDataSourceDiagnosticsArmed = false;
        }
        return;
    }

    if (State.DataSourceValidationPhase == 4)
    {
        if (Snapshot.IsValid()
            && Snapshot->DataSource
                == EUERayTracingAudioRuntimeDataSource::Hybrid)
        {
            State.HybridKernelCount =
                (Snapshot->BakedConvolutionKernel.IsValid() ? 1 : 0)
                + (Snapshot->BakedConvolutionKernelRight.IsValid() ? 1 : 0)
                + (Snapshot->RealtimeConvolutionKernelLeft.IsValid() ? 1 : 0)
                + (Snapshot->RealtimeConvolutionKernelRight.IsValid() ? 1 : 0);
        }
        const bool bSnapshotReady = Snapshot.IsValid()
            && Snapshot->DataSource == EUERayTracingAudioRuntimeDataSource::Hybrid
            && bBakedKernelsValid
            && bRealtimeKernelsValid
            && Snapshot->IndirectResult.bUsedParametricTail
            && Source->BakedAssetStatus == EUERayTracingAudioBakedAssetStatus::Ready;
        if (bSnapshotReady
            && ObserveMode(
                EUERayTracingAudioRuntimeDataSource::Hybrid,
                State.HybridAudioStats))
        {
            // F3 is defined as Original versus realtime Full rendering. Leave
            // the interactive scene in that stable mode after the Baked and
            // Hybrid validation phases have completed.
            Source->IndirectMode =
                EUERayTracingAudioIndirectMode::HybridReverb;
            Source->SetIndirectDataSource(
                EUERayTracingAudioIndirectDataSource::Realtime);
            Source->IndirectMix = 1.75f;
            LogResult(
                true,
                TEXT("hardware stereo Bake and all continuously audible runtime IR modes passed"));
        }
    }
}

void FUERayTracingAudioRuntimeValidation::TickBakeRepeatability(
    FScenarioState& State,
    FUERayTracingAudioManager& Manager)
{
    UUERayTracingAudioSourceComponent* Source = State.Source.Get();
    UUERayTracingAudioListenerComponent* Listener = State.Listener.Get();
    auto FailBakeValidation = [&State](const FString& Reason)
    {
        UE_LOG(
            LogUERayTracingAudio,
            Error,
            TEXT("UERayTracingAudio validation bake repeatability: passed=0 pass=%d reason=%s."),
            State.BakePassIndex + 1,
            *Reason);
        State.bBakeRepeatabilityLogged = true;
        State.BakeJob.Reset();
    };

    if (!IsValid(Source) || !IsValid(Listener))
    {
        FailBakeValidation(TEXT("source or listener became invalid"));
        return;
    }

    if (!State.BakeJob.IsValid())
    {
        if (State.BakePassIndex >= 2)
        {
            FailBakeValidation(TEXT("invalid bake pass state"));
            return;
        }
        if (State.BakePassIndex == 0)
        {
            State.BakeStartTimeSeconds = FPlatformTime::Seconds();
        }
        State.BakeJob = Manager.StartImpulseResponseBake(
            Source,
            Listener,
            MakeValidationBakeSettings());
        UE_LOG(
            LogUERayTracingAudio,
            Display,
            TEXT("UERayTracingAudio validation hardware bake pass %d started: rays=%d bounces=%d duration=%.3f sample_rate=%d."),
            State.BakePassIndex + 1,
            ValidationBakeRays,
            ValidationBakeBounces,
            ValidationBakeDurationSeconds,
            ValidationBakeSampleRate);
        return;
    }

    const EUERayTracingAudioBakeJobState JobState = State.BakeJob->GetState();
    if (JobState == EUERayTracingAudioBakeJobState::Pending
        || JobState == EUERayTracingAudioBakeJobState::Running)
    {
        return;
    }
    if (JobState == EUERayTracingAudioBakeJobState::Failed)
    {
        FailBakeValidation(FString::Printf(TEXT("GPU bake failed: %s"), *State.BakeJob->GetError()));
        return;
    }
    if (JobState == EUERayTracingAudioBakeJobState::Cancelled)
    {
        FailBakeValidation(TEXT("GPU bake was cancelled"));
        return;
    }

    FUERayTracingAudioBakeResult Result;
    if (!State.BakeJob->GetResult(Result))
    {
        FailBakeValidation(TEXT("completed job did not expose a result"));
        return;
    }

    const int32 ExpectedFrames = FMath::RoundToInt(
        ValidationBakeDurationSeconds * static_cast<float>(ValidationBakeSampleRate));
    const int32 NumChannels = FMath::Max(Result.NumChannels, 1);
    const int32 ExpectedSamples = ExpectedFrames * NumChannels;
    const double Energy = CalculateBakeEnergy(Result.Samples);
    const float DurationSeconds = Result.BinDurationSeconds
        * static_cast<float>(Result.Samples.Num() / NumChannels);
    if (Result.Samples.Num() != ExpectedSamples
        || !AreBakeSamplesFinite(Result.Samples)
        || !FMath::IsFinite(Result.BinDurationSeconds)
        || Result.BinDurationSeconds <= 0.0f
        || !FMath::IsNearlyEqual(DurationSeconds, ValidationBakeDurationSeconds, 1.0e-4f)
        || !FMath::IsFinite(Energy)
        || Energy <= UE_DOUBLE_SMALL_NUMBER)
    {
        FailBakeValidation(FString::Printf(
            TEXT("invalid IR contract: samples=%d expected=%d duration=%.6f energy=%.9g"),
            Result.Samples.Num(),
            ExpectedSamples,
            DurationSeconds,
            Energy));
        return;
    }

    if (State.BakePassIndex == 0)
    {
        State.FirstBakeSamples = Result.Samples;
        State.FirstBakeEnergy = Energy;
        State.FirstBakeBinDurationSeconds = Result.BinDurationSeconds;
        State.BakePassIndex = 1;
        State.BakeJob.Reset();
        return;
    }

    double DifferenceEnergy = 0.0;
    if (State.FirstBakeSamples.Num() == Result.Samples.Num())
    {
        for (int32 SampleIndex = 0; SampleIndex < Result.Samples.Num(); ++SampleIndex)
        {
            const double Difference = static_cast<double>(State.FirstBakeSamples[SampleIndex])
                - static_cast<double>(Result.Samples[SampleIndex]);
            DifferenceEnergy += Difference * Difference;
        }
    }
    else
    {
        DifferenceEnergy = TNumericLimits<double>::Max();
    }

    const double EnergyRelativeDelta = FMath::Abs(State.FirstBakeEnergy - Energy)
        / FMath::Max(FMath::Max(State.FirstBakeEnergy, Energy), UE_DOUBLE_SMALL_NUMBER);
    const double SampleRelativeRms = FMath::Sqrt(
        DifferenceEnergy / FMath::Max(State.FirstBakeEnergy, UE_DOUBLE_SMALL_NUMBER));
    const bool bPassed = State.FirstBakeSamples.Num() == Result.Samples.Num()
        && FMath::IsNearlyEqual(
            State.FirstBakeBinDurationSeconds,
            Result.BinDurationSeconds,
            1.0e-7f)
        && EnergyRelativeDelta <= ValidationBakeRelativeTolerance
        && SampleRelativeRms <= ValidationBakeRelativeTolerance;
    const double ElapsedMilliseconds =
        (FPlatformTime::Seconds() - State.BakeStartTimeSeconds) * 1000.0;

    if (bPassed)
    {
        UE_LOG(
            LogUERayTracingAudio,
            Display,
            TEXT("UERayTracingAudio validation bake repeatability: passed=1 samples=%d duration=%.6f first_energy=%.9g second_energy=%.9g energy_relative_delta=%.6f sample_relative_rms=%.6f tolerance=%.3f elapsed_ms=%.2f."),
            Result.Samples.Num(),
            DurationSeconds,
            State.FirstBakeEnergy,
            Energy,
            EnergyRelativeDelta,
            SampleRelativeRms,
            ValidationBakeRelativeTolerance,
            ElapsedMilliseconds);
    }
    else
    {
        UE_LOG(
            LogUERayTracingAudio,
            Error,
            TEXT("UERayTracingAudio validation bake repeatability: passed=0 samples=%d first_samples=%d duration=%.6f first_energy=%.9g second_energy=%.9g energy_relative_delta=%.6f sample_relative_rms=%.6f tolerance=%.3f."),
            Result.Samples.Num(),
            State.FirstBakeSamples.Num(),
            DurationSeconds,
            State.FirstBakeEnergy,
            Energy,
            EnergyRelativeDelta,
            SampleRelativeRms,
            ValidationBakeRelativeTolerance);
    }

    State.BakePassIndex = 2;
    State.bBakeRepeatabilityLogged = true;
    State.BakeJob.Reset();
    State.FirstBakeSamples.Reset();
}

#endif
