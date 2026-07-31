#include "Components/UERayTracingAudioSourceComponent.h"

#include "Assets/UERayTracingAudioImpulseResponseAsset.h"
#include "Audio/UERayTracingAudioConvolution.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "Components/AudioComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "AudioDevice.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "UERayTracingAudioModule.h"

namespace
{
    struct FRuntimeImpulseTailEstimate
    {
        bool bHasTail = false;
        float LateReverbGain = 0.0f;
        float ParametricDelaySeconds = 0.0f;
        FVector ReverbTimes = FVector::ZeroVector;
    };

    int32 GetRuntimeAudioSampleRate(const UWorld* World)
    {
        if (World)
        {
            if (const FAudioDevice* AudioDevice = World->GetAudioDeviceRaw())
            {
                return FMath::Max(FMath::RoundToInt(AudioDevice->GetSampleRate()), 8000);
            }
        }
        return 48000;
    }

    void BuildRuntimeImpulseResponseChannels(
        const UUERayTracingAudioImpulseResponseAsset& Asset,
        const int32 TargetSampleRate,
        TArray<float>& OutLeftSamples,
        TArray<float>& OutRightSamples)
    {
        const int32 NumChannels = FMath::Max(Asset.NumChannels, 1);
        const int32 NumFrames = Asset.Samples.Num() / NumChannels;
        if (NumFrames <= 0)
        {
            OutLeftSamples.Reset();
            OutRightSamples.Reset();
            return;
        }

        TArray<float> LeftSamples;
        TArray<float> RightSamples;
        LeftSamples.SetNumUninitialized(NumFrames);
        RightSamples.SetNumUninitialized(NumFrames);
        for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
        {
            if (Asset.ChannelFormat == EUERayTracingAudioImpulseResponseChannelFormat::Stereo
                && NumChannels >= 2)
            {
                LeftSamples[FrameIndex] = Asset.Samples[(FrameIndex * NumChannels)];
                RightSamples[FrameIndex] = Asset.Samples[(FrameIndex * NumChannels) + 1];
                continue;
            }
            if (Asset.ChannelFormat == EUERayTracingAudioImpulseResponseChannelFormat::FirstOrderAmbisonics)
            {
                LeftSamples[FrameIndex] = Asset.Samples[FrameIndex * NumChannels];
                RightSamples[FrameIndex] = LeftSamples[FrameIndex];
                continue;
            }

            float Sum = 0.0f;
            for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
            {
                Sum += Asset.Samples[(FrameIndex * NumChannels) + ChannelIndex];
            }
            LeftSamples[FrameIndex] = Sum / static_cast<float>(NumChannels);
            RightSamples[FrameIndex] = LeftSamples[FrameIndex];
        }

        const int32 SourceSampleRate = FMath::Max(Asset.BakeSettings.SampleRate, 1);
        if (SourceSampleRate == TargetSampleRate || NumFrames == 1)
        {
            OutLeftSamples = MoveTemp(LeftSamples);
            OutRightSamples = MoveTemp(RightSamples);
            return;
        }

        const int32 NumOutputFrames = FMath::Max(
            FMath::RoundToInt(
                static_cast<double>(NumFrames) * static_cast<double>(TargetSampleRate)
                / static_cast<double>(SourceSampleRate)),
            1);
        OutLeftSamples.SetNumUninitialized(NumOutputFrames);
        OutRightSamples.SetNumUninitialized(NumOutputFrames);
        const double SourceFramesPerOutputFrame =
            static_cast<double>(SourceSampleRate) / static_cast<double>(TargetSampleRate);
        for (int32 OutputFrameIndex = 0; OutputFrameIndex < NumOutputFrames; ++OutputFrameIndex)
        {
            const double SourcePosition = FMath::Min(
                static_cast<double>(OutputFrameIndex) * SourceFramesPerOutputFrame,
                static_cast<double>(NumFrames - 1));
            const int32 FrameA = FMath::FloorToInt(SourcePosition);
            const int32 FrameB = FMath::Min(FrameA + 1, NumFrames - 1);
            const float Alpha = static_cast<float>(SourcePosition - static_cast<double>(FrameA));
            OutLeftSamples[OutputFrameIndex] = FMath::Lerp(LeftSamples[FrameA], LeftSamples[FrameB], Alpha);
            OutRightSamples[OutputFrameIndex] = FMath::Lerp(RightSamples[FrameA], RightSamples[FrameB], Alpha);
        }
    }

    float GetHybridTransitionSeconds(const UUERayTracingAudioSourceComponent& Source)
    {
        return FMath::Max(Source.GetIndirectDurationSeconds(), 0.05f)
            * FMath::Clamp(Source.GetHybridTransitionRatio(), 0.05f, 0.95f);
    }

    void ApplyHybridImpulseWindow(
        TArray<float>& Samples,
        const int32 SampleRate,
        const float TransitionSeconds,
        const bool bKeepEarly)
    {
        if (Samples.IsEmpty() || SampleRate <= 0)
        {
            return;
        }

        // A 10-50 ms complementary linear window avoids double energy when
        // baked and realtime IRs agree while keeping the split free of a hard
        // sample discontinuity.
        const float FadeSeconds = FMath::Clamp(TransitionSeconds * 0.2f, 0.01f, 0.05f);
        const float FadeStartSeconds = FMath::Max(TransitionSeconds - (0.5f * FadeSeconds), 0.0f);
        const float FadeEndSeconds = TransitionSeconds + (0.5f * FadeSeconds);
        for (int32 SampleIndex = 0; SampleIndex < Samples.Num(); ++SampleIndex)
        {
            const float TimeSeconds = static_cast<float>(SampleIndex) / static_cast<float>(SampleRate);
            float EarlyWeight = 0.0f;
            if (TimeSeconds <= FadeStartSeconds)
            {
                EarlyWeight = 1.0f;
            }
            else if (TimeSeconds < FadeEndSeconds)
            {
                EarlyWeight = 1.0f
                    - ((TimeSeconds - FadeStartSeconds)
                        / FMath::Max(FadeEndSeconds - FadeStartSeconds, UE_SMALL_NUMBER));
            }
            Samples[SampleIndex] *= bKeepEarly ? EarlyWeight : (1.0f - EarlyWeight);
        }
    }

    FRuntimeImpulseTailEstimate EstimateRuntimeImpulseTail(
        const TArray<float>& LeftSamples,
        const TArray<float>& RightSamples,
        const int32 SampleRate,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            LeftKernel,
        const FUERayTracingAudioConvolutionKernel::FKernelPtr&
            RightKernel)
    {
        FRuntimeImpulseTailEstimate Estimate;
        const bool bWasTruncated =
            (LeftKernel
                && LeftKernel->WasRuntimeTailTruncated())
            || (RightKernel
                && RightKernel->WasRuntimeTailTruncated());
        if (!bWasTruncated || SampleRate <= 0)
        {
            return Estimate;
        }

        const int32 RuntimeBlockSize = LeftKernel
            ? LeftKernel->GetBlockSize()
            : RightKernel->GetBlockSize();
        const int32 RuntimeSampleLimit =
            RuntimeBlockSize
            * UERayTracingAudioConvolutionLimits::
                MaxRuntimePartitionsPerLane;
        const int32 TaperSamples =
            FMath::Clamp(
                RuntimeBlockSize / 4,
                1,
                256);
        const int32 TailStartSample =
            FMath::Max(
                RuntimeSampleLimit - TaperSamples,
                0);
        const int32 NumSamples =
            FMath::Max(
                LeftSamples.Num(),
                RightSamples.Num());
        double TailEnergy = 0.0;
        for (int32 SampleIndex = TailStartSample;
            SampleIndex < NumSamples;
            ++SampleIndex)
        {
            const double Left = LeftSamples.IsValidIndex(
                SampleIndex)
                && FMath::IsFinite(LeftSamples[SampleIndex])
                ? static_cast<double>(
                    LeftSamples[SampleIndex])
                : 0.0;
            const double Right = RightSamples.IsValidIndex(
                SampleIndex)
                && FMath::IsFinite(RightSamples[SampleIndex])
                ? static_cast<double>(
                    RightSamples[SampleIndex])
                : Left;
            TailEnergy += 0.5
                * ((Left * Left) + (Right * Right));
        }

        Estimate.LateReverbGain =
            FMath::Clamp(
                static_cast<float>(TailEnergy),
                0.0f,
                4.0f);
        Estimate.bHasTail =
            Estimate.LateReverbGain > UE_SMALL_NUMBER;
        if (!Estimate.bHasTail)
        {
            return Estimate;
        }

        Estimate.ParametricDelaySeconds =
            static_cast<float>(TailStartSample)
            / static_cast<float>(SampleRate);
        const float RemainingTailSeconds =
            static_cast<float>(
                FMath::Max(
                    NumSamples - TailStartSample,
                    1))
            / static_cast<float>(SampleRate);
        const float MidReverbTime =
            FMath::Clamp(
                RemainingTailSeconds,
                0.15f,
                30.0f);
        Estimate.ReverbTimes = FVector(
            FMath::Clamp(
                MidReverbTime * 0.85f,
                0.15f,
                30.0f),
            MidReverbTime,
            FMath::Clamp(
                MidReverbTime * 1.15f,
                0.15f,
                30.0f));
        return Estimate;
    }
}

UUERayTracingAudioSourceComponent::UUERayTracingAudioSourceComponent()
    : bEnableDirectSound(true)
    , OccludedGain(0.2f)
    , SourceRadiusCm(30.0f)
    , NumOcclusionSamples(8)
    , bUseVolumetricOcclusion(true)
    , bHardOcclusion(false)
    , AirAbsorptionPerMeter(0.0002f, 0.0006f, 0.0012f)
    , bEnableIndirectSound(true)
    , IndirectMode(EUERayTracingAudioIndirectMode::MinimalConvolution)
    , IndirectDataSource(EUERayTracingAudioIndirectDataSource::Realtime)
    , BakedImpulseResponseAsset(nullptr)
    , bAllowStaleBakedAsset(false)
    , BakedPlacementToleranceCm(1.0f)
    , NumReflectionRays(128)
    , MaxReflectionBounces(2)
    , IndirectDurationSeconds(1.0f)
    , MaxEarlyReflectionTaps(16)
    , HybridTransitionRatio(0.35f)
    , IndirectMix(1.0f)
    , bIsOccluded(false)
    , DistanceAttenuation(1.0f)
    , DirectVisibility(1.0f)
    , OverallGain(1.0f)
    , bHasIndirectPath(false)
    , NumValidReflectionPaths(0)
    , IndirectGain(0.0f)
    , EarlyReflectionGain(0.0f)
    , LateReverbGain(0.0f)
    , AverageReflectionDelaySeconds(0.0f)
    , ReverbTimes(FVector::ZeroVector)
    , BakedAssetStatus(EUERayTracingAudioBakedAssetStatus::NotRequired)
    , BakedAssetStatusMessage(TEXT("Realtime indirect data source does not require a baked asset."))
{
    PrimaryComponentTick.bCanEverTick = true;
}

void UUERayTracingAudioSourceComponent::BeginPlay()
{
    Super::BeginPlay();
    FUERayTracingAudioModule::GetManager().AddSource(this);
}

void UUERayTracingAudioSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    RemoveSimulationSnapshots();
    FUERayTracingAudioModule::GetManager().RemoveSource(this);
    Super::EndPlay(EndPlayReason);
}

void UUERayTracingAudioSourceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    FUERayTracingAudioManager& Manager = FUERayTracingAudioModule::GetManager();
    RefreshBakedConvolutionKernel();
    const bool bRequestRealtimeIndirect = bEnableIndirectSound
        && IndirectDataSource != EUERayTracingAudioIndirectDataSource::Baked;
    Manager.RequestSourceSimulation(this, bEnableDirectSound, bRequestRealtimeIndirect);

    FUERayTracingAudioSourceSimulationResult LatestSimulation;
    const bool bHasLatestSimulation = Manager.GetLatestSourceSimulation(this, LatestSimulation);

    if (!bEnableDirectSound)
    {
        DirectSoundResult = FUERayTracingAudioDirectSimulationResult();
        bIsOccluded = false;
        DistanceAttenuation = 1.0f;
        DirectVisibility = 1.0f;
        OverallGain = 1.0f;
    }
    else if (bHasLatestSimulation && LatestSimulation.bHasDirectResult)
    {
        DirectSoundResult = LatestSimulation.DirectResult;
        bIsOccluded = DirectSoundResult.bIsOccluded;
        DistanceAttenuation = DirectSoundResult.DistanceAttenuation;
        DirectVisibility = DirectSoundResult.DirectVisibility;
        OverallGain = DirectSoundResult.OverallGain;
    }

    if (!bEnableIndirectSound)
    {
        ResetRealtimeConvolutionKernel();
        IndirectSoundResult = FUERayTracingAudioIndirectSimulationResult();
        bHasIndirectPath = false;
        NumValidReflectionPaths = 0;
        IndirectGain = 0.0f;
        EarlyReflectionGain = 0.0f;
        LateReverbGain = 0.0f;
        AverageReflectionDelaySeconds = 0.0f;
        ReverbTimes = FVector::ZeroVector;
        PublishSimulationSnapshot();
        return;
    }

    if (IndirectDataSource == EUERayTracingAudioIndirectDataSource::Baked)
    {
        ResetRealtimeConvolutionKernel();
        ApplyBakedOnlyResult();
    }
    else if (bHasLatestSimulation && LatestSimulation.bHasIndirectResult)
    {
        IndirectSoundResult = LatestSimulation.IndirectResult;
        bHasIndirectPath = IndirectSoundResult.bHasValidPaths;
        NumValidReflectionPaths = IndirectSoundResult.NumValidPaths;
        IndirectGain = IndirectSoundResult.IndirectGain;
        EarlyReflectionGain = IndirectSoundResult.EarlyReflectionGain;
        LateReverbGain = IndirectSoundResult.LateReverbGain;
        AverageReflectionDelaySeconds = IndirectSoundResult.AverageDelaySeconds;
        ReverbTimes = IndirectSoundResult.ReverbTimes;
    }
    RefreshRealtimeConvolutionKernel(LatestSimulation);
    ApplyRuntimeTailFallback();
    bHasIndirectPath =
        IndirectSoundResult.bHasValidPaths;
    IndirectGain = IndirectSoundResult.IndirectGain;
    LateReverbGain =
        IndirectSoundResult.LateReverbGain;
    ReverbTimes = IndirectSoundResult.ReverbTimes;
    PublishSimulationSnapshot();
}

void UUERayTracingAudioSourceComponent::PublishSimulationSnapshot()
{
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        RemoveSimulationSnapshots();
        return;
    }

    TInlineComponentArray<UAudioComponent*> AudioComponents(Owner);
    TArray<uint64> CurrentAudioComponentIds;
    CurrentAudioComponentIds.Reserve(AudioComponents.Num());
    for (const UAudioComponent* AudioComponent : AudioComponents)
    {
        if (IsValid(AudioComponent))
        {
            CurrentAudioComponentIds.Add(AudioComponent->GetAudioComponentID());
        }
    }

    const FUERayTracingAudioConvolutionKernel*
        CurrentBakedKernelIdentity =
            BakedConvolutionKernel.Get();
    const FUERayTracingAudioConvolutionKernel*
        CurrentBakedRightKernelIdentity =
            BakedConvolutionKernelRight.IsValid()
                ? BakedConvolutionKernelRight.Get()
                : BakedConvolutionKernel.Get();
    const FUERayTracingAudioConvolutionKernel*
        CurrentRealtimeLeftKernelIdentity =
            RealtimeConvolutionKernelLeft.Get();
    const FUERayTracingAudioConvolutionKernel*
        CurrentRealtimeRightKernelIdentity =
            RealtimeConvolutionKernelRight.IsValid()
                ? RealtimeConvolutionKernelRight.Get()
                : RealtimeConvolutionKernelLeft.Get();
    const auto AdvanceLaneRevision = [](
        const FUERayTracingAudioConvolutionKernel*&
            PublishedIdentity,
        const FUERayTracingAudioConvolutionKernel*
            CurrentIdentity,
        uint64& Revision)
    {
        if (PublishedIdentity == CurrentIdentity)
        {
            return false;
        }
        PublishedIdentity = CurrentIdentity;
        ++Revision;
        if (Revision == 0)
        {
            ++Revision;
        }
        return true;
    };
    bool bConvolutionSetChanged = false;
    bConvolutionSetChanged |= AdvanceLaneRevision(
        PublishedBakedKernelIdentity,
        CurrentBakedKernelIdentity,
        BakedLeftConvolutionRevision);
    bConvolutionSetChanged |= AdvanceLaneRevision(
        PublishedBakedRightKernelIdentity,
        CurrentBakedRightKernelIdentity,
        BakedRightConvolutionRevision);
    bConvolutionSetChanged |= AdvanceLaneRevision(
        PublishedRealtimeLeftKernelIdentity,
        CurrentRealtimeLeftKernelIdentity,
        RealtimeLeftConvolutionRevision);
    bConvolutionSetChanged |= AdvanceLaneRevision(
        PublishedRealtimeRightKernelIdentity,
        CurrentRealtimeRightKernelIdentity,
        RealtimeRightConvolutionRevision);
    if (bConvolutionSetChanged)
    {
        ++ConvolutionRevision;
        if (ConvolutionRevision == 0)
        {
            ++ConvolutionRevision;
        }
    }
    FUERayTracingAudioConvolutionRevisions LaneRevisions;
    LaneRevisions.BakedLeft =
        BakedLeftConvolutionRevision;
    LaneRevisions.BakedRight =
        BakedRightConvolutionRevision;
    LaneRevisions.RealtimeLeft =
        RealtimeLeftConvolutionRevision;
    LaneRevisions.RealtimeRight =
        RealtimeRightConvolutionRevision;

    FUERayTracingAudioModule& Module =
        FUERayTracingAudioModule::Get();
    FUERayTracingAudioSimulationSnapshotRegistry& Registry =
        FUERayTracingAudioModule::GetManager().
            GetSnapshotRegistry();
    for (const uint64 PreviousId : PublishedAudioComponentIds)
    {
        if (!CurrentAudioComponentIds.Contains(PreviousId))
        {
            Registry.Remove(PreviousId);
            Module.RemoveConvolutionTargets(PreviousId);
        }
    }

    ++SnapshotGeneration;
    for (const UAudioComponent* AudioComponent : AudioComponents)
    {
        if (!IsValid(AudioComponent))
        {
            continue;
        }
        const uint64 AudioComponentId =
            AudioComponent->GetAudioComponentID();
        FUERayTracingAudioSimulationSnapshot Snapshot;
        Snapshot.DirectResult = DirectSoundResult;
        Snapshot.IndirectResult = IndirectSoundResult;
        Snapshot.BakedConvolutionKernel = BakedConvolutionKernel;
        Snapshot.BakedConvolutionKernelRight = BakedConvolutionKernelRight;
        Snapshot.RealtimeConvolutionKernelLeft = RealtimeConvolutionKernelLeft;
        Snapshot.RealtimeConvolutionKernelRight = RealtimeConvolutionKernelRight;
        switch (IndirectDataSource)
        {
        case EUERayTracingAudioIndirectDataSource::Baked:
            Snapshot.DataSource = EUERayTracingAudioRuntimeDataSource::Baked;
            break;
        case EUERayTracingAudioIndirectDataSource::Hybrid:
            Snapshot.DataSource = EUERayTracingAudioRuntimeDataSource::Hybrid;
            break;
        default:
            Snapshot.DataSource = EUERayTracingAudioRuntimeDataSource::Realtime;
            break;
        }
        Snapshot.IndirectMix = IndirectMix;
        Snapshot.IndirectDurationSeconds = IndirectDurationSeconds;
        if (BakedConvolutionKernel.IsValid())
        {
            Snapshot.IndirectDurationSeconds = FMath::Max(
                Snapshot.IndirectDurationSeconds,
                BakedConvolutionKernel->GetDurationSeconds());
        }
        if (RealtimeConvolutionKernelLeft.IsValid())
        {
            Snapshot.IndirectDurationSeconds = FMath::Max(
                Snapshot.IndirectDurationSeconds,
                RealtimeConvolutionKernelLeft->GetDurationSeconds());
        }
        Snapshot.ConvolutionRevision =
            ConvolutionRevision;
        Snapshot.ConvolutionRevisions =
            LaneRevisions;
        Snapshot.Generation = SnapshotGeneration;
        if (Registry.Publish(
                AudioComponentId,
                MoveTemp(Snapshot)))
        {
            Module.PublishConvolutionTargets(
                AudioComponent->GetAudioDevice(),
                AudioComponentId,
                LaneRevisions,
                BakedConvolutionKernel,
                BakedConvolutionKernelRight,
                RealtimeConvolutionKernelLeft,
                RealtimeConvolutionKernelRight);
        }
    }

    PublishedAudioComponentIds = MoveTemp(CurrentAudioComponentIds);
}

void UUERayTracingAudioSourceComponent::RefreshBakedConvolutionKernel()
{
    UUERayTracingAudioImpulseResponseAsset* Asset = BakedImpulseResponseAsset;
    if (IndirectDataSource == EUERayTracingAudioIndirectDataSource::Realtime)
    {
        SetBakedAssetStatus(
            EUERayTracingAudioBakedAssetStatus::NotRequired,
            TEXT("Realtime indirect data source does not require a baked asset."));
        CachedBakedAsset.Reset();
        CachedBakedAssetId.Invalidate();
        CachedBakedSampleRate = 0;
        CachedBakedHybridTransitionSeconds = 0.0f;
        BakedConvolutionKernel.Reset();
        BakedConvolutionKernelRight.Reset();
        ResetBakedRuntimeTail();
        return;
    }

    if (!IsValid(Asset))
    {
        SetBakedAssetStatus(
            EUERayTracingAudioBakedAssetStatus::MissingAsset,
            TEXT("Baked or Hybrid mode requires a Baked Impulse Response Asset."));
        CachedBakedAsset.Reset();
        CachedBakedAssetId.Invalidate();
        CachedBakedSampleRate = 0;
        CachedBakedHybridTransitionSeconds = 0.0f;
        BakedConvolutionKernel.Reset();
        BakedConvolutionKernelRight.Reset();
        ResetBakedRuntimeTail();
        return;
    }

    FString ValidationError;
    if (!Asset->Validate(ValidationError))
    {
        SetBakedAssetStatus(EUERayTracingAudioBakedAssetStatus::InvalidAsset, MoveTemp(ValidationError));
        CachedBakedAsset.Reset();
        CachedBakedAssetId.Invalidate();
        CachedBakedSampleRate = 0;
        CachedBakedHybridTransitionSeconds = 0.0f;
        BakedConvolutionKernel.Reset();
        BakedConvolutionKernelRight.Reset();
        ResetBakedRuntimeTail();
        return;
    }

    UWorld* World = GetWorld();
    const FString AssetWorldPath = UWorld::RemovePIEPrefix(Asset->SourceWorld.GetLongPackageName());
    const FString CurrentWorldPath = IsValid(World)
        ? UWorld::RemovePIEPrefix(FSoftObjectPath(World).GetLongPackageName())
        : FString();
    if (CurrentWorldPath.IsEmpty() || !AssetWorldPath.Equals(CurrentWorldPath, ESearchCase::IgnoreCase))
    {
        SetBakedAssetStatus(
            EUERayTracingAudioBakedAssetStatus::WorldMismatch,
            FString::Printf(
                TEXT("Baked IR belongs to '%s', but this source is in '%s'."),
                *AssetWorldPath,
                *CurrentWorldPath));
        CachedBakedAsset.Reset();
        CachedBakedAssetId.Invalidate();
        CachedBakedSampleRate = 0;
        CachedBakedHybridTransitionSeconds = 0.0f;
        BakedConvolutionKernel.Reset();
        BakedConvolutionKernelRight.Reset();
        ResetBakedRuntimeTail();
        return;
    }

    FUERayTracingAudioManager& Manager = FUERayTracingAudioModule::GetManager();
    UUERayTracingAudioListenerComponent* Listener =
        Manager.GetCurrentListener(World);
    if (!IsValid(Listener))
    {
        SetBakedAssetStatus(
            EUERayTracingAudioBakedAssetStatus::MissingListener,
            TEXT("Baked IR validation requires an active Ray Tracing Audio Listener."));
        CachedBakedAsset.Reset();
        CachedBakedAssetId.Invalidate();
        CachedBakedSampleRate = 0;
        CachedBakedHybridTransitionSeconds = 0.0f;
        BakedConvolutionKernel.Reset();
        BakedConvolutionKernelRight.Reset();
        ResetBakedRuntimeTail();
        return;
    }

    EUERayTracingAudioBakedAssetStatus StaleStatus = EUERayTracingAudioBakedAssetStatus::Ready;
    FString StaleReason;
    if (!Asset->bHasPlacementMetadata)
    {
        StaleStatus = EUERayTracingAudioBakedAssetStatus::MissingPlacementMetadata;
        StaleReason = TEXT("This legacy IR asset has no source/listener placement metadata and must be rebaked.");
    }
    else
    {
        const FString CurrentSceneSignature = Manager.GetCurrentSceneSignature(World);
        if (CurrentSceneSignature != Asset->SceneSignature)
        {
            StaleStatus = EUERayTracingAudioBakedAssetStatus::StaleScene;
            StaleReason = FString::Printf(
                TEXT("Scene/material signature changed (baked %s, current %s)."),
                *Asset->SceneSignature,
                *CurrentSceneSignature);
        }
        else
        {
            const float Tolerance = FMath::Max(BakedPlacementToleranceCm, 0.0f);
            const bool bSourceMoved = !GetSourceLocation().Equals(Asset->SourceLocation, Tolerance);
            const bool bListenerMoved = !Listener->GetListenerLocation().Equals(Asset->ListenerLocation, Tolerance);
            if (bSourceMoved || bListenerMoved)
            {
                StaleStatus = EUERayTracingAudioBakedAssetStatus::StalePlacement;
                StaleReason = TEXT("Source or listener moved beyond the baked placement tolerance.");
            }
        }
    }

    if (StaleStatus != EUERayTracingAudioBakedAssetStatus::Ready && !bAllowStaleBakedAsset)
    {
        SetBakedAssetStatus(StaleStatus, MoveTemp(StaleReason));
        CachedBakedAsset.Reset();
        CachedBakedAssetId.Invalidate();
        CachedBakedSampleRate = 0;
        CachedBakedHybridTransitionSeconds = 0.0f;
        BakedConvolutionKernel.Reset();
        BakedConvolutionKernelRight.Reset();
        ResetBakedRuntimeTail();
        return;
    }

    if (StaleStatus != EUERayTracingAudioBakedAssetStatus::Ready)
    {
        SetBakedAssetStatus(
            EUERayTracingAudioBakedAssetStatus::StaleAllowed,
            FString::Printf(TEXT("Using stale baked IR by explicit override: %s"), *StaleReason));
    }
    else
    {
        SetBakedAssetStatus(
            EUERayTracingAudioBakedAssetStatus::Ready,
            TEXT("Baked IR matches the current world, scene, materials, source, and listener."));
    }

    const int32 RuntimeSampleRate = GetRuntimeAudioSampleRate(GetWorld());
    const float HybridTransitionSeconds =
        IndirectDataSource == EUERayTracingAudioIndirectDataSource::Hybrid
        ? GetHybridTransitionSeconds(*this)
        : 0.0f;
    if (CachedBakedAsset.Get() == Asset
        && CachedBakedAssetId == Asset->BakeId
        && CachedBakedSampleRate == RuntimeSampleRate
        && FMath::IsNearlyEqual(
            CachedBakedHybridTransitionSeconds,
            HybridTransitionSeconds,
            1.0e-6f)
        && BakedConvolutionKernel.IsValid()
        && BakedConvolutionKernelRight.IsValid())
    {
        return;
    }

    CachedBakedAsset = Asset;
    CachedBakedAssetId = Asset->BakeId;
    CachedBakedSampleRate = RuntimeSampleRate;
    CachedBakedHybridTransitionSeconds = HybridTransitionSeconds;
    TArray<float> RuntimeImpulseResponseLeft;
    TArray<float> RuntimeImpulseResponseRight;
    BuildRuntimeImpulseResponseChannels(
        *Asset,
        RuntimeSampleRate,
        RuntimeImpulseResponseLeft,
        RuntimeImpulseResponseRight);
    if (IndirectDataSource == EUERayTracingAudioIndirectDataSource::Hybrid)
    {
        ApplyHybridImpulseWindow(
            RuntimeImpulseResponseLeft,
            RuntimeSampleRate,
            HybridTransitionSeconds,
            true);
        ApplyHybridImpulseWindow(
            RuntimeImpulseResponseRight,
            RuntimeSampleRate,
            HybridTransitionSeconds,
            true);
    }
    BakedConvolutionKernel = FUERayTracingAudioConvolutionKernel::Build(
        RuntimeImpulseResponseLeft,
        RuntimeSampleRate);
    BakedConvolutionKernelRight = FUERayTracingAudioConvolutionKernel::Build(
        RuntimeImpulseResponseRight,
        RuntimeSampleRate);
    const FRuntimeImpulseTailEstimate TailEstimate =
        EstimateRuntimeImpulseTail(
            RuntimeImpulseResponseLeft,
            RuntimeImpulseResponseRight,
            RuntimeSampleRate,
            BakedConvolutionKernel,
            BakedConvolutionKernelRight);
    bBakedRuntimeUsesParametricTail =
        TailEstimate.bHasTail;
    BakedRuntimeLateReverbGain =
        TailEstimate.LateReverbGain;
    BakedRuntimeParametricDelaySeconds =
        TailEstimate.ParametricDelaySeconds;
    BakedRuntimeReverbTimes =
        TailEstimate.ReverbTimes;

    if (!BakedConvolutionKernel.IsValid() || !BakedConvolutionKernelRight.IsValid())
    {
        ResetBakedRuntimeTail();
        SetBakedAssetStatus(
            EUERayTracingAudioBakedAssetStatus::InvalidAsset,
            TEXT("Baked IR could not be converted into a runtime convolution kernel."));
    }
}

void UUERayTracingAudioSourceComponent::RefreshRealtimeConvolutionKernel(
    const FUERayTracingAudioSourceSimulationResult& LatestSimulation)
{
    if (!bEnableIndirectSound
        || IndirectDataSource == EUERayTracingAudioIndirectDataSource::Baked
        || IndirectMode == EUERayTracingAudioIndirectMode::ParametricReverb)
    {
        ResetRealtimeConvolutionKernel();
        return;
    }

    if (!LatestSimulation.bHasIndirectResult || LatestSimulation.IndirectGeneration == 0)
    {
        return;
    }

    const int32 RuntimeSampleRate = GetRuntimeAudioSampleRate(GetWorld());
    const bool bUseHybridTail =
        IndirectDataSource == EUERayTracingAudioIndirectDataSource::Hybrid
        && BakedConvolutionKernel.IsValid()
        && BakedConvolutionKernelRight.IsValid();
    const float HybridTransitionSeconds = bUseHybridTail
        ? GetHybridTransitionSeconds(*this)
        : 0.0f;

    if (CachedRealtimeIndirectGeneration == LatestSimulation.IndirectGeneration
        && CachedRealtimeSampleRate == RuntimeSampleRate
        && bCachedRealtimeUsesHybridTail == bUseHybridTail
        && FMath::IsNearlyEqual(
            CachedRealtimeHybridTransitionSeconds,
            HybridTransitionSeconds,
            1.0e-6f))
    {
        return;
    }

    CachedRealtimeIndirectGeneration = LatestSimulation.IndirectGeneration;
    CachedRealtimeSampleRate = RuntimeSampleRate;
    bCachedRealtimeUsesHybridTail = bUseHybridTail;
    CachedRealtimeHybridTransitionSeconds = HybridTransitionSeconds;
    const FUERayTracingAudioIndirectSimulationResult& Result = LatestSimulation.IndirectResult;
    if (!Result.bHasValidPaths
        || Result.ReconstructedImpulseResponse.IsEmpty()
        || Result.ImpulseResponseBinDurationSeconds <= 0.0f)
    {
        RealtimeConvolutionKernelLeft.Reset();
        RealtimeConvolutionKernelRight.Reset();
        bRealtimeRuntimeUsesParametricTail = false;
        RealtimeRuntimeLateReverbGain = 0.0f;
        RealtimeRuntimeParametricDelaySeconds = 0.0f;
        RealtimeRuntimeReverbTimes = FVector::ZeroVector;
        return;
    }

    const int32 NumImpulseSamples = FMath::Max(
        FMath::CeilToInt(
            static_cast<float>(Result.ReconstructedImpulseResponse.Num())
            * Result.ImpulseResponseBinDurationSeconds
            * static_cast<float>(RuntimeSampleRate)),
        1);
    TArray<float> LeftImpulseResponse;
    TArray<float> RightImpulseResponse;
    LeftImpulseResponse.SetNumZeroed(NumImpulseSamples);
    RightImpulseResponse.SetNumZeroed(NumImpulseSamples);

    FVector ListenerRight = FVector::RightVector;
    if (const UUERayTracingAudioListenerComponent* Listener =
        FUERayTracingAudioModule::GetManager().GetCurrentListener(GetWorld()))
    {
        ListenerRight = FVector::CrossProduct(
            FVector::UpVector,
            Listener->GetListenerForward().GetSafeNormal()).GetSafeNormal();
        if (ListenerRight.IsNearlyZero())
        {
            ListenerRight = FVector::RightVector;
        }
    }
    for (int32 BinIndex = 0; BinIndex < Result.ReconstructedImpulseResponse.Num(); ++BinIndex)
    {
        const float Amplitude = Result.ReconstructedImpulseResponse[BinIndex];
        if (!FMath::IsFinite(Amplitude) || FMath::Abs(Amplitude) <= UE_SMALL_NUMBER)
        {
            continue;
        }

        const float DelaySeconds = (static_cast<float>(BinIndex) + 0.5f)
            * Result.ImpulseResponseBinDurationSeconds;
        const int32 SampleIndex = FMath::Clamp(
            FMath::RoundToInt(DelaySeconds * static_cast<float>(RuntimeSampleRate)),
            0,
            NumImpulseSamples - 1);
        const FVector DirectionMoment = Result.EnergyField.DelayBinDirection.IsValidIndex(BinIndex)
            ? Result.EnergyField.DelayBinDirection[BinIndex]
            : FVector::ZeroVector;
        const FVector ArrivalDirection = DirectionMoment.GetSafeNormal();
        const float Pan = ArrivalDirection.IsNearlyZero()
            ? 0.0f
            : FMath::Clamp(FVector::DotProduct(ArrivalDirection, ListenerRight), -1.0f, 1.0f);
        const float PanAngle = (Pan + 1.0f) * PI * 0.25f;
        LeftImpulseResponse[SampleIndex] += Amplitude * FMath::Cos(PanAngle);
        RightImpulseResponse[SampleIndex] += Amplitude * FMath::Sin(PanAngle);
    }
    if (bUseHybridTail)
    {
        ApplyHybridImpulseWindow(
            LeftImpulseResponse,
            RuntimeSampleRate,
            HybridTransitionSeconds,
            false);
        ApplyHybridImpulseWindow(
            RightImpulseResponse,
            RuntimeSampleRate,
            HybridTransitionSeconds,
            false);
    }

    RealtimeConvolutionKernelLeft = FUERayTracingAudioConvolutionKernel::Build(
        LeftImpulseResponse,
        RuntimeSampleRate);
    RealtimeConvolutionKernelRight = FUERayTracingAudioConvolutionKernel::Build(
        RightImpulseResponse,
        RuntimeSampleRate);
    const FRuntimeImpulseTailEstimate TailEstimate =
        EstimateRuntimeImpulseTail(
            LeftImpulseResponse,
            RightImpulseResponse,
            RuntimeSampleRate,
            RealtimeConvolutionKernelLeft,
            RealtimeConvolutionKernelRight);
    bRealtimeRuntimeUsesParametricTail =
        TailEstimate.bHasTail;
    RealtimeRuntimeLateReverbGain =
        TailEstimate.LateReverbGain;
    RealtimeRuntimeParametricDelaySeconds =
        TailEstimate.ParametricDelaySeconds;
    RealtimeRuntimeReverbTimes =
        TailEstimate.ReverbTimes;
}

void UUERayTracingAudioSourceComponent::ResetRealtimeConvolutionKernel()
{
    RealtimeConvolutionKernelLeft.Reset();
    RealtimeConvolutionKernelRight.Reset();
    CachedRealtimeIndirectGeneration = 0;
    CachedRealtimeSampleRate = 0;
    bCachedRealtimeUsesHybridTail = false;
    CachedRealtimeHybridTransitionSeconds = 0.0f;
    bRealtimeRuntimeUsesParametricTail = false;
    RealtimeRuntimeLateReverbGain = 0.0f;
    RealtimeRuntimeParametricDelaySeconds = 0.0f;
    RealtimeRuntimeReverbTimes = FVector::ZeroVector;
}

void UUERayTracingAudioSourceComponent::ResetBakedRuntimeTail()
{
    bBakedRuntimeUsesParametricTail = false;
    BakedRuntimeLateReverbGain = 0.0f;
    BakedRuntimeParametricDelaySeconds = 0.0f;
    BakedRuntimeReverbTimes = FVector::ZeroVector;
}

void UUERayTracingAudioSourceComponent::ApplyRuntimeTailFallback()
{
    const bool bUseBakedTail =
        IndirectDataSource
            != EUERayTracingAudioIndirectDataSource::Realtime
        && bBakedRuntimeUsesParametricTail
        && BakedConvolutionKernel.IsValid();
    const bool bUseRealtimeTail =
        IndirectDataSource
            != EUERayTracingAudioIndirectDataSource::Baked
        && bRealtimeRuntimeUsesParametricTail
        && RealtimeConvolutionKernelLeft.IsValid();
    if (!bUseBakedTail && !bUseRealtimeTail)
    {
        return;
    }

    const bool bAlreadyUsesParametricTail =
        IndirectSoundResult.bUsedParametricTail;
    float EarliestTailDelay =
        bAlreadyUsesParametricTail
            && FMath::IsFinite(
                IndirectSoundResult.ParametricDelaySeconds)
        ? FMath::Max(
            IndirectSoundResult.ParametricDelaySeconds,
            0.0f)
        : TNumericLimits<float>::Max();
    float EstimatedLateEnergy = 0.0f;
    FVector EstimatedReverbTimes =
        FVector::ZeroVector;
    const auto MergeTail = [
        &EarliestTailDelay,
        &EstimatedLateEnergy,
        &EstimatedReverbTimes](
        const float Gain,
        const float DelaySeconds,
        const FVector& TailReverbTimes)
    {
        EstimatedLateEnergy += FMath::Max(
            Gain,
            0.0f);
        EarliestTailDelay = FMath::Min(
            EarliestTailDelay,
            FMath::Max(DelaySeconds, 0.0f));
        EstimatedReverbTimes.X = FMath::Max(
            EstimatedReverbTimes.X,
            TailReverbTimes.X);
        EstimatedReverbTimes.Y = FMath::Max(
            EstimatedReverbTimes.Y,
            TailReverbTimes.Y);
        EstimatedReverbTimes.Z = FMath::Max(
            EstimatedReverbTimes.Z,
            TailReverbTimes.Z);
    };
    if (bUseBakedTail)
    {
        MergeTail(
            BakedRuntimeLateReverbGain,
            BakedRuntimeParametricDelaySeconds,
            BakedRuntimeReverbTimes);
    }
    if (bUseRealtimeTail)
    {
        MergeTail(
            RealtimeRuntimeLateReverbGain,
            RealtimeRuntimeParametricDelaySeconds,
            RealtimeRuntimeReverbTimes);
    }

    IndirectSoundResult.bHasListener = true;
    IndirectSoundResult.bHasValidPaths = true;
    IndirectSoundResult.bUsedParametricTail = true;
    IndirectSoundResult.IndirectGain = FMath::Max(
        IndirectSoundResult.IndirectGain,
        bUseBakedTail ? 1.0f : 0.0f);
    IndirectSoundResult.LateReverbGain = FMath::Max(
        IndirectSoundResult.LateReverbGain,
        FMath::Clamp(
            EstimatedLateEnergy,
            0.0f,
            4.0f));
    IndirectSoundResult.ParametricDelaySeconds =
        EarliestTailDelay == TNumericLimits<float>::Max()
        ? 0.0f
        : EarliestTailDelay;
    if (IndirectSoundResult.ReverbTimes.IsNearlyZero())
    {
        IndirectSoundResult.ReverbTimes =
            EstimatedReverbTimes;
    }
    if (!FMath::IsFinite(
            IndirectSoundResult.ParametricEq.X)
        || !FMath::IsFinite(
            IndirectSoundResult.ParametricEq.Y)
        || !FMath::IsFinite(
            IndirectSoundResult.ParametricEq.Z))
    {
        IndirectSoundResult.ParametricEq =
            FVector::OneVector;
    }
}

void UUERayTracingAudioSourceComponent::SetBakedAssetStatus(
    EUERayTracingAudioBakedAssetStatus NewStatus,
    FString NewMessage)
{
    if (BakedAssetStatus == NewStatus && BakedAssetStatusMessage == NewMessage)
    {
        return;
    }

    BakedAssetStatus = NewStatus;
    BakedAssetStatusMessage = MoveTemp(NewMessage);
    if (NewStatus == EUERayTracingAudioBakedAssetStatus::Ready
        || NewStatus == EUERayTracingAudioBakedAssetStatus::NotRequired)
    {
        UE_LOG(LogUERayTracingAudio, Display, TEXT("Baked IR status for '%s': %s"), *GetNameSafe(GetOwner()), *BakedAssetStatusMessage);
    }
    else
    {
        UE_LOG(LogUERayTracingAudio, Warning, TEXT("Baked IR status for '%s': %s"), *GetNameSafe(GetOwner()), *BakedAssetStatusMessage);
    }
}

void UUERayTracingAudioSourceComponent::ApplyBakedOnlyResult()
{
    IndirectSoundResult = FUERayTracingAudioIndirectSimulationResult();
    IndirectSoundResult.bHasListener = true;
    IndirectSoundResult.bHasValidPaths =
        BakedConvolutionKernel.IsValid() && BakedConvolutionKernelRight.IsValid();
    IndirectSoundResult.IndirectGain = IndirectSoundResult.bHasValidPaths ? 1.0f : 0.0f;
    IndirectSoundResult.ImpulseResponseBinDurationSeconds = BakedImpulseResponseAsset
        ? BakedImpulseResponseAsset->BinDurationSeconds
        : 0.0f;

    bHasIndirectPath = IndirectSoundResult.bHasValidPaths;
    NumValidReflectionPaths = 0;
    IndirectGain = IndirectSoundResult.IndirectGain;
    EarlyReflectionGain = 0.0f;
    LateReverbGain = 0.0f;
    AverageReflectionDelaySeconds = 0.0f;
    ReverbTimes = FVector::ZeroVector;
}

void UUERayTracingAudioSourceComponent::RemoveSimulationSnapshots()
{
    FUERayTracingAudioModule& Module =
        FUERayTracingAudioModule::Get();
    FUERayTracingAudioSimulationSnapshotRegistry& Registry =
        FUERayTracingAudioModule::GetManager().
            GetSnapshotRegistry();
    for (const uint64 AudioComponentId : PublishedAudioComponentIds)
    {
        Registry.Remove(AudioComponentId);
        Module.RemoveConvolutionTargets(
            AudioComponentId);
    }
    PublishedAudioComponentIds.Reset();
}

FVector UUERayTracingAudioSourceComponent::GetSourceLocation() const
{
    return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

FVector UUERayTracingAudioSourceComponent::GetSourceForward() const
{
    return GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
}

float UUERayTracingAudioSourceComponent::GetOccludedGain() const
{
    return OccludedGain;
}

float UUERayTracingAudioSourceComponent::GetSourceRadiusCm() const
{
    return SourceRadiusCm;
}

int32 UUERayTracingAudioSourceComponent::GetNumOcclusionSamples() const
{
    return NumOcclusionSamples;
}

bool UUERayTracingAudioSourceComponent::ShouldUseVolumetricOcclusion() const
{
    return bUseVolumetricOcclusion;
}

bool UUERayTracingAudioSourceComponent::ShouldUseHardOcclusion() const
{
    return bHardOcclusion;
}

FVector UUERayTracingAudioSourceComponent::GetAirAbsorptionPerMeter() const
{
    return AirAbsorptionPerMeter;
}

EUERayTracingAudioIndirectMode UUERayTracingAudioSourceComponent::GetIndirectMode() const
{
    return IndirectMode;
}

int32 UUERayTracingAudioSourceComponent::GetNumReflectionRays() const
{
    return NumReflectionRays;
}

int32 UUERayTracingAudioSourceComponent::GetMaxReflectionBounces() const
{
    return MaxReflectionBounces;
}

float UUERayTracingAudioSourceComponent::GetIndirectDurationSeconds() const
{
    return IndirectDurationSeconds;
}

int32 UUERayTracingAudioSourceComponent::GetMaxEarlyReflectionTaps() const
{
    return MaxEarlyReflectionTaps;
}

float UUERayTracingAudioSourceComponent::GetHybridTransitionRatio() const
{
    return HybridTransitionRatio;
}

float UUERayTracingAudioSourceComponent::GetIndirectMix() const
{
    return IndirectMix;
}

const FUERayTracingAudioDirectSimulationResult& UUERayTracingAudioSourceComponent::GetDirectSoundResult() const
{
    return DirectSoundResult;
}

const FUERayTracingAudioIndirectSimulationResult& UUERayTracingAudioSourceComponent::GetIndirectSoundResult() const
{
    return IndirectSoundResult;
}

float UUERayTracingAudioSourceComponent::GetCurrentOverallGain() const
{
    return DirectSoundResult.OverallGain;
}
