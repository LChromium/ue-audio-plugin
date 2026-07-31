#if WITH_DEV_AUTOMATION_TESTS

#include "Audio/UERayTracingAudioSimulationSnapshot.h"
#include "Async/Async.h"
#include "Misc/AutomationTest.h"

namespace
{
    bool IsConsistentSnapshot(
        const FUERayTracingAudioSimulationSnapshot& Snapshot)
    {
        const int32 ExpectedValue =
            static_cast<int32>(Snapshot.Generation % 4096ull);
        return Snapshot.DirectResult.DistanceCm
                == static_cast<float>(ExpectedValue)
            && Snapshot.IndirectResult.NumValidPaths == ExpectedValue
            && Snapshot.IndirectMix == static_cast<float>(ExpectedValue);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUERayTracingAudioSimulationSnapshotRegistryTest,
    "UERayTracingAudio.Snapshots.PublishReadRemove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERayTracingAudioSimulationSnapshotRegistryTest::RunTest(const FString& Parameters)
{
    FUERayTracingAudioSimulationSnapshotRegistry Registry;
    FUERayTracingAudioSimulationSnapshot Snapshot;
    Snapshot.DirectResult.OverallGain = 0.25f;
    Snapshot.IndirectResult.NumValidPaths = 7;
    Snapshot.DataSource = EUERayTracingAudioRuntimeDataSource::Hybrid;
    Snapshot.Generation = 3;

    Registry.Publish(42, MoveTemp(Snapshot));
    FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr Published = Registry.Read(42);
    TestTrue(TEXT("Published snapshot can be read"), Published.IsValid());
    if (Published)
    {
        TestEqual(TEXT("Generation is preserved"), Published->Generation, static_cast<uint64>(3));
        TestEqual(TEXT("Direct result is preserved"), Published->DirectResult.OverallGain, 0.25f);
        TestEqual(TEXT("Indirect result is preserved"), Published->IndirectResult.NumValidPaths, 7);
        TestEqual(
            TEXT("Runtime data source provenance is preserved"),
            Published->DataSource,
            EUERayTracingAudioRuntimeDataSource::Hybrid);
    }

    Registry.Remove(42);
    TestFalse(TEXT("Removed snapshot is no longer visible"), Registry.Read(42).IsValid());
    Published = FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr();

    auto PublishGeneration = [&Registry](const uint64 Generation)
    {
        FUERayTracingAudioSimulationSnapshot Next;
        Next.Generation = Generation;
        Registry.Publish(42, MoveTemp(Next));
    };

    PublishGeneration(1);
    FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr First =
        Registry.Read(42);
    PublishGeneration(2);
    FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr Second =
        Registry.Read(42);
    PublishGeneration(3);
    FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr Third =
        Registry.Read(42);
    TestTrue(
        TEXT("Three independently pinned generations remain readable"),
        First.IsValid()
        && Second.IsValid()
        && Third.IsValid()
        && First->Generation == 1
        && Second->Generation == 2
        && Third->Generation == 3);

    const uint64 DroppedBefore = Registry.GetDroppedPublishCount();
    PublishGeneration(4);
    FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr StillThird =
        Registry.Read(42);
    TestTrue(
        TEXT("A fourth publication is bounded while all three slots are pinned"),
        StillThird.IsValid() && StillThird->Generation == 3);
    TestEqual(
        TEXT("A bounded publication failure is observable"),
        Registry.GetDroppedPublishCount(),
        DroppedBefore + 1);

    First = FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr();
    PublishGeneration(4);
    FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr Fourth =
        Registry.Read(42);
    TestTrue(
        TEXT("Publication resumes after one read handle reaches quiescence"),
        Fourth.IsValid() && Fourth->Generation == 4);

    Registry.Reset();
    TestFalse(
        TEXT("Reset hides the current generation without invalidating held handles"),
        Registry.Read(42).IsValid());
    TestTrue(
        TEXT("A handle pinned before Reset retains its complete snapshot value"),
        Fourth.IsValid() && Fourth->Generation == 4);

    TestTrue(
        TEXT("Audio reader pinning uses lock-free atomics"),
        Registry.IsReaderPinningLockFreeForTesting());

    std::atomic<bool> bBeginConcurrentRead { false };
    std::atomic<bool> bStopConcurrentRead { false };
    std::atomic<uint64> ConsistentReadCount { 0 };
    std::atomic<uint64> TornReadCount { 0 };
    auto ReadUntilStopped =
        [&Registry,
         &bBeginConcurrentRead,
         &bStopConcurrentRead,
         &ConsistentReadCount,
         &TornReadCount]()
        {
            while (!bBeginConcurrentRead.load(std::memory_order_acquire))
            {
                FPlatformProcess::YieldThread();
            }
            while (!bStopConcurrentRead.load(std::memory_order_acquire))
            {
                FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr
                    ConcurrentSnapshot = Registry.Read(84);
                if (ConcurrentSnapshot)
                {
                    if (IsConsistentSnapshot(*ConcurrentSnapshot.Get()))
                    {
                        ConsistentReadCount.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                    else
                    {
                        TornReadCount.fetch_add(
                            1,
                            std::memory_order_relaxed);
                    }
                }
            }
        };

    TFuture<void> FirstReader = Async(
        EAsyncExecution::ThreadPool,
        ReadUntilStopped);
    TFuture<void> SecondReader = Async(
        EAsyncExecution::ThreadPool,
        ReadUntilStopped);
    bBeginConcurrentRead.store(true, std::memory_order_release);
    for (uint64 Generation = 1; Generation <= 5000; ++Generation)
    {
        FUERayTracingAudioSimulationSnapshot ConcurrentSnapshot;
        const int32 ExpectedValue =
            static_cast<int32>(Generation % 4096ull);
        ConcurrentSnapshot.Generation = Generation;
        ConcurrentSnapshot.DirectResult.DistanceCm =
            static_cast<float>(ExpectedValue);
        ConcurrentSnapshot.IndirectResult.NumValidPaths = ExpectedValue;
        ConcurrentSnapshot.IndirectMix =
            static_cast<float>(ExpectedValue);
        Registry.Publish(84, MoveTemp(ConcurrentSnapshot));
        if ((Generation % 29ull) == 0)
        {
            Registry.Remove(84);
        }
    }
    bStopConcurrentRead.store(true, std::memory_order_release);
    FirstReader.Wait();
    SecondReader.Wait();
    TestTrue(
        TEXT("Concurrent readers observed published generations"),
        ConsistentReadCount.load(std::memory_order_relaxed) > 0);
    TestEqual(
        TEXT("Concurrent publish/remove never exposes a torn snapshot"),
        TornReadCount.load(std::memory_order_relaxed),
        static_cast<uint64>(0));
    return true;
}

#endif
