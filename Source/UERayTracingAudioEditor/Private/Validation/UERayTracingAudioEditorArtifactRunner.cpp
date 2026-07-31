#include "Validation/UERayTracingAudioEditorArtifactRunner.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Assets/UERayTracingAudioImpulseResponseAsset.h"
#include "Bake/UERayTracingAudioBakeJob.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"
#include "UERayTracingAudioModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

bool FUERayTracingAudioEditorArtifactRunner::Start(
    UUERayTracingAudioSourceComponent& InSource,
    UUERayTracingAudioListenerComponent& InListener,
    USoundWave& InInputSoundWave,
    const FString& InDirectPreset,
    const int32 ReflectionBounces,
    FString& OutError,
    const FString& InReflectionEnvironment)
{
    if (BakeJob.IsValid() || bComplete)
    {
        OutError = TEXT("The Editor artifact runner has already been started.");
        return false;
    }

    TArray<uint8> RawPcmBytes;
    uint32 InputSampleRate = 0;
    uint16 NumChannels = 0;
    if (!InInputSoundWave.GetImportedSoundWaveData(RawPcmBytes, InputSampleRate, NumChannels)
        || RawPcmBytes.IsEmpty()
        || RawPcmBytes.Num() % sizeof(int16) != 0
        || InputSampleRate == 0
        || NumChannels == 0)
    {
        OutError = TEXT("The selected project SoundWave has no readable imported PCM16 data.");
        return false;
    }

    FUERayTracingAudioBakeSettings Settings;
    Settings.NumRays = 4096;
    Settings.MaxBounces = FMath::Clamp(ReflectionBounces, 1, 64);
    Settings.DurationSeconds = 1.0f;
    Settings.SampleRate = 16000;
    Settings.bRequireHardwareRayTracing = true;

    Source = &InSource;
    Listener = &InListener;
    InputSoundWave = &InInputSoundWave;
    DirectPreset = InDirectPreset.IsEmpty() ? TEXT("custom") : InDirectPreset;
    ReflectionEnvironment = InReflectionEnvironment.IsEmpty() ? TEXT("enclosed") : InReflectionEnvironment;
    Result.InputAssetPath = InInputSoundWave.GetPathName();
    BakeJob = FUERayTracingAudioModule::GetManager().StartImpulseResponseBake(
        &InSource,
        &InListener,
        Settings,
        true);
    if (!BakeJob.IsValid() || BakeJob->GetState() == EUERayTracingAudioBakeJobState::Failed)
    {
        OutError = BakeJob.IsValid()
            ? BakeJob->GetError()
            : TEXT("The hardware A/B validation bake job could not be created.");
        BakeJob.Reset();
        return false;
    }

    OutError.Reset();
    return true;
}

void FUERayTracingAudioEditorArtifactRunner::Tick()
{
    if (bComplete || !BakeJob.IsValid())
    {
        return;
    }

    if (BakeJob->GetState() == EUERayTracingAudioBakeJobState::Completed)
    {
        FinishBake();
    }
    else if (BakeJob->GetState() == EUERayTracingAudioBakeJobState::Failed)
    {
        SetFailed(FString::Printf(TEXT("Hardware Editor A/B bake failed: %s"), *BakeJob->GetError()));
    }
    else if (BakeJob->GetState() == EUERayTracingAudioBakeJobState::Cancelled)
    {
        SetFailed(TEXT("Hardware Editor A/B bake was cancelled."));
    }
}

bool FUERayTracingAudioEditorArtifactRunner::IsComplete() const
{
    return bComplete;
}

const FUERayTracingAudioEditorArtifactResult& FUERayTracingAudioEditorArtifactRunner::GetResult() const
{
    return Result;
}

void FUERayTracingAudioEditorArtifactRunner::FinishBake()
{
    FUERayTracingAudioBakeResult BakeResult;
    if (!BakeJob->GetResult(BakeResult))
    {
        SetFailed(TEXT("The completed Editor A/B bake has no readable result."));
        return;
    }
    if (!BakeResult.bUsedHardwareRayTracing)
    {
        SetFailed(TEXT("The Editor A/B bake did not use hardware ray tracing."));
        return;
    }

    const FString SessionId = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
    const FString RequestedPackageName = FString::Printf(
        TEXT("/Game/UERayTracingAudio/Validation/HardwareIR_%s_%s"),
        *DirectPreset,
        *SessionId);
    FString UniquePackageName;
    FString UniqueAssetName;
    FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(
        RequestedPackageName,
        TEXT(""),
        UniquePackageName,
        UniqueAssetName);
    UPackage* Package = CreatePackage(*UniquePackageName);
    if (!Package)
    {
        SetFailed(TEXT("Could not create the hardware validation IR package."));
        return;
    }

    UUERayTracingAudioImpulseResponseAsset* ImpulseResponseAsset =
        NewObject<UUERayTracingAudioImpulseResponseAsset>(
            Package,
            *UniqueAssetName,
            RF_Public | RF_Standalone | RF_Transactional);
    if (!ImpulseResponseAsset)
    {
        SetFailed(TEXT("Could not create the hardware validation IR asset."));
        return;
    }

    TArray<float> OfflineImpulseResponse = BakeResult.Samples;
    ImpulseResponseAsset->Initialize(
        BakeResult.SourceWorld,
        BakeResult.SceneVersion,
        MoveTemp(BakeResult.SceneSignature),
        BakeResult.SourceLocation,
        BakeResult.ListenerLocation,
        BakeResult.BakeSettings,
        BakeResult.ChannelFormat,
        BakeResult.NumChannels,
        BakeResult.BinDurationSeconds,
        MoveTemp(BakeResult.Samples));
    FString ValidationError;
    if (!ImpulseResponseAsset->Validate(ValidationError))
    {
        SetFailed(FString::Printf(TEXT("Hardware validation IR asset is invalid: %s"), *ValidationError));
        return;
    }

    FAssetRegistryModule::AssetCreated(ImpulseResponseAsset);
    Package->MarkPackageDirty();
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        UniquePackageName,
        FPackageName::GetAssetPackageExtension());
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, ImpulseResponseAsset, *PackageFilename, SaveArgs))
    {
        SetFailed(FString::Printf(TEXT("Could not save hardware validation IR asset: %s"), *PackageFilename));
        return;
    }
    Result.ImpulseResponseAssetPath = FString::Printf(
        TEXT("%s.%s"),
        *UniquePackageName,
        *UniqueAssetName);

    USoundWave* SoundWave = InputSoundWave.Get();
    UUERayTracingAudioSourceComponent* SourceComponent = Source.Get();
    UUERayTracingAudioListenerComponent* ListenerComponent = Listener.Get();
    if (!IsValid(SoundWave) || !IsValid(SourceComponent) || !IsValid(ListenerComponent))
    {
        SetFailed(TEXT("The Editor A/B source, listener, or SoundWave became invalid after the bake."));
        return;
    }

    TArray<uint8> RawPcmBytes;
    uint32 InputSampleRate = 0;
    uint16 NumChannels = 0;
    if (!SoundWave->GetImportedSoundWaveData(RawPcmBytes, InputSampleRate, NumChannels)
        || RawPcmBytes.IsEmpty()
        || RawPcmBytes.Num() % sizeof(int16) != 0)
    {
        SetFailed(TEXT("Could not reload imported PCM16 for the hardware validation render."));
        return;
    }

    FUERayTracingAudioOfflineRenderRequest Request;
    Request.InputInterleavedPcm.SetNumUninitialized(RawPcmBytes.Num() / sizeof(int16));
    FMemory::Memcpy(Request.InputInterleavedPcm.GetData(), RawPcmBytes.GetData(), RawPcmBytes.Num());
    Request.InputSampleRate = static_cast<int32>(InputSampleRate);
    Request.NumChannels = static_cast<int32>(NumChannels);
    Request.ImpulseResponse = MoveTemp(OfflineImpulseResponse);
    Request.ImpulseResponseNumChannels = BakeResult.NumChannels;
    Request.OutputSampleRate = BakeResult.BakeSettings.SampleRate;
    Request.DirectGain = BakeResult.DirectResult.OverallGain;
    Request.WetMix = SourceComponent->GetIndirectMix();
    Request.DirectPreset = DirectPreset;
    Request.ReflectionEnvironment = ReflectionEnvironment;
    Request.DirectDistanceCm = BakeResult.DirectResult.DistanceCm;
    Request.DirectVisibility = BakeResult.DirectResult.DirectVisibility;
    Request.DirectOcclusion = BakeResult.DirectResult.Occlusion;
    Request.DirectDistanceAttenuation = BakeResult.DirectResult.DistanceAttenuation;
    Request.DirectAirAbsorptionAverage = (
        BakeResult.DirectResult.AirAbsorption.X
        + BakeResult.DirectResult.AirAbsorption.Y
        + BakeResult.DirectResult.AirAbsorption.Z) / 3.0f;
    Request.ReflectionRayCount = BakeResult.BakeSettings.NumRays;
    Request.ReflectionBounceCount = BakeResult.BakeSettings.MaxBounces;
    Request.HardwareIndirectValidPaths = BakeResult.IndirectValidPathCount;
    Request.HardwareIndirectGain = BakeResult.IndirectGain;
    Request.HardwareEarlyReflectionGain = BakeResult.EarlyReflectionGain;
    Request.HardwareLateReverbGain = BakeResult.LateReverbGain;
    Request.HardwareEarliestArrivalSeconds = BakeResult.EarliestArrivalSeconds;
    Request.HardwareAverageDelaySeconds = BakeResult.AverageDelaySeconds;
    Request.HardwareReverbTimes = BakeResult.ReverbTimes;
    Request.HardwareDominantArrivalDirection = BakeResult.DominantArrivalDirection;
    Request.HardwareDirectionalEnergyRatio = BakeResult.DirectionalEnergyRatio;
    Request.HardwareDirectionalBinCount = BakeResult.DirectionalBinCount;
    Request.HardwareImpulseResponseEnergy = BakeResult.ImpulseResponseEnergy;
    Request.bHasCpuReference = BakeResult.bHasCpuReference;
    Request.CpuReferenceIndirectValidPaths = BakeResult.CpuReferenceValidPathCount;
    Request.CpuReferenceIndirectGain = BakeResult.CpuReferenceIndirectGain;
    Request.CpuReferenceEarlyReflectionGain = BakeResult.CpuReferenceEarlyReflectionGain;
    Request.CpuReferenceLateReverbGain = BakeResult.CpuReferenceLateReverbGain;
    Request.CpuReferenceDominantArrivalDirection = BakeResult.CpuReferenceDominantArrivalDirection;
    Request.CpuReferenceDirectionalEnergyRatio = BakeResult.CpuReferenceDirectionalEnergyRatio;
    Request.CpuReferenceDirectionalBinCount = BakeResult.CpuReferenceDirectionalBinCount;
    Request.CpuReferenceImpulseResponseEnergy = BakeResult.CpuReferenceImpulseResponseEnergy;
    Request.OutputDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("UERayTracingAudio"),
        TEXT("ListeningAcceptance"),
        TEXT("HardwareValidation"),
        SessionId));
    Request.OutputFilenamePrefix = FString::Printf(
        TEXT("%s_%s_%s"),
        *SoundWave->GetName(),
        *DirectPreset,
        *UniqueAssetName);
    Request.InputAssetPath = SoundWave->GetPathName();
    Request.SourceActorPath = SourceComponent->GetOwner()->GetPathName();
    Request.ListenerActorPath = ListenerComponent->GetOwner()->GetPathName();
    Request.SceneSignature = ImpulseResponseAsset->SceneSignature;
    Request.bUsedHardwareRayTracing = true;
    Result.OfflineRender = FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(
        MoveTemp(Request));
    if (!Result.OfflineRender.bSucceeded)
    {
        SetFailed(FString::Printf(
            TEXT("Hardware validation offline render failed: %s"),
            *Result.OfflineRender.Error));
        return;
    }

    const FString ImportDestination = FString::Printf(
        TEXT("/Game/UERayTracingAudio/ValidationAudio/%s"),
        *SessionId);
    const TArray<FString> WaveFiles =
    {
        Result.OfflineRender.ReferenceWaveFilename,
        Result.OfflineRender.DirectWaveFilename,
        Result.OfflineRender.WetWaveFilename,
        Result.OfflineRender.FullWaveFilename
    };
    const TArray<UObject*> ImportedAssets = FAssetToolsModule::GetModule().Get().ImportAssets(
        WaveFiles,
        ImportDestination);
    if (ImportedAssets.Num() != WaveFiles.Num())
    {
        SetFailed(TEXT("One or more hardware validation comparison WAVs could not be imported as SoundWave assets."));
        return;
    }
    for (UObject* ImportedAsset : ImportedAssets)
    {
        if (!IsValid(ImportedAsset))
        {
            SetFailed(TEXT("A hardware validation comparison import returned an invalid asset."));
            return;
        }
        Result.ImportedComparisonAssetPaths.Add(ImportedAsset->GetPathName());
        if (UPackage* ImportedPackage = ImportedAsset->GetOutermost())
        {
            ImportedPackage->MarkPackageDirty();
            const FString ImportedPackageFilename = FPackageName::LongPackageNameToFilename(
                ImportedPackage->GetName(),
                FPackageName::GetAssetPackageExtension());
            FSavePackageArgs ImportedSaveArgs;
            ImportedSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            ImportedSaveArgs.SaveFlags = SAVE_NoError;
            if (!UPackage::SavePackage(
                    ImportedPackage,
                    ImportedAsset,
                    *ImportedPackageFilename,
                    ImportedSaveArgs))
            {
                SetFailed(FString::Printf(
                    TEXT("Could not save imported comparison asset: %s"),
                    *ImportedAsset->GetPathName()));
                return;
            }
        }
    }

    SourceComponent->BakedImpulseResponseAsset = ImpulseResponseAsset;
    SourceComponent->IndirectDataSource = EUERayTracingAudioIndirectDataSource::Hybrid;
    Result.bSucceeded = true;
    bComplete = true;
    BakeJob.Reset();
}

void FUERayTracingAudioEditorArtifactRunner::SetFailed(FString Error)
{
    Result.bSucceeded = false;
    Result.Error = MoveTemp(Error);
    bComplete = true;
    BakeJob.Reset();
}
