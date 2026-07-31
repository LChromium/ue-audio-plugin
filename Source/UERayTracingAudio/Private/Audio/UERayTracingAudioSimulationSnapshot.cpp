#include "Audio/UERayTracingAudioSimulationSnapshot.h"

#include "Misc/ScopeLock.h"

namespace
{
    static_assert(
        std::atomic<uint32>::is_always_lock_free,
        "Snapshot reader pins must remain lock-free on the audio path.");
    static_assert(
        std::atomic<uint64>::is_always_lock_free,
        "Snapshot sequence reads must remain lock-free on the audio path.");
    static_assert(
        std::atomic<int32>::is_always_lock_free,
        "Snapshot slot publication must remain lock-free on the audio path.");

    constexpr uint64 EmptyAudioComponentId = 0;
    constexpr uint64 TombstoneAudioComponentId = MAX_uint64;

    uint32 HashAudioComponentId(const uint64 AudioComponentId)
    {
        const uint64 Mixed =
            AudioComponentId * 11400714819323198485ull;
        return static_cast<uint32>(Mixed ^ (Mixed >> 32));
    }
}

struct FUERayTracingAudioSimulationSnapshotRegistry::FEntry
{
    struct FSlot
    {
        std::atomic<uint32> ReaderCount { 0 };
        FUERayTracingAudioSimulationSnapshot Snapshot;
    };

    std::atomic<uint64> AudioComponentId { EmptyAudioComponentId };
    std::atomic<uint64> Sequence { 0 };
    std::atomic<int32> PublishedSlot { INDEX_NONE };
    FSlot Slots[SnapshotSlotsPerEntry];
};

FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::
    FSnapshotReadHandle(
        const FUERayTracingAudioSimulationSnapshot* InSnapshot,
        std::atomic<uint32>* InReaderCount)
    : Snapshot(InSnapshot)
    , ReaderCount(InReaderCount)
{
}

FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::
    ~FSnapshotReadHandle()
{
    Release();
}

FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::
    FSnapshotReadHandle(FSnapshotReadHandle&& Other) noexcept
    : Snapshot(Other.Snapshot)
    , ReaderCount(Other.ReaderCount)
{
    Other.Snapshot = nullptr;
    Other.ReaderCount = nullptr;
}

FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle&
FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::operator=(
    FSnapshotReadHandle&& Other) noexcept
{
    if (this != &Other)
    {
        Release();
        Snapshot = Other.Snapshot;
        ReaderCount = Other.ReaderCount;
        Other.Snapshot = nullptr;
        Other.ReaderCount = nullptr;
    }
    return *this;
}

bool FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::
    IsValid() const
{
    return Snapshot != nullptr;
}

FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::
    operator bool() const
{
    return IsValid();
}

const FUERayTracingAudioSimulationSnapshot*
FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::Get() const
{
    return Snapshot;
}

const FUERayTracingAudioSimulationSnapshot*
FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::
    operator->() const
{
    return Snapshot;
}

void FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotReadHandle::
    Release()
{
    if (ReaderCount)
    {
        ReaderCount->fetch_sub(1, std::memory_order_seq_cst);
    }
    Snapshot = nullptr;
    ReaderCount = nullptr;
}

FUERayTracingAudioSimulationSnapshotRegistry::
    FUERayTracingAudioSimulationSnapshotRegistry()
    : Entries(new FEntry[EntryCapacity])
{
}

FUERayTracingAudioSimulationSnapshotRegistry::
    ~FUERayTracingAudioSimulationSnapshotRegistry() = default;

int32 FUERayTracingAudioSimulationSnapshotRegistry::FindEntry(
    const uint64 AudioComponentId) const
{
    if (AudioComponentId == EmptyAudioComponentId
        || AudioComponentId == TombstoneAudioComponentId)
    {
        return INDEX_NONE;
    }

    const uint32 StartIndex =
        HashAudioComponentId(AudioComponentId)
        & (EntryCapacity - 1);
    for (int32 Probe = 0; Probe < EntryCapacity; ++Probe)
    {
        const int32 EntryIndex =
            static_cast<int32>((StartIndex + Probe) & (EntryCapacity - 1));
        const uint64 ExistingId =
            Entries[EntryIndex].AudioComponentId.load(
                std::memory_order_acquire);
        if (ExistingId == AudioComponentId)
        {
            return EntryIndex;
        }
        if (ExistingId == EmptyAudioComponentId)
        {
            return INDEX_NONE;
        }
    }
    return INDEX_NONE;
}

int32 FUERayTracingAudioSimulationSnapshotRegistry::FindOrReserveEntry(
    const uint64 AudioComponentId)
{
    const uint32 StartIndex =
        HashAudioComponentId(AudioComponentId)
        & (EntryCapacity - 1);
    int32 FirstTombstoneIndex = INDEX_NONE;
    for (int32 Probe = 0; Probe < EntryCapacity; ++Probe)
    {
        const int32 EntryIndex =
            static_cast<int32>((StartIndex + Probe) & (EntryCapacity - 1));
        FEntry& Entry = Entries[EntryIndex];
        const uint64 ExistingId =
            Entry.AudioComponentId.load(std::memory_order_acquire);
        if (ExistingId == AudioComponentId)
        {
            return EntryIndex;
        }
        if (ExistingId == TombstoneAudioComponentId
            && FirstTombstoneIndex == INDEX_NONE)
        {
            FirstTombstoneIndex = EntryIndex;
            continue;
        }
        if (ExistingId == EmptyAudioComponentId)
        {
            const int32 ReservedIndex =
                FirstTombstoneIndex != INDEX_NONE
                ? FirstTombstoneIndex
                : EntryIndex;
            FEntry& ReservedEntry = Entries[ReservedIndex];
            ReservedEntry.Sequence.fetch_add(
                1,
                std::memory_order_seq_cst);
            ReservedEntry.PublishedSlot.store(
                INDEX_NONE,
                std::memory_order_release);
            ReservedEntry.AudioComponentId.store(
                AudioComponentId,
                std::memory_order_release);
            ReservedEntry.Sequence.fetch_add(
                1,
                std::memory_order_seq_cst);
            return ReservedIndex;
        }
    }

    if (FirstTombstoneIndex != INDEX_NONE)
    {
        FEntry& ReservedEntry = Entries[FirstTombstoneIndex];
        ReservedEntry.Sequence.fetch_add(1, std::memory_order_seq_cst);
        ReservedEntry.PublishedSlot.store(
            INDEX_NONE,
            std::memory_order_release);
        ReservedEntry.AudioComponentId.store(
            AudioComponentId,
            std::memory_order_release);
        ReservedEntry.Sequence.fetch_add(1, std::memory_order_seq_cst);
    }
    return FirstTombstoneIndex;
}

bool FUERayTracingAudioSimulationSnapshotRegistry::Publish(
    const uint64 AudioComponentId,
    FUERayTracingAudioSimulationSnapshot&& Snapshot)
{
    if (AudioComponentId == EmptyAudioComponentId
        || AudioComponentId == TombstoneAudioComponentId)
    {
        return false;
    }

    FScopeLock ScopeLock(&WriterLock);
    const int32 EntryIndex = FindOrReserveEntry(AudioComponentId);
    if (EntryIndex == INDEX_NONE)
    {
        DroppedPublishCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    FEntry& Entry = Entries[EntryIndex];
    // Sequence and reader pins deliberately share the global seq_cst order.
    // If a reader pins after this writer observed zero readers, its second
    // sequence read must observe this odd commit. If it validated first, this
    // writer's subsequent reader-count load must observe the pin. That closes
    // the classic reclamation race without waiting or taking a callback lock.
    Entry.Sequence.fetch_add(1, std::memory_order_seq_cst);
    const int32 CurrentSlot =
        Entry.PublishedSlot.load(std::memory_order_acquire);
    int32 TargetSlot = INDEX_NONE;
    for (int32 SlotOffset = 1;
        SlotOffset <= SnapshotSlotsPerEntry;
        ++SlotOffset)
    {
        const int32 CandidateSlot =
            CurrentSlot == INDEX_NONE
            ? SlotOffset - 1
            : (CurrentSlot + SlotOffset) % SnapshotSlotsPerEntry;
        if (CandidateSlot != CurrentSlot
            && Entry.Slots[CandidateSlot].ReaderCount.load(
                std::memory_order_seq_cst) == 0)
        {
            TargetSlot = CandidateSlot;
            break;
        }
    }
    if (TargetSlot == INDEX_NONE
        && CurrentSlot != INDEX_NONE
        && Entry.Slots[CurrentSlot].ReaderCount.load(
            std::memory_order_seq_cst) == 0)
    {
        TargetSlot = CurrentSlot;
    }

    if (TargetSlot == INDEX_NONE)
    {
        Entry.Sequence.fetch_add(1, std::memory_order_seq_cst);
        DroppedPublishCount.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Entry.Slots[TargetSlot].Snapshot = MoveTemp(Snapshot);
    Entry.PublishedSlot.store(TargetSlot, std::memory_order_release);
    Entry.Sequence.fetch_add(1, std::memory_order_seq_cst);
    return true;
}

FUERayTracingAudioSimulationSnapshotRegistry::FSnapshotPtr
FUERayTracingAudioSimulationSnapshotRegistry::Read(
    const uint64 AudioComponentId) const
{
    const int32 EntryIndex = FindEntry(AudioComponentId);
    if (EntryIndex == INDEX_NONE)
    {
        return FSnapshotPtr();
    }

    FEntry& Entry = Entries[EntryIndex];
    for (int32 Attempt = 0; Attempt < MaxReadAttempts; ++Attempt)
    {
        const uint64 SequenceBefore =
            Entry.Sequence.load(std::memory_order_seq_cst);
        if ((SequenceBefore & 1ull) != 0)
        {
            ReadRetryCount.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (Entry.AudioComponentId.load(std::memory_order_acquire)
            != AudioComponentId)
        {
            return FSnapshotPtr();
        }

        const int32 SlotIndex =
            Entry.PublishedSlot.load(std::memory_order_acquire);
        if (SlotIndex == INDEX_NONE)
        {
            return FSnapshotPtr();
        }
        FEntry::FSlot& Slot = Entry.Slots[SlotIndex];
        Slot.ReaderCount.fetch_add(1, std::memory_order_seq_cst);
        const uint64 SequenceAfter =
            Entry.Sequence.load(std::memory_order_seq_cst);
        const bool bStable =
            SequenceBefore == SequenceAfter
            && (SequenceAfter & 1ull) == 0
            && Entry.AudioComponentId.load(std::memory_order_acquire)
                == AudioComponentId
            && Entry.PublishedSlot.load(std::memory_order_acquire)
                == SlotIndex;
        if (bStable)
        {
            return FSnapshotPtr(&Slot.Snapshot, &Slot.ReaderCount);
        }

        Slot.ReaderCount.fetch_sub(1, std::memory_order_seq_cst);
        ReadRetryCount.fetch_add(1, std::memory_order_relaxed);
    }
    return FSnapshotPtr();
}

void FUERayTracingAudioSimulationSnapshotRegistry::Remove(
    const uint64 AudioComponentId)
{
    FScopeLock ScopeLock(&WriterLock);
    const int32 EntryIndex = FindEntry(AudioComponentId);
    if (EntryIndex == INDEX_NONE)
    {
        return;
    }

    FEntry& Entry = Entries[EntryIndex];
    Entry.Sequence.fetch_add(1, std::memory_order_seq_cst);
    Entry.PublishedSlot.store(INDEX_NONE, std::memory_order_release);
    Entry.AudioComponentId.store(
        TombstoneAudioComponentId,
        std::memory_order_release);
    Entry.Sequence.fetch_add(1, std::memory_order_seq_cst);
}

void FUERayTracingAudioSimulationSnapshotRegistry::Reset()
{
    FScopeLock ScopeLock(&WriterLock);
    for (int32 EntryIndex = 0;
        EntryIndex < EntryCapacity;
        ++EntryIndex)
    {
        FEntry& Entry = Entries[EntryIndex];
        const uint64 AudioComponentId =
            Entry.AudioComponentId.load(std::memory_order_acquire);
        if (AudioComponentId == EmptyAudioComponentId
            || AudioComponentId == TombstoneAudioComponentId)
        {
            continue;
        }
        Entry.Sequence.fetch_add(1, std::memory_order_seq_cst);
        Entry.PublishedSlot.store(INDEX_NONE, std::memory_order_release);
        Entry.AudioComponentId.store(
            TombstoneAudioComponentId,
            std::memory_order_release);
        Entry.Sequence.fetch_add(1, std::memory_order_seq_cst);
    }
}

uint64 FUERayTracingAudioSimulationSnapshotRegistry::
    GetDroppedPublishCount() const
{
    return DroppedPublishCount.load(std::memory_order_relaxed);
}

uint64 FUERayTracingAudioSimulationSnapshotRegistry::GetReadRetryCount() const
{
    return ReadRetryCount.load(std::memory_order_relaxed);
}

#if WITH_DEV_AUTOMATION_TESTS
bool FUERayTracingAudioSimulationSnapshotRegistry::
    IsReaderPinningLockFreeForTesting() const
{
    return std::atomic<uint32>::is_always_lock_free
        && std::atomic<uint64>::is_always_lock_free
        && std::atomic<int32>::is_always_lock_free;
}
#endif
