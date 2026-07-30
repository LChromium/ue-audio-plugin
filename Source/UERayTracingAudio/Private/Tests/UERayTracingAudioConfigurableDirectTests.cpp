#if WITH_DEV_AUTOMATION_TESTS

#include "Components/BoxComponent.h"
#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Misc/AutomationTest.h"
#include "API/UERayTracingAudioContext.h"
#include "Settings/UERayTracingAudioProjectSettings.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioProjectSettingsTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.ProjectSettings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioProjectSettingsTest::RunTest(const FString&)
{
    UUERayTracingAudioProjectSettings* Settings =
        NewObject<UUERayTracingAudioProjectSettings>();
    TestEqual(TEXT("default reference"), Settings->ReferenceDistanceCm, 100.0f);
    TestEqual(TEXT("default maximum"), Settings->MaxDistanceCm, 5000.0f);
    TestEqual(TEXT("default speed"), Settings->SpeedOfSoundCmPerSecond, 34300.0f);
    TestEqual(
        TEXT("default low-mid crossover"),
        Settings->AirAbsorptionLowMidCrossoverHz,
        500.0f);
    TestEqual(
        TEXT("default mid-high crossover"),
        Settings->AirAbsorptionMidHighCrossoverHz,
        4000.0f);

    Settings->ReferenceDistanceCm = -10.0f;
    Settings->MaxDistanceCm = 0.25f;
    Settings->SpeedOfSoundCmPerSecond = 0.0f;
    const FUERayTracingAudioContextSettings Valid =
        Settings->GetValidatedContextSettings();
    TestEqual(TEXT("reference clamped"), Valid.ReferenceDistanceCm, 1.0f);
    TestEqual(TEXT("maximum follows reference"), Valid.MaxDistanceCm, 1.0f);
    TestEqual(TEXT("positive speed"), Valid.SpeedOfSoundCmPerSecond, 1.0f);

    Settings->AirAbsorptionLowMidCrossoverHz = -100.0f;
    Settings->AirAbsorptionMidHighCrossoverHz = 100000.0f;
    const FVector2f ValidCrossovers =
        Settings->GetValidatedAirAbsorptionCrossoversHz(48000.0f);
    TestEqual(TEXT("low-mid crossover clamped"), ValidCrossovers.X, 20.0f);
    TestEqual(TEXT("mid-high crossover clamped to Nyquist"), ValidCrossovers.Y, 24000.0f);

    Settings->AirAbsorptionLowMidCrossoverHz = 1000.0f;
    Settings->AirAbsorptionMidHighCrossoverHz = 100.0f;
    const FVector2f OrderedCrossovers =
        Settings->GetValidatedAirAbsorptionCrossoversHz(48000.0f);
    TestEqual(TEXT("valid low-mid crossover retained"), OrderedCrossovers.X, 1000.0f);
    TestEqual(TEXT("mid-high crossover follows low-mid"), OrderedCrossovers.Y, 1000.0f);

    FUERayTracingAudioContext Context(Valid);
    TestEqual(
        TEXT("context receives reference"),
        Context.GetReferenceDistanceCm(),
        1.0f);
    TestEqual(
        TEXT("context receives maximum"),
        Context.GetMaxDistanceCm(),
        1.0f);
    TestEqual(
        TEXT("context receives speed"),
        Context.GetSpeedOfSoundCmPerSecond(),
        1.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioWorldScopedListenerTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.WorldScopedListener",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioWorldScopedListenerTest::RunTest(const FString&)
{
    UWorld* WorldA = UWorld::CreateWorld(
        EWorldType::Game,
        false,
        TEXT("UERayTracingAudioListenerWorldA"));
    UWorld* WorldB = UWorld::CreateWorld(
        EWorldType::Game,
        false,
        TEXT("UERayTracingAudioListenerWorldB"));
    TestNotNull(TEXT("World A"), WorldA);
    TestNotNull(TEXT("World B"), WorldB);

    if (WorldA && WorldB)
    {
        AActor* SourceActorA = WorldA->SpawnActor<AActor>();
        AActor* ListenerActorA = WorldA->SpawnActor<AActor>();
        AActor* ListenerActorB = WorldB->SpawnActor<AActor>();
        UUERayTracingAudioSourceComponent* SourceA =
            NewObject<UUERayTracingAudioSourceComponent>(SourceActorA);
        UUERayTracingAudioListenerComponent* ListenerA =
            NewObject<UUERayTracingAudioListenerComponent>(ListenerActorA);
        UUERayTracingAudioListenerComponent* ListenerB =
            NewObject<UUERayTracingAudioListenerComponent>(ListenerActorB);

        FUERayTracingAudioManager Manager;
        Manager.AddListener(ListenerB);
        FUERayTracingAudioDirectSimulationResult WrongWorld =
            Manager.SimulateDirectSource(SourceA);
        TestFalse(
            TEXT("a source cannot consume another world's listener"),
            WrongWorld.bHasListener);

        Manager.AddListener(ListenerA);
        TestTrue(
            TEXT("World A listener"),
            Manager.GetCurrentListener(WorldA) == ListenerA);
        TestTrue(
            TEXT("World B listener"),
            Manager.GetCurrentListener(WorldB) == ListenerB);
        Manager.RemoveListener(ListenerA);
        TestNull(
            TEXT("World A listener is removed"),
            Manager.GetCurrentListener(WorldA));
        TestTrue(
            TEXT("removing Listener A does not affect World B"),
            Manager.GetCurrentListener(WorldB) == ListenerB);
    }

    if (WorldA)
    {
        WorldA->DestroyWorld(false);
    }
    if (WorldB)
    {
        WorldB->DestroyWorld(false);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioWorldScopedGeometryTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.WorldScopedGeometry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioWorldScopedGeometryTest::RunTest(const FString&)
{
    UWorld* WorldA = UWorld::CreateWorld(
        EWorldType::Game,
        false,
        TEXT("UERayTracingAudioGeometryWorldA"));
    UWorld* WorldB = UWorld::CreateWorld(
        EWorldType::Game,
        false,
        TEXT("UERayTracingAudioGeometryWorldB"));
    TestNotNull(TEXT("World A"), WorldA);
    TestNotNull(TEXT("World B"), WorldB);

    if (WorldA && WorldB)
    {
        AActor* GeometryActorB = WorldB->SpawnActor<AActor>();
        UBoxComponent* BoxB = NewObject<UBoxComponent>(GeometryActorB);
        GeometryActorB->SetRootComponent(BoxB);
        BoxB->SetBoxExtent(FVector(100.0, 200.0, 300.0));
        UUERayTracingAudioGeometryComponent* GeometryB =
            NewObject<UUERayTracingAudioGeometryComponent>(GeometryActorB);

        FUERayTracingAudioManager Manager;
        Manager.AddGeometry(GeometryB);
        const FString WorldASignature =
            Manager.GetCurrentSceneSignature(WorldA);
        const FString WorldBSignature =
            Manager.GetCurrentSceneSignature(WorldB);
        TestEqual(
            TEXT("World A keeps the empty-scene signature"),
            WorldASignature,
            FString(TEXT("00000000")));
        TestNotEqual(
            TEXT("World B includes its registered geometry"),
            WorldBSignature,
            FString(TEXT("00000000")));
    }

    if (WorldA)
    {
        WorldA->DestroyWorld(false);
    }
    if (WorldB)
    {
        WorldB->DestroyWorld(false);
    }
    return true;
}

#endif
