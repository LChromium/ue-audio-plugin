#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UUERayTracingAudioListenerComponent;
class UUERayTracingAudioSourceComponent;
class UWorld;

enum class EUERayTracingAudioEditorValidationSceneMode : uint8
{
    Persistent,
    Transient
};

enum class EUERayTracingAudioEditorDirectPreset : uint8
{
    Clear,
    SoftOccluded,
    HardOccluded
};

// Controls which room geometry is spawned around the fixed Source/Listener pair.
// Enclosed keeps the existing 7-box room used by the Direct preset regression suite.
// OpenSpace and NearWall are used by the R3 reflection-environment validation, which
// needs a much sparser room to prove early-reflection/late-reverb behavior scales with
// how much geometry is nearby, independent of the Direct occlusion tests.
enum class EUERayTracingAudioEditorReflectionEnvironment : uint8
{
    Enclosed,
    OpenSpace,
    NearWall
};

enum class EUERayTracingAudioEditorAirAbsorptionProfile : uint8
{
    Off,
    Default,
    Stress
};

struct FUERayTracingAudioEditorValidationSceneResult
{
    bool bSucceeded = false;
    bool bCreatedActors = false;
    FString Message;
    TWeakObjectPtr<UUERayTracingAudioSourceComponent> Source;
    TWeakObjectPtr<UUERayTracingAudioListenerComponent> Listener;
    int32 GeometryCount = 0;
    EUERayTracingAudioEditorDirectPreset DirectPreset = EUERayTracingAudioEditorDirectPreset::Clear;
    EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment = EUERayTracingAudioEditorReflectionEnvironment::Enclosed;
    EUERayTracingAudioEditorAirAbsorptionProfile AirAbsorptionProfile =
        EUERayTracingAudioEditorAirAbsorptionProfile::Default;
    FVector AirAbsorptionPerMeter = FVector::ZeroVector;
    float SourceListenerDistanceCm = 0.0f;
};

class FUERayTracingAudioEditorValidationScene
{
public:
    static UUERayTracingAudioSourceComponent* FindTaggedSource(
        UWorld* World);

    // DistanceCmOverride only affects the Clear preset (source/listener stay on the
    // same line of sight used by the fixed room, just moved further apart) and is used
    // by the R2 distance-scan / air-absorption validation. A value <= 0 keeps the
    // existing fixed 200 cm offset used by the Direct preset regression suite.
    static FUERayTracingAudioEditorValidationSceneResult EnsureScene(
        UWorld& World,
        EUERayTracingAudioEditorValidationSceneMode Mode,
        EUERayTracingAudioEditorDirectPreset DirectPreset = EUERayTracingAudioEditorDirectPreset::Clear,
        EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment = EUERayTracingAudioEditorReflectionEnvironment::Enclosed,
        float DistanceCmOverride = -1.0f,
        EUERayTracingAudioEditorAirAbsorptionProfile AirProfile =
            EUERayTracingAudioEditorAirAbsorptionProfile::Default);

    static EUERayTracingAudioEditorDirectPreset ParseDirectPreset(const FString& Value);
    static const TCHAR* GetDirectPresetName(EUERayTracingAudioEditorDirectPreset DirectPreset);
    static EUERayTracingAudioEditorReflectionEnvironment ParseReflectionEnvironment(const FString& Value);
    static const TCHAR* GetReflectionEnvironmentName(EUERayTracingAudioEditorReflectionEnvironment ReflectionEnvironment);
    static EUERayTracingAudioEditorAirAbsorptionProfile ParseAirAbsorptionProfile(
        const FString& Value);
    static const TCHAR* GetAirAbsorptionProfileName(
        EUERayTracingAudioEditorAirAbsorptionProfile AirProfile);
};
