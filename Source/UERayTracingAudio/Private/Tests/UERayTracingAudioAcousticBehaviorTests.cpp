#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "API/UERayTracingAudioContext.h"
#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Audio/UERayTracingAudioIndirectRenderer.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "RayTracing/UERayTracingAudioRayTracingDevice.h"
#include "Scene/UERayTracingAudioScene.h"
#include "Simulation/UERayTracingAudioSimulator.h"
#include "Tests/UERayTracingAudioPreparedRendererTestHarness.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioSceneSnapshotValueTest,
    "UERayTracingAudio.Scene.ImmutableValueSnapshot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioSceneSnapshotValueTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioGeometryExport First;
    First.bUseStaticMeshTriangles = true;
    First.StaticMeshCacheKey = TEXT("/Engine/TestMesh");
    First.StaticMeshLODIndex = 0;
    First.StaticMeshContentHash = 0x12345678u;
    First.Vertices = { FVector::ZeroVector, FVector::ForwardVector, FVector::RightVector };
    First.Indices = { 0u, 1u, 2u };

    FUERayTracingAudioGeometryExport Changed = First;
    Changed.StaticMeshContentHash = 0x87654321u;
    TestTrue(
        TEXT("Mesh content changes invalidate the plugin-managed BLAS key"),
        First.GetRayTracingGeometryCacheKey() != Changed.GetRayTracingGeometryCacheKey());

    FUERayTracingAudioScene Scene;
    const uint64 InitialCacheKey = Scene.GetCacheKey();
    TArray<FUERayTracingAudioGeometryExport> Snapshot;
    Snapshot.Add(MoveTemp(First));
    Scene.SetStaticGeometry(MoveTemp(Snapshot));
    TestTrue(TEXT("Publishing geometry advances the versioned scene cache key"), Scene.GetCacheKey() != InitialCacheKey);
    TestEqual(TEXT("The scene owns its immutable geometry values"), Scene.GetStaticGeometry().Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDirectRayBatchInterfaceTest,
    "UERayTracingAudio.RHI.DirectRayBatchInterface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDirectRayBatchInterfaceTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioRayTracingDevice Device;
    FUERayTracingAudioTraceRequest Request;
    TArray<TArray<FUERayTracingAudioRay>> RayBatches;
    RayBatches.AddDefaulted();
    RayBatches.AddDefaulted_GetRef().Add(
        FUERayTracingAudioRay{ FVector::ZeroVector, FVector::ForwardVector * 100.0f });

    TArray<TSharedPtr<FUERayTracingAudioAsyncRayQuery, ESPMode::ThreadSafe>> Queries =
        Device.SubmitRaysBatch(Request, RayBatches);
    TestEqual(TEXT("Batch handles remain index-aligned with source ray groups"), Queries.Num(), RayBatches.Num());

    bool bSucceeded = false;
    TArray<bool> Hits;
    TestTrue(TEXT("An empty ray group completes immediately"), Queries[0]->ConsumeResult(bSucceeded, Hits));
    TestTrue(TEXT("An empty ray group succeeds"), bSucceeded);
    TestTrue(TEXT("An empty ray group returns no hits"), Hits.IsEmpty());

    bSucceeded = true;
    Hits.Reset();
    TestTrue(TEXT("A non-empty group without a scene completes as unavailable"), Queries[1]->ConsumeResult(bSucceeded, Hits));
    TestFalse(TEXT("A non-empty group without a scene does not claim hardware success"), bSucceeded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioIndirectBatchInterfaceTest,
    "UERayTracingAudio.RHI.IndirectBatchInterface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioIndirectBatchInterfaceTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioRayTracingDevice Device;
    TArray<FUERayTracingAudioEnergyFieldTraceRequest> Requests;
    Requests.SetNum(2);
    Requests[0].NumReflectionRays = 8;
    Requests[1].NumReflectionRays = 16;

    TArray<TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>> Queries =
        Device.SubmitIndirectEnergyFieldBatch(Requests);
    TestEqual(TEXT("Indirect batch handles remain index-aligned with source requests"), Queries.Num(), Requests.Num());

    for (const TSharedPtr<FUERayTracingAudioAsyncEnergyFieldQuery, ESPMode::ThreadSafe>& Query : Queries)
    {
        bool bSucceeded = true;
        FUERayTracingAudioEnergyFieldTraceResult Result;
        TestTrue(TEXT("A request without an immutable scene completes immediately"), Query->ConsumeResult(bSucceeded, Result));
        TestFalse(TEXT("A request without a scene does not claim hardware success"), bSucceeded);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioIndirectRendererTest,
    "UERayTracingAudio.Audio.IndirectRenderer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioIndirectRendererTest::RunTest(const FString& Parameters)
{
    constexpr int32 TestSampleRate = 8000;
    FUERayTracingAudioIndirectRenderer Renderer;
    Renderer.Initialize(TestSampleRate, 4.0f);
    Renderer.Reset(TestSampleRate);
    FUERayTracingAudioPreparedRendererTestHarness PreparedRenderer(
        TestSampleRate);

    FUERayTracingAudioSimulationSnapshot Snapshot;
    Snapshot.IndirectMix = 1.0f;
    Snapshot.IndirectDurationSeconds = 0.25f;
    Snapshot.IndirectResult.bHasValidPaths = true;
    Snapshot.IndirectResult.bUsedParametricTail = true;
    Snapshot.IndirectResult.LateReverbGain = 0.5f;
    Snapshot.IndirectResult.ParametricDelaySeconds = 0.0f;
    Snapshot.IndirectResult.ReverbTimes = FVector(0.2f, 0.3f, 0.4f);
    Snapshot.IndirectResult.ParametricEq = FVector::OneVector;
    PreparedRenderer.Configure(Renderer, &Snapshot);

    bool bProducedFiniteTail = false;
    for (int32 SampleIndex = 0; SampleIndex < 2000; ++SampleIndex)
    {
        const FVector2f Wet = Renderer.ProcessSample(SampleIndex == 0 ? 1.0f : 0.0f);
        TestTrue(TEXT("Indirect renderer never emits NaN/Inf"), FMath::IsFinite(Wet.X) && FMath::IsFinite(Wet.Y));
        bProducedFiniteTail |= FMath::Abs(Wet.X) > UE_SMALL_NUMBER || FMath::Abs(Wet.Y) > UE_SMALL_NUMBER;
    }
    TestTrue(TEXT("Parametric module produces a late response to an impulse"), bProducedFiniteTail);
    TestTrue(TEXT("The configured late module reports output state"), Renderer.HasOutput());

    Snapshot.IndirectResult.bUsedParametricTail = false;
    PreparedRenderer.Configure(Renderer, &Snapshot);
    TestFalse(TEXT("Switching to convolution-only clears the previous comb state"), Renderer.HasOutput());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioIndirectAudioBridgeTest,
    "UERayTracingAudio.Audio.IndirectAudioBridge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioIndirectAudioBridgeTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioAudioDiagnostics::ResetHardRealtime();
    FUERayTracingAudioIndirectAudioBridge Bridge;
    Bridge.Initialize(2, 4);
    // Occlusion and Spatialization both initialize the shared device bridge. The
    // second call must be idempotent or it can discard a callback already in flight.
    TArrayView<FVector2f> InFlightWriteView = Bridge.BeginWrite(1, 42, 2);
    TestTrue(TEXT("Initialization preallocates the callback-sized bridge storage"), Bridge.GetSourceCapacityForTesting(1) >= 4);
    Bridge.Initialize(2, 4);
    TestEqual(TEXT("Repeated matching initialization preserves an in-flight write"), InFlightWriteView.Num(), 2);
    if (InFlightWriteView.Num() == 2)
    {
        InFlightWriteView[0] = FVector2f(0.25f, 0.5f);
        InFlightWriteView[1] = FVector2f(0.75f, 1.0f);
    }
    Bridge.EndWrite(1, 42);

    TArrayView<const FVector2f> ReadView;
    TestFalse(TEXT("A reused voice cannot consume another component's wet buffer"), Bridge.Consume(1, 99, ReadView));
    TestTrue(TEXT("The matching component consumes the wet buffer"), Bridge.Consume(1, 42, ReadView));
    TestEqual(TEXT("The consumed frame count is preserved"), ReadView.Num(), 2);
    if (ReadView.Num() == 2)
    {
        TestTrue(TEXT("The first stereo frame is preserved"), ReadView[0].Equals(FVector2f(0.25f, 0.5f)));
        TestTrue(TEXT("The second stereo frame is preserved"), ReadView[1].Equals(FVector2f(0.75f, 1.0f)));
    }
    TestFalse(TEXT("A wet buffer can only be consumed once"), Bridge.Consume(1, 42, ReadView));

    Bridge.ClearSource(1);
    TestFalse(TEXT("Release clears pending source state"), Bridge.Consume(1, 42, ReadView));
    TestTrue(TEXT("Release retains the callback buffer capacity"), Bridge.GetSourceCapacityForTesting(1) >= 4);

    TArrayView<FVector2f> ShortWriteView = Bridge.BeginWrite(1, 43, 1);
    TestEqual(TEXT("A shorter callback can reuse the preallocated buffer"), ShortWriteView.Num(), 1);
    if (ShortWriteView.Num() == 1)
    {
        ShortWriteView[0] = FVector2f(0.125f, 0.875f);
    }
    Bridge.EndWrite(1, 43);
    TestTrue(TEXT("The shorter callback remains consumable"), Bridge.Consume(1, 43, ReadView));
    TestEqual(TEXT("The shorter callback preserves its frame count"), ReadView.Num(), 1);
    if (ReadView.Num() == 1)
    {
        TestTrue(TEXT("The shorter callback preserves asymmetric stereo"), ReadView[0].Equals(FVector2f(0.125f, 0.875f)));
    }

    const uint64 OverflowCountBefore = Bridge.GetCapacityOverflowCount();
    TestTrue(TEXT("An oversized callback is rejected without growing storage"), Bridge.BeginWrite(1, 44, 5).IsEmpty());
    Bridge.EndWrite(1, 44);
    TestFalse(TEXT("A rejected callback cannot publish stale wet audio"), Bridge.Consume(1, 44, ReadView));
    TestEqual(TEXT("An oversized callback records one capacity overflow"), Bridge.GetCapacityOverflowCount(), OverflowCountBefore + 1);
    TestEqual(
        TEXT("An oversized callback records one observable realtime capacity miss"),
        FUERayTracingAudioAudioDiagnostics::ReadHardRealtime().
            CallbackCapacityMissCount,
        1ULL);
    TestTrue(TEXT("An oversized callback does not grow storage"), Bridge.GetSourceCapacityForTesting(1) >= 4);

    TArrayView<FVector2f> RecoveryWriteView = Bridge.BeginWrite(1, 45, 4);
    TestEqual(TEXT("A legal callback recovers after an oversized request"), RecoveryWriteView.Num(), 4);
    Bridge.EndWrite(1, 45);
    TestTrue(TEXT("The recovered callback is consumable"), Bridge.Consume(1, 45, ReadView));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioPreparedConvolutionBridgeTest,
    "UERayTracingAudio.Audio.PreparedConvolutionBridge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioPreparedConvolutionBridgeTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 AudioComponentId = 101;
    constexpr uint64 KernelRevision = 7;
    constexpr int32 BlockSize = 8;
    constexpr int32 CrossfadeSamples = 8;
    FUERayTracingAudioIndirectAudioBridge Bridge;
    Bridge.Initialize(2, 16, 48000);

    FUERayTracingAudioConvolutionKernel::FKernelPtr Kernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.75f },
            48000,
            BlockSize);
    TestTrue(TEXT("Prepared bridge kernel is valid"), Kernel.IsValid());
    if (!Kernel)
    {
        return false;
    }
    TWeakPtr<
        const FUERayTracingAudioConvolutionKernel,
        ESPMode::ThreadSafe> KernelLifetime = Kernel;
    Bridge.PublishConvolutionTargets(
        AudioComponentId,
        KernelRevision,
        Kernel,
        nullptr,
        nullptr,
        nullptr);
    Kernel.Reset();

    FUERayTracingAudioPreparedCrossfadingConvolver FirstVoice;
    FUERayTracingAudioPreparedCrossfadingConvolver SecondVoice;
    TestEqual(
        TEXT("First voice posts a bounded prepare request"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            KernelRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            FirstVoice,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);
    TestEqual(
        TEXT("Second voice posts an independent prepare request"),
        Bridge.ConfigureConvolver(
            1,
            AudioComponentId,
            KernelRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            SecondVoice,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);

    Bridge.ServiceConvolutionGameThread();
    TestEqual(
        TEXT("First voice adopts its prepared state"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            KernelRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            FirstVoice,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Adopted);
    TestEqual(
        TEXT("Second voice adopts a distinct prepared state"),
        Bridge.ConfigureConvolver(
            1,
            AudioComponentId,
            KernelRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            SecondVoice,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Adopted);

    bool bFirstVoiceBecameAudible = false;
    bool bSecondVoiceStayedSilent = true;
    for (int32 SampleIndex = 0;
        SampleIndex < (BlockSize + CrossfadeSamples) * 3;
        ++SampleIndex)
    {
        bFirstVoiceBecameAudible |=
            FMath::Abs(FirstVoice.ProcessSample(1.0f)) > 0.05f;
        bSecondVoiceStayedSilent &=
            FMath::IsNearlyZero(
                SecondVoice.ProcessSample(0.0f),
                1.0e-6f);
    }
    TestTrue(
        TEXT("The first voice develops independent convolution history"),
        bFirstVoiceBecameAudible);
    TestTrue(
        TEXT("The second voice does not share the first voice's history"),
        bSecondVoiceStayedSilent);

    const int32 PreparedStateCountBefore =
        Bridge.GetPreparedStateCountForTesting();
    TestEqual(
        TEXT("An unchanged revision does not request another workspace"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            KernelRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            FirstVoice,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Unchanged);
    Bridge.ServiceConvolutionGameThread();
    TestEqual(
        TEXT("An unchanged revision allocates no additional state"),
        Bridge.GetPreparedStateCountForTesting(),
        PreparedStateCountBefore);

    Bridge.RemoveConvolutionTargets(AudioComponentId);
    Bridge.ReleaseConvolver(
        0,
        EUERayTracingAudioConvolutionLane::BakedLeft,
        FirstVoice);
    Bridge.ReleaseConvolver(
        1,
        EUERayTracingAudioConvolutionLane::BakedLeft,
        SecondVoice);
    TestTrue(
        TEXT("Audio release only returns leases; it does not release kernels"),
        KernelLifetime.IsValid());
    Bridge.ServiceConvolutionGameThread();
    TestFalse(
        TEXT("Game-thread service performs the final kernel reclamation"),
        KernelLifetime.IsValid());
    TestEqual(
        TEXT("All prepared workspace bytes are reclaimed on the control path"),
        Bridge.GetPreparedWorkspaceBytes(),
        static_cast<uint64>(0));
    TestEqual(
        TEXT("The bounded audio return path did not overflow"),
        Bridge.GetConvolutionReturnOverflowCount(),
        static_cast<uint64>(0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioPreparedConvolutionTransitionHeadroomTest,
    "UERayTracingAudio.Audio.PreparedConvolutionTransitionHeadroom",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioPreparedConvolutionTransitionHeadroomTest::RunTest(
    const FString& Parameters)
{
    constexpr int32 NumSources = 128;
    constexpr int32 NumLanes = 4;
    constexpr uint64 AudioComponentId = 0xA170'0201ULL;
    constexpr int32 BlockSize = 8;
    FUERayTracingAudioIndirectAudioBridge Bridge;
    Bridge.Initialize(NumSources, 16, 48000);

    const FUERayTracingAudioConvolutionKernel::FKernelPtr FirstKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.75f },
            48000,
            BlockSize);
    const FUERayTracingAudioConvolutionKernel::FKernelPtr SecondKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.25f },
            48000,
            BlockSize);
    TestTrue(TEXT("Initial pool kernel is valid"), FirstKernel.IsValid());
    TestTrue(TEXT("Replacement pool kernel is valid"), SecondKernel.IsValid());
    if (!FirstKernel || !SecondKernel)
    {
        return false;
    }

    TArray<TUniquePtr<
        FUERayTracingAudioPreparedCrossfadingConvolver>> Convolvers;
    Convolvers.Reserve(NumSources * NumLanes);
    for (int32 Index = 0;
        Index < NumSources * NumLanes;
        ++Index)
    {
        Convolvers.Add(MakeUnique<
            FUERayTracingAudioPreparedCrossfadingConvolver>());
    }
    const EUERayTracingAudioConvolutionLane Lanes[NumLanes] =
    {
        EUERayTracingAudioConvolutionLane::BakedLeft,
        EUERayTracingAudioConvolutionLane::BakedRight,
        EUERayTracingAudioConvolutionLane::RealtimeLeft,
        EUERayTracingAudioConvolutionLane::RealtimeRight
    };

    Bridge.PublishConvolutionTargets(
        AudioComponentId,
        1,
        FirstKernel,
        FirstKernel,
        FirstKernel,
        FirstKernel);
    for (int32 SourceIndex = 0;
        SourceIndex < NumSources;
        ++SourceIndex)
    {
        for (int32 LaneIndex = 0;
            LaneIndex < NumLanes;
            ++LaneIndex)
        {
            Bridge.ConfigureConvolver(
                SourceIndex,
                AudioComponentId,
                1,
                true,
                Lanes[LaneIndex],
                *Convolvers[
                    (SourceIndex * NumLanes) + LaneIndex],
                8);
        }
    }
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    for (int32 SourceIndex = 0;
        SourceIndex < NumSources;
        ++SourceIndex)
    {
        for (int32 LaneIndex = 0;
            LaneIndex < NumLanes;
            ++LaneIndex)
        {
            TestEqual(
                TEXT("Every initial lane adopts a prepared state"),
                Bridge.ConfigureConvolver(
                    SourceIndex,
                    AudioComponentId,
                    1,
                    true,
                    Lanes[LaneIndex],
                    *Convolvers[
                        (SourceIndex * NumLanes) + LaneIndex],
                    8),
                EUERayTracingAudioConvolverConfigureResult::Adopted);
            for (int32 SampleIndex = 0;
                SampleIndex < BlockSize + 8;
                ++SampleIndex)
            {
                Convolvers[
                    (SourceIndex * NumLanes) + LaneIndex]->
                        ProcessSample(0.0f);
            }
        }
    }
    TestEqual(
        TEXT("All 512 active lanes own independent states"),
        Bridge.GetPreparedStateCountForTesting(),
        NumSources * NumLanes);

    Bridge.PublishConvolutionTargets(
        AudioComponentId,
        2,
        SecondKernel,
        SecondKernel,
        SecondKernel,
        SecondKernel);
    for (int32 SourceIndex = 0;
        SourceIndex < NumSources;
        ++SourceIndex)
    {
        for (int32 LaneIndex = 0;
            LaneIndex < NumLanes;
            ++LaneIndex)
        {
            Bridge.ConfigureConvolver(
                SourceIndex,
                AudioComponentId,
                2,
                true,
                Lanes[LaneIndex],
                *Convolvers[
                    (SourceIndex * NumLanes) + LaneIndex],
                8);
        }
    }
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("The pool reserves a second state for every in-flight transition"),
        Bridge.GetPreparedStateCountForTesting(),
        NumSources * NumLanes * 2);

    Bridge.RemoveConvolutionTargets(AudioComponentId);
    for (int32 SourceIndex = 0;
        SourceIndex < NumSources;
        ++SourceIndex)
    {
        for (int32 LaneIndex = 0;
            LaneIndex < NumLanes;
            ++LaneIndex)
        {
            Bridge.ReleaseConvolver(
                SourceIndex,
                Lanes[LaneIndex],
                *Convolvers[
                    (SourceIndex * NumLanes) + LaneIndex]);
        }
    }
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("Pool headroom is fully reclaimable"),
        Bridge.GetPreparedWorkspaceBytes(),
        static_cast<uint64>(0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioPreparedConvolutionFairServiceTest,
    "UERayTracingAudio.Audio.PreparedConvolutionFairService",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioPreparedConvolutionFairServiceTest::RunTest(
    const FString& Parameters)
{
    constexpr int32 HighSourceId = 7;
    constexpr uint64 LowComponentId = 0xA170'0202ULL;
    constexpr uint64 HighComponentId = 0xA170'0203ULL;
    FUERayTracingAudioIndirectAudioBridge Bridge;
    Bridge.Initialize(HighSourceId + 1, 16, 48000);

    const FUERayTracingAudioConvolutionKernel::FKernelPtr Kernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.5f },
            48000,
            8);
    TestTrue(TEXT("Fair-service kernel is valid"), Kernel.IsValid());
    if (!Kernel)
    {
        return false;
    }

    FUERayTracingAudioPreparedCrossfadingConvolver LowConvolver;
    FUERayTracingAudioPreparedCrossfadingConvolver HighConvolver;
    Bridge.PublishConvolutionTargets(
        HighComponentId,
        1,
        Kernel,
        nullptr,
        nullptr,
        nullptr);
    Bridge.ConfigureConvolver(
        HighSourceId,
        HighComponentId,
        1,
        true,
        EUERayTracingAudioConvolutionLane::BakedLeft,
        HighConvolver,
        8);

    for (uint64 LowRevision = 1;
        LowRevision <= 4;
        ++LowRevision)
    {
        Bridge.PublishConvolutionTargets(
            LowComponentId,
            LowRevision,
            Kernel,
            nullptr,
            nullptr,
            nullptr);
        Bridge.ConfigureConvolver(
            0,
            LowComponentId,
            LowRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            LowConvolver,
            8);
        Bridge.ServiceConvolutionGameThread(1);
    }

    TestEqual(
        TEXT("A bounded round-robin service reaches a high source under low-source churn"),
        Bridge.ConfigureConvolver(
            HighSourceId,
            HighComponentId,
            1,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            HighConvolver,
            8),
        EUERayTracingAudioConvolverConfigureResult::Adopted);

    Bridge.RemoveConvolutionTargets(LowComponentId);
    Bridge.RemoveConvolutionTargets(HighComponentId);
    Bridge.ReleaseConvolver(
        0,
        EUERayTracingAudioConvolutionLane::BakedLeft,
        LowConvolver);
    Bridge.ReleaseConvolver(
        HighSourceId,
        EUERayTracingAudioConvolutionLane::BakedLeft,
        HighConvolver);
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioPreparedConvolutionLaneRevisionTest,
    "UERayTracingAudio.Audio.PreparedConvolutionLaneRevisions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioPreparedConvolutionLaneRevisionTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 AudioComponentId = 102;
    constexpr int32 BlockSize = 8;
    constexpr int32 CrossfadeSamples = 8;
    FUERayTracingAudioIndirectAudioBridge Bridge;
    Bridge.Initialize(1, 16, 48000);

    const FUERayTracingAudioConvolutionKernel::FKernelPtr BakedKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.75f },
            48000,
            BlockSize);
    const FUERayTracingAudioConvolutionKernel::FKernelPtr
        FirstRealtimeKernel =
            FUERayTracingAudioConvolutionKernel::Build(
                TArray<float>{ 0.25f },
                48000,
                BlockSize);
    TestTrue(TEXT("Baked lane kernel is valid"), BakedKernel.IsValid());
    TestTrue(
        TEXT("Initial realtime lane kernel is valid"),
        FirstRealtimeKernel.IsValid());
    if (!BakedKernel || !FirstRealtimeKernel)
    {
        return false;
    }

    FUERayTracingAudioConvolutionRevisions InitialRevisions;
    InitialRevisions.BakedLeft = 11;
    InitialRevisions.BakedRight = 11;
    InitialRevisions.RealtimeLeft = 21;
    InitialRevisions.RealtimeRight = 21;
    Bridge.PublishConvolutionTargets(
        AudioComponentId,
        InitialRevisions,
        BakedKernel,
        nullptr,
        FirstRealtimeKernel,
        nullptr);

    FUERayTracingAudioPreparedCrossfadingConvolver BakedConvolver;
    FUERayTracingAudioPreparedCrossfadingConvolver RealtimeConvolver;
    TestEqual(
        TEXT("Baked lane posts its initial request"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            InitialRevisions.BakedLeft,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            BakedConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);
    TestEqual(
        TEXT("Realtime lane posts its initial request"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            InitialRevisions.RealtimeLeft,
            true,
            EUERayTracingAudioConvolutionLane::RealtimeLeft,
            RealtimeConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("Baked lane adopts its initial prepared state"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            InitialRevisions.BakedLeft,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            BakedConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Adopted);
    TestEqual(
        TEXT("Realtime lane adopts its initial prepared state"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            InitialRevisions.RealtimeLeft,
            true,
            EUERayTracingAudioConvolutionLane::RealtimeLeft,
            RealtimeConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Adopted);
    for (int32 SampleIndex = 0;
        SampleIndex < (BlockSize + CrossfadeSamples) * 3;
        ++SampleIndex)
    {
        BakedConvolver.ProcessSample(0.0f);
        RealtimeConvolver.ProcessSample(0.0f);
    }
    const int32 InitialPreparedStateCount =
        Bridge.GetPreparedStateCountForTesting();

    const FUERayTracingAudioConvolutionKernel::FKernelPtr
        UpdatedRealtimeKernel =
            FUERayTracingAudioConvolutionKernel::Build(
                TArray<float>{ 0.5f },
                48000,
                BlockSize);
    TestTrue(
        TEXT("Updated realtime lane kernel is valid"),
        UpdatedRealtimeKernel.IsValid());
    FUERayTracingAudioConvolutionRevisions UpdatedRevisions =
        InitialRevisions;
    ++UpdatedRevisions.RealtimeLeft;
    ++UpdatedRevisions.RealtimeRight;
    Bridge.PublishConvolutionTargets(
        AudioComponentId,
        UpdatedRevisions,
        BakedKernel,
        nullptr,
        UpdatedRealtimeKernel,
        nullptr);

    TestEqual(
        TEXT("An unchanged baked lane does not request new workspace"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            UpdatedRevisions.BakedLeft,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            BakedConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Unchanged);
    TestEqual(
        TEXT("Only the changed realtime lane requests new workspace"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            UpdatedRevisions.RealtimeLeft,
            true,
            EUERayTracingAudioConvolutionLane::RealtimeLeft,
            RealtimeConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("A one-lane revision change prepares exactly one new state"),
        Bridge.GetPreparedStateCountForTesting(),
        InitialPreparedStateCount + 1);
    TestEqual(
        TEXT("The changed realtime lane adopts its replacement"),
        Bridge.ConfigureConvolver(
            0,
            AudioComponentId,
            UpdatedRevisions.RealtimeLeft,
            true,
            EUERayTracingAudioConvolutionLane::RealtimeLeft,
            RealtimeConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Adopted);

    Bridge.RemoveConvolutionTargets(AudioComponentId);
    TestTrue(
        TEXT("Baked lane releases its prepared state"),
        Bridge.ReleaseConvolver(
            0,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            BakedConvolver));
    TestTrue(
        TEXT("Realtime lane releases its crossfade states"),
        Bridge.ReleaseConvolver(
            0,
            EUERayTracingAudioConvolutionLane::RealtimeLeft,
            RealtimeConvolver));
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("Per-lane revision test returns every workspace"),
        Bridge.GetPreparedWorkspaceBytes(),
        static_cast<uint64>(0));
    TestEqual(
        TEXT("Per-lane revision test has no return overflow"),
        Bridge.GetConvolutionReturnOverflowCount(),
        static_cast<uint64>(0));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioPreparedConvolutionDeferredTargetTest,
    "UERayTracingAudio.Audio.PreparedConvolutionDeferredTarget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioPreparedConvolutionDeferredTargetTest::RunTest(
    const FString& Parameters)
{
    constexpr uint64 FirstAudioComponentId = 103;
    constexpr uint64 SecondAudioComponentId = 104;
    constexpr uint64 SharedRevision = 41;
    constexpr int32 BlockSize = 8;
    constexpr int32 CrossfadeSamples = 8;
    FUERayTracingAudioIndirectAudioBridge Bridge;
    Bridge.Initialize(1, 16, 48000);

    const FUERayTracingAudioConvolutionKernel::FKernelPtr FirstKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.25f },
            48000,
            BlockSize);
    const FUERayTracingAudioConvolutionKernel::FKernelPtr SecondKernel =
        FUERayTracingAudioConvolutionKernel::Build(
            TArray<float>{ 0.75f },
            48000,
            BlockSize);
    TestTrue(TEXT("Deferred first kernel is valid"), FirstKernel.IsValid());
    TestTrue(TEXT("Deferred second kernel is valid"), SecondKernel.IsValid());
    if (!FirstKernel || !SecondKernel)
    {
        return false;
    }

    FUERayTracingAudioPreparedCrossfadingConvolver Convolver;
    TestEqual(
        TEXT("Audio may request a kernel before its target recipe arrives"),
        Bridge.ConfigureConvolver(
            0,
            FirstAudioComponentId,
            SharedRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            Convolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    Bridge.PublishConvolutionTargets(
        FirstAudioComponentId,
        SharedRevision,
        FirstKernel,
        nullptr,
        nullptr,
        nullptr);
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("A request remains pending until its late target recipe arrives"),
        Bridge.ConfigureConvolver(
            0,
            FirstAudioComponentId,
            SharedRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            Convolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Adopted);
    for (int32 SampleIndex = 0;
        SampleIndex < (BlockSize + CrossfadeSamples) * 3;
        ++SampleIndex)
    {
        Convolver.ProcessSample(0.0f);
    }

    Bridge.PublishConvolutionTargets(
        SecondAudioComponentId,
        SharedRevision,
        SecondKernel,
        nullptr,
        nullptr,
        nullptr);
    TestEqual(
        TEXT("SourceId reuse compares owner as well as revision"),
        Bridge.ConfigureConvolver(
            0,
            SecondAudioComponentId,
            SharedRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            Convolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("Reused SourceId adopts the second component's prepared state"),
        Bridge.ConfigureConvolver(
            0,
            SecondAudioComponentId,
            SharedRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            Convolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Adopted);

    Bridge.RemoveConvolutionTargets(FirstAudioComponentId);
    Bridge.RemoveConvolutionTargets(SecondAudioComponentId);
    TestTrue(
        TEXT("Deferred-target test releases every active lease"),
        Bridge.ReleaseConvolver(
            0,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            Convolver));
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("Deferred-target test leaves no workspace"),
        Bridge.GetPreparedWorkspaceBytes(),
        static_cast<uint64>(0));

    constexpr uint64 NeverAdoptedAudioComponentId = 105;
    FUERayTracingAudioPreparedCrossfadingConvolver
        NeverAdoptedConvolver;
    Bridge.PublishConvolutionTargets(
        NeverAdoptedAudioComponentId,
        SharedRevision,
        FirstKernel,
        nullptr,
        nullptr,
        nullptr);
    TestEqual(
        TEXT("A soon-to-be-released voice posts a prepare request"),
        Bridge.ConfigureConvolver(
            0,
            NeverAdoptedAudioComponentId,
            SharedRevision,
            true,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            NeverAdoptedConvolver,
            CrossfadeSamples),
        EUERayTracingAudioConvolverConfigureResult::Requested);
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestTrue(
        TEXT("The unclaimed ready state owns prepared workspace"),
        Bridge.GetPreparedWorkspaceBytes() > 0);
    TestTrue(
        TEXT("Release posts cancellation even before ready adoption"),
        Bridge.ReleaseConvolver(
            0,
            EUERayTracingAudioConvolutionLane::BakedLeft,
            NeverAdoptedConvolver));
    Bridge.ServiceConvolutionGameThread(MAX_int32);
    TestEqual(
        TEXT("Cancellation reclaims an unclaimed ready state"),
        Bridge.GetPreparedWorkspaceBytes(),
        static_cast<uint64>(0));
    Bridge.RemoveConvolutionTargets(
        NeverAdoptedAudioComponentId);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDeterministicDirectSamplingTest,
    "UERayTracingAudio.Acoustics.DeterministicDirectSampling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDeterministicDirectSamplingTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioContext Context;
    FUERayTracingAudioRayTracingDevice Device;
    FUERayTracingAudioSimulator Simulator(Context);
    FUERayTracingAudioDirectSimulationInput Input;
    Input.ListenerLocation = FVector(0.0, 0.0, 0.0);
    Input.SourceLocation = FVector(500.0, 50.0, 25.0);
    Input.SourceRadiusCm = 40.0f;
    Input.NumOcclusionSamples = 8;
    Input.bUseVolumetricOcclusion = true;

    const FUERayTracingAudioDirectSimulationQuery First = Simulator.BuildDirectSoundQuery(Device, Input);
    const FUERayTracingAudioDirectSimulationQuery Second = Simulator.BuildDirectSoundQuery(Device, Input);
    TestEqual(TEXT("Ray count remains deterministic"), First.Rays.Num(), Second.Rays.Num());
    for (int32 RayIndex = 0; RayIndex < First.Rays.Num() && Second.Rays.IsValidIndex(RayIndex); ++RayIndex)
    {
        TestTrue(TEXT("Ray start remains deterministic"), First.Rays[RayIndex].Start.Equals(Second.Rays[RayIndex].Start));
        TestTrue(TEXT("Ray end remains deterministic"), First.Rays[RayIndex].End.Equals(Second.Rays[RayIndex].End));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioDirectDistanceAmplitudeFalloffTest,
    "UERayTracingAudio.Acoustics.DirectDistanceAmplitudeFalloff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioDirectDistanceAmplitudeFalloffTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioContext Context;
    FUERayTracingAudioRayTracingDevice Device;
    FUERayTracingAudioSimulator Simulator(Context);
    FUERayTracingAudioDirectSimulationInput Input;
    Input.ListenerLocation = FVector::ZeroVector;
    Input.AirAbsorptionPerMeter = FVector::ZeroVector;
    Input.bUseVolumetricOcclusion = false;
    Input.bHardOcclusion = false;

    struct FDistanceExpectation
    {
        float DistanceCm;
        float ExpectedAmplitude;
    };
    constexpr FDistanceExpectation Expectations[] =
    {
        { 100.0f, 1.0f },
        { 200.0f, 0.5f },
        { 400.0f, 0.25f }
    };

    for (const FDistanceExpectation& Expectation : Expectations)
    {
        Input.SourceLocation = FVector(Expectation.DistanceCm, 0.0f, 0.0f);
        FUERayTracingAudioDirectSimulationQuery Query =
            Simulator.BuildDirectSoundQuery(Device, Input);

        TArray<bool> ClearPathHits;
        ClearPathHits.Add(false);
        const FUERayTracingAudioDirectSimulationResult Result =
            Simulator.FinalizeDirectSound(
                Input,
                MoveTemp(Query),
                MoveTemp(ClearPathHits));

        TestTrue(
            *FString::Printf(
                TEXT("%.0f cm direct amplitude follows 1/r"),
                Expectation.DistanceCm),
            FMath::IsNearlyEqual(
                Result.DistanceAttenuation,
                Expectation.ExpectedAmplitude,
                UE_KINDA_SMALL_NUMBER));
        TestTrue(
            *FString::Printf(
                TEXT("%.0f cm clear direct output remains continuous and non-zero"),
                Expectation.DistanceCm),
            Result.OverallGain > 0.0f
                && FMath::IsNearlyEqual(
                    Result.OverallGain,
                    Expectation.ExpectedAmplitude,
                    UE_KINDA_SMALL_NUMBER));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioOcclusionModeBehaviorTest,
    "UERayTracingAudio.Acoustics.OcclusionModes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioOcclusionModeBehaviorTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioContext Context;
    FUERayTracingAudioSimulator Simulator(Context);
    FUERayTracingAudioDirectSimulationInput Input;
    Input.OccludedGain = 0.2f;

    auto BuildHalfVisibleQuery = []()
    {
        FUERayTracingAudioDirectSimulationQuery Query;
        Query.bVolumetric = true;
        Query.NumSamplePoints = 4;
        Query.Rays.SetNum(8);
        Query.BaseResult.DistanceAttenuation = 1.0f;
        Query.BaseResult.AirAbsorption = FVector::OneVector;
        return Query;
    };
    auto BuildHalfVisibleHits = []()
    {
        TArray<bool> Hits;
        Hits.Init(false, 8);
        Hits[4] = true;
        Hits[5] = true;
        return Hits;
    };

    Input.bHardOcclusion = false;
    const FUERayTracingAudioDirectSimulationResult Soft = Simulator.FinalizeDirectSound(
        Input,
        BuildHalfVisibleQuery(),
        BuildHalfVisibleHits());
    TestEqual(TEXT("Half of the valid samples are visible"), Soft.DirectVisibility, 0.5f);
    TestTrue(TEXT("Soft occlusion preserves the configured floor"), FMath::IsNearlyEqual(Soft.Occlusion, 0.6f));

    Input.bHardOcclusion = true;
    const FUERayTracingAudioDirectSimulationResult Hard = Simulator.FinalizeDirectSound(
        Input,
        BuildHalfVisibleQuery(),
        BuildHalfVisibleHits());
    TestEqual(TEXT("Hard occlusion follows visibility"), Hard.Occlusion, 0.5f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioCpuFallbackIndirectTest,
    "UERayTracingAudio.Acoustics.CpuFallbackIndirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioCpuFallbackIndirectTest::RunTest(const FString& Parameters)
{
    TArray<FUERayTracingAudioGeometryExport> Room;
    auto AddWall = [&Room](const FVector& Min, const FVector& Max)
    {
        FUERayTracingAudioGeometryExport& Wall = Room.AddDefaulted_GetRef();
        Wall.Bounds = FBox(Min, Max);
        Wall.Absorption = FVector(0.15f, 0.2f, 0.3f);
        Wall.Scattering = 0.35f;
    };

    constexpr float HalfExtent = 500.0f;
    constexpr float Thickness = 10.0f;
    AddWall(FVector(HalfExtent - Thickness, -HalfExtent, -HalfExtent), FVector(HalfExtent, HalfExtent, HalfExtent));
    AddWall(FVector(-HalfExtent, -HalfExtent, -HalfExtent), FVector(-HalfExtent + Thickness, HalfExtent, HalfExtent));
    AddWall(FVector(-HalfExtent, HalfExtent - Thickness, -HalfExtent), FVector(HalfExtent, HalfExtent, HalfExtent));
    AddWall(FVector(-HalfExtent, -HalfExtent, -HalfExtent), FVector(HalfExtent, -HalfExtent + Thickness, HalfExtent));
    AddWall(FVector(-HalfExtent, -HalfExtent, HalfExtent - Thickness), FVector(HalfExtent, HalfExtent, HalfExtent));
    AddWall(FVector(-HalfExtent, -HalfExtent, -HalfExtent), FVector(HalfExtent, HalfExtent, -HalfExtent + Thickness));

    FUERayTracingAudioScene Scene;
    Scene.SetStaticGeometry(MoveTemp(Room));

    FUERayTracingAudioContext Context;
    FUERayTracingAudioRayTracingDevice Device;
    FUERayTracingAudioSimulator Simulator(Context);
    FUERayTracingAudioIndirectSimulationInput Input;
    Input.Scene = &Scene;
    Input.ListenerLocation = FVector::ZeroVector;
    Input.ListenerForward = FVector::ForwardVector;
    Input.SourceLocation = FVector(100.0f, 0.0f, 0.0f);
    Input.NumReflectionRays = 64;
    Input.MaxReflectionBounces = 2;
    Input.NumDelayBins = 16000;
    Input.DurationSeconds = 1.0f;

    const FUERayTracingAudioIndirectSimulationResult Result = Simulator.SimulateIndirectSound(Device, Input);
    TestTrue(TEXT("The pure CPU fallback finds indirect paths in a closed room"), Result.bHasValidPaths);
    TestTrue(TEXT("The CPU fallback emits a finite positive gain"), FMath::IsFinite(Result.IndirectGain) && Result.IndirectGain > 0.0f);
    TestEqual(TEXT("The CPU fallback preserves the requested energy-field resolution"), Result.EnergyField.DelayBinEnergy.Num(), Input.NumDelayBins);
    TestTrue(
        TEXT("A 16 kHz bake bin duration remains valid below the geometry epsilon"),
        Result.ImpulseResponseBinDurationSeconds > 0.0f
            && Result.ImpulseResponseBinDurationSeconds < UE_KINDA_SMALL_NUMBER);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioLowEnergyImpulseReconstructionTest,
    "UERayTracingAudio.Acoustics.LowEnergyImpulseReconstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioLowEnergyImpulseReconstructionTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioContext Context;
    FUERayTracingAudioSimulator Simulator(Context);
    FUERayTracingAudioIndirectSimulationInput Input;
    Input.NumDelayBins = 4;
    Input.DurationSeconds = 0.1f;
    Input.MaxEarlyReflectionTaps = 4;
    Input.EffectType = EUERayTracingAudioIndirectEffectType::Convolution;

    FUERayTracingAudioEnergyFieldTraceResult TraceResult;
    TraceResult.DelayBinEnergy.Init(FVector::ZeroVector, Input.NumDelayBins);
    TraceResult.DelayBinDirection.Init(FVector::ZeroVector, Input.NumDelayBins);
    TraceResult.DelayBinEnergy[1] = FVector(2.0e-8f, 1.5e-8f, 1.0e-8f);
    TraceResult.DelayBinDirection[1] = FVector(0.0f, 1.5e-8f, 0.0f);
    TraceResult.DelayBinEnergy[3] = FVector(2.0e-8f);
    TraceResult.DelayBinDirection[3] = FVector(0.0f, 2.0e-8f, 0.0f);
    TraceResult.NumValidContributions = 2;
    TraceResult.EarliestArrivalSeconds = 0.025f;

    const FUERayTracingAudioIndirectSimulationResult Result =
        Simulator.FinalizeIndirectSound(Input, MoveTemp(TraceResult));
    TestTrue(TEXT("A quantized low-energy path remains valid"), Result.bHasValidPaths);
    TestTrue(TEXT("Low-energy accumulation produces positive gain"), Result.IndirectGain > 0.0f);
    TestTrue(
        TEXT("Low-energy accumulation reconstructs a non-zero impulse sample"),
        Result.ReconstructedImpulseResponse.IsValidIndex(1)
            && Result.ReconstructedImpulseResponse[1] > 0.0f);
    TestTrue(TEXT("The 80 ms split preserves early-reflection energy"), Result.EarlyReflectionGain > 0.0f);
    TestTrue(TEXT("The 80 ms split exposes late-reverb energy"), Result.LateReverbGain > 0.0f);
    TestEqual(TEXT("Directional moments survive into both active bins"), Result.DirectionalBinCount, 2);
    TestTrue(TEXT("Directional energy coverage is retained"), Result.DirectionalEnergyRatio >= 0.99f);
    TestTrue(
        TEXT("The dominant arrival direction is retained"),
        FVector::DotProduct(Result.DominantArrivalDirection, FVector::RightVector) >= 0.99f);
    return true;
}

#endif
