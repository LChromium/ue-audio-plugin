#if WITH_DEV_AUTOMATION_TESTS

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

#endif
