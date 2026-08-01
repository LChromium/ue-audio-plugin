#include "Validation/UERayTracingAudioEditorValidationScene.h"

#include "Algo/AnyOf.h"
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
    constexpr float ValidationWetMix = 0.8f;

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

    const FName KnownValidationRoles[] =
    {
        FName(TEXT("VRTA_AB_Floor")),
        FName(TEXT("VRTA_AB_Ceiling")),
        FName(TEXT("VRTA_AB_BackWall")),
        FName(TEXT("VRTA_AB_FrontWall")),
        FName(TEXT("VRTA_AB_LeftWall")),
        FName(TEXT("VRTA_AB_RightWall")),
        FName(TEXT("VRTA_AB_OcclusionWall")),
        FName(TEXT("VRTA_AB_NearWall")),
        FName(TEXT("VRTA_AB_Source")),
        FName(TEXT("VRTA_AB_Listener")),
        FName(TEXT("VRTA_AB_RoomLight")),
        FName(TEXT("VRTA_AB_SourceLight")),
        FName(TEXT("VRTA_AB_ListenerLight")),
        FName(TEXT("VRTA_AB_Camera"))
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

    float GetValidationDistanceCm(const float DistanceCmOverride)
    {
        if (FMath::IsNearlyEqual(DistanceCmOverride, 100.0f))
        {
            return 100.0f;
        }
        if (FMath::IsNearlyEqual(DistanceCmOverride, 400.0f))
        {
            return 400.0f;
        }
        return 200.0f;
    }

    FVector GetAirAbsorptionPerMeter(
        const EUERayTracingAudioEditorAirAbsorptionProfile AirProfile)
    {
        switch (AirProfile)
        {
        case EUERayTracingAudioEditorAirAbsorptionProfile::Off:
            return FVector::ZeroVector;
        case EUERayTracingAudioEditorAirAbsorptionProfile::Stress:
            return FVector(0.01f, 0.04f, 0.12f);
        default:
            return FVector(0.0002f, 0.0006f, 0.0012f);
        }
    }

    EObjectFlags GetObjectFlags(const EUERayTracingAudioEditorValidationSceneMode Mode)
    {
        return Mode == EUERayTracingAudioEditorValidationSceneMode::Transient
            ? RF_Transient
            : RF_Transactional;
    }

    template <typename TComponent, typename TConfigure>
    TComponent* CreateValidationInstanceComponent(
        AActor& Owner,
        const FName ComponentName,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutMutated,
        TConfigure&& Configure)
    {
        if (Mode
            == EUERayTracingAudioEditorValidationSceneMode::Persistent)
        {
            Owner.Modify();
        }

        TComponent* Component = NewObject<TComponent>(
            &Owner,
            ComponentName,
            GetObjectFlags(Mode));
        if (!Component)
        {
            return nullptr;
        }

        Configure(*Component);
        Owner.AddInstanceComponent(Component);
        Component->OnComponentCreated();
        Component->RegisterComponent();
        bOutMutated = true;
        return Component;
    }

    AActor* FindTaggedActor(UWorld& World, const FName Role)
    {
        AActor* Match = nullptr;
        for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
        {
            if (ActorIt->ActorHasTag(ValidationSceneTag) && ActorIt->ActorHasTag(Role))
            {
                if (Match)
                {
                    return nullptr;
                }
                Match = *ActorIt;
            }
        }
        return Match;
    }

    void NormalizeKnownRoleTags(
        UWorld& World,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutMutated)
    {
        for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
        {
            AActor* Actor = *ActorIt;
            if (!Actor->ActorHasTag(ValidationSceneTag))
            {
                continue;
            }
            TArray<FName> ActorRoles;
            for (const FName Role : KnownValidationRoles)
            {
                if (Actor->ActorHasTag(Role))
                {
                    ActorRoles.Add(Role);
                }
            }
            if (ActorRoles.Num() <= 1)
            {
                continue;
            }

            FName PreferredRole = ActorRoles[0];
            const FName SourceRole(TEXT("VRTA_AB_Source"));
            const FName ListenerRole(TEXT("VRTA_AB_Listener"));
            if (Actor->ActorHasTag(SourceRole)
                && Actor->FindComponentByClass<
                    UUERayTracingAudioSourceComponent>())
            {
                PreferredRole = SourceRole;
            }
            else if (Actor->ActorHasTag(ListenerRole)
                && Actor->FindComponentByClass<
                    UUERayTracingAudioListenerComponent>())
            {
                PreferredRole = ListenerRole;
            }

            if (Mode
                == EUERayTracingAudioEditorValidationSceneMode::Persistent)
            {
                Actor->Modify();
            }
            for (const FName Role : ActorRoles)
            {
                if (Role != PreferredRole)
                {
                    Actor->Tags.Remove(Role);
                }
            }
            bOutMutated = true;
        }
    }

    template <typename TActor>
    TActor* NormalizeTaggedActorForRole(
        UWorld& World,
        const FName Role,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutMutated)
    {
        TActor* Keep = nullptr;
        FString KeepPath;
        TArray<TWeakObjectPtr<AActor>> Matches;
        for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
        {
            AActor* Actor = *ActorIt;
            if (!Actor->ActorHasTag(ValidationSceneTag)
                || !Actor->ActorHasTag(Role))
            {
                continue;
            }
            Matches.Add(Actor);
            if (TActor* TypedActor = Cast<TActor>(Actor))
            {
                const FString ActorPath = TypedActor->GetPathName();
                if (!Keep || ActorPath < KeepPath)
                {
                    Keep = TypedActor;
                    KeepPath = ActorPath;
                }
            }
        }

        for (const TWeakObjectPtr<AActor>& Match : Matches)
        {
            AActor* Actor = Match.Get();
            if (!IsValid(Actor) || Actor == Keep)
            {
                continue;
            }
            if (Mode
                == EUERayTracingAudioEditorValidationSceneMode::Persistent)
            {
                Actor->Modify();
            }
            if (World.DestroyActor(
                    Actor,
                    false,
                    Mode
                        == EUERayTracingAudioEditorValidationSceneMode::Persistent))
            {
                bOutMutated = true;
            }
        }
        return Keep;
    }

    bool RemoveStaleTaggedGeometry(
        UWorld& World,
        const TArrayView<const FGeometryDefinition> DesiredDefinitions,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutMutated)
    {
        TArray<TWeakObjectPtr<AActor>> ActorsToDestroy;
        for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
        {
            AActor* Actor = *ActorIt;
            if (!Actor->ActorHasTag(ValidationSceneTag))
            {
                continue;
            }

            const bool bHasKnownGeometryRole = Algo::AnyOf(
                MakeArrayView(KnownValidationRoles, 8),
                [Actor](const FName Role)
                {
                    return Actor->ActorHasTag(Role);
                });
            if (!bHasKnownGeometryRole
                && !Actor->FindComponentByClass<
                    UUERayTracingAudioGeometryComponent>())
            {
                continue;
            }

            const bool bRoleIsDesired = Algo::AnyOf(
                DesiredDefinitions,
                [Actor](const FGeometryDefinition& Definition)
                {
                    return Actor->ActorHasTag(FName(Definition.Role));
                });
            if (!bRoleIsDesired)
            {
                ActorsToDestroy.Add(Actor);
            }
        }

        for (const TWeakObjectPtr<AActor>& ActorPtr : ActorsToDestroy)
        {
            AActor* Actor = ActorPtr.Get();
            if (!IsValid(Actor))
            {
                continue;
            }
            if (Mode == EUERayTracingAudioEditorValidationSceneMode::Persistent)
            {
                Actor->Modify();
            }
            if (!World.DestroyActor(
                    Actor,
                    false,
                    Mode
                        == EUERayTracingAudioEditorValidationSceneMode::Persistent))
            {
                return false;
            }
            bOutMutated = true;
        }
        return true;
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
        const EComponentMobility::Type Mobility,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutCreated,
        bool& bOutMutated)
    {
        const FName RoleName(Role);
        if (AStaticMeshActor* Existing =
            NormalizeTaggedActorForRole<AStaticMeshActor>(
                World,
                RoleName,
                Mode,
                bOutMutated))
        {
            UStaticMeshComponent* MeshComponent =
                Existing->GetStaticMeshComponent();
            if (!MeshComponent)
            {
                return nullptr;
            }
            const bool bChanged =
                !Existing->GetActorTransform().Equals(Transform)
                || Existing->GetActorLabel() != Label
                || MeshComponent->GetStaticMesh() != &Mesh
                || MeshComponent->GetMobility() != Mobility
                || MeshComponent->GetCollisionEnabled()
                    != ECollisionEnabled::QueryAndPhysics
                || !MeshComponent->IsVisible();
            if (bChanged
                && Mode
                    == EUERayTracingAudioEditorValidationSceneMode::Persistent)
            {
                Existing->Modify();
                MeshComponent->Modify();
            }
            TagActor(*Existing, RoleName, Label);
            Existing->SetActorTransform(Transform);
            MeshComponent->SetStaticMesh(&Mesh);
            MeshComponent->SetMobility(Mobility);
            MeshComponent->SetCollisionEnabled(
                ECollisionEnabled::QueryAndPhysics);
            MeshComponent->SetVisibility(true, true);
            bOutMutated |= bChanged;
            return Existing;
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
        MeshComponent->SetMobility(Mobility);
        MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
        MeshComponent->SetVisibility(true, true);
        bOutCreated = true;
        bOutMutated = true;
        return Actor;
    }

    UUERayTracingAudioGeometryComponent* EnsureGeometryComponent(
        AStaticMeshActor& Actor,
        const FGeometryDefinition& Definition,
        const EUERayTracingAudioEditorValidationSceneMode Mode,
        bool& bOutMutated)
    {
        TInlineComponentArray<UUERayTracingAudioGeometryComponent*>
            GeometryComponents(&Actor);
        UUERayTracingAudioGeometryComponent* Existing = nullptr;
        FString ExistingPath;
        const FName CanonicalComponentName(
            TEXT("ValidationAcousticGeometry"));
        for (UUERayTracingAudioGeometryComponent* Candidate
            : GeometryComponents)
        {
            if (!IsValid(Candidate))
            {
                continue;
            }

            const FString CandidatePath = Candidate->GetPathName();
            const bool bCandidateIsNative =
                Candidate->CreationMethod
                != EComponentCreationMethod::Instance;
            const bool bExistingIsNative = Existing
                && Existing->CreationMethod
                    != EComponentCreationMethod::Instance;
            const bool bCandidateIsCanonical =
                Candidate->GetFName() == CanonicalComponentName;
            const bool bExistingIsCanonical = Existing
                && Existing->GetFName() == CanonicalComponentName;
            if (!Existing
                || (bCandidateIsNative && !bExistingIsNative)
                || (bCandidateIsNative == bExistingIsNative
                    && bCandidateIsCanonical
                    && !bExistingIsCanonical)
                || (bCandidateIsNative == bExistingIsNative
                    && bCandidateIsCanonical == bExistingIsCanonical
                    && CandidatePath < ExistingPath))
            {
                Existing = Candidate;
                ExistingPath = CandidatePath;
            }
        }

        if (Existing)
        {
            for (UUERayTracingAudioGeometryComponent* Candidate
                : GeometryComponents)
            {
                if (!IsValid(Candidate) || Candidate == Existing)
                {
                    continue;
                }
                if (Candidate->CreationMethod
                    != EComponentCreationMethod::Instance)
                {
                    return nullptr;
                }
                if (Mode
                    == EUERayTracingAudioEditorValidationSceneMode::Persistent)
                {
                    Actor.Modify();
                    Candidate->Modify();
                }
                Candidate->DestroyComponent();
                bOutMutated = true;
            }

            const bool bChanged =
                !Existing->bExportToAcousticScene
                || !Existing->bAffectsDirectSound
                || Existing->ExportMode
                    != EUERayTracingAudioGeometryExportMode::BoundingBox
                || !Existing->Absorption.Equals(
                    Definition.Absorption,
                    UE_KINDA_SMALL_NUMBER)
                || !Existing->Transmission.IsNearlyZero()
                || !FMath::IsNearlyEqual(
                    Existing->Scattering,
                    Definition.Scattering);
            if (bChanged
                && Mode
                    == EUERayTracingAudioEditorValidationSceneMode::Persistent)
            {
                Existing->Modify();
            }
            Existing->bExportToAcousticScene = true;
            Existing->bAffectsDirectSound = true;
            Existing->ExportMode =
                EUERayTracingAudioGeometryExportMode::BoundingBox;
            Existing->Absorption = Definition.Absorption;
            Existing->Transmission = FVector::ZeroVector;
            Existing->Scattering = Definition.Scattering;
            bOutMutated |= bChanged;
            return Existing;
        }

        return CreateValidationInstanceComponent<
            UUERayTracingAudioGeometryComponent>(
            Actor,
            FName(TEXT("ValidationAcousticGeometry")),
            Mode,
            bOutMutated,
            [&Definition](
                UUERayTracingAudioGeometryComponent& Geometry)
            {
                Geometry.bExportToAcousticScene = true;
                Geometry.bAffectsDirectSound = true;
                Geometry.ExportMode =
                    EUERayTracingAudioGeometryExportMode::BoundingBox;
                Geometry.Absorption = Definition.Absorption;
                Geometry.Transmission = FVector::ZeroVector;
                Geometry.Scattering = Definition.Scattering;
            });
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
        bool& bOutCreated,
        bool& bOutMutated)
    {
        if (APointLight* Existing =
            NormalizeTaggedActorForRole<APointLight>(
                World,
                FName(Role),
                Mode,
                bOutMutated))
        {
            const bool bChanged =
                !Existing->GetActorLocation().Equals(Location)
                || Existing->GetActorLabel() != Label
                || !Existing->PointLightComponent
                || !FMath::IsNearlyEqual(
                    Existing->PointLightComponent->Intensity,
                    Intensity)
                || !FMath::IsNearlyEqual(
                    Existing->PointLightComponent->AttenuationRadius,
                    Radius)
                || Existing->PointLightComponent->GetLightColor()
                    != Color
                || !Existing->PointLightComponent->CastShadows;
            if (bChanged && Mode
                == EUERayTracingAudioEditorValidationSceneMode::Persistent)
            {
                Existing->Modify();
                if (Existing->PointLightComponent)
                {
                    Existing->PointLightComponent->Modify();
                }
            }
            TagActor(*Existing, FName(Role), Label);
            Existing->SetActorLocation(Location);
            if (Existing->PointLightComponent)
            {
                Existing->PointLightComponent->SetIntensity(Intensity);
                Existing->PointLightComponent->SetAttenuationRadius(Radius);
                Existing->PointLightComponent->SetLightColor(Color);
                Existing->PointLightComponent->SetCastShadows(true);
            }
            bOutMutated |= bChanged;
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

UUERayTracingAudioSourceComponent*
FUERayTracingAudioEditorValidationScene::FindTaggedSource(UWorld* World)
{
    if (!IsValid(World))
    {
        return nullptr;
    }

    const FName SourceRole(TEXT("VRTA_AB_Source"));
    AActor* SourceActor = FindTaggedActor(*World, SourceRole);
    return IsValid(SourceActor)
        ? SourceActor->FindComponentByClass<
            UUERayTracingAudioSourceComponent>()
        : nullptr;
}

FUERayTracingAudioEditorValidationSceneResult FUERayTracingAudioEditorValidationScene::EnsureScene(
    UWorld& World,
    const EUERayTracingAudioEditorValidationSceneMode Mode,
    const EUERayTracingAudioEditorDirectPreset DirectPreset,
    const EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment,
    const float DistanceCmOverride,
    const EUERayTracingAudioEditorAirAbsorptionProfile AirProfile,
    const int32 ReflectionBounces)
{
    FUERayTracingAudioEditorValidationSceneResult Result;
    Result.ReflectionBounces = FMath::Clamp(ReflectionBounces, 1, 64);
    Result.DirectPreset = DirectPreset;
    Result.ReflectionEnvironment = ReflectionEnvironment;
    Result.AirAbsorptionProfile = AirProfile;
    Result.AirAbsorptionPerMeter = GetAirAbsorptionPerMeter(AirProfile);
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
    bool bMutatedActors = false;
    NormalizeKnownRoleTags(World, Mode, bMutatedActors);
    if (!RemoveStaleTaggedGeometry(
            World,
            GeometryDefinitions,
            Mode,
            bMutatedActors))
    {
        Result.Message =
            TEXT("Could not remove stale tagged validation geometry.");
        return Result;
    }

    FBox SceneBounds(ForceInit);
    for (const FGeometryDefinition& Definition : GeometryDefinitions)
    {
        AStaticMeshActor* GeometryActor = EnsureStaticMeshActor(
            World,
            *CubeMesh,
            Definition.Label,
            Definition.Role,
            FTransform(FRotator::ZeroRotator, ValidationOrigin + Definition.Offset, Definition.Scale),
            EComponentMobility::Static,
            Mode,
            Result.bCreatedActors,
            bMutatedActors);
        if (!GeometryActor
            || !EnsureGeometryComponent(
                *GeometryActor,
                Definition,
                Mode,
                bMutatedActors))
        {
            Result.Message = FString::Printf(TEXT("Could not create validation geometry: %s."), Definition.Label);
            return Result;
        }
        SceneBounds += GeometryActor->GetComponentsBoundingBox(true);
        ++Result.GeometryCount;
    }

    // DistanceCmOverride only applies to the Clear preset: it slides the source further
    // along the existing listener-to-source line of sight so the R2 distance-scan and
    // air-absorption validation can compare several distances without disturbing the
    // occluded presets' geometry-relative offsets.
    const float ClearDistanceCm =
        GetValidationDistanceCm(DistanceCmOverride);
    const FVector Direction =
        (ClearSourceOffset - ListenerOffset).GetSafeNormal();
    const FVector ClearOffsetForThisRun =
        ListenerOffset + (Direction * ClearDistanceCm);
    const FVector DesiredSourceLocation =
        ValidationOrigin
        + (DirectPreset == EUERayTracingAudioEditorDirectPreset::Clear
            ? ClearOffsetForThisRun
            : OccludedSourceOffset);
    const FString DesiredSourceLabel = FString::Printf(
        TEXT("VRTA A-B Primary Source (Orange) - %s"),
        GetDirectPresetName(DirectPreset));
    AStaticMeshActor* SourceActor = EnsureStaticMeshActor(
        World,
        *SphereMesh,
        *DesiredSourceLabel,
        TEXT("VRTA_AB_Source"),
        FTransform(
            FRotator::ZeroRotator,
            DesiredSourceLocation,
            FVector(0.38)),
        EComponentMobility::Movable,
        Mode,
        Result.bCreatedActors,
        bMutatedActors);
    if (!SourceActor)
    {
        Result.Message = TEXT("Could not create the Editor A/B source actor.");
        return Result;
    }
    UUERayTracingAudioSourceComponent* Source =
        SourceActor->FindComponentByClass<UUERayTracingAudioSourceComponent>();
    if (!Source)
    {
        Source = CreateValidationInstanceComponent<
            UUERayTracingAudioSourceComponent>(
            *SourceActor,
            FName(TEXT("ValidationSource")),
            Mode,
            bMutatedActors,
            [](UUERayTracingAudioSourceComponent&) {});
    }
    if (!Source)
    {
        Result.Message =
            TEXT("Could not create the validation Source component.");
        return Result;
    }
    const bool bSourceSettingsChanged =
        !FMath::IsNearlyEqual(Source->OccludedGain, 0.35f)
        || !FMath::IsNearlyEqual(Source->SourceRadiusCm, 30.0f)
        || Source->NumOcclusionSamples != 8
        || !Source->bUseVolumetricOcclusion
        || Source->bHardOcclusion
            != (DirectPreset
                == EUERayTracingAudioEditorDirectPreset::HardOccluded)
        || !Source->AirAbsorptionPerMeter.Equals(
            Result.AirAbsorptionPerMeter,
            UE_KINDA_SMALL_NUMBER)
        || Source->NumReflectionRays != 4096
        || Source->MaxReflectionBounces != Result.ReflectionBounces
        || !FMath::IsNearlyEqual(Source->IndirectDurationSeconds, 2.0f)
        || Source->IndirectMode
            != EUERayTracingAudioIndirectMode::HybridReverb
        || Source->IndirectDataSource
            != EUERayTracingAudioIndirectDataSource::Realtime
        || !FMath::IsNearlyEqual(
            Source->IndirectMix,
            ValidationWetMix);
    if (bSourceSettingsChanged
        && Mode == EUERayTracingAudioEditorValidationSceneMode::Persistent)
    {
        Source->Modify();
    }
    Source->OccludedGain = 0.35f;
    Source->SourceRadiusCm = 30.0f;
    Source->NumOcclusionSamples = 8;
    Source->bUseVolumetricOcclusion = true;
    Source->bHardOcclusion = DirectPreset == EUERayTracingAudioEditorDirectPreset::HardOccluded;
    Source->AirAbsorptionPerMeter = Result.AirAbsorptionPerMeter;
    Source->NumReflectionRays = 4096;
    Source->MaxReflectionBounces = Result.ReflectionBounces;
    Source->IndirectDurationSeconds = 2.0f;
    Source->IndirectMode = EUERayTracingAudioIndirectMode::HybridReverb;
    Source->SetIndirectDataSource(
        EUERayTracingAudioIndirectDataSource::Realtime);
    // This tagged listening fixture keeps reflections readily audible while
    // retaining a recognizable dry cue in the Full artifact. This does not
    // change the product default or ordinary Source components.
    Source->IndirectMix = ValidationWetMix;
    bMutatedActors |= bSourceSettingsChanged;
    UAudioComponent* ValidationAudio =
        SourceActor->FindComponentByClass<UAudioComponent>();
    if (!ValidationAudio)
    {
        ValidationAudio = CreateValidationInstanceComponent<UAudioComponent>(
                *SourceActor,
                FName(TEXT("ValidationAudio")),
                Mode,
                bMutatedActors,
                [](UAudioComponent& Audio)
                {
                    Audio.bAutoActivate = false;
                    Audio.bAllowSpatialization = true;
                });
        if (!ValidationAudio)
        {
            Result.Message =
                TEXT("Could not create the validation Audio component.");
            return Result;
        }
    }
    else
    {
        const bool bAudioChanged =
            ValidationAudio->bAutoActivate
            || !ValidationAudio->bAllowSpatialization;
        if (bAudioChanged
            && Mode
                == EUERayTracingAudioEditorValidationSceneMode::Persistent)
        {
            ValidationAudio->Modify();
        }
        ValidationAudio->bAutoActivate = false;
        ValidationAudio->bAllowSpatialization = true;
        bMutatedActors |= bAudioChanged;
    }
    Result.Source = Source;

    AStaticMeshActor* ListenerActor = EnsureStaticMeshActor(
        World,
        *SphereMesh,
        TEXT("VRTA A-B Listener (Blue)"),
        TEXT("VRTA_AB_Listener"),
        FTransform(FRotator::ZeroRotator, ValidationOrigin + ListenerOffset, FVector(0.32)),
        EComponentMobility::Movable,
        Mode,
        Result.bCreatedActors,
        bMutatedActors);
    if (!ListenerActor)
    {
        Result.Message = TEXT("Could not create the Editor A/B listener actor.");
        return Result;
    }
    UUERayTracingAudioListenerComponent* Listener =
        ListenerActor->FindComponentByClass<UUERayTracingAudioListenerComponent>();
    if (!Listener)
    {
        Listener = CreateValidationInstanceComponent<
            UUERayTracingAudioListenerComponent>(
            *ListenerActor,
            FName(TEXT("ValidationListener")),
            Mode,
            bMutatedActors,
            [](UUERayTracingAudioListenerComponent&) {});
    }
    if (!Listener)
    {
        Result.Message =
            TEXT("Could not create the validation Listener component.");
        return Result;
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
        Result.bCreatedActors,
        bMutatedActors);
    EnsurePointLight(
        World,
        TEXT("VRTA A-B Source Light (Orange)"),
        TEXT("VRTA_AB_SourceLight"),
        SourceActor->GetActorLocation() + FVector(0.0, 0.0, 35.0),
        FLinearColor(1.0f, 0.16f, 0.02f),
        2600.0f,
        420.0f,
        Mode,
        Result.bCreatedActors,
        bMutatedActors);
    EnsurePointLight(
        World,
        TEXT("VRTA A-B Listener Light (Blue)"),
        TEXT("VRTA_AB_ListenerLight"),
        ListenerActor->GetActorLocation() + FVector(0.0, 0.0, 35.0),
        FLinearColor(0.04f, 0.28f, 1.0f),
        2200.0f,
        380.0f,
        Mode,
        Result.bCreatedActors,
        bMutatedActors);

    const FVector CameraLocation =
        ValidationOrigin + FVector(-480.0, -520.0, 280.0);
    const FVector CameraTarget =
        ValidationOrigin + FVector(30.0, 0.0, 150.0);
    const FTransform CameraTransform(
        (CameraTarget - CameraLocation).Rotation(),
        CameraLocation);
    ACameraActor* Camera =
        NormalizeTaggedActorForRole<ACameraActor>(
            World,
            FName(TEXT("VRTA_AB_Camera")),
            Mode,
            bMutatedActors);
    if (!Camera)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags = GetObjectFlags(Mode);
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (ACameraActor* NewCamera = World.SpawnActor<ACameraActor>(
            ACameraActor::StaticClass(),
            CameraTransform,
            SpawnParameters))
        {
            TagActor(
                *NewCamera,
                FName(TEXT("VRTA_AB_Camera")),
                TEXT("VRTA A-B Validation Camera"));
            Result.bCreatedActors = true;
        }
    }
    else
    {
        const bool bCameraChanged =
            !Camera->GetActorTransform().Equals(CameraTransform)
            || Camera->GetActorLabel()
                != TEXT("VRTA A-B Validation Camera");
        if (bCameraChanged
            && Mode
                == EUERayTracingAudioEditorValidationSceneMode::Persistent)
        {
            Camera->Modify();
        }
        Camera->SetActorTransform(CameraTransform);
        TagActor(
            *Camera,
            FName(TEXT("VRTA_AB_Camera")),
            TEXT("VRTA A-B Validation Camera"));
        bMutatedActors |= bCameraChanged;
    }

    SceneBounds += SourceActor->GetComponentsBoundingBox(true);
    SceneBounds += ListenerActor->GetComponentsBoundingBox(true);
    FocusEditorView(SceneBounds);

    if (Mode == EUERayTracingAudioEditorValidationSceneMode::Persistent
        && (Result.bCreatedActors || bMutatedActors))
    {
        World.MarkPackageDirty();
    }

    const auto CountActorsForRole = [&World](const FName Role)
    {
        int32 Count = 0;
        for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
        {
            Count += ActorIt->ActorHasTag(ValidationSceneTag)
                    && ActorIt->ActorHasTag(Role)
                ? 1
                : 0;
        }
        return Count;
    };
    bool bGeometryNormalized = true;
    for (const FGeometryDefinition& Definition : GeometryDefinitions)
    {
        AStaticMeshActor* Actor = Cast<AStaticMeshActor>(
            FindTaggedActor(World, FName(Definition.Role)));
        TInlineComponentArray<UUERayTracingAudioGeometryComponent*>
            GeometryComponents(Actor);
        UUERayTracingAudioGeometryComponent* Geometry =
            IsValid(Actor)
            ? Actor->FindComponentByClass<
                UUERayTracingAudioGeometryComponent>()
            : nullptr;
        UStaticMeshComponent* MeshComponent = IsValid(Actor)
            ? Actor->GetStaticMeshComponent()
            : nullptr;
        bGeometryNormalized &=
            CountActorsForRole(FName(Definition.Role)) == 1
            && IsValid(Actor)
            && IsValid(MeshComponent)
            && MeshComponent->GetStaticMesh() == CubeMesh
            && MeshComponent->GetMobility()
                == EComponentMobility::Static
            && MeshComponent->GetCollisionEnabled()
                == ECollisionEnabled::QueryAndPhysics
            && MeshComponent->IsVisible()
            && GeometryComponents.Num() == 1
            && IsValid(Geometry)
            && Geometry->bExportToAcousticScene
            && Geometry->bAffectsDirectSound
            && Geometry->ExportMode
                == EUERayTracingAudioGeometryExportMode::BoundingBox
            && Geometry->Absorption.Equals(
                Definition.Absorption,
                UE_KINDA_SMALL_NUMBER)
            && Geometry->Transmission.IsNearlyZero()
            && FMath::IsNearlyEqual(
                Geometry->Scattering,
                Definition.Scattering);
    }
    const bool bCoreRolesNormalized =
        CountActorsForRole(FName(TEXT("VRTA_AB_Source"))) == 1
        && CountActorsForRole(FName(TEXT("VRTA_AB_Listener"))) == 1
        && CountActorsForRole(FName(TEXT("VRTA_AB_RoomLight"))) == 1
        && CountActorsForRole(FName(TEXT("VRTA_AB_SourceLight"))) == 1
        && CountActorsForRole(FName(TEXT("VRTA_AB_ListenerLight"))) == 1
        && CountActorsForRole(FName(TEXT("VRTA_AB_Camera"))) == 1
        && SourceActor->GetActorTransform().Equals(FTransform(
            FRotator::ZeroRotator,
            DesiredSourceLocation,
            FVector(0.38)))
        && SourceActor->GetStaticMeshComponent()->GetStaticMesh()
            == SphereMesh
        && SourceActor->GetStaticMeshComponent()->GetCollisionEnabled()
            == ECollisionEnabled::QueryAndPhysics
        && SourceActor->GetStaticMeshComponent()->IsVisible()
        && ListenerActor->GetActorTransform().Equals(FTransform(
            FRotator::ZeroRotator,
            ValidationOrigin + ListenerOffset,
            FVector(0.32)))
        && ListenerActor->GetStaticMeshComponent()->GetStaticMesh()
            == SphereMesh
        && ListenerActor->GetStaticMeshComponent()->GetCollisionEnabled()
            == ECollisionEnabled::QueryAndPhysics
        && ListenerActor->GetStaticMeshComponent()->IsVisible();

    Result.bSucceeded = Result.Source.IsValid()
        && Result.Listener.IsValid()
        && Result.GeometryCount == GeometryDefinitions.Num()
        && bGeometryNormalized
        && bCoreRolesNormalized;
    Result.Message = Result.bSucceeded
        ? FString::Printf(
            TEXT("Editor A/B validation scene ready: source=1 listener=1 geometry=%d lighting=1 direct_preset=%s reflection_environment=%s source_listener_distance_cm=%.2f air_absorption_profile=%s air_absorption_per_meter=(%.6f,%.6f,%.6f)."),
            Result.GeometryCount,
            GetDirectPresetName(DirectPreset),
            GetReflectionEnvironmentName(ReflectionEnvironment),
            Result.SourceListenerDistanceCm,
            GetAirAbsorptionProfileName(AirProfile),
            Result.AirAbsorptionPerMeter.X,
            Result.AirAbsorptionPerMeter.Y,
            Result.AirAbsorptionPerMeter.Z)
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

EUERayTracingAudioEditorAirAbsorptionProfile
FUERayTracingAudioEditorValidationScene::ParseAirAbsorptionProfile(
    const FString& Value)
{
    if (Value.Equals(TEXT("off"), ESearchCase::IgnoreCase))
    {
        return EUERayTracingAudioEditorAirAbsorptionProfile::Off;
    }
    if (Value.Equals(TEXT("stress"), ESearchCase::IgnoreCase))
    {
        return EUERayTracingAudioEditorAirAbsorptionProfile::Stress;
    }
    return EUERayTracingAudioEditorAirAbsorptionProfile::Default;
}

const TCHAR*
FUERayTracingAudioEditorValidationScene::GetAirAbsorptionProfileName(
    const EUERayTracingAudioEditorAirAbsorptionProfile AirProfile)
{
    switch (AirProfile)
    {
    case EUERayTracingAudioEditorAirAbsorptionProfile::Off:
        return TEXT("off");
    case EUERayTracingAudioEditorAirAbsorptionProfile::Stress:
        return TEXT("stress");
    default:
        return TEXT("default");
    }
}
