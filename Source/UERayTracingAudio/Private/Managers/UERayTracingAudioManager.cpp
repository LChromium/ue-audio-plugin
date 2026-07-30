#include "Managers/UERayTracingAudioManager.h"

#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Engine/World.h"
#include "Misc/Crc.h"
#include "UERayTracingAudioModule.h"

FUERayTracingAudioManager::FUERayTracingAudioManager(
    const FUERayTracingAudioContextSettings& ContextSettings)
    : Context(ContextSettings)
    , Simulator(Context)
{
    WorldStateTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(
            this,
            &FUERayTracingAudioManager::TickWorldStateCleanup));
}

FUERayTracingAudioManager::~FUERayTracingAudioManager()
{
    if (WorldStateTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(WorldStateTickerHandle);
        WorldStateTickerHandle.Reset();
    }
}

void FUERayTracingAudioManager::AddSource(
    UUERayTracingAudioSourceComponent* Source)
{
    Sources.Add(Source);
}

void FUERayTracingAudioManager::RemoveSource(
    UUERayTracingAudioSourceComponent* Source)
{
    Sources.Remove(Source);
}

void FUERayTracingAudioManager::AddListener(
    UUERayTracingAudioListenerComponent* Listener)
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

void FUERayTracingAudioManager::RemoveListener(
    UUERayTracingAudioListenerComponent* Listener)
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
    }
}

void FUERayTracingAudioManager::AddGeometry(
    UUERayTracingAudioGeometryComponent* Geometry)
{
    GeometryComponents.Add(Geometry);
    MarkSceneDirty(IsValid(Geometry) ? Geometry->GetWorld() : nullptr);
}

void FUERayTracingAudioManager::RemoveGeometry(
    UUERayTracingAudioGeometryComponent* Geometry)
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

    for (const TWeakObjectPtr<UUERayTracingAudioGeometryComponent>& GeometryComponent
        : GeometryComponents)
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

UUERayTracingAudioListenerComponent*
FUERayTracingAudioManager::GetCurrentListener(const UWorld* World) const
{
    const TWeakObjectPtr<UUERayTracingAudioListenerComponent>* Listener =
        IsValid(World) ? ListenersByWorld.Find(World) : nullptr;
    return Listener && Listener->IsValid() ? Listener->Get() : nullptr;
}

FUERayTracingAudioDirectSimulationResult
FUERayTracingAudioManager::SimulateDirectSource(
    UUERayTracingAudioSourceComponent* Source)
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
    OutInput.AirAbsorptionPerMeter = Source->GetAirAbsorptionPerMeter();
    return true;
}

FUERayTracingAudioIndirectSimulationResult
FUERayTracingAudioManager::SimulateIndirectSource(
    UUERayTracingAudioSourceComponent* Source)
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
        OutInput.EffectType =
            EUERayTracingAudioIndirectEffectType::Parametric;
        break;
    case EUERayTracingAudioIndirectMode::HybridReverb:
        OutInput.EffectType = EUERayTracingAudioIndirectEffectType::Hybrid;
        break;
    default:
        OutInput.EffectType =
            EUERayTracingAudioIndirectEffectType::Convolution;
        break;
    }

    return true;
}

FString FUERayTracingAudioManager::BuildSceneSignature(
    const FUERayTracingAudioScene& Scene) const
{
    TArray<uint32> GeometryHashes;
    GeometryHashes.Reserve(Scene.GetStaticGeometry().Num());
    for (const FUERayTracingAudioGeometryExport& Geometry
        : Scene.GetStaticGeometry())
    {
        uint32 GeometryHash = GetTypeHash(Geometry.Transform.ToString());
        GeometryHash = FCrc::MemCrc32(
            &Geometry.Extent,
            sizeof(Geometry.Extent),
            GeometryHash);
        GeometryHash = FCrc::MemCrc32(
            &Geometry.Absorption,
            sizeof(Geometry.Absorption),
            GeometryHash);
        GeometryHash = HashCombineFast(
            GeometryHash,
            GetTypeHash(Geometry.bVisibleForDirectSound));
        GeometryHash = HashCombineFast(
            GeometryHash,
            GetTypeHash(Geometry.bUseStaticMeshTriangles));
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
        : FCrc::MemCrc32(
            GeometryHashes.GetData(),
            GeometryHashes.Num() * sizeof(uint32));
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

void FUERayTracingAudioManager::RemoveDeadWorldState()
{
    check(IsInGameThread());

    for (auto ListenerIt = ListenersByWorld.CreateIterator(); ListenerIt;
        ++ListenerIt)
    {
        if (!ListenerIt.Key().IsValid() || !ListenerIt.Value().IsValid())
        {
            ListenerIt.RemoveCurrent();
        }
    }

    for (auto StateIt = AcousticStatesByWorld.CreateIterator(); StateIt;
        ++StateIt)
    {
        if (!StateIt.Key().IsValid())
        {
            StateIt.RemoveCurrent();
        }
    }
}

bool FUERayTracingAudioManager::TickWorldStateCleanup(float)
{
    check(IsInGameThread());
    RemoveDeadWorldState();
    return true;
}

const FUERayTracingAudioScene& FUERayTracingAudioManager::GetScene(
    UWorld* World)
{
    check(IsInGameThread());
    check(IsValid(World));
    RebuildScene(World);
    return GetOrCreateWorldAcousticState(World).Scene;
}

const FUERayTracingAudioRayTracingDevice&
FUERayTracingAudioManager::GetRayTracingDevice() const
{
    return RayTracingDevice;
}
