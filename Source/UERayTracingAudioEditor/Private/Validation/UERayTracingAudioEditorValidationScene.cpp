#include "Validation/UERayTracingAudioEditorValidationScene.h"

#include "Camera/CameraActor.h"
#include "Components/AudioComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Editor.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ScopedTransaction.h"

namespace
{
    const FName ValidationSceneTag(TEXT("VRTA_EditorValidationScene"));
    const FVector ValidationOrigin(5000.0, 0.0, 0.0);
    const FVector ListenerOffset(-100.0, 0.0, 120.0);
    const FVector ClearSourceOffset(-100.0, 200.0, 120.0);
    const FVector OccludedSourceOffset(100.0, 0.0, 120.0);

    struct FGeometryDefinition
    {
        const TCHAR* Label;
        const TCHAR* Role;
        FVector Offset;
        FVector Scale;
        FVector Absorption;
        float Scattering;
    };

    const FGeometryDefinition EnclosedGeometryDefinitions[] =
    {
        { TEXT("VRTA A-B Floor"), TEXT("VRTA_AB_Floor"), FVector(0.0, 0.0, 0.0), FVector(12.0, 10.0, 0.2), FVector(0.08, 0.10, 0.14), 0.20f },
        { TEXT("VRTA A-B Ceiling"), TEXT("VRTA_AB_Ceiling"), FVector(0.0, 0.0, 400.0), FVector(12.0, 10.0, 0.2), FVector(0.22, 0.30, 0.45), 0.35f },
        { TEXT("VRTA A-B Back Wall"), TEXT("VRTA_AB_BackWall"), FVector(0.0, 500.0, 200.0), FVector(12.0, 0.2, 4.0), FVector(0.12, 0.18, 0.28), 0.30f },
        { TEXT("VRTA A-B Front Wall"), TEXT("VRTA_AB_FrontWall"), FVector(0.0, -500.0, 200.0), FVector(12.0, 0.2, 4.0), FVector(0.12, 0.18, 0.28), 0.30f },
        { TEXT("VRTA A-B Left Wall"), TEXT("VRTA_AB_LeftWall"), FVector(-600.0, 0.0, 200.0), FVector(0.2, 10.0, 4.0), FVector(0.18, 0.24, 0.38), 0.40f },
        { TEXT("VRTA A-B Right Wall"), TEXT("VRTA_AB_RightWall"), FVector(600.0, 0.0, 200.0), FVector(0.2, 10.0, 4.0), FVector(0.18, 0.24, 0.38), 0.40f },
        { TEXT("VRTA A-B Occlusion Wall"), TEXT("VRTA_AB_OcclusionWall"), FVector(0.0, 0.0, 100.0), FVector(0.3, 5.0, 2.0), FVector(0.06, 0.10, 0.18), 0.20f }
    };
    // A single wall 250 cm from the Source/Listener pair (nothing else nearby), used by
    // the R3 near-wall reflection validation to prove early reflections still form with
    // partial-but-sparse geometry rather than only in a fully enclosed room.
    const FGeometryDefinition NearWallGeometryDefinitions[] =
    {
        { TEXT("VRTA A-B Near Wall"), TEXT("VRTA_AB_NearWall"), FVector(0.0, 250.0, 200.0), FVector(6.0, 0.2, 4.0), FVector(0.12, 0.18, 0.28), 0.30f }
    };

    TArrayView<const FGeometryDefinition> GetGeometryDefinitions(
        const EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment)
    {
        switch (ReflectionEnvironment)
        {
        case EUERayTracingAudioEditorReflectionEnvironment::OpenSpace:
            // No geometry at all: baseline for the R3 open-space reflection validation,
            // which needs a near-zero indirect gain floor to compare Enclosed/NearWall against.
            return TArrayView<const FGeometryDefinition>();
        case EUERayTracingAudioEditorReflectionEnvironment::NearWall:
            return MakeArrayView(NearWallGeometryDefinitions);
        default:
            return MakeArrayView(EnclosedGeometryDefinitions);
        }
    }

    EObjectFlags GetObjectFlags(const EUERayTracingAudioEditorValidationSceneMode Mode)
    {
        return Mode == EUERayTracingAudioEditorValidationSceneMode::Transient
            ? RF_Transient
            : RF_Transactional;
    }

    AActor* FindTaggedActor(UWorld& World, const FName Role)
    {
        for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
        {
            if (ActorIt->ActorHasTag(ValidationSceneTag) && ActorIt->ActorHasTag(Role))
            {
                return *ActorIt;
            }
        }
        return nullptr;
    }

    void TagActor(AActor& Actor, const FName Role, const TCHAR* Label)
    {
        Actor.Tags.AddUnique(ValidationSceneTag);
        Actor.Tags.AddUnique(Role);
        Actor.SetActorLabel(Label);
    }

    AStaticMeshActor* EnsureStaticMeshActor(
        UWorld& World,
        UStaticMesh& Mesh,
        const TCHAR* Label,
        const TCHAR* Role,
        const FTransform& Transform,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutCreated)
    {
        const FName RoleName(Role);
        if (AActor* Existing = FindTaggedActor(World, RoleName))
        {
            return Cast<AStaticMeshActor>(Existing);
        }

        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = GetObjectFlags(Mode);
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(),
            Transform,
            SpawnParameters);
        if (!Actor || !Actor->GetStaticMeshComponent())
        {
            return nullptr;
        }

        TagActor(*Actor, RoleName, Label);
        UStaticMeshComponent* MeshComponent = Actor->GetStaticMeshComponent();
        MeshComponent->SetStaticMesh(&Mesh);
        MeshComponent->SetMobility(EComponentMobility::Static);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        MeshComponent->SetVisibility(true, true);
        bOutCreated = true;
        return Actor;
    }

    UUERayTracingAudioGeometryComponent* EnsureGeometryComponent(
        AStaticMeshActor& Actor,
        const FGeometryDefinition& Definition,
        const EUERayTracingAudioEditorValidationSceneMode Mode)
    {
        if (UUERayTracingAudioGeometryComponent* Existing =
            Actor.FindComponentByClass<UUERayTracingAudioGeometryComponent>())
        {
            return Existing;
        }

        UUERayTracingAudioGeometryComponent* Geometry =
            NewObject<UUERayTracingAudioGeometryComponent>(
                &Actor,
                TEXT("ValidationAcousticGeometry"),
                GetObjectFlags(Mode));
        if (!Geometry)
        {
            return nullptr;
        }

        Actor.AddInstanceComponent(Geometry);
        Geometry->bExportToAcousticScene = true;
        Geometry->bAffectsDirectSound = true;
        Geometry->ExportMode = EUERayTracingAudioGeometryExportMode::BoundingBox;
        Geometry->Absorption = Definition.Absorption;
        Geometry->Transmission = FVector::ZeroVector;
        Geometry->Scattering = Definition.Scattering;
        Geometry->RegisterComponent();
        return Geometry;
    }

    void EnsurePointLight(
        UWorld& World,
        const TCHAR* Label,
        const TCHAR* Role,
        const FVector& Location,
        const FLinearColor& Color,
        const float Intensity,
        const float Radius,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutCreated)
    {
        if (APointLight* Existing = Cast<APointLight>(FindTaggedActor(World, FName(Role))))
        {
            Existing->SetActorLocation(Location);
            if (Existing->PointLightComponent)
            {
                Existing->PointLightComponent->SetIntensity(Intensity);
                Existing->PointLightComponent->SetAttenuationRadius(Radius);
                Existing->PointLightComponent->SetLightColor(Color);
                Existing->PointLightComponent->SetCastShadows(true);
            }
            return;
        }

        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = GetObjectFlags(Mode);
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        APointLight* Light = World.SpawnActor<APointLight>(
            APointLight::StaticClass(),
            FTransform(FRotator::ZeroRotator, Location),
            SpawnParameters);
        if (!Light || !Light->PointLightComponent)
        {
            return;
        }

        TagActor(*Light, FName(Role), Label);
        Light->PointLightComponent->SetIntensity(Intensity);
        Light->PointLightComponent->SetAttenuationRadius(Radius);
        Light->PointLightComponent->SetLightColor(Color);
        Light->PointLightComponent->SetCastShadows(true);
        bOutCreated = true;
    }

    void FocusEditorView(const FBox& Bounds)
    {
        if (GEditor && Bounds.IsValid)
        {
            GEditor->MoveViewportCamerasToBox(Bounds.ExpandBy(120.0), false);
        }
    }
}

FUERayTracingAudioEditorValidationSceneResult FUERayTracingAudioEditorValidationScene::EnsureScene(
    UWorld& World,
    const EUERayTracingAudioEditorValidationSceneMode Mode,
    const EUERayTracingAudioEditorDirectPreset DirectPreset,
    const EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment,
    const float DistanceCmOverride)
{
    FUERayTracingAudioEditorValidationSceneResult Result;
    Result.DirectPreset = DirectPreset;
    Result.ReflectionEnvironment = ReflectionEnvironment;
    if (World.IsGameWorld())
    {
        Result.Message = TEXT("The Editor A/B validation scene can only be created in an editor world.");
        return Result;
    }

    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (!IsValid(CubeMesh) || !IsValid(SphereMesh))
    {
        Result.Message = TEXT("Could not load the engine Cube and Sphere meshes for the Editor A/B scene.");
        return Result;
    }

    TUniquePtr<FScopedTransaction> Transaction;
    if (Mode == EUERayTracingAudioEditorValidationSceneMode::Persistent)
    {
        Transaction = MakeUnique<FScopedTransaction>(
            FText::FromString(TEXT("Create UE Ray Tracing Audio A/B Validation Scene")));
        World.Modify();
    }

    const TArrayView<const FGeometryDefinition> GeometryDefinitions = GetGeometryDefinitions(ReflectionEnvironment);
    FBox SceneBounds(ForceInit);
    for (const FGeometryDefinition& Definition : GeometryDefinitions)
    {
        AStaticMeshActor* GeometryActor = EnsureStaticMeshActor(
            World,
            *CubeMesh,
            Definition.Label,
            Definition.Role,
            FTransform(FRotator::ZeroRotator, ValidationOrigin + Definition.Offset, Definition.Scale),
            Mode,
            Result.bCreatedActors);
        if (!GeometryActor || !EnsureGeometryComponent(*GeometryActor, Definition, Mode))
        {
            Result.Message = FString::Printf(TEXT("Could not create validation geometry: %s."), Definition.Label);
            return Result;
        }
        SceneBounds += GeometryActor->GetComponentsBoundingBox(true);
        ++Result.GeometryCount;
    }

    AStaticMeshActor* SourceActor = EnsureStaticMeshActor(
        World,
        *SphereMesh,
        TEXT("VRTA A-B Primary Source (Orange)"),
        TEXT("VRTA_AB_Source"),
        FTransform(FRotator::ZeroRotator, ValidationOrigin + ClearSourceOffset, FVector(0.38)),
        Mode,
        Result.bCreatedActors);
    if (!SourceActor)
    {
        Result.Message = TEXT("Could not create the Editor A/B source actor.");
        return Result;
    }
    // DistanceCmOverride only applies to the Clear preset: it slides the source further
    // along the existing listener-to-source line of sight so the R2 distance-scan and
    // air-absorption validation can compare several distances without disturbing the
    // occluded presets' geometry-relative offsets.
    FVector ClearOffsetForThisRun = ClearSourceOffset;
    if (DistanceCmOverride > 0.0f)
    {
        const FVector Direction = (ClearSourceOffset - ListenerOffset).GetSafeNormal();
        ClearOffsetForThisRun = ListenerOffset + (Direction * DistanceCmOverride);
    }
    SourceActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
    SourceActor->SetActorLocation(
        ValidationOrigin + (DirectPreset == EUERayTracingAudioEditorDirectPreset::Clear
            ? ClearOffsetForThisRun
            : OccludedSourceOffset));
    SourceActor->SetActorLabel(FString::Printf(
        TEXT("VRTA A-B Primary Source (Orange) - %s"),
        GetDirectPresetName(DirectPreset)));
    UUERayTracingAudioSourceComponent* Source =
        SourceActor->FindComponentByClass<UUERayTracingAudioSourceComponent>();
    if (!Source)
    {
        Source = NewObject<UUERayTracingAudioSourceComponent>(
            SourceActor,
            TEXT("ValidationSource"),
            GetObjectFlags(Mode));
        SourceActor->AddInstanceComponent(Source);
        Source->RegisterComponent();
    }
    Source->OccludedGain = 0.35f;
    Source->SourceRadiusCm = 30.0f;
    Source->NumOcclusionSamples = 8;
    Source->bUseVolumetricOcclusion = true;
    Source->bHardOcclusion = DirectPreset == EUERayTracingAudioEditorDirectPreset::HardOccluded;
    Source->NumReflectionRays = 4096;
    Source->MaxReflectionBounces = 8;
    Source->IndirectDurationSeconds = 2.0f;
    Source->IndirectMode = EUERayTracingAudioIndirectMode::HybridReverb;
    Source->IndirectDataSource = EUERayTracingAudioIndirectDataSource::Realtime;
    // This scene is an explicit listening fixture, so reflections and the
    // late-reverb tail should be readily audible without hiding the direct cue.
    Source->IndirectMix = 1.0f;
    if (!SourceActor->FindComponentByClass<UAudioComponent>())
    {
        UAudioComponent* Audio = NewObject<UAudioComponent>(
            SourceActor,
            TEXT("ValidationAudio"),
            GetObjectFlags(Mode));
        SourceActor->AddInstanceComponent(Audio);
        Audio->bAutoActivate = false;
        Audio->bAllowSpatialization = true;
        Audio->RegisterComponent();
    }
    Result.Source = Source;

    AStaticMeshActor* ListenerActor = EnsureStaticMeshActor(
        World,
        *SphereMesh,
        TEXT("VRTA A-B Listener (Blue)"),
        TEXT("VRTA_AB_Listener"),
        FTransform(FRotator::ZeroRotator, ValidationOrigin + ListenerOffset, FVector(0.32)),
        Mode,
        Result.bCreatedActors);
    if (!ListenerActor)
    {
        Result.Message = TEXT("Could not create the Editor A/B listener actor.");
        return Result;
    }
    ListenerActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
    ListenerActor->SetActorLocation(ValidationOrigin + ListenerOffset);
    UUERayTracingAudioListenerComponent* Listener =
        ListenerActor->FindComponentByClass<UUERayTracingAudioListenerComponent>();
    if (!Listener)
    {
        Listener = NewObject<UUERayTracingAudioListenerComponent>(
            ListenerActor,
            TEXT("ValidationListener"),
            GetObjectFlags(Mode));
        ListenerActor->AddInstanceComponent(Listener);
        Listener->RegisterComponent();
    }
    Result.Listener = Listener;
    Result.SourceListenerDistanceCm = FVector::Distance(
        SourceActor->GetActorLocation(),
        ListenerActor->GetActorLocation());

    EnsurePointLight(
        World,
        TEXT("VRTA A-B Room Light"),
        TEXT("VRTA_AB_RoomLight"),
        ValidationOrigin + FVector(-160.0, -120.0, 330.0),
        FLinearColor(1.0f, 0.82f, 0.58f),
        12000.0f,
        1400.0f,
        Mode,
        Result.bCreatedActors);
    EnsurePointLight(
        World,
        TEXT("VRTA A-B Source Light (Orange)"),
        TEXT("VRTA_AB_SourceLight"),
        SourceActor->GetActorLocation() + FVector(0.0, 0.0, 35.0),
        FLinearColor(1.0f, 0.16f, 0.02f),
        2600.0f,
        420.0f,
        Mode,
        Result.bCreatedActors);
    EnsurePointLight(
        World,
        TEXT("VRTA A-B Listener Light (Blue)"),
        TEXT("VRTA_AB_ListenerLight"),
        ListenerActor->GetActorLocation() + FVector(0.0, 0.0, 35.0),
        FLinearColor(0.04f, 0.28f, 1.0f),
        2200.0f,
        380.0f,
        Mode,
        Result.bCreatedActors);

    if (!FindTaggedActor(World, FName(TEXT("VRTA_AB_Camera"))))
    {
        const FVector CameraLocation = ValidationOrigin + FVector(-480.0, -520.0, 280.0);
        const FVector CameraTarget = ValidationOrigin + FVector(30.0, 0.0, 150.0);
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = GetObjectFlags(Mode);
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (ACameraActor* Camera = World.SpawnActor<ACameraActor>(
            ACameraActor::StaticClass(),
            FTransform((CameraTarget - CameraLocation).Rotation(), CameraLocation),
            SpawnParameters))
        {
            TagActor(*Camera, FName(TEXT("VRTA_AB_Camera")), TEXT("VRTA A-B Validation Camera"));
            Result.bCreatedActors = true;
        }
    }

    SceneBounds += SourceActor->GetComponentsBoundingBox(true);
    SceneBounds += ListenerActor->GetComponentsBoundingBox(true);
    FocusEditorView(SceneBounds);

    if (Mode == EUERayTracingAudioEditorValidationSceneMode::Persistent && Result.bCreatedActors)
    {
        World.MarkPackageDirty();
    }

    Result.bSucceeded = Result.Source.IsValid()
        && Result.Listener.IsValid()
        && Result.GeometryCount == GeometryDefinitions.Num();
    Result.Message = Result.bSucceeded
        ? FString::Printf(
            TEXT("Editor A/B validation scene ready: source=1 listener=1 geometry=%d lighting=1 direct_preset=%s reflection_environment=%s source_listener_distance_cm=%.2f."),
            Result.GeometryCount,
            GetDirectPresetName(DirectPreset),
            GetReflectionEnvironmentName(ReflectionEnvironment),
            Result.SourceListenerDistanceCm)
        : TEXT("Editor A/B validation scene is incomplete.");
    return Result;
}

EUERayTracingAudioEditorDirectPreset FUERayTracingAudioEditorValidationScene::ParseDirectPreset(
    const FString& Value)
{
    if (Value.Equals(TEXT("soft_occluded"), ESearchCase::IgnoreCase))
    {
        return EUERayTracingAudioEditorDirectPreset::SoftOccluded;
    }
    if (Value.Equals(TEXT("hard_occluded"), ESearchCase::IgnoreCase))
    {
        return EUERayTracingAudioEditorDirectPreset::HardOccluded;
    }
    return EUERayTracingAudioEditorDirectPreset::Clear;
}

const TCHAR* FUERayTracingAudioEditorValidationScene::GetDirectPresetName(
    const EUERayTracingAudioEditorDirectPreset DirectPreset)
{
    switch (DirectPreset)
    {
    case EUERayTracingAudioEditorDirectPreset::SoftOccluded:
        return TEXT("soft_occluded");
    case EUERayTracingAudioEditorDirectPreset::HardOccluded:
        return TEXT("hard_occluded");
    default:
        return TEXT("clear");
    }
}

EUERayTracingAudioEditorReflectionEnvironment FUERayTracingAudioEditorValidationScene::ParseReflectionEnvironment(
    const FString& Value)
{
    if (Value.Equals(TEXT("open_space"), ESearchCase::IgnoreCase))
    {
        return EUERayTracingAudioEditorReflectionEnvironment::OpenSpace;
    }
    if (Value.Equals(TEXT("near_wall"), ESearchCase::IgnoreCase))
    {
        return EUERayTracingAudioEditorReflectionEnvironment::NearWall;
    }
    return EUERayTracingAudioEditorReflectionEnvironment::Enclosed;
}

const TCHAR* FUERayTracingAudioEditorValidationScene::GetReflectionEnvironmentName(
    const EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment)
{
    switch (ReflectionEnvironment)
    {
    case EUERayTracingAudioEditorReflectionEnvironment::OpenSpace:
        return TEXT("open_space");
    case EUERayTracingAudioEditorReflectionEnvironment::NearWall:
        return TEXT("near_wall");
    default:
        return TEXT("enclosed");
    }
}
