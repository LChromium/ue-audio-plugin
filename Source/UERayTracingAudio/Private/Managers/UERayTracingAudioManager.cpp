#include "Managers/UERayTracingAudioManager.h"

#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Crc.h"
#include "UERayTracingAudioModule.h"

namespace
{
    TAutoConsoleVariable<int32> CVarUERayTracingAudioMaxSourcesPerFrame(
        TEXT("au.UERayTracingAudio.MaxSourcesPerFrame"),
        4,
        TEXT("Maximum number of queued ray-traced audio sources simulated per game frame."),
        ECVF_Default);

    TAutoConsoleVariable<int32> CVarUERayTracingAudioMaxRaysPerFrame(
        TEXT("au.UERayTracingAudio.MaxRaysPerFrame"),
        4096,
        TEXT("Soft upper bound for direct rays plus the indirect rays-per-bounce estimate submitted per game frame. Zero pauses new simulation submissions."),
        ECVF_Default);

    TAutoConsoleVariable<float> CVarUERayTracingAudioMaxQueueLatencyMs(
        TEXT("au.UERayTracingAudio.MaxQueueLatencyMs"),
        100.0f,
        TEXT("Queue latency threshold used for rate-limited overload warnings."),
        ECVF_Default);

    int32 GQueuedSources = 0;
    int32 GInFlightQueries = 0;
    int32 GSubmittedRayEstimate = 0;
    int32 GDegradedSources = 0;
    float GMaxQueueLatencyMs = 0.0f;

    FAutoConsoleVariableRef CVarUERayTracingAudioQueuedSources(
        TEXT("au.UERayTracingAudio.Stats.QueuedSources"),
        GQueuedSources,
        TEXT("Current number of queued source simulation requests."),
        ECVF_ReadOnly);
    FAutoConsoleVariableRef CVarUERayTracingAudioInFlightQueries(
        TEXT("au.UERayTracingAudio.Stats.InFlightQueries"),
        GInFlightQueries,
        TEXT("Current number of direct plus indirect GPU queries in flight."),
        ECVF_ReadOnly);
    FAutoConsoleVariableRef CVarUERayTracingAudioSubmittedRayEstimate(
        TEXT("au.UERayTracingAudio.Stats.SubmittedRayEstimate"),
        GSubmittedRayEstimate,
        TEXT("Estimated ray count submitted during the latest manager tick."),
        ECVF_ReadOnly);
    FAutoConsoleVariableRef CVarUERayTracingAudioDegradedSources(
        TEXT("au.UERayTracingAudio.Stats.DegradedSources"),
        GDegradedSources,
        TEXT("Number of sources whose indirect ray count was reduced during the latest manager tick."),
        ECVF_ReadOnly);
    FAutoConsoleVariableRef CVarUERayTracingAudioObservedQueueLatency(
        TEXT("au.UERayTracingAudio.Stats.MaxQueueLatencyMs"),
        GMaxQueueLatencyMs,
        TEXT("Maximum queue latency observed during the latest manager tick."),
        ECVF_ReadOnly);
    TAutoConsoleVariable<int32> CVarUERayTracingAudioDirectBatchSources(
        TEXT("au.UERayTracingAudio.Stats.DirectBatchSources"),
        0,
        TEXT("Number of source ray groups merged into the latest direct RHI batch."),
        ECVF_ReadOnly);
    TAutoConsoleVariable<int32> CVarUERayTracingAudioIndirectBatchSources(
        TEXT("au.UERayTracingAudio.Stats.IndirectBatchSources"),
        0,
        TEXT("Number of source energy-field requests merged into the latest indirect RHI batch."),
        ECVF_ReadOnly);

    double GLastOverloadWarningSeconds = -DBL_MAX;

    void BuildDirectionalStereoImpulseResponse(
        const FUERayTracingAudioIndirectSimulationInput& Input,
        const FUERayTracingAudioIndirectSimulationResult& Result,
        TArray<float>& OutInterleavedSamples)
    {
        OutInterleavedSamples.Init(0.0f, Result.ReconstructedImpulseResponse.Num() * 2);
        FVector ListenerRight = FVector::CrossProduct(
            FVector::UpVector,
            Input.ListenerForward.GetSafeNormal()).GetSafeNormal();
        if (ListenerRight.IsNearlyZero())
        {
            ListenerRight = FVector::RightVector;
        }

        for (int32 BinIndex = 0; BinIndex < Result.ReconstructedImpulseResponse.Num(); ++BinIndex)
        {
            const float Amplitude = Result.ReconstructedImpulseResponse[BinIndex];
            if (!FMath::IsFinite(Amplitude) || FMath::Abs(Amplitude) <= UE_SMALL_NUMBER)
            {
                continue;
            }

            const FVector DirectionMoment = Result.EnergyField.DelayBinDirection.IsValidIndex(BinIndex)
                ? Result.EnergyField.DelayBinDirection[BinIndex]
                : FVector::ZeroVector;
            const FVector ArrivalDirection = DirectionMoment.GetSafeNormal();
            const float Pan = ArrivalDirection.IsNearlyZero()
                ? 0.0f
                : FMath::Clamp(FVector::DotProduct(ArrivalDirection, ListenerRight), -1.0f, 1.0f);
            const float PanAngle = (Pan + 1.0f) * PI * 0.25f;
            OutInterleavedSamples[(BinIndex * 2)] = Amplitude * FMath::Cos(PanAngle);
            OutInterleavedSamples[(BinIndex * 2) + 1] = Amplitude * FMath::Sin(PanAngle);
        }
    }

    FUERayTracingAudioDirectSimulationResult StabilizeDirectResult(
        FUERayTracingAudioDirectSimulationResult NewResult,
        const FUERayTracingAudioSourceSimulationResult& PreviousResult,
        const float DeltaTimeSeconds)
    {
        if (NewResult.DistanceAttenuation <= 0.0f)
        {
            return NewResult;
        }
        if (!PreviousResult.bHasDirectResult)
        {
            return NewResult;
        }

        const bool bAttacking = NewResult.DirectVisibility > PreviousResult.DirectResult.DirectVisibility;
        const float TimeConstant = bAttacking ? 0.06f : 0.12f;
        const float Alpha = FMath::Clamp(
            1.0f - FMath::Exp(-FMath::Max(DeltaTimeSeconds, 1.0f / 120.0f) / TimeConstant),
            0.0f,
            1.0f);
        NewResult.DirectVisibility = FMath::Lerp(
            PreviousResult.DirectResult.DirectVisibility,
            NewResult.DirectVisibility,
            Alpha);
        NewResult.Occlusion = FMath::Lerp(
            PreviousResult.DirectResult.Occlusion,
            NewResult.Occlusion,
            Alpha);
        NewResult.OverallGain = FMath::Lerp(
            PreviousResult.DirectResult.OverallGain,
            NewResult.OverallGain,
            Alpha);
        NewResult.bIsOccluded = NewResult.DirectVisibility < (1.0f - KINDA_SMALL_NUMBER);
        return NewResult;
    }
}

FUERayTracingAudioManager::FUERayTracingAudioManager(
    const FUERayTracingAudioContextSettings& ContextSettings)
    : Context(ContextSettings)
    , Simulator(Context)
    , SnapshotRegistry(
        MakeShared<
            FUERayTracingAudioSimulationSnapshotRegistry,
            ESPMode::ThreadSafe>())
{
    SimulationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FUERayTracingAudioManager::TickSimulationQueue));
}

FUERayTracingAudioManager::~FUERayTracingAudioManager()
{
    if (SimulationTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(SimulationTickerHandle);
        SimulationTickerHandle.Reset();
    }
    for (TPair<
            TWeakObjectPtr<UUERayTracingAudioSourceComponent>,
            FSourceSimulationState>& Pair : SourceSimulationStates)
    {
        CancelSourceQueries(Pair.Value);
    }
    for (const TSharedPtr<FUERayTracingAudioBakeJob>& Job : ActiveBakeJobs)
    {
        if (Job.IsValid())
        {
            Job->Cancel();
        }
    }
    SnapshotRegistry->Reset();
}

void FUERayTracingAudioManager::AddSource(UUERayTracingAudioSourceComponent* Source)
{
    Sources.Add(Source);
}

void FUERayTracingAudioManager::RemoveSource(UUERayTracingAudioSourceComponent* Source)
{
    Sources.Remove(Source);
    if (FSourceSimulationState* State = SourceSimulationStates.Find(Source))
    {
        CancelSourceQueries(*State);
    }
    SourceSimulationStates.Remove(Source);
    PendingSimulationSources.RemoveAll(
        [Source](const TWeakObjectPtr<UUERayTracingAudioSourceComponent>& PendingSource)
        {
            return !PendingSource.IsValid() || PendingSource.Get() == Source;
        });
}

void FUERayTracingAudioManager::AddListener(UUERayTracingAudioListenerComponent* Listener)
{
    check(IsInGameThread());

    UWorld* World = IsValid(Listener) ? Listener->GetWorld() : nullptr;
    if (!IsValid(World))
    {
        return;
    }

    TWeakObjectPtr<UUERayTracingAudioListenerComponent>& CurrentListener =
        ListenersByWorld.FindOrAdd(World);
    if (CurrentListener.IsValid() && CurrentListener.Get() != Listener)
    {
        UE_LOG(
            LogUERayTracingAudio,
            Warning,
            TEXT("World '%s' already has a Ray Tracing Audio Listener '%s'; keeping the first Listener and ignoring '%s'."),
            *World->GetName(),
            *CurrentListener->GetName(),
            *Listener->GetName());
        return;
    }

    CurrentListener = Listener;
}

void FUERayTracingAudioManager::RemoveListener(UUERayTracingAudioListenerComponent* Listener)
{
    check(IsInGameThread());

    UWorld* World = Listener ? Listener->GetWorld() : nullptr;
    if (!World)
    {
        return;
    }

    const TWeakObjectPtr<UUERayTracingAudioListenerComponent>* CurrentListener =
        ListenersByWorld.Find(World);
    if (CurrentListener && CurrentListener->Get() == Listener)
    {
        ListenersByWorld.Remove(World);
        InvalidateWorldSources(World);
    }
}

void FUERayTracingAudioManager::AddGeometry(UUERayTracingAudioGeometryComponent* Geometry)
{
    GeometryComponents.Add(Geometry);
    MarkSceneDirty(IsValid(Geometry) ? Geometry->GetWorld() : nullptr);
}

void FUERayTracingAudioManager::RemoveGeometry(UUERayTracingAudioGeometryComponent* Geometry)
{
    UWorld* World = Geometry ? Geometry->GetWorld() : nullptr;
    GeometryComponents.Remove(Geometry);
    MarkSceneDirty(World);
}

void FUERayTracingAudioManager::MarkSceneDirty(UWorld* World)
{
    check(IsInGameThread());

    if (IsValid(World))
    {
        GetOrCreateWorldAcousticState(World).bSceneDirty = true;
    }
}

void FUERayTracingAudioManager::RebuildScene(UWorld* World)
{
    check(IsInGameThread());

    if (!IsValid(World))
    {
        return;
    }

    FWorldAcousticState& WorldState = GetOrCreateWorldAcousticState(World);
    if (!WorldState.bSceneDirty)
    {
        return;
    }

    TArray<FUERayTracingAudioGeometryExport> GeometryExports;
    GeometryExports.Reserve(GeometryComponents.Num());

    for (const TWeakObjectPtr<UUERayTracingAudioGeometryComponent>& GeometryComponent : GeometryComponents)
    {
        if (!GeometryComponent.IsValid()
            || GeometryComponent->GetWorld() != World)
        {
            continue;
        }

        FUERayTracingAudioGeometryExport GeometryExport;
        if (GeometryComponent->BuildGeometryExport(GeometryExport))
        {
            GeometryExports.Add(MoveTemp(GeometryExport));
        }
    }

    WorldState.Scene.SetStaticGeometry(MoveTemp(GeometryExports));
    WorldState.SceneSignature = BuildSceneSignature(WorldState.Scene);
    WorldState.bSceneDirty = false;
}

FString FUERayTracingAudioManager::GetCurrentSceneSignature(UWorld* World)
{
    check(IsInGameThread());

    if (!IsValid(World))
    {
        return TEXT("00000000");
    }

    RebuildScene(World);
    return GetOrCreateWorldAcousticState(World).SceneSignature;
}

UUERayTracingAudioListenerComponent* FUERayTracingAudioManager::GetCurrentListener(
    const UWorld* World) const
{
    const TWeakObjectPtr<UUERayTracingAudioListenerComponent>* Listener =
        IsValid(World) ? ListenersByWorld.Find(World) : nullptr;
    return Listener && Listener->IsValid() ? Listener->Get() : nullptr;
}

FUERayTracingAudioDirectSimulationResult FUERayTracingAudioManager::SimulateDirectSource(UUERayTracingAudioSourceComponent* Source)
{
    FUERayTracingAudioDirectSimulationResult Result;
    FUERayTracingAudioDirectSimulationInput Input;
    if (!BuildDirectSimulationInput(Source, Input))
    {
        return Result;
    }

    return Simulator.SimulateDirectSound(RayTracingDevice, Input);
}

bool FUERayTracingAudioManager::BuildDirectSimulationInput(
    UUERayTracingAudioSourceComponent* Source,
    FUERayTracingAudioDirectSimulationInput& OutInput)
{

    if (!IsValid(Source))
    {
        return false;
    }

    UWorld* World = Source->GetWorld();
    UUERayTracingAudioListenerComponent* Listener = GetCurrentListener(World);
    if (!IsValid(World) || !IsValid(Listener))
    {
        return false;
    }

    RebuildScene(World);
    FWorldAcousticState& WorldState = GetOrCreateWorldAcousticState(World);

    OutInput = FUERayTracingAudioDirectSimulationInput();
    OutInput.World = World;
    OutInput.Scene = &WorldState.Scene;
    OutInput.ListenerLocation = Listener->GetListenerLocation();
    OutInput.ListenerActor = Listener->GetOwner();
    OutInput.SourceLocation = Source->GetSourceLocation();
    OutInput.SourceForward = Source->GetSourceForward();
    OutInput.SourceActor = Source->GetOwner();
    OutInput.OccludedGain = Source->GetOccludedGain();
    OutInput.SourceRadiusCm = Source->GetSourceRadiusCm();
    OutInput.NumOcclusionSamples = Source->GetNumOcclusionSamples();
    OutInput.bUseVolumetricOcclusion = Source->ShouldUseVolumetricOcclusion();
    OutInput.bHardOcclusion = Source->ShouldUseHardOcclusion();
    OutInput.AirAbsorptionPerMeter = Source->GetAirAbsorptionPerMeter();
    return true;
}

FUERayTracingAudioIndirectSimulationResult FUERayTracingAudioManager::SimulateIndirectSource(UUERayTracingAudioSourceComponent* Source)
{
    FUERayTracingAudioIndirectSimulationResult Result;
    FUERayTracingAudioIndirectSimulationInput Input;
    if (!BuildIndirectSimulationInput(Source, Input))
    {
        return Result;
    }

    return Simulator.SimulateIndirectSound(RayTracingDevice, Input);
}

bool FUERayTracingAudioManager::BuildIndirectSimulationInput(
    UUERayTracingAudioSourceComponent* Source,
    FUERayTracingAudioIndirectSimulationInput& OutInput)
{
    if (!IsValid(Source))
    {
        return false;
    }

    UWorld* World = Source->GetWorld();
    UUERayTracingAudioListenerComponent* Listener = GetCurrentListener(World);
    if (!IsValid(World) || !IsValid(Listener))
    {
        return false;
    }

    RebuildScene(World);
    FWorldAcousticState& WorldState = GetOrCreateWorldAcousticState(World);

    OutInput = FUERayTracingAudioIndirectSimulationInput();
    OutInput.World = World;
    OutInput.Scene = &WorldState.Scene;
    OutInput.ListenerLocation = Listener->GetListenerLocation();
    OutInput.ListenerForward = Listener->GetListenerForward();
    OutInput.ListenerActor = Listener->GetOwner();
    OutInput.SourceLocation = Source->GetSourceLocation();
    OutInput.SourceForward = Source->GetSourceForward();
    OutInput.SourceActor = Source->GetOwner();
    OutInput.SourceRadiusCm = Source->GetSourceRadiusCm();
    OutInput.NumReflectionRays = Source->GetNumReflectionRays();
    OutInput.MaxReflectionBounces = Source->GetMaxReflectionBounces();
    OutInput.DurationSeconds = Source->GetIndirectDurationSeconds();
    OutInput.DeltaTimeSeconds = World->GetDeltaSeconds();
    OutInput.MaxEarlyReflectionTaps = Source->GetMaxEarlyReflectionTaps();
    OutInput.NumDelayBins = 96;
    OutInput.HybridTransitionRatio = Source->GetHybridTransitionRatio();
    OutInput.AirAbsorptionPerMeter = Source->GetAirAbsorptionPerMeter();

    switch (Source->GetIndirectMode())
    {
    case EUERayTracingAudioIndirectMode::ParametricReverb:
        OutInput.EffectType = EUERayTracingAudioIndirectEffectType::Parametric;
        break;

    case EUERayTracingAudioIndirectMode::HybridReverb:
        OutInput.EffectType = EUERayTracingAudioIndirectEffectType::Hybrid;
        break;

    default:
        OutInput.EffectType = EUERayTracingAudioIndirectEffectType::Convolution;
        break;
    }

    return true;
}

void FUERayTracingAudioManager::RequestSourceSimulation(
    UUERayTracingAudioSourceComponent* Source,
    bool bRequestDirect,
    bool bRequestIndirect)
{
    check(IsInGameThread());

    if (!IsValid(Source))
    {
        return;
    }

    FSourceSimulationState& State = SourceSimulationStates.FindOrAdd(Source);
    UWorld* World = Source->GetWorld();
    if (!IsValid(World) || !IsValid(GetCurrentListener(World)))
    {
        if (IsValid(World))
        {
            ListenersByWorld.Remove(World);
            InvalidateWorldSources(World);
        }
        return;
    }
    State.bDirectRequested = bRequestDirect && !State.DirectQuery.IsValid();
    State.bIndirectRequested = bRequestIndirect && !State.IndirectQuery.IsValid();

    if (!State.bDirectRequested && !State.bIndirectRequested)
    {
        if (State.bQueued)
        {
            PendingSimulationSources.Remove(Source);
            State.bQueued = false;
        }
        return;
    }

    if (!State.bQueued)
    {
        State.bQueued = true;
        State.FirstQueuedTimeSeconds = FPlatformTime::Seconds();
        PendingSimulationSources.Add(Source);
    }
}

bool FUERayTracingAudioManager::GetLatestSourceSimulation(
    const UUERayTracingAudioSourceComponent* Source,
    FUERayTracingAudioSourceSimulationResult& OutResult) const
{
    check(IsInGameThread());

    if (!IsValid(Source))
    {
        return false;
    }

    const FSourceSimulationState* State = SourceSimulationStates.Find(Source);
    if (!State || (!State->LatestResult.bHasDirectResult && !State->LatestResult.bHasIndirectResult))
    {
        return false;
    }

    OutResult = State->LatestResult;
    return true;
}

bool FUERayTracingAudioManager::TickSimulationQueue(float DeltaTime)
{
    check(IsInGameThread());

    FUERayTracingAudioModule::Get().
        ServiceIndirectAudioBridges();
    SnapshotRegistry->ReclaimRetiredSnapshots();
    PollBakeJobs();
    PollCompletedDirectQueries();
    PollCompletedIndirectQueries();
    RemoveDeadWorldState();

    const int32 SourceBudget = FMath::Max(CVarUERayTracingAudioMaxSourcesPerFrame.GetValueOnGameThread(), 0);
    const int32 RayBudget = FMath::Max(CVarUERayTracingAudioMaxRaysPerFrame.GetValueOnGameThread(), 0);
    int32 NumProcessed = 0;
    int32 SubmittedRayEstimate = 0;
    int32 NumDegradedSources = 0;
    const double NowSeconds = FPlatformTime::Seconds();

    struct FPendingDirectBatchSubmission
    {
        TWeakObjectPtr<UUERayTracingAudioSourceComponent> Source;
        TWeakObjectPtr<UWorld> World;
        const FUERayTracingAudioScene* Scene = nullptr;
        TArray<FUERayTracingAudioRay> Rays;
    };
    TArray<FPendingDirectBatchSubmission> PendingDirectBatch;
    PendingDirectBatch.Reserve(SourceBudget);
    struct FPendingIndirectBatchSubmission
    {
        TWeakObjectPtr<UUERayTracingAudioSourceComponent> Source;
        FUERayTracingAudioEnergyFieldTraceRequest Request;
    };
    TArray<FPendingIndirectBatchSubmission> PendingIndirectBatch;
    PendingIndirectBatch.Reserve(SourceBudget);

    GMaxQueueLatencyMs = 0.0f;
    for (const TWeakObjectPtr<UUERayTracingAudioSourceComponent>& QueuedSource : PendingSimulationSources)
    {
        if (const FSourceSimulationState* QueuedState = SourceSimulationStates.Find(QueuedSource))
        {
            if (QueuedState->FirstQueuedTimeSeconds > 0.0)
            {
                GMaxQueueLatencyMs = FMath::Max(
                    GMaxQueueLatencyMs,
                    static_cast<float>((NowSeconds - QueuedState->FirstQueuedTimeSeconds) * 1000.0));
            }
        }
    }

    while (NumProcessed < SourceBudget
        && RayBudget > 0
        && SubmittedRayEstimate < RayBudget
        && !PendingSimulationSources.IsEmpty())
    {
        const TWeakObjectPtr<UUERayTracingAudioSourceComponent> WeakSource = PendingSimulationSources[0];
        PendingSimulationSources.RemoveAt(0, 1, EAllowShrinking::No);

        UUERayTracingAudioSourceComponent* Source = WeakSource.Get();
        FSourceSimulationState* State = SourceSimulationStates.Find(WeakSource);
        if (!IsValid(Source) || !State || !Sources.Contains(WeakSource))
        {
            SourceSimulationStates.Remove(WeakSource);
            continue;
        }

        const bool bSimulateDirect = State->bDirectRequested;
        const bool bSimulateIndirect = State->bIndirectRequested;
        const double FirstQueuedTimeSeconds = State->FirstQueuedTimeSeconds;
        State->bDirectRequested = false;
        State->bIndirectRequested = false;
        State->bQueued = false;
        State->FirstQueuedTimeSeconds = 0.0;

        if (bSimulateDirect)
        {
            FUERayTracingAudioDirectSimulationInput Input;
            if (BuildDirectSimulationInput(Source, Input))
            {
                if (RayTracingDevice.IsRayTracingAvailable() && Input.Scene && !Input.Scene->IsEmpty())
                {
                    FUERayTracingAudioDirectSimulationQuery DirectSimulationQuery =
                        Simulator.BuildDirectSoundQuery(RayTracingDevice, Input);

                    SubmittedRayEstimate += DirectSimulationQuery.Rays.Num();

                    State->PendingDirectInput = Input;
                    State->PendingDirectQuery = DirectSimulationQuery;
                    FPendingDirectBatchSubmission& Submission = PendingDirectBatch.AddDefaulted_GetRef();
                    Submission.Source = WeakSource;
                    Submission.World = Input.World;
                    Submission.Scene = Input.Scene;
                    Submission.Rays = DirectSimulationQuery.Rays;
                }
                else
                {
                    State->LatestResult.DirectResult = StabilizeDirectResult(
                        Simulator.SimulateDirectSound(RayTracingDevice, Input),
                        State->LatestResult,
                        Input.World ? Input.World->GetDeltaSeconds() : 1.0f / 60.0f);
                    State->LatestResult.bHasDirectResult = true;
                    State->LatestResult.DirectGeneration = ++NextSimulationGeneration;
                    State->LatestResult.Generation = State->LatestResult.DirectGeneration;
                }
            }
        }

        if (bSimulateIndirect)
        {
            FUERayTracingAudioIndirectSimulationInput Input;
            if (BuildIndirectSimulationInput(Source, Input))
            {
                if (RayTracingDevice.IsRayTracingAvailable() && Input.Scene && !Input.Scene->IsEmpty())
                {
                    FUERayTracingAudioEnergyFieldTraceRequest TraceRequest;
                    TraceRequest.World = Input.World;
                    TraceRequest.Scene = Input.Scene;
                    TraceRequest.ListenerLocation = Input.ListenerLocation;
                    TraceRequest.ListenerForward = Input.ListenerForward;
                    TraceRequest.SourceLocation = Input.SourceLocation;
                    TraceRequest.AirAbsorptionPerMeter = Input.AirAbsorptionPerMeter;
                    TraceRequest.ListenerActor = Input.ListenerActor;
                    TraceRequest.SourceActor = Input.SourceActor;
                    const int32 RemainingRayBudget = FMath::Max(RayBudget - SubmittedRayEstimate, 0);
                    const int32 RaysPerListenerSample = FMath::Max(Input.MaxReflectionBounces * 2, 1);
                    const int32 BudgetedReflectionRays = FMath::Clamp(
                        RemainingRayBudget / RaysPerListenerSample,
                        0,
                        Input.NumReflectionRays);
                    if (BudgetedReflectionRays <= 0)
                    {
                        State->bIndirectRequested = true;
                        State->bQueued = true;
                        State->FirstQueuedTimeSeconds = FirstQueuedTimeSeconds > 0.0
                            ? FirstQueuedTimeSeconds
                            : NowSeconds;
                        PendingSimulationSources.Add(WeakSource);
                    }
                    else
                    {
                        TraceRequest.NumReflectionRays = BudgetedReflectionRays;
                        TraceRequest.MaxReflectionBounces = Input.MaxReflectionBounces;
                        TraceRequest.NumDelayBins = Input.NumDelayBins;
                        TraceRequest.DurationSeconds = Input.DurationSeconds;
                        TraceRequest.ReferenceDistance = FMath::Max(Context.GetReferenceDistanceCm(), 1.0f);
                        TraceRequest.SpeedOfSound = FMath::Max(Context.GetSpeedOfSoundCmPerSecond(), 1.0f);
                        TraceRequest.MaxTraceDistance = FMath::Max(Context.GetMaxDistanceCm(), 100.0f);

                        if (BudgetedReflectionRays < Input.NumReflectionRays)
                        {
                            ++NumDegradedSources;
                        }
                        SubmittedRayEstimate += BudgetedReflectionRays * RaysPerListenerSample;
                        Input.NumReflectionRays = BudgetedReflectionRays;
                        State->PendingIndirectInput = Input;
                        FPendingIndirectBatchSubmission& Submission = PendingIndirectBatch.AddDefaulted_GetRef();
                        Submission.Source = WeakSource;
                        Submission.Request = TraceRequest;
                    }
                }
                else
                {
                    const int32 RemainingRayBudget = FMath::Max(RayBudget - SubmittedRayEstimate, 1);
                    const int32 RaysPerListenerSample = FMath::Max(Input.MaxReflectionBounces * 2, 1);
                    const int32 OriginalReflectionRays = Input.NumReflectionRays;
                    Input.NumReflectionRays = FMath::Clamp(
                        RemainingRayBudget / RaysPerListenerSample,
                        1,
                        OriginalReflectionRays);
                    NumDegradedSources += Input.NumReflectionRays < OriginalReflectionRays ? 1 : 0;
                    SubmittedRayEstimate += Input.NumReflectionRays * RaysPerListenerSample;
                    State->LatestResult.IndirectResult = Simulator.SimulateIndirectSound(RayTracingDevice, Input);
                    State->LatestResult.bHasIndirectResult = true;
                    State->LatestResult.IndirectGeneration = ++NextSimulationGeneration;
                    State->LatestResult.Generation = State->LatestResult.IndirectGeneration;
                }
            }
        }

        ++NumProcessed;
    }

    if (!PendingDirectBatch.IsEmpty())
    {
        int32 NumMappedSources = 0;
        TSet<const FUERayTracingAudioScene*> SubmittedScenes;
        for (int32 FirstSubmissionIndex = 0;
            FirstSubmissionIndex < PendingDirectBatch.Num();
            ++FirstSubmissionIndex)
        {
            FPendingDirectBatchSubmission& FirstSubmission =
                PendingDirectBatch[FirstSubmissionIndex];
            if (!FirstSubmission.Scene
                || SubmittedScenes.Contains(FirstSubmission.Scene))
            {
                continue;
            }

            SubmittedScenes.Add(FirstSubmission.Scene);
            FUERayTracingAudioTraceRequest TraceRequest;
            TraceRequest.World = FirstSubmission.World.Get();
            TraceRequest.Scene = FirstSubmission.Scene;
            TraceRequest.SceneCacheKey = FirstSubmission.Scene->GetCacheKey();

            TArray<int32> SubmissionIndices;
            TArray<TArray<FUERayTracingAudioRay>> RayBatches;
            for (int32 SubmissionIndex = FirstSubmissionIndex;
                SubmissionIndex < PendingDirectBatch.Num();
                ++SubmissionIndex)
            {
                FPendingDirectBatchSubmission& Submission =
                    PendingDirectBatch[SubmissionIndex];
                if (Submission.Scene == FirstSubmission.Scene)
                {
                    SubmissionIndices.Add(SubmissionIndex);
                    RayBatches.Add(MoveTemp(Submission.Rays));
                }
            }

            TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>> Queries =
                RayTracingDevice.SubmitRaysBatch(TraceRequest, RayBatches);
            const int32 NumMappedQueries =
                FMath::Min(Queries.Num(), SubmissionIndices.Num());
            for (int32 QueryIndex = 0; QueryIndex < NumMappedQueries; ++QueryIndex)
            {
                const int32 SubmissionIndex = SubmissionIndices[QueryIndex];
                if (FSourceSimulationState* State = SourceSimulationStates.Find(
                    PendingDirectBatch[SubmissionIndex].Source))
                {
                    State->DirectQuery = MoveTemp(Queries[QueryIndex]);
                    ++NumMappedSources;
                }
            }
        }
        CVarUERayTracingAudioDirectBatchSources->Set(NumMappedSources, ECVF_SetByCode);
    }

    if (!PendingIndirectBatch.IsEmpty())
    {
        int32 NumMappedSources = 0;
        TSet<const FUERayTracingAudioScene*> SubmittedScenes;
        for (int32 FirstSubmissionIndex = 0;
            FirstSubmissionIndex < PendingIndirectBatch.Num();
            ++FirstSubmissionIndex)
        {
            FPendingIndirectBatchSubmission& FirstSubmission =
                PendingIndirectBatch[FirstSubmissionIndex];
            const FUERayTracingAudioScene* Scene = FirstSubmission.Request.Scene;
            if (!Scene || SubmittedScenes.Contains(Scene))
            {
                continue;
            }

            SubmittedScenes.Add(Scene);
            TArray<int32> SubmissionIndices;
            TArray<FUERayTracingAudioEnergyFieldTraceRequest> Requests;
            for (int32 SubmissionIndex = FirstSubmissionIndex;
                SubmissionIndex < PendingIndirectBatch.Num();
                ++SubmissionIndex)
            {
                FPendingIndirectBatchSubmission& Submission =
                    PendingIndirectBatch[SubmissionIndex];
                if (Submission.Request.Scene == Scene)
                {
                    SubmissionIndices.Add(SubmissionIndex);
                    Requests.Add(MoveTemp(Submission.Request));
                }
            }

            TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>> Queries =
                RayTracingDevice.SubmitIndirectEnergyFieldBatch(Requests);
            const int32 NumMappedQueries =
                FMath::Min(Queries.Num(), SubmissionIndices.Num());
            for (int32 QueryIndex = 0; QueryIndex < NumMappedQueries; ++QueryIndex)
            {
                const int32 SubmissionIndex = SubmissionIndices[QueryIndex];
                if (FSourceSimulationState* State = SourceSimulationStates.Find(
                    PendingIndirectBatch[SubmissionIndex].Source))
                {
                    State->IndirectQuery = MoveTemp(Queries[QueryIndex]);
                    ++NumMappedSources;
                }
            }
        }
        CVarUERayTracingAudioIndirectBatchSources->Set(NumMappedSources, ECVF_SetByCode);
    }

    GQueuedSources = PendingSimulationSources.Num();
    GSubmittedRayEstimate = SubmittedRayEstimate;
    GDegradedSources = NumDegradedSources;
    GInFlightQueries = 0;
    for (const TPair<TWeakObjectPtr<UUERayTracingAudioSourceComponent>, FSourceSimulationState>& Pair : SourceSimulationStates)
    {
        GInFlightQueries += Pair.Value.DirectQuery.IsValid() ? 1 : 0;
        GInFlightQueries += Pair.Value.IndirectQuery.IsValid() ? 1 : 0;
    }

    const float WarningLatencyMs = FMath::Max(CVarUERayTracingAudioMaxQueueLatencyMs.GetValueOnGameThread(), 0.0f);
    const bool bOverloaded = (WarningLatencyMs > 0.0f && GMaxQueueLatencyMs > WarningLatencyMs)
        || PendingSimulationSources.Num() > FMath::Max(SourceBudget * 4, 8);
    if (bOverloaded && NowSeconds - GLastOverloadWarningSeconds >= 5.0)
    {
        UE_LOG(
            LogUERayTracingAudio,
            Warning,
            TEXT("Simulation queue overloaded: queued=%d, in-flight=%d, max-latency=%.1f ms, submitted-ray-estimate=%d, degraded-sources=%d."),
            GQueuedSources,
            GInFlightQueries,
            GMaxQueueLatencyMs,
            GSubmittedRayEstimate,
            GDegradedSources);
        GLastOverloadWarningSeconds = NowSeconds;
    }

    return true;
}

TSharedPtr<FUERayTracingAudioBakeJob> FUERayTracingAudioManager::StartImpulseResponseBake(
    UUERayTracingAudioSourceComponent* Source,
    UUERayTracingAudioListenerComponent* Listener,
    const FUERayTracingAudioBakeSettings& Settings,
    bool bCaptureCpuReference)
{
    check(IsInGameThread());

    TSharedPtr<FUERayTracingAudioBakeJob> Job = MakeShared<FUERayTracingAudioBakeJob>();
    Job->bCaptureCpuReference = bCaptureCpuReference;
    if (!IsValid(Source) || !IsValid(Listener))
    {
        Job->SetFailed(TEXT("Bake requires a valid source and listener component."));
        return Job;
    }

    UWorld* World = Source->GetWorld();
    if (!IsValid(World) || Listener->GetWorld() != World)
    {
        Job->SetFailed(TEXT("Bake source and listener must belong to the same valid world."));
        return Job;
    }

    if (Settings.bRequireHardwareRayTracing && !RayTracingDevice.IsRayTracingAvailable())
    {
        Job->SetFailed(TEXT("Hardware ray tracing is required by the bake settings but is unavailable."));
        return Job;
    }

    if (!RebuildSceneForBake(World))
    {
        Job->SetFailed(TEXT("Could not rebuild the acoustic scene for the bake."));
        return Job;
    }
    FWorldAcousticState& WorldState = GetOrCreateWorldAcousticState(World);

    FUERayTracingAudioBakeSettings ClampedSettings = Settings;
    ClampedSettings.NumRays = FMath::Clamp(ClampedSettings.NumRays, 1, 1048576);
    ClampedSettings.MaxBounces = FMath::Clamp(ClampedSettings.MaxBounces, 1, 64);
    ClampedSettings.DurationSeconds = FMath::Clamp(ClampedSettings.DurationSeconds, 0.05f, 30.0f);
    ClampedSettings.SampleRate = FMath::Clamp(ClampedSettings.SampleRate, 8000, 192000);

    FUERayTracingAudioIndirectSimulationInput& Input = Job->SimulationInput;
    Input.World = World;
    Input.Scene = &WorldState.Scene;
    Input.ListenerLocation = Listener->GetListenerLocation();
    Input.ListenerForward = Listener->GetListenerForward();
    Input.ListenerActor = Listener->GetOwner();
    Input.SourceLocation = Source->GetSourceLocation();
    Input.SourceForward = Source->GetSourceForward();
    // A bake is a deterministic snapshot, so it must not read or update the
    // realtime temporal-smoothing history keyed by SourceActor.
    Input.SourceActor = nullptr;
    Input.SourceRadiusCm = Source->GetSourceRadiusCm();
    Input.NumReflectionRays = ClampedSettings.NumRays;
    Input.MaxReflectionBounces = ClampedSettings.MaxBounces;
    Input.DurationSeconds = ClampedSettings.DurationSeconds;
    Input.DeltaTimeSeconds = World->GetDeltaSeconds();
    Input.MaxEarlyReflectionTaps = 64;
    Input.NumDelayBins = FMath::Max(
        FMath::CeilToInt(ClampedSettings.DurationSeconds * static_cast<float>(ClampedSettings.SampleRate)),
        1);
    Input.HybridTransitionRatio = 0.35f;
    Input.AirAbsorptionPerMeter = Source->GetAirAbsorptionPerMeter();
    Input.EffectType = EUERayTracingAudioIndirectEffectType::Convolution;

    FUERayTracingAudioDirectSimulationInput& DirectInput = Job->DirectSimulationInput;
    DirectInput.World = World;
    DirectInput.Scene = &WorldState.Scene;
    DirectInput.ListenerLocation = Listener->GetListenerLocation();
    DirectInput.ListenerActor = Listener->GetOwner();
    DirectInput.SourceLocation = Source->GetSourceLocation();
    DirectInput.SourceForward = Source->GetSourceForward();
    DirectInput.SourceActor = Source->GetOwner();
    DirectInput.OccludedGain = Source->GetOccludedGain();
    DirectInput.SourceRadiusCm = Source->GetSourceRadiusCm();
    DirectInput.NumOcclusionSamples = Source->GetNumOcclusionSamples();
    DirectInput.bUseVolumetricOcclusion = Source->ShouldUseVolumetricOcclusion();
    DirectInput.bHardOcclusion = Source->ShouldUseHardOcclusion();
    DirectInput.AirAbsorptionPerMeter = Source->GetAirAbsorptionPerMeter();
    Job->DirectSimulationQuery = Simulator.BuildDirectSoundQuery(RayTracingDevice, DirectInput);

    FUERayTracingAudioTraceRequest DirectTraceRequest;
    DirectTraceRequest.World = World;
    DirectTraceRequest.Scene = &WorldState.Scene;
    DirectTraceRequest.IgnoredActor = Listener->GetOwner();
    DirectTraceRequest.SecondaryIgnoredActor = Source->GetOwner();
    Job->DirectQuery = RayTracingDevice.SubmitRays(
        DirectTraceRequest,
        Job->DirectSimulationQuery.Rays);
    if (!Job->DirectQuery.IsValid())
    {
        Job->SetFailed(TEXT("Could not submit the hardware direct-sound query for the bake."));
        return Job;
    }

    FUERayTracingAudioEnergyFieldTraceRequest TraceRequest;
    TraceRequest.World = World;
    TraceRequest.Scene = &WorldState.Scene;
    TraceRequest.ListenerLocation = Input.ListenerLocation;
    TraceRequest.ListenerForward = Input.ListenerForward;
    TraceRequest.SourceLocation = Input.SourceLocation;
    TraceRequest.AirAbsorptionPerMeter = Input.AirAbsorptionPerMeter;
    TraceRequest.ListenerActor = Listener->GetOwner();
    TraceRequest.SourceActor = Source->GetOwner();
    TraceRequest.NumReflectionRays = Input.NumReflectionRays;
    TraceRequest.MaxReflectionBounces = Input.MaxReflectionBounces;
    TraceRequest.NumDelayBins = Input.NumDelayBins;
    TraceRequest.DurationSeconds = Input.DurationSeconds;
    TraceRequest.ReferenceDistance = FMath::Max(Context.GetReferenceDistanceCm(), 1.0f);
    TraceRequest.SpeedOfSound = FMath::Max(Context.GetSpeedOfSoundCmPerSecond(), 1.0f);
    TraceRequest.MaxTraceDistance = FMath::Max(Context.GetMaxDistanceCm(), 100.0f);

    Job->Result.SourceWorld = FSoftObjectPath(World);
    Job->Result.SceneVersion = WorldState.Scene.GetVersion();
    Job->Result.SceneSignature = BuildSceneSignature(WorldState.Scene);
    Job->Result.SourceLocation = Source->GetSourceLocation();
    Job->Result.ListenerLocation = Listener->GetListenerLocation();
    Job->Result.BakeSettings = ClampedSettings;
    Job->Result.ChannelFormat = EUERayTracingAudioImpulseResponseChannelFormat::Stereo;
    Job->Result.NumChannels = 2;
    Job->Result.bUsedHardwareRayTracing = false;
    Job->Query = RayTracingDevice.SubmitIndirectEnergyField(TraceRequest);
    if (!Job->Query.IsValid())
    {
        Job->SetFailed(TEXT("Could not submit the hardware indirect-sound query for the bake."));
        return Job;
    }
    Job->State = EUERayTracingAudioBakeJobState::Running;
    Job->Progress = 0.1f;
    Job->StatusText = TEXT("Tracing acoustic paths on the render thread");
    ActiveBakeJobs.Add(Job);
    return Job;
}

void FUERayTracingAudioManager::PollBakeJobs()
{
    for (int32 JobIndex = ActiveBakeJobs.Num() - 1; JobIndex >= 0; --JobIndex)
    {
        const TSharedPtr<FUERayTracingAudioBakeJob>& Job = ActiveBakeJobs[JobIndex];
        if (!Job.IsValid() || Job->State == EUERayTracingAudioBakeJobState::Cancelled)
        {
            ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
            continue;
        }

        if (Job->State != EUERayTracingAudioBakeJobState::Running)
        {
            ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
            continue;
        }

        if (!Job->bDirectCompleted)
        {
            if (!Job->DirectQuery.IsValid())
            {
                Job->SetFailed(TEXT("The hardware direct-sound bake query was lost."));
                ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
                continue;
            }

            if (Job->DirectQuery->IsComplete())
            {
                Job->bDirectUsedHardwareRayTracing =
                    Job->DirectQuery->WasHardwareRayTracingUsed();
                bool bDirectSucceeded = false;
                TArray<bool> DirectHitResults;
                if (!Job->DirectQuery->ConsumeResult(bDirectSucceeded, DirectHitResults) || !bDirectSucceeded)
                {
                    Job->SetFailed(TEXT("The hardware direct-sound bake query failed."));
                    ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
                    continue;
                }
                if (Job->Result.BakeSettings.bRequireHardwareRayTracing
                    && !Job->bDirectUsedHardwareRayTracing)
                {
                    Job->SetFailed(TEXT("The direct-sound bake completed without an actual hardware ray tracing dispatch."));
                    ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
                    continue;
                }

                Job->Result.DirectResult = Simulator.FinalizeDirectSound(
                    Job->DirectSimulationInput,
                    MoveTemp(Job->DirectSimulationQuery),
                    MoveTemp(DirectHitResults));
                Job->DirectQuery.Reset();
                Job->bDirectCompleted = true;
                Job->Progress = FMath::Max(Job->Progress, 0.35f);
                Job->StatusText = TEXT("Direct path complete; tracing indirect paths");
            }
        }

        if (!Job->bIndirectCompleted)
        {
            if (!Job->Query.IsValid())
            {
                Job->SetFailed(TEXT("The hardware indirect-sound bake query was lost."));
                ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
                continue;
            }

            if (Job->Query->IsComplete())
            {
                Job->bIndirectUsedHardwareRayTracing =
                    Job->Query->WasHardwareRayTracingUsed();
                bool bSucceeded = false;
                FUERayTracingAudioEnergyFieldTraceResult TraceResult;
                if (!Job->Query->ConsumeResult(bSucceeded, TraceResult))
                {
                    continue;
                }
                Job->Query.Reset();

                if (!bSucceeded)
                {
                    Job->SetFailed(TEXT("The hardware ray tracing bake query failed."));
                    ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
                    continue;
                }
                if (Job->Result.BakeSettings.bRequireHardwareRayTracing
                    && !Job->bIndirectUsedHardwareRayTracing)
                {
                    Job->SetFailed(TEXT("The indirect-sound bake completed without an actual hardware ray tracing dispatch."));
                    ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
                    continue;
                }

                Job->Progress = 0.9f;
                Job->StatusText = TEXT("Reconstructing impulse response");
                FUERayTracingAudioIndirectSimulationResult SimulationResult = Simulator.FinalizeIndirectSound(
                    Job->SimulationInput,
                    MoveTemp(TraceResult));

                if (SimulationResult.ReconstructedImpulseResponse.IsEmpty())
                {
                    Job->SetFailed(TEXT("The bake completed but produced no impulse response samples."));
                    ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
                    continue;
                }

                Job->Result.IndirectValidPathCount = SimulationResult.NumValidPaths;
                Job->Result.IndirectGain = SimulationResult.IndirectGain;
                Job->Result.EarlyReflectionGain = SimulationResult.EarlyReflectionGain;
                Job->Result.LateReverbGain = SimulationResult.LateReverbGain;
                Job->Result.EarliestArrivalSeconds = SimulationResult.EarliestArrivalSeconds;
                Job->Result.AverageDelaySeconds = SimulationResult.AverageDelaySeconds;
                Job->Result.ReverbTimes = SimulationResult.ReverbTimes;
                Job->Result.DominantArrivalDirection = SimulationResult.DominantArrivalDirection;
                Job->Result.DirectionalEnergyRatio = SimulationResult.DirectionalEnergyRatio;
                Job->Result.DirectionalBinCount = SimulationResult.DirectionalBinCount;
                TArray<float> DirectionalStereoImpulseResponse;
                BuildDirectionalStereoImpulseResponse(
                    Job->SimulationInput,
                    SimulationResult,
                    DirectionalStereoImpulseResponse);
                for (const float Sample : DirectionalStereoImpulseResponse)
                {
                    Job->Result.ImpulseResponseEnergy += static_cast<double>(Sample) * static_cast<double>(Sample);
                }

                if (Job->bCaptureCpuReference)
                {
                    const FUERayTracingAudioIndirectSimulationResult CpuReference =
                        Simulator.SimulateIndirectSound(RayTracingDevice, Job->SimulationInput);
                    Job->Result.bHasCpuReference = true;
                    Job->Result.CpuReferenceValidPathCount = CpuReference.NumValidPaths;
                    Job->Result.CpuReferenceIndirectGain = CpuReference.IndirectGain;
                    Job->Result.CpuReferenceEarlyReflectionGain = CpuReference.EarlyReflectionGain;
                    Job->Result.CpuReferenceLateReverbGain = CpuReference.LateReverbGain;
                    Job->Result.CpuReferenceDominantArrivalDirection = CpuReference.DominantArrivalDirection;
                    Job->Result.CpuReferenceDirectionalEnergyRatio = CpuReference.DirectionalEnergyRatio;
                    Job->Result.CpuReferenceDirectionalBinCount = CpuReference.DirectionalBinCount;
                    TArray<float> CpuDirectionalStereoImpulseResponse;
                    BuildDirectionalStereoImpulseResponse(
                        Job->SimulationInput,
                        CpuReference,
                        CpuDirectionalStereoImpulseResponse);
                    for (const float Sample : CpuDirectionalStereoImpulseResponse)
                    {
                        Job->Result.CpuReferenceImpulseResponseEnergy +=
                            static_cast<double>(Sample) * static_cast<double>(Sample);
                    }
                }

                Job->Result.BinDurationSeconds = SimulationResult.ImpulseResponseBinDurationSeconds;
                Job->Result.Samples = MoveTemp(DirectionalStereoImpulseResponse);
                Job->Result.bUsedHardwareRayTracing =
                    Job->bDirectUsedHardwareRayTracing
                    && Job->bIndirectUsedHardwareRayTracing;
                Job->bIndirectCompleted = true;
            }
        }

        if (!Job->bDirectCompleted || !Job->bIndirectCompleted)
        {
            Job->Progress = FMath::Max(Job->Progress, 0.5f);
            continue;
        }

        Job->State = EUERayTracingAudioBakeJobState::Completed;
        Job->Progress = 1.0f;
        Job->StatusText = TEXT("Completed");
        ActiveBakeJobs.RemoveAtSwap(JobIndex, 1, EAllowShrinking::No);
    }
}

bool FUERayTracingAudioManager::RebuildSceneForBake(UWorld* World)
{
    if (!IsValid(World))
    {
        return false;
    }

    TArray<FUERayTracingAudioGeometryExport> GeometryExports;
    for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
    {
        TInlineComponentArray<UUERayTracingAudioGeometryComponent*> Components(*ActorIt);
        for (const UUERayTracingAudioGeometryComponent* GeometryComponent : Components)
        {
            if (!IsValid(GeometryComponent))
            {
                continue;
            }

            FUERayTracingAudioGeometryExport GeometryExport;
            if (GeometryComponent->BuildGeometryExport(GeometryExport))
            {
                GeometryExports.Add(MoveTemp(GeometryExport));
            }
        }
    }

    FWorldAcousticState& WorldState = GetOrCreateWorldAcousticState(World);
    WorldState.Scene.SetStaticGeometry(MoveTemp(GeometryExports));
    WorldState.SceneSignature = BuildSceneSignature(WorldState.Scene);
    WorldState.bSceneDirty = false;
    return true;
}

FString FUERayTracingAudioManager::BuildSceneSignature(
    const FUERayTracingAudioScene& Scene) const
{
    TArray<uint32> GeometryHashes;
    GeometryHashes.Reserve(Scene.GetStaticGeometry().Num());
    for (const FUERayTracingAudioGeometryExport& Geometry : Scene.GetStaticGeometry())
    {
        uint32 GeometryHash = GetTypeHash(Geometry.Transform.ToString());
        GeometryHash = HashCombineFast(GeometryHash, GetTypeHash(Geometry.StaticMeshCacheKey));
        GeometryHash = FCrc::MemCrc32(&Geometry.Extent, sizeof(Geometry.Extent), GeometryHash);
        GeometryHash = FCrc::MemCrc32(&Geometry.Absorption, sizeof(Geometry.Absorption), GeometryHash);
        GeometryHash = FCrc::MemCrc32(&Geometry.Transmission, sizeof(Geometry.Transmission), GeometryHash);
        GeometryHash = FCrc::MemCrc32(&Geometry.Scattering, sizeof(Geometry.Scattering), GeometryHash);
        GeometryHash = HashCombineFast(GeometryHash, GetTypeHash(Geometry.bVisibleForDirectSound));
        GeometryHash = HashCombineFast(GeometryHash, GetTypeHash(Geometry.bUseStaticMeshTriangles));
        if (!Geometry.Vertices.IsEmpty())
        {
            GeometryHash = FCrc::MemCrc32(
                Geometry.Vertices.GetData(),
                Geometry.Vertices.Num() * sizeof(FVector),
                GeometryHash);
        }
        if (!Geometry.Indices.IsEmpty())
        {
            GeometryHash = FCrc::MemCrc32(
                Geometry.Indices.GetData(),
                Geometry.Indices.Num() * sizeof(uint32),
                GeometryHash);
        }
        GeometryHashes.Add(GeometryHash);
    }
    GeometryHashes.Sort();
    const uint32 Hash = GeometryHashes.IsEmpty()
        ? 0u
        : FCrc::MemCrc32(GeometryHashes.GetData(), GeometryHashes.Num() * sizeof(uint32));
    return FString::Printf(TEXT("%08X"), Hash);
}

FUERayTracingAudioManager::FWorldAcousticState&
FUERayTracingAudioManager::GetOrCreateWorldAcousticState(UWorld* World)
{
    check(IsInGameThread());
    check(IsValid(World));

    TUniquePtr<FWorldAcousticState>& WorldState =
        AcousticStatesByWorld.FindOrAdd(World);
    if (!WorldState)
    {
        WorldState = MakeUnique<FWorldAcousticState>();
    }
    return *WorldState;
}

void FUERayTracingAudioManager::CancelSourceQueries(
    FSourceSimulationState& State)
{
    if (State.DirectQuery.IsValid())
    {
        State.DirectQuery->Cancel();
        State.DirectQuery.Reset();
    }
    if (State.IndirectQuery.IsValid())
    {
        State.IndirectQuery->Cancel();
        State.IndirectQuery.Reset();
    }
    State.PendingDirectInput =
        FUERayTracingAudioDirectSimulationInput();
    State.PendingDirectQuery =
        FUERayTracingAudioDirectSimulationQuery();
    State.PendingIndirectInput =
        FUERayTracingAudioIndirectSimulationInput();
    State.bDirectRequested = false;
    State.bIndirectRequested = false;
    State.bQueued = false;
    State.FirstQueuedTimeSeconds = 0.0;
}

void FUERayTracingAudioManager::InvalidateWorldSources(UWorld* World)
{
    if (!IsValid(World))
    {
        return;
    }

    TSet<TWeakObjectPtr<UUERayTracingAudioSourceComponent>>
        InvalidatedSources;
    for (TPair<
            TWeakObjectPtr<UUERayTracingAudioSourceComponent>,
            FSourceSimulationState>& Pair : SourceSimulationStates)
    {
        UUERayTracingAudioSourceComponent* Source = Pair.Key.Get();
        FSourceSimulationState& State = Pair.Value;
        const bool bMatchesWorld =
            (IsValid(Source) && Source->GetWorld() == World)
            || State.PendingDirectInput.World == World
            || State.PendingIndirectInput.World == World;
        if (!bMatchesWorld)
        {
            continue;
        }

        const bool bAlreadyInvalidated =
            !State.DirectQuery.IsValid()
            && !State.IndirectQuery.IsValid()
            && !State.bDirectRequested
            && !State.bIndirectRequested
            && !State.bQueued
            && State.LatestResult.bHasDirectResult
            && !State.LatestResult.DirectResult.bHasListener
            && State.LatestResult.bHasIndirectResult
            && !State.LatestResult.IndirectResult.bHasListener;
        CancelSourceQueries(State);
        InvalidatedSources.Add(Pair.Key);
        if (bAlreadyInvalidated)
        {
            continue;
        }

        State.LatestResult.DirectResult =
            FUERayTracingAudioDirectSimulationResult();
        State.LatestResult.IndirectResult =
            FUERayTracingAudioIndirectSimulationResult();
        State.LatestResult.bHasDirectResult = true;
        State.LatestResult.bHasIndirectResult = true;
        State.LatestResult.DirectGeneration =
            ++NextSimulationGeneration;
        State.LatestResult.IndirectGeneration =
            ++NextSimulationGeneration;
        State.LatestResult.Generation =
            State.LatestResult.IndirectGeneration;
    }

    PendingSimulationSources.RemoveAll(
        [&InvalidatedSources](
            const TWeakObjectPtr<
                UUERayTracingAudioSourceComponent>& PendingSource)
        {
            return !PendingSource.IsValid()
                || InvalidatedSources.Contains(PendingSource);
        });

    for (int32 JobIndex = ActiveBakeJobs.Num() - 1;
        JobIndex >= 0;
        --JobIndex)
    {
        const TSharedPtr<FUERayTracingAudioBakeJob>& Job =
            ActiveBakeJobs[JobIndex];
        if (Job.IsValid()
            && (Job->SimulationInput.World == World
                || Job->DirectSimulationInput.World == World))
        {
            Job->Cancel();
            ActiveBakeJobs.RemoveAtSwap(
                JobIndex,
                1,
                EAllowShrinking::No);
        }
    }
}

void FUERayTracingAudioManager::RemoveDeadWorldState()
{
    check(IsInGameThread());

    TArray<TWeakObjectPtr<UWorld>> WorldsWithoutListeners;
    for (auto ListenerIt = ListenersByWorld.CreateIterator(); ListenerIt; ++ListenerIt)
    {
        if (ListenerIt.Key().IsValid()
            && !ListenerIt.Value().IsValid())
        {
            WorldsWithoutListeners.Add(ListenerIt.Key());
        }
        if (!ListenerIt.Key().IsValid()
            || !ListenerIt.Value().IsValid())
        {
            ListenerIt.RemoveCurrent();
        }
    }
    for (const TWeakObjectPtr<UWorld>& World : WorldsWithoutListeners)
    {
        InvalidateWorldSources(World.Get());
    }

    for (auto SourceIt = SourceSimulationStates.CreateIterator();
        SourceIt;
        ++SourceIt)
    {
        if (!SourceIt.Key().IsValid())
        {
            CancelSourceQueries(SourceIt.Value());
            SourceIt.RemoveCurrent();
        }
    }

    for (auto StateIt = AcousticStatesByWorld.CreateIterator(); StateIt; ++StateIt)
    {
        if (StateIt.Key().IsValid())
        {
            continue;
        }

        const FUERayTracingAudioScene* Scene = &StateIt.Value()->Scene;
        for (TPair<
            TWeakObjectPtr<UUERayTracingAudioSourceComponent>,
            FSourceSimulationState>& Pair : SourceSimulationStates)
        {
            if (Pair.Value.PendingDirectInput.Scene == Scene
                || Pair.Value.PendingIndirectInput.Scene == Scene)
            {
                CancelSourceQueries(Pair.Value);
            }
        }
        for (int32 JobIndex = ActiveBakeJobs.Num() - 1;
            JobIndex >= 0;
            --JobIndex)
        {
            const TSharedPtr<FUERayTracingAudioBakeJob>& Job =
                ActiveBakeJobs[JobIndex];
            if (Job.IsValid()
                && (Job->SimulationInput.Scene == Scene
                    || Job->DirectSimulationInput.Scene == Scene))
            {
                Job->Cancel();
                ActiveBakeJobs.RemoveAtSwap(
                    JobIndex,
                    1,
                    EAllowShrinking::No);
            }
        }
        StateIt.RemoveCurrent();
    }
}

void FUERayTracingAudioManager::PollCompletedDirectQueries()
{
    for (TPair<TWeakObjectPtr<UUERayTracingAudioSourceComponent>, FSourceSimulationState>& Pair : SourceSimulationStates)
    {
        FSourceSimulationState& State = Pair.Value;
        if (!State.DirectQuery.IsValid() || !State.DirectQuery->IsComplete())
        {
            continue;
        }

        bool bSucceeded = false;
        TArray<bool> HitResults;
        const bool bUsedHardwareRayTracing =
            State.DirectQuery->WasHardwareRayTracingUsed();
        if (!State.DirectQuery->ConsumeResult(bSucceeded, HitResults))
        {
            continue;
        }

        State.DirectQuery.Reset();
        UUERayTracingAudioSourceComponent* Source = Pair.Key.Get();
        if (bSucceeded && IsValid(Source) && Source->bEnableDirectSound)
        {
            State.PendingDirectQuery.BaseResult.
                bUsedHardwareRayTracing =
                    bUsedHardwareRayTracing;
            State.LatestResult.DirectResult = StabilizeDirectResult(
                Simulator.FinalizeDirectSound(
                    State.PendingDirectInput,
                    MoveTemp(State.PendingDirectQuery),
                    MoveTemp(HitResults)),
                State.LatestResult,
                State.PendingDirectInput.World
                    ? State.PendingDirectInput.World->GetDeltaSeconds()
                    : 1.0f / 60.0f);
            State.LatestResult.bHasDirectResult = true;
            State.LatestResult.DirectGeneration = ++NextSimulationGeneration;
            State.LatestResult.Generation = State.LatestResult.DirectGeneration;
        }
    }
}

void FUERayTracingAudioManager::PollCompletedIndirectQueries()
{
    for (TPair<TWeakObjectPtr<UUERayTracingAudioSourceComponent>, FSourceSimulationState>& Pair : SourceSimulationStates)
    {
        FSourceSimulationState& State = Pair.Value;
        if (!State.IndirectQuery.IsValid() || !State.IndirectQuery->IsComplete())
        {
            continue;
        }

        bool bSucceeded = false;
        FUERayTracingAudioEnergyFieldTraceResult TraceResult;
        const bool bUsedHardwareRayTracing =
            State.IndirectQuery->WasHardwareRayTracingUsed();
        if (!State.IndirectQuery->ConsumeResult(bSucceeded, TraceResult))
        {
            continue;
        }

        State.IndirectQuery.Reset();
        UUERayTracingAudioSourceComponent* Source = Pair.Key.Get();
        if (bSucceeded && IsValid(Source) && Source->bEnableIndirectSound)
        {
            State.LatestResult.IndirectResult = Simulator.FinalizeIndirectSound(
                State.PendingIndirectInput,
                MoveTemp(TraceResult));
            State.LatestResult.IndirectResult.
                bUsedHardwareRayTracing =
                    bUsedHardwareRayTracing;
            State.LatestResult.bHasIndirectResult = true;
            State.LatestResult.IndirectGeneration = ++NextSimulationGeneration;
            State.LatestResult.Generation = State.LatestResult.IndirectGeneration;
        }
    }
}

const FUERayTracingAudioScene& FUERayTracingAudioManager::GetScene(UWorld* World)
{
    check(IsInGameThread());
    check(IsValid(World));
    RebuildScene(World);
    return GetOrCreateWorldAcousticState(World).Scene;
}

const FUERayTracingAudioRayTracingDevice& FUERayTracingAudioManager::GetRayTracingDevice() const
{
    return RayTracingDevice;
}

FUERayTracingAudioSimulationSnapshotRegistry& FUERayTracingAudioManager::GetSnapshotRegistry()
{
    return SnapshotRegistry.Get();
}

TSharedRef<
    FUERayTracingAudioSimulationSnapshotRegistry,
    ESPMode::ThreadSafe>
FUERayTracingAudioManager::GetSnapshotRegistrySharedRef()
{
    return SnapshotRegistry;
}
