#if WITH_DEV_AUTOMATION_TESTS

#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Audio/UERayTracingAudioOcclusion.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "Audio/UERayTracingAudioThreeBandAirAbsorption.h"
#include "Components/BoxComponent.h"
#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Misc/AutomationTest.h"
#include "API/UERayTracingAudioContext.h"
#include "Settings/UERayTracingAudioOcclusionSettings.h"
#include "Settings/UERayTracingAudioProjectSettings.h"
#include "UObject/UObjectGlobals.h"
#include "Validation/UERayTracingAudioDirectSweep.h"
#if WITH_UERAYTRACINGAUDIO_VALIDATION
#include "Validation/UERayTracingAudioRuntimeValidation.h"
#endif

#if __has_include("Audio/UERayTracingAudioAudioDiagnosticsInternal.h")
#include "Audio/UERayTracingAudioAudioDiagnosticsInternal.h"
#define UE_RAY_TRACING_AUDIO_HAS_DIRECT_TARGET_GENERATION 1
#else
#define UE_RAY_TRACING_AUDIO_HAS_DIRECT_TARGET_GENERATION 0
#endif

#include <limits>
#include <type_traits>

namespace
{
    FUERayTracingAudioDirectSweepMetrics MakeDirectSweepMetrics(
        const TArray<float>& DistancesCm,
        const TArray<float>& Visibilities,
        const TArray<float>& OverallGains,
        const int32 CpuFallbackIndex = INDEX_NONE)
    {
        FUERayTracingAudioDirectSweepMetrics Metrics;
        Metrics.Reset();
        const int32 ObservationCount = FMath::Min3(
            DistancesCm.Num(),
            Visibilities.Num(),
            OverallGains.Num());
        for (int32 Index = 0; Index < ObservationCount; ++Index)
        {
            FUERayTracingAudioDirectSimulationResult Result;
            Result.bHasListener = true;
            Result.bRayTracingAvailable =
                Index != CpuFallbackIndex;
            Result.bUsedHardwareRayTracing =
                Index != CpuFallbackIndex;
            Result.DistanceCm = DistancesCm[Index];
            Result.DirectVisibility = Visibilities[Index];
            Result.OverallGain = OverallGains[Index];
            Metrics.Observe(
                static_cast<uint64>(Index + 1),
                Result);
        }
        return Metrics;
    }

    FUERayTracingAudioDirectSweepMetrics MakePassingDirectSweepMetrics()
    {
        return MakeDirectSweepMetrics(
            {
                200.0f, 200.0f, 200.0f, 200.0f,
                200.0f, 200.0f, 200.0f, 200.0f
            },
            {
                1.0f, 0.95f, 0.60f, 0.05f,
                0.0f, 0.20f, 0.95f, 1.0f
            },
            {
                0.50f, 0.48f, 0.30f, 0.175f,
                0.175f, 0.20f, 0.48f, 0.50f
            });
    }

    FUERayTracingAudioDirectAudioStats MakePassingDirectAudioStats()
    {
        FUERayTracingAudioDirectAudioStats Stats;
        Stats.BufferCount = 100;
        Stats.NonSilentInputBufferCount = 100;
        Stats.DirectPresentInputBufferCount = 100;
        Stats.MaxConsecutiveSilentDirectBufferCount = 0;
        Stats.NonFiniteDirectSampleCount = 0;
        Stats.OverUnitDirectSampleCount = 0;
        Stats.MaxBandGainStep = 0.01f;
        return Stats;
    }

    template <typename DiagnosticsType, typename = void>
    struct THasDirectAudioDiagnostics : std::false_type
    {
    };

    template <typename DiagnosticsType>
    struct THasDirectAudioDiagnostics<
        DiagnosticsType,
        std::void_t<
            decltype(DiagnosticsType::ResetDirect()),
            decltype(DiagnosticsType::RecordDirectBuffer(
                uint64{},
                int32{},
                float{},
                float{},
                float{},
                uint64{},
                uint64{})),
            decltype(DiagnosticsType::ReadDirect())>>
        : std::true_type
    {
    };

    struct FDirectDiagnosticsEpochObservation
    {
        bool bApiPresent = false;
        uint64 ResetBufferCount = 0;
        float ResetMaxBandGainStep = 0.0f;
        uint64 BufferCount = 0;
        uint64 NonSilentInputBufferCount = 0;
        uint64 DirectPresentInputBufferCount = 0;
        uint64 MaxConsecutiveSilentDirectBufferCount = 0;
        uint64 NonFiniteDirectSampleCount = 0;
        uint64 OverUnitDirectSampleCount = 0;
        float MaxBandGainStep = 0.0f;
    };

    template <typename DiagnosticsType>
    FDirectDiagnosticsEpochObservation ObserveDirectDiagnosticsEpoch()
    {
        FDirectDiagnosticsEpochObservation Observation;
        if constexpr (!THasDirectAudioDiagnostics<DiagnosticsType>::value)
        {
            return Observation;
        }
        else
        {
            constexpr uint64 AudioComponentId = 0xD1AEC7ULL;
            DiagnosticsType::SetTargetAudioComponentId(AudioComponentId);
            DiagnosticsType::ResetDirect();
            DiagnosticsType::RecordDirectBuffer(
                AudioComponentId,
                32,
                0.5f,
                0.25f,
                0.02f,
                0,
                0);
            DiagnosticsType::ResetDirect();
            const auto ResetStats = DiagnosticsType::ReadDirect();
            Observation.ResetBufferCount = ResetStats.BufferCount;
            Observation.ResetMaxBandGainStep =
                ResetStats.MaxBandGainStep;

            DiagnosticsType::RecordDirectBuffer(
                AudioComponentId,
                64,
                0.5f,
                0.0f,
                0.004f,
                1,
                2);
            DiagnosticsType::RecordDirectBuffer(
                AudioComponentId,
                64,
                0.5f,
                0.25f,
                0.002f,
                0,
                0);
            const auto Stats = DiagnosticsType::ReadDirect();
            DiagnosticsType::SetTargetAudioComponentId(0);

            Observation.bApiPresent = true;
            Observation.BufferCount = Stats.BufferCount;
            Observation.NonSilentInputBufferCount =
                Stats.NonSilentInputBufferCount;
            Observation.DirectPresentInputBufferCount =
                Stats.DirectPresentInputBufferCount;
            Observation.MaxConsecutiveSilentDirectBufferCount =
                Stats.MaxConsecutiveSilentDirectBufferCount;
            Observation.NonFiniteDirectSampleCount =
                Stats.NonFiniteDirectSampleCount;
            Observation.OverUnitDirectSampleCount =
                Stats.OverUnitDirectSampleCount;
            Observation.MaxBandGainStep = Stats.MaxBandGainStep;
            return Observation;
        }
    }

    struct FDirectTargetSwitchObservation
    {
        bool bApiPresent = false;
        uint64 OldGeneration = 0;
        uint64 CurrentGeneration = 0;
        uint64 StaleBufferCount = 0;
        uint64 CurrentBufferCount = 0;
        uint64 CurrentDirectPresentBufferCount = 0;
    };

    FDirectTargetSwitchObservation ObserveDirectTargetSwitch()
    {
        FDirectTargetSwitchObservation Observation;
#if UE_RAY_TRACING_AUDIO_HAS_DIRECT_TARGET_GENERATION
            constexpr uint64 OldAudioComponentId = 0xA11D1A6ULL;
            constexpr uint64 NewAudioComponentId = 0xB22D1A6ULL;
            FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
                OldAudioComponentId);
            FUERayTracingAudioAudioDiagnostics::ResetDirect();
            const FUERayTracingAudioDirectDiagnosticsTargetToken OldTarget =
                FUERayTracingAudioAudioDiagnosticsInternal::CaptureTarget(
                    OldAudioComponentId);
            Observation.OldGeneration = OldTarget.Generation;

            FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
                NewAudioComponentId);
            FUERayTracingAudioAudioDiagnostics::ResetDirect();
            FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
                OldAudioComponentId);
            FUERayTracingAudioAudioDiagnostics::ResetDirect();
            const FUERayTracingAudioDirectDiagnosticsTargetToken CurrentTarget =
                FUERayTracingAudioAudioDiagnosticsInternal::CaptureTarget(
                    OldAudioComponentId);
            Observation.CurrentGeneration =
                CurrentTarget.Generation;

            FUERayTracingAudioAudioDiagnosticsInternal::RecordDirectBuffer(
                OldTarget,
                32,
                0.5f,
                0.25f,
                0.125f,
                0,
                0);
            Observation.StaleBufferCount =
                FUERayTracingAudioAudioDiagnostics::ReadDirect().BufferCount;

            FUERayTracingAudioAudioDiagnosticsInternal::RecordDirectBuffer(
                CurrentTarget,
                32,
                0.5f,
                0.25f,
                0.125f,
                0,
                0);
            const auto CurrentStats =
                FUERayTracingAudioAudioDiagnostics::ReadDirect();
            FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(0);
            FUERayTracingAudioAudioDiagnostics::ResetDirect();

            Observation.bApiPresent = true;
            Observation.CurrentBufferCount =
                CurrentStats.BufferCount;
            Observation.CurrentDirectPresentBufferCount =
                CurrentStats.DirectPresentInputBufferCount;
#endif
        return Observation;
    }

    struct FDirectResetGenerationObservation
    {
        bool bApiPresent = false;
        uint64 PreResetAudioComponentId = 0;
        uint64 PostResetAudioComponentId = 0;
        uint64 PreResetGeneration = 0;
        uint64 PostResetGeneration = 0;
        uint64 StaleBufferCount = 0;
        uint64 CurrentBufferCount = 0;
        uint64 CurrentDirectPresentBufferCount = 0;
    };

    FDirectResetGenerationObservation ObserveDirectResetGeneration()
    {
        FDirectResetGenerationObservation Observation;
#if UE_RAY_TRACING_AUDIO_HAS_DIRECT_TARGET_GENERATION
        constexpr uint64 AudioComponentId = 0xAE5E7D1A6ULL;
        FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
            AudioComponentId);
        FUERayTracingAudioAudioDiagnostics::ResetDirect();
        const FUERayTracingAudioDirectDiagnosticsTargetToken PreResetTarget =
            FUERayTracingAudioAudioDiagnosticsInternal::CaptureTarget(
                AudioComponentId);
        Observation.PreResetAudioComponentId =
            PreResetTarget.AudioComponentId;
        Observation.PreResetGeneration = PreResetTarget.Generation;

        FUERayTracingAudioAudioDiagnostics::ResetDirect();
        FUERayTracingAudioAudioDiagnosticsInternal::RecordDirectBuffer(
            PreResetTarget,
            32,
            0.5f,
            0.25f,
            0.125f,
            0,
            0);
        Observation.StaleBufferCount =
            FUERayTracingAudioAudioDiagnostics::ReadDirect().BufferCount;

        const FUERayTracingAudioDirectDiagnosticsTargetToken PostResetTarget =
            FUERayTracingAudioAudioDiagnosticsInternal::CaptureTarget(
                AudioComponentId);
        Observation.PostResetAudioComponentId =
            PostResetTarget.AudioComponentId;
        Observation.PostResetGeneration = PostResetTarget.Generation;
        FUERayTracingAudioAudioDiagnosticsInternal::RecordDirectBuffer(
            PostResetTarget,
            32,
            0.5f,
            0.25f,
            0.125f,
            0,
            0);
        const auto CurrentStats =
            FUERayTracingAudioAudioDiagnostics::ReadDirect();
        FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(0);
        FUERayTracingAudioAudioDiagnostics::ResetDirect();

        Observation.bApiPresent = true;
        Observation.CurrentBufferCount =
            CurrentStats.BufferCount;
        Observation.CurrentDirectPresentBufferCount =
            CurrentStats.DirectPresentInputBufferCount;
#endif
        return Observation;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDirectSweepTrajectoryTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.DirectSweepTrajectory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDirectSweepTrajectoryTest::RunTest(
    const FString&)
{
    const FVector ListenerLocation(-100.0, 0.0, 180.0);
    const FVector ClearEndpoint(-100.0, 200.0, 180.0);
    const FVector OccludedEndpoint(100.0, 0.0, 180.0);
    TestTrue(
        TEXT("The outbound arc starts at the existing clear fixture endpoint"),
        FUERayTracingAudioDirectSweepTrajectory::Evaluate(
            ListenerLocation,
            0.0f).Equals(ClearEndpoint, 0.01f));
    TestTrue(
        TEXT("The outbound arc ends at the existing occluded fixture endpoint"),
        FUERayTracingAudioDirectSweepTrajectory::Evaluate(
            ListenerLocation,
            1.0f).Equals(OccludedEndpoint, 0.01f));

    bool bCrossedWallOutbound = false;
    bool bCrossedWallReturning = false;
    float PreviousOutboundX =
        FUERayTracingAudioDirectSweepTrajectory::Evaluate(
            ListenerLocation,
            0.0f).X;
    float PreviousReturningX =
        FUERayTracingAudioDirectSweepTrajectory::Evaluate(
            ListenerLocation,
            1.0f).X;
    for (int32 Index = 0; Index <= 100; ++Index)
    {
        const float Alpha =
            static_cast<float>(Index) / 100.0f;
        const FVector Outbound =
            FUERayTracingAudioDirectSweepTrajectory::Evaluate(
                ListenerLocation,
                Alpha);
        const FVector Returning =
            FUERayTracingAudioDirectSweepTrajectory::Evaluate(
                ListenerLocation,
                1.0f - Alpha);
        TestTrue(
            TEXT("Every outbound point remains on the two-metre arc"),
            FMath::IsNearlyEqual(
                FVector::Distance(
                    Outbound,
                    ListenerLocation),
                200.0f,
                0.01f));
        TestTrue(
            TEXT("Every returning point remains on the same two-metre arc"),
            FMath::IsNearlyEqual(
                FVector::Distance(
                    Returning,
                    ListenerLocation),
                200.0f,
                0.01f));
        bCrossedWallOutbound |=
            PreviousOutboundX <= 0.0f
            && Outbound.X > 0.0f;
        bCrossedWallReturning |=
            PreviousReturningX >= 0.0f
            && Returning.X < 0.0f;
        PreviousOutboundX = Outbound.X;
        PreviousReturningX = Returning.X;
    }
    TestTrue(
        TEXT("The outbound arc crosses the wall plane"),
        bCrossedWallOutbound);
    TestTrue(
        TEXT("The reverse arc crosses the wall plane"),
        bCrossedWallReturning);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDirectSweepMetricsTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.DirectSweepMetrics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDirectSweepMetricsTest::RunTest(
    const FString&)
{
    const FUERayTracingAudioDirectAudioStats PassingAudioStats =
        MakePassingDirectAudioStats();
    TestTrue(
        TEXT("A continuous hardware clear-occluded-clear sweep passes"),
        MakePassingDirectSweepMetrics().Passes(
            PassingAudioStats,
            true,
            true));

    FUERayTracingAudioDirectSweepMetrics TooFewGenerations =
        MakeDirectSweepMetrics(
            {
                200.0f, 200.0f, 200.0f, 200.0f,
                200.0f, 200.0f, 200.0f
            },
            {
                1.0f, 0.95f, 0.60f, 0.05f,
                0.0f, 0.20f, 0.95f
            },
            {
                0.50f, 0.48f, 0.30f, 0.175f,
                0.175f, 0.20f, 0.48f
            });
    TestFalse(
        TEXT("Fewer than eight distinct Direct generations fail"),
        TooFewGenerations.Passes(
            PassingAudioStats,
            true,
            true));

    FUERayTracingAudioDirectSweepMetrics DistanceDrift =
        MakeDirectSweepMetrics(
            {
                200.0f, 200.0f, 203.0f, 200.0f,
                200.0f, 200.0f, 200.0f, 200.0f
            },
            {
                1.0f, 0.95f, 0.60f, 0.05f,
                0.0f, 0.20f, 0.95f, 1.0f
            },
            {
                0.50f, 0.48f, 0.30f, 0.175f,
                0.175f, 0.20f, 0.48f, 0.50f
            });
    TestFalse(
        TEXT("Distance outside the two-centimetre tolerance fails"),
        DistanceDrift.Passes(
            PassingAudioStats,
            true,
            true));

    FUERayTracingAudioDirectSweepMetrics VisibilityDidNotCross =
        MakeDirectSweepMetrics(
            {
                200.0f, 200.0f, 200.0f, 200.0f,
                200.0f, 200.0f, 200.0f, 200.0f
            },
            {
                1.0f, 0.95f, 0.60f, 0.50f,
                0.40f, 0.60f, 0.95f, 1.0f
            },
            {
                0.50f, 0.48f, 0.30f, 0.25f,
                0.20f, 0.30f, 0.48f, 0.50f
            });
    TestFalse(
        TEXT("A sweep that never reaches occluded visibility fails"),
        VisibilityDidNotCross.Passes(
            PassingAudioStats,
            true,
            true));

    FUERayTracingAudioDirectSweepMetrics ZeroSoftGain =
        MakeDirectSweepMetrics(
            {
                200.0f, 200.0f, 200.0f, 200.0f,
                200.0f, 200.0f, 200.0f, 200.0f
            },
            {
                1.0f, 0.95f, 0.60f, 0.05f,
                0.0f, 0.20f, 0.95f, 1.0f
            },
            {
                0.50f, 0.48f, 0.30f, 0.175f,
                0.0f, 0.20f, 0.48f, 0.50f
            });
    TestFalse(
        TEXT("A zero Soft Occlusion Direct target gain fails"),
        ZeroSoftGain.Passes(
            PassingAudioStats,
            true,
            true));

    FUERayTracingAudioDirectAudioStats DropoutAudioStats =
        PassingAudioStats;
    DropoutAudioStats.MaxConsecutiveSilentDirectBufferCount = 1;
    TestFalse(
        TEXT("A Direct dropout during non-silent input fails"),
        MakePassingDirectSweepMetrics().Passes(
            DropoutAudioStats,
            true,
            true));

    FUERayTracingAudioDirectAudioStats DiscontinuousAudioStats =
        PassingAudioStats;
    DiscontinuousAudioStats.MaxBandGainStep = 0.0101f;
    TestFalse(
        TEXT("A per-sample Direct gain step above 0.01 fails"),
        MakePassingDirectSweepMetrics().Passes(
            DiscontinuousAudioStats,
            true,
            true));

    TestFalse(
        TEXT("CPU-only observations cannot pass the hardware gate"),
        MakePassingDirectSweepMetrics().Passes(
            PassingAudioStats,
            false,
            true));
    TestFalse(
        TEXT("Any accepted CPU-fallback generation fails hardware provenance"),
        MakeDirectSweepMetrics(
            {
                200.0f, 200.0f, 200.0f, 200.0f,
                200.0f, 200.0f, 200.0f, 200.0f
            },
            {
                1.0f, 0.95f, 0.60f, 0.05f,
                0.0f, 0.20f, 0.95f, 1.0f
            },
            {
                0.50f, 0.48f, 0.30f, 0.175f,
                0.175f, 0.20f, 0.48f, 0.50f
            },
            4).Passes(
                PassingAudioStats,
                true,
                true));
    TestFalse(
        TEXT("The automatic sweep cannot move Source before the baseline CPU reference is logged"),
        FUERayTracingAudioDirectSweepPolicy::ShouldStartAutomatic(
            true,
            false,
            true,
            false,
            true,
            7,
            true));
    TestTrue(
        TEXT("An owner starts after baseline evidence from a fresh hardware Direct generation"),
        FUERayTracingAudioDirectSweepPolicy::ShouldStartAutomatic(
            true,
            false,
            true,
            true,
            true,
            7,
            true));
    TestFalse(
        TEXT("A non-owner cannot start the process-global automatic fixture"),
        FUERayTracingAudioDirectSweepPolicy::ShouldStartAutomatic(
            true,
            false,
            false,
            true,
            true,
            7,
            true));
    TestFalse(
        TEXT("CPU fallback cannot start the automatic hardware sweep"),
        FUERayTracingAudioDirectSweepPolicy::ShouldStartAutomatic(
            true,
            false,
            true,
            true,
            true,
            7,
            false));
    TestFalse(
        TEXT("Hardware wait remains open at its exact deadline"),
        FUERayTracingAudioDirectSweepPolicy::HasHardwareWaitTimedOut(
            true,
            false,
            true,
            30.0,
            30.0));
    TestTrue(
        TEXT("Hardware wait terminates immediately after its deadline"),
        FUERayTracingAudioDirectSweepPolicy::HasHardwareWaitTimedOut(
            true,
            false,
            true,
            30.001,
            30.0));
    TestFalse(
        TEXT("A non-owner never emits an automatic timeout marker"),
        FUERayTracingAudioDirectSweepPolicy::HasHardwareWaitTimedOut(
            true,
            false,
            false,
            30.001,
            30.0));
    TestFalse(
        TEXT("A terminalized automatic request cannot time out twice"),
        FUERayTracingAudioDirectSweepPolicy::HasHardwareWaitTimedOut(
            true,
            true,
            true,
            60.0,
            30.0));
    TestFalse(
        TEXT("A sweep cannot pass before Source state restoration"),
        MakePassingDirectSweepMetrics().Passes(
            PassingAudioStats,
            true,
            false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioRuntimeSetterReflectionTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.RuntimeSetterReflection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioRuntimeSetterReflectionTest::RunTest(
    const FString&)
{
    UClass* SourceClass =
        UUERayTracingAudioSourceComponent::StaticClass();
    UFunction* DataSourceSetter = SourceClass->FindFunctionByName(
        TEXT("SetIndirectDataSource"));
    UFunction* BakedAssetSetter = SourceClass->FindFunctionByName(
        TEXT("SetBakedImpulseResponseAsset"));
    TestNotNull(
        TEXT("SetIndirectDataSource is exposed as a UFUNCTION"),
        DataSourceSetter);
    TestNotNull(
        TEXT("SetBakedImpulseResponseAsset is exposed as a UFUNCTION"),
        BakedAssetSetter);
    if (DataSourceSetter)
    {
        TestTrue(
            TEXT("SetIndirectDataSource is Blueprint-callable"),
            DataSourceSetter->HasAnyFunctionFlags(
                FUNC_BlueprintCallable));
    }
    if (BakedAssetSetter)
    {
        TestTrue(
            TEXT("SetBakedImpulseResponseAsset is Blueprint-callable"),
            BakedAssetSetter->HasAnyFunctionFlags(
                FUNC_BlueprintCallable));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDirectDiagnosticsEpochTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.DirectDiagnosticsEpoch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDirectDiagnosticsEpochTest::RunTest(
    const FString&)
{
    const FDirectDiagnosticsEpochObservation Observation =
        ObserveDirectDiagnosticsEpoch<
            FUERayTracingAudioAudioDiagnostics>();
    TestTrue(
        TEXT("Direct diagnostics expose reset, record, and read APIs"),
        Observation.bApiPresent);
    if (!Observation.bApiPresent)
    {
        return false;
    }

    TestEqual(
        TEXT("A reset epoch hides previously published Direct buffers"),
        Observation.ResetBufferCount,
        0ULL);
    TestEqual(
        TEXT("A reset epoch hides the previous maximum band-gain step"),
        Observation.ResetMaxBandGainStep,
        0.0f);
    TestEqual(
        TEXT("Only two post-reset Direct buffers are visible"),
        Observation.BufferCount,
        2ULL);
    TestEqual(
        TEXT("Both post-reset Direct buffers have input"),
        Observation.NonSilentInputBufferCount,
        2ULL);
    TestEqual(
        TEXT("Only one input-bearing buffer has Direct output"),
        Observation.DirectPresentInputBufferCount,
        1ULL);
    TestEqual(
        TEXT("One silent Direct buffer is recorded as one run"),
        Observation.MaxConsecutiveSilentDirectBufferCount,
        1ULL);
    TestEqual(
        TEXT("Non-finite Direct samples stay in the Direct epoch"),
        Observation.NonFiniteDirectSampleCount,
        1ULL);
    TestEqual(
        TEXT("Over-unit Direct samples stay in the Direct epoch"),
        Observation.OverUnitDirectSampleCount,
        2ULL);
    TestTrue(
        TEXT("The maximum per-sample band-gain step is retained"),
        FMath::IsNearlyEqual(
            Observation.MaxBandGainStep,
            0.004f,
            1.0e-6f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDirectTargetGenerationTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.DirectTargetGeneration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDirectTargetGenerationTest::RunTest(
    const FString&)
{
    const FDirectTargetSwitchObservation Observation =
        ObserveDirectTargetSwitch();
    TestTrue(
        TEXT("Direct diagnostics expose target-generation capture and publication"),
        Observation.bApiPresent);
    if (!Observation.bApiPresent)
    {
        return false;
    }

    TestTrue(
        TEXT("The old target captures a nonzero generation"),
        Observation.OldGeneration != 0);
    TestTrue(
        TEXT("The current ABA target captures a nonzero generation"),
        Observation.CurrentGeneration != 0);
    TestNotEqual(
        TEXT("Switching away and back advances the diagnostic generation"),
        Observation.OldGeneration,
        Observation.CurrentGeneration);
    TestEqual(
        TEXT("A stale old-target writer cannot initialize the new epoch"),
        Observation.StaleBufferCount,
        0ULL);
    TestEqual(
        TEXT("The current target initializes exactly one Direct buffer"),
        Observation.CurrentBufferCount,
        1ULL);
    TestEqual(
        TEXT("The current target's Direct-present buffer is retained"),
        Observation.CurrentDirectPresentBufferCount,
        1ULL);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDirectResetGenerationTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.DirectResetGeneration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDirectResetGenerationTest::RunTest(
    const FString&)
{
    const FDirectResetGenerationObservation Observation =
        ObserveDirectResetGeneration();
    TestTrue(
        TEXT("Direct diagnostics expose reset-generation capture and publication"),
        Observation.bApiPresent);
    if (!Observation.bApiPresent)
    {
        return false;
    }

    TestTrue(
        TEXT("The pre-reset target captures a nonzero generation"),
        Observation.PreResetGeneration != 0);
    TestTrue(
        TEXT("The post-reset target captures a nonzero generation"),
        Observation.PostResetGeneration != 0);
    TestEqual(
        TEXT("Reset keeps the selected diagnostic component ID"),
        Observation.PostResetAudioComponentId,
        Observation.PreResetAudioComponentId);
    TestNotEqual(
        TEXT("Reset advances the diagnostic target generation"),
        Observation.PostResetGeneration,
        Observation.PreResetGeneration);
    TestEqual(
        TEXT("A pre-reset writer cannot initialize the reset epoch"),
        Observation.StaleBufferCount,
        0ULL);
    TestEqual(
        TEXT("A post-reset writer initializes exactly one Direct buffer"),
        Observation.CurrentBufferCount,
        1ULL);
    TestEqual(
        TEXT("The post-reset Direct-present buffer is retained"),
        Observation.CurrentDirectPresentBufferCount,
        1ULL);
    return true;
}

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

        Manager.AddSource(SourceA);
        Manager.AddListener(ListenerA);
        Manager.RequestSourceSimulation(SourceA, true, true);
        Manager.RemoveListener(ListenerA);
        FUERayTracingAudioSourceSimulationResult RemovedListenerResult;
        TestTrue(
            TEXT("Listener removal publishes a replacement simulation snapshot"),
            Manager.GetLatestSourceSimulation(
                SourceA,
                RemovedListenerResult));
        TestTrue(
            TEXT("Listener removal publishes an explicit no-listener Direct result"),
            RemovedListenerResult.bHasDirectResult
                && !RemovedListenerResult.DirectResult.bHasListener);
        TestTrue(
            TEXT("No-listener Direct falls back to unity rather than stale attenuation"),
            FMath::IsNearlyEqual(
                RemovedListenerResult.DirectResult.OverallGain,
                1.0f));
        TestTrue(
            TEXT("Listener removal publishes zero realtime Wet"),
            RemovedListenerResult.bHasIndirectResult
                && !RemovedListenerResult.IndirectResult.bHasListener
                && !RemovedListenerResult.IndirectResult.bHasValidPaths
                && FMath::IsNearlyZero(
                    RemovedListenerResult.IndirectResult.IndirectGain));

        AActor* WeakListenerActor = WorldA->SpawnActor<AActor>();
        AActor* WeakSourceActor = WorldA->SpawnActor<AActor>();
        UUERayTracingAudioListenerComponent* WeakListener =
            NewObject<UUERayTracingAudioListenerComponent>(
                WeakListenerActor);
        UUERayTracingAudioSourceComponent* WeakSource =
            NewObject<UUERayTracingAudioSourceComponent>(WeakSourceActor);
        Manager.AddSource(WeakSource);
        Manager.AddListener(WeakListener);
        WeakListenerActor->Destroy();
        Manager.RequestSourceSimulation(WeakSource, true, true);
        FUERayTracingAudioSourceSimulationResult InvalidWeakListenerResult;
        TestTrue(
            TEXT("An invalid weak Listener publishes a replacement snapshot"),
            Manager.GetLatestSourceSimulation(
                WeakSource,
                InvalidWeakListenerResult));
        TestTrue(
            TEXT("An invalid weak Listener cannot leave stale Direct or Wet"),
            InvalidWeakListenerResult.bHasDirectResult
                && !InvalidWeakListenerResult.DirectResult.bHasListener
                && InvalidWeakListenerResult.bHasIndirectResult
                && !InvalidWeakListenerResult.IndirectResult.bHasListener);
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

#if WITH_UERAYTRACINGAUDIO_VALIDATION
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioRuntimeValidationWorldLifecycleTest,
    "UERayTracingAudio.Validation.WorldLifecycleOwnership",
    EAutomationTestFlags::EditorContext
        | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioRuntimeValidationWorldLifecycleTest::RunTest(
    const FString& Parameters)
{
    static_cast<void>(Parameters);
    UWorld* WorldA = UWorld::CreateWorld(
        EWorldType::Game,
        false,
        TEXT("UERayTracingAudioValidationLifecycleA"));
    UWorld* WorldB = UWorld::CreateWorld(
        EWorldType::Game,
        false,
        TEXT("UERayTracingAudioValidationLifecycleB"));
    UWorld* WorldC = UWorld::CreateWorld(
        EWorldType::Game,
        false,
        TEXT("UERayTracingAudioValidationLifecycleC"));
    TestNotNull(TEXT("Lifecycle World A exists"), WorldA);
    TestNotNull(TEXT("Lifecycle World B exists"), WorldB);
    TestNotNull(TEXT("Lifecycle World C exists"), WorldC);
    if (!WorldA || !WorldB || !WorldC)
    {
        if (WorldA)
        {
            WorldA->DestroyWorld(false);
        }
        if (WorldB)
        {
            WorldB->DestroyWorld(false);
        }
        if (WorldC)
        {
            WorldC->DestroyWorld(false);
        }
        return false;
    }

    FUERayTracingAudioRuntimeValidation RuntimeValidation;
    RuntimeValidation.InitializedWorlds.Add(WorldA);
    RuntimeValidation.InitializedWorlds.Add(WorldB);
    FUERayTracingAudioRuntimeValidation::FScenarioState& StateA =
        RuntimeValidation.Scenarios.AddDefaulted_GetRef();
    StateA.World = WorldA;
    StateA.bValidationOwner =
        RuntimeValidation.ClaimValidationOwnership();
    const bool bFirstScenarioOwnedValidation =
        StateA.bValidationOwner;
    FUERayTracingAudioRuntimeValidation::FScenarioState& StateB =
        RuntimeValidation.Scenarios.AddDefaulted_GetRef();
    StateB.World = WorldB;
    StateB.bValidationOwner =
        RuntimeValidation.ClaimValidationOwnership();
    TestTrue(
        TEXT("The first scenario owns validation"),
        bFirstScenarioOwnedValidation);
    TestFalse(TEXT("A concurrent scenario is not promoted"), StateB.bValidationOwner);

    RuntimeValidation.HandleWorldBeginTearDown(WorldB);
    TestTrue(
        TEXT("Tearing down a non-owner preserves ownership"),
        RuntimeValidation.bValidationOwnerAssigned);
    TestEqual(
        TEXT("The non-owner scenario is removed"),
        RuntimeValidation.Scenarios.Num(),
        1);
    TestFalse(
        TEXT("The non-owner world may initialize again"),
        RuntimeValidation.InitializedWorlds.Contains(WorldB));

    RuntimeValidation.HandleWorldBeginTearDown(WorldA);
    TestFalse(
        TEXT("Owner teardown releases validation ownership"),
        RuntimeValidation.bValidationOwnerAssigned);
    TestTrue(
        TEXT("Owner teardown removes its scenario"),
        RuntimeValidation.Scenarios.IsEmpty());

    RuntimeValidation.InitializedWorlds.Add(WorldC);
    FUERayTracingAudioRuntimeValidation::FScenarioState& StateC =
        RuntimeValidation.Scenarios.AddDefaulted_GetRef();
    StateC.World = WorldC;
    StateC.bValidationOwner =
        RuntimeValidation.ClaimValidationOwnership();
    TestTrue(
        TEXT("A later scenario can claim released ownership"),
        StateC.bValidationOwner);
    RuntimeValidation.HandleWorldBeginTearDown(WorldC);

    WorldA->DestroyWorld(false);
    WorldB->DestroyWorld(false);
    WorldC->DestroyWorld(false);
    return true;
}
#endif

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioUnityReconstructionTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.UnityReconstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioUnityReconstructionTest::RunTest(
    const FString&)
{
    FUERayTracingAudioThreeBandAirAbsorption Processor;
    Processor.Initialize(48000, 2, 500.0f, 4000.0f);
    FRandomStream Random(0x57A11);
    float MaximumAbsoluteError = 0.0f;
    for (int32 FrameIndex = 0; FrameIndex < 2048; ++FrameIndex)
    {
        for (int32 ChannelIndex = 0; ChannelIndex < 2; ++ChannelIndex)
        {
            const float Input =
                Random.GetFraction() * 2.0f - 1.0f;
            const float Output = Processor.ProcessSample(
                Input,
                ChannelIndex,
                FVector::OneVector);
            MaximumAbsoluteError = FMath::Max(
                MaximumAbsoluteError,
                FMath::Abs(Output - Input));
        }
    }
    TestTrue(
        *FString::Printf(
            TEXT("Unity band gains reconstruct every sample (maximum absolute error %.9g)"),
            MaximumAbsoluteError),
        MaximumAbsoluteError < 1.0e-6f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioNonFiniteInputRecoveryTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.NonFiniteInputRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioNonFiniteInputRecoveryTest::RunTest(
    const FString&)
{
    FUERayTracingAudioThreeBandAirAbsorption Processor;
    Processor.Initialize(48000, 1, 500.0f, 4000.0f);
    const FVector UnequalBandGains(1.0f, 0.5f, 0.1f);

    Processor.ProcessSample(
        std::numeric_limits<float>::quiet_NaN(),
        0,
        UnequalBandGains);
    const float RecoveredOutput = Processor.ProcessSample(
        1.0f,
        0,
        UnequalBandGains);

    TestTrue(
        TEXT("A non-finite input sample does not poison persistent filter state"),
        FMath::IsFinite(RecoveredOutput));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioStereoIsolationTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.StereoIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioStereoIsolationTest::RunTest(
    const FString&)
{
    FUERayTracingAudioThreeBandAirAbsorption Processor;
    Processor.Initialize(48000, 2, 500.0f, 4000.0f);
    const FVector UnequalBandGains(1.0f, 0.5f, 0.1f);
    Processor.ProcessSample(1.0f, 0, UnequalBandGains);

    float RightChannelPeak = 0.0f;
    for (int32 SampleIndex = 0; SampleIndex < 128; ++SampleIndex)
    {
        Processor.ProcessSample(
            0.0f,
            0,
            UnequalBandGains);
        RightChannelPeak = FMath::Max(
            RightChannelPeak,
            FMath::Abs(Processor.ProcessSample(
                0.0f,
                1,
                UnequalBandGains)));
    }
    TestTrue(
        TEXT("A left impulse does not alter right-channel filter state"),
        RightChannelPeak < 1.0e-7f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioFrequencyDependentAirAbsorptionTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.FrequencyDependentAirAbsorption",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioFrequencyDependentAirAbsorptionTest::RunTest(
    const FString&)
{
    constexpr int32 SampleRate = 48000;
    constexpr int32 NumFrames = 4800;

    const auto RenderFrequency = [](const float FrequencyHz, const uint64 AudioComponentId)
    {
        const TSharedRef<
            FUERayTracingAudioSimulationSnapshotRegistry,
            ESPMode::ThreadSafe> SnapshotRegistry =
                MakeShared<
                    FUERayTracingAudioSimulationSnapshotRegistry,
                    ESPMode::ThreadSafe>();
        const TSharedRef<
            FUERayTracingAudioIndirectAudioBridge,
            ESPMode::ThreadSafe> Bridge =
                MakeShared<
                    FUERayTracingAudioIndirectAudioBridge,
                    ESPMode::ThreadSafe>();
        FUERayTracingAudioOcclusionPlugin Plugin(
            SnapshotRegistry,
            Bridge,
            FVector2f(500.0f, 4000.0f));

        FAudioPluginInitializationParams InitializationParams;
        InitializationParams.NumSources = 1;
        InitializationParams.NumOutputChannels = 1;
        InitializationParams.SampleRate = SampleRate;
        InitializationParams.BufferLength = NumFrames;
        Plugin.Initialize(InitializationParams);
        Plugin.OnInitSource(0, NAME_None, 1, nullptr);

        FUERayTracingAudioSimulationSnapshot Snapshot;
        Snapshot.DirectResult.bHasListener = true;
        Snapshot.DirectResult.DistanceAttenuation = 1.0f;
        Snapshot.DirectResult.Occlusion = 1.0f;
        Snapshot.DirectResult.AirAbsorption = FVector(1.0f, 1.0f, 0.1f);
        Snapshot.IndirectMix = 0.0f;
        SnapshotRegistry->Publish(AudioComponentId, MoveTemp(Snapshot));

        FAudioPluginSourceOutputData OutputData;
        OutputData.AudioBuffer.SetNumUninitialized(NumFrames);
        double InputSquareSum = 0.0;
        for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
        {
            const float Input = FMath::Sin(
                2.0f * PI * FrequencyHz
                * static_cast<float>(FrameIndex)
                / static_cast<float>(SampleRate));
            OutputData.AudioBuffer[FrameIndex] = Input;
            InputSquareSum +=
                static_cast<double>(Input)
                * static_cast<double>(Input);
        }

        FAudioPluginSourceInputData InputData;
        InputData.SourceId = 0;
        InputData.AudioComponentId = AudioComponentId;
        InputData.AudioBuffer = &OutputData.AudioBuffer;
        InputData.NumChannels = 1;
        InputData.ListenerOrientation = FQuat::Identity;
        InputData.SpatializationParams = nullptr;
        Plugin.ProcessAudio(InputData, OutputData);

        double OutputSquareSum = 0.0;
        for (const float Output : OutputData.AudioBuffer)
        {
            OutputSquareSum +=
                static_cast<double>(Output)
                * static_cast<double>(Output);
        }
        Plugin.Shutdown();
        return FVector2d(
            FMath::Sqrt(InputSquareSum / static_cast<double>(NumFrames)),
            FMath::Sqrt(OutputSquareSum / static_cast<double>(NumFrames)));
    };

    const FVector2d LowRms = RenderFrequency(100.0f, 0x100ULL);
    const FVector2d HighRms = RenderFrequency(10000.0f, 0x10000ULL);
    AddInfo(
        *FString::Printf(
            TEXT(
                "Frequency-dependent output RMS: low=%.9f high=%.9f ratio=%.6f"),
            LowRms.Y,
            HighRms.Y,
            HighRms.Y > 0.0 ? LowRms.Y / HighRms.Y : 0.0));
    TestTrue(
        TEXT("The low and high test buffers have equal input RMS"),
        FMath::IsNearlyEqual(LowRms.X, HighRms.X, 1.0e-6));
    TestTrue(
        *FString::Printf(
            TEXT(
                "Low-frequency direct output is at least twice the high-frequency output "
                "(low RMS %.9f, high RMS %.9f, ratio %.6f)"),
            LowRms.Y,
            HighRms.Y,
            HighRms.Y > 0.0 ? LowRms.Y / HighRms.Y : 0.0),
        LowRms.Y >= HighRms.Y * 2.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDisabledAirAbsorptionTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.DisabledAirAbsorption",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDisabledAirAbsorptionTest::RunTest(
    const FString&)
{
    constexpr uint64 AudioComponentId = 0xD15AB1EDULL;
    const TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry =
            MakeShared<
                FUERayTracingAudioSimulationSnapshotRegistry,
                ESPMode::ThreadSafe>();
    const TSharedRef<
        FUERayTracingAudioIndirectAudioBridge,
        ESPMode::ThreadSafe> Bridge =
            MakeShared<
                FUERayTracingAudioIndirectAudioBridge,
                ESPMode::ThreadSafe>();
    FUERayTracingAudioOcclusionPlugin Plugin(
        SnapshotRegistry,
        Bridge,
        FVector2f(500.0f, 4000.0f));

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 1;
    InitializationParams.SampleRate = 48000;
    InitializationParams.BufferLength = 4;
    Plugin.Initialize(InitializationParams);
    UUERayTracingAudioOcclusionSettings* Settings =
        NewObject<UUERayTracingAudioOcclusionSettings>();
    Settings->bApplyAirAbsorption = false;
    Plugin.OnInitSource(0, NAME_None, 1, Settings);

    FUERayTracingAudioSimulationSnapshot Snapshot;
    Snapshot.DirectResult.bHasListener = true;
    Snapshot.DirectResult.DistanceAttenuation = 0.5f;
    Snapshot.DirectResult.Occlusion = 0.5f;
    Snapshot.DirectResult.AirAbsorption =
        FVector(0.05f, 0.1f, 0.2f);
    Snapshot.IndirectMix = 0.0f;
    SnapshotRegistry->Publish(
        AudioComponentId,
        MoveTemp(Snapshot));

    FAudioPluginSourceOutputData OutputData;
    OutputData.AudioBuffer =
        { 1.0f, -1.0f, 0.5f, -0.5f };
    const Audio::FAlignedFloatBuffer InputCopy =
        OutputData.AudioBuffer;
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = AudioComponentId;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    InputData.NumChannels = 1;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;
    Plugin.ProcessAudio(InputData, OutputData);

    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(
                TEXT("Disabled air absorption preserves broadband sample %d"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                InputCopy[SampleIndex] * 0.25f,
                1.0e-6f));
    }
    Plugin.Shutdown();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioPreparedCapacityTest,
    "UERayTracingAudio.Audio.ConfigurableDirect.PreparedCapacity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioPreparedCapacityTest::RunTest(
    const FString&)
{
    FUERayTracingAudioThreeBandAirAbsorption PreparedProcessor;
    PreparedProcessor.Initialize(48000, 1, 500.0f, 4000.0f);
    TestFalse(
        TEXT("A mono processor rejects an unsupported stereo channel count"),
        PreparedProcessor.CanProcess(2));
    PreparedProcessor.ProcessSample(
        1.0f,
        1,
        FVector(1.0f, 0.5f, 0.1f));
    TestFalse(
        TEXT("An unsupported sample cannot resize prepared channel state"),
        PreparedProcessor.CanProcess(2));

    constexpr uint64 AudioComponentId = 0xCA9AC17EULL;
    const TSharedRef<
        FUERayTracingAudioSimulationSnapshotRegistry,
        ESPMode::ThreadSafe> SnapshotRegistry =
            MakeShared<
                FUERayTracingAudioSimulationSnapshotRegistry,
                ESPMode::ThreadSafe>();
    const TSharedRef<
        FUERayTracingAudioIndirectAudioBridge,
        ESPMode::ThreadSafe> Bridge =
            MakeShared<
                FUERayTracingAudioIndirectAudioBridge,
                ESPMode::ThreadSafe>();
    FUERayTracingAudioOcclusionPlugin Plugin(
        SnapshotRegistry,
        Bridge,
        FVector2f(500.0f, 4000.0f));

    FAudioPluginInitializationParams InitializationParams;
    InitializationParams.NumSources = 1;
    InitializationParams.NumOutputChannels = 1;
    InitializationParams.SampleRate = 48000;
    InitializationParams.BufferLength = 2;
    Plugin.Initialize(InitializationParams);
    Plugin.OnInitSource(0, NAME_None, 1, nullptr);

    FUERayTracingAudioSimulationSnapshot Snapshot;
    Snapshot.DirectResult.bHasListener = true;
    Snapshot.DirectResult.DistanceAttenuation = 0.5f;
    Snapshot.DirectResult.Occlusion = 0.5f;
    Snapshot.DirectResult.AirAbsorption =
        FVector(0.05f, 0.1f, 0.2f);
    Snapshot.IndirectMix = 0.0f;

    FAudioPluginSourceOutputData OutputData;
    OutputData.AudioBuffer =
        { 1.0f, -1.0f, 1.0f, -1.0f };
    FAudioPluginSourceInputData InputData;
    InputData.SourceId = 0;
    InputData.AudioComponentId = AudioComponentId;
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    InputData.NumChannels = 2;
    InputData.ListenerOrientation = FQuat::Identity;
    InputData.SpatializationParams = nullptr;

    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
        AudioComponentId);
    Plugin.ProcessAudio(InputData, OutputData);
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(
                TEXT("Fallback begins at unity before a valid snapshot, sample %d"),
                SampleIndex),
            FMath::IsNearlyEqual(
                FMath::Abs(OutputData.AudioBuffer[SampleIndex]),
                1.0f,
                1.0e-6f));
    }

    SnapshotRegistry->Publish(
        AudioComponentId,
        MoveTemp(Snapshot));
    OutputData.AudioBuffer.Reset();
    InputData.AudioBuffer = &OutputData.AudioBuffer;
    Plugin.ProcessAudio(InputData, OutputData);

    OutputData.AudioBuffer =
        { 1.0f, -1.0f, 0.5f, -0.5f };
    const Audio::FAlignedFloatBuffer InputCopy =
        OutputData.AudioBuffer;
    FUERayTracingAudioAudioDiagnostics::ResetDirect();
    FUERayTracingAudioAudioDiagnostics::ResetHardRealtime();
    Plugin.ProcessAudio(InputData, OutputData);
    const FUERayTracingAudioHardRealtimeStats HardRealtimeStats =
        FUERayTracingAudioAudioDiagnostics::ReadHardRealtime();
    const FUERayTracingAudioDirectAudioStats DirectStats =
        FUERayTracingAudioAudioDiagnostics::ReadDirect();
    TestEqual(
        TEXT("An unsupported callback channel count records one capacity miss"),
        HardRealtimeStats.CallbackCapacityMissCount,
        1ULL);
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        const int32 FrameIndex = SampleIndex / 2;
        const float ExpectedGain =
            FrameIndex == 0 ? 0.625f : 0.25f;
        TestTrue(
            *FString::Printf(
                TEXT("Capacity fallback sample %d uses the scalar broadband ramp"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                InputCopy[SampleIndex] * ExpectedGain,
                1.0e-6f));
    }
    TestTrue(
        TEXT("Fallback diagnostics report the actual per-frame scalar gain step"),
        FMath::IsNearlyEqual(
            DirectStats.MaxBandGainStep,
            0.375f,
            1.0e-6f));

    Plugin.OnReleaseSource(0);
    Plugin.OnInitSource(0, NAME_None, 1, nullptr);
    constexpr uint64 FirstSnapshotAudioComponentId =
        0xF1A57CA9ULL;
    FUERayTracingAudioSimulationSnapshot FirstSnapshot;
    FirstSnapshot.DirectResult.bHasListener = true;
    FirstSnapshot.DirectResult.DistanceAttenuation = 0.5f;
    FirstSnapshot.DirectResult.Occlusion = 0.5f;
    FirstSnapshot.DirectResult.AirAbsorption =
        FVector(0.05f, 0.1f, 0.2f);
    FirstSnapshot.IndirectMix = 0.0f;
    SnapshotRegistry->Publish(
        FirstSnapshotAudioComponentId,
        MoveTemp(FirstSnapshot));
    InputData.AudioComponentId =
        FirstSnapshotAudioComponentId;
    OutputData.AudioBuffer.Reset();
    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(
        FirstSnapshotAudioComponentId);
    FUERayTracingAudioAudioDiagnostics::ResetDirect();
    Plugin.ProcessAudio(InputData, OutputData);

    OutputData.AudioBuffer =
        { 1.0f, -1.0f, 0.5f, -0.5f };
    const Audio::FAlignedFloatBuffer FirstSnapshotInputCopy =
        OutputData.AudioBuffer;
    FUERayTracingAudioAudioDiagnostics::ResetDirect();
    Plugin.ProcessAudio(InputData, OutputData);
    const FUERayTracingAudioDirectAudioStats FirstSnapshotStats =
        FUERayTracingAudioAudioDiagnostics::ReadDirect();
    for (int32 SampleIndex = 0;
        SampleIndex < OutputData.AudioBuffer.Num();
        ++SampleIndex)
    {
        TestTrue(
            *FString::Printf(
                TEXT("A zero-frame first snapshot seeds fallback sample %d at its target"),
                SampleIndex),
            FMath::IsNearlyEqual(
                OutputData.AudioBuffer[SampleIndex],
                FirstSnapshotInputCopy[SampleIndex] * 0.25f,
                1.0e-6f));
    }
    TestEqual(
        TEXT("A seeded first fallback snapshot has no synthetic gain step"),
        FirstSnapshotStats.MaxBandGainStep,
        0.0f);
    Plugin.Shutdown();
    FUERayTracingAudioAudioDiagnostics::SetTargetAudioComponentId(0);
    FUERayTracingAudioAudioDiagnostics::ResetDirect();
    FUERayTracingAudioAudioDiagnostics::ResetHardRealtime();
    return true;
}

#endif

#undef UE_RAY_TRACING_AUDIO_HAS_DIRECT_TARGET_GENERATION
