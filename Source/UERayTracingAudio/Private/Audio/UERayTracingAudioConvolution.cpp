#include "Audio/UERayTracingAudioConvolution.h"

namespace
{
    void Transform(TArray<FUERayTracingAudioComplexSample>& Data, bool bInverse)
    {
        const int32 Num = Data.Num();
        for (int32 Index = 1, Reversed = 0; Index < Num; ++Index)
        {
            int32 Bit = Num >> 1;
            for (; Reversed & Bit; Bit >>= 1)
            {
                Reversed ^= Bit;
            }
            Reversed ^= Bit;
            if (Index < Reversed)
            {
                Swap(Data[Index], Data[Reversed]);
            }
        }

        for (int32 Length = 2; Length <= Num; Length <<= 1)
        {
            const float Angle = (bInverse ? 2.0f : -2.0f) * PI / static_cast<float>(Length);
            const FUERayTracingAudioComplexSample Root{ FMath::Cos(Angle), FMath::Sin(Angle) };
            for (int32 Start = 0; Start < Num; Start += Length)
            {
                FUERayTracingAudioComplexSample Weight{ 1.0f, 0.0f };
                for (int32 Offset = 0; Offset < Length / 2; ++Offset)
                {
                    const FUERayTracingAudioComplexSample Even = Data[Start + Offset];
                    const FUERayTracingAudioComplexSample Odd = Data[Start + Offset + (Length / 2)] * Weight;
                    Data[Start + Offset] = Even + Odd;
                    Data[Start + Offset + (Length / 2)] = Even - Odd;
                    Weight = Weight * Root;
                }
            }
        }

        if (bInverse)
        {
            const float Scale = 1.0f / static_cast<float>(Num);
            for (FUERayTracingAudioComplexSample& Sample : Data)
            {
                Sample.Real *= Scale;
                Sample.Imag *= Scale;
            }
        }
    }
}

FUERayTracingAudioComplexSample FUERayTracingAudioComplexSample::operator+(const FUERayTracingAudioComplexSample& Other) const
{
    return { Real + Other.Real, Imag + Other.Imag };
}

FUERayTracingAudioComplexSample FUERayTracingAudioComplexSample::operator-(const FUERayTracingAudioComplexSample& Other) const
{
    return { Real - Other.Real, Imag - Other.Imag };
}

FUERayTracingAudioComplexSample FUERayTracingAudioComplexSample::operator*(const FUERayTracingAudioComplexSample& Other) const
{
    return { (Real * Other.Real) - (Imag * Other.Imag), (Real * Other.Imag) + (Imag * Other.Real) };
}

FUERayTracingAudioComplexSample& FUERayTracingAudioComplexSample::operator+=(const FUERayTracingAudioComplexSample& Other)
{
    Real += Other.Real;
    Imag += Other.Imag;
    return *this;
}

FUERayTracingAudioConvolutionKernel::FKernelPtr FUERayTracingAudioConvolutionKernel::Build(
    const TArray<float>& ImpulseResponse,
    int32 InSampleRate,
    int32 InBlockSize)
{
    if (ImpulseResponse.IsEmpty() || InSampleRate <= 0 || InBlockSize <= 0 || !FMath::IsPowerOfTwo(InBlockSize))
    {
        return nullptr;
    }

    TSharedPtr<FUERayTracingAudioConvolutionKernel, ESPMode::ThreadSafe> Kernel =
        MakeShared<FUERayTracingAudioConvolutionKernel, ESPMode::ThreadSafe>();
    Kernel->SampleRate = InSampleRate;
    Kernel->BlockSize = InBlockSize;
    Kernel->OriginalNumImpulseSamples =
        ImpulseResponse.Num();
    const int32 MaxRuntimeImpulseSamples =
        InBlockSize
        * UERayTracingAudioConvolutionLimits::
            MaxRuntimePartitionsPerLane;
    Kernel->NumImpulseSamples = FMath::Min(
        ImpulseResponse.Num(),
        MaxRuntimeImpulseSamples);
    Kernel->bRuntimeTailTruncated =
        Kernel->NumImpulseSamples
        < Kernel->OriginalNumImpulseSamples;
    const int32 TransformSize = InBlockSize * 2;
    const int32 NumPartitions = FMath::DivideAndRoundUp(
        Kernel->NumImpulseSamples,
        InBlockSize);
    Kernel->FrequencyPartitions.SetNum(NumPartitions);
    const int32 TaperSamples =
        Kernel->bRuntimeTailTruncated
        ? FMath::Clamp(InBlockSize / 4, 1, 256)
        : 0;
    const int32 TaperStartSample =
        Kernel->NumImpulseSamples - TaperSamples;

    for (int32 PartitionIndex = 0; PartitionIndex < NumPartitions; ++PartitionIndex)
    {
        TArray<FUERayTracingAudioComplexSample>& Partition = Kernel->FrequencyPartitions[PartitionIndex];
        Partition.SetNumZeroed(TransformSize);
        const int32 SourceOffset = PartitionIndex * InBlockSize;
        const int32 CopyCount = FMath::Min(
            InBlockSize,
            Kernel->NumImpulseSamples - SourceOffset);
        for (int32 SampleIndex = 0; SampleIndex < CopyCount; ++SampleIndex)
        {
            const int32 ImpulseSampleIndex =
                SourceOffset + SampleIndex;
            const float SourceSample =
                ImpulseResponse[ImpulseSampleIndex];
            float Sample = FMath::IsFinite(SourceSample)
                ? SourceSample
                : 0.0f;
            if (TaperSamples > 0
                && ImpulseSampleIndex >= TaperStartSample)
            {
                const float TaperAlpha =
                    static_cast<float>(
                        Kernel->NumImpulseSamples
                        - ImpulseSampleIndex)
                    / static_cast<float>(TaperSamples);
                Sample *= FMath::Clamp(
                    TaperAlpha,
                    0.0f,
                    1.0f);
            }
            Partition[SampleIndex].Real = Sample;
        }
        Transform(Partition, false);
    }
    return Kernel;
}

int32 FUERayTracingAudioConvolutionKernel::GetSampleRate() const { return SampleRate; }
int32 FUERayTracingAudioConvolutionKernel::GetBlockSize() const { return BlockSize; }
int32 FUERayTracingAudioConvolutionKernel::GetNumPartitions() const { return FrequencyPartitions.Num(); }
float FUERayTracingAudioConvolutionKernel::GetDurationSeconds() const
{
    return SampleRate > 0 ? static_cast<float>(NumImpulseSamples) / static_cast<float>(SampleRate) : 0.0f;
}
float FUERayTracingAudioConvolutionKernel::GetOriginalDurationSeconds() const
{
    return SampleRate > 0
        ? static_cast<float>(OriginalNumImpulseSamples)
            / static_cast<float>(SampleRate)
        : 0.0f;
}
bool FUERayTracingAudioConvolutionKernel::WasRuntimeTailTruncated() const
{
    return bRuntimeTailTruncated;
}

void FUERayTracingAudioPartitionedConvolver::SetKernel(FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel)
{
    if (Kernel == InKernel)
    {
        return;
    }

    Kernel = MoveTemp(InKernel);
    Reset();
    if (!Kernel.IsValid())
    {
        return;
    }

    const int32 BlockSize = Kernel->BlockSize;
    const int32 TransformSize = BlockSize * 2;
    InputHistory.SetNum(Kernel->FrequencyPartitions.Num());
    for (TArray<FUERayTracingAudioComplexSample>& History : InputHistory)
    {
        History.SetNumZeroed(TransformSize);
    }
    FrequencyOutput.SetNumZeroed(TransformSize);
    InputBlock.SetNumZeroed(BlockSize);
    OutputBlock.SetNumZeroed(BlockSize);
    Overlap.SetNumZeroed(BlockSize);
    OutputReadIndex = BlockSize;
}

float FUERayTracingAudioPartitionedConvolver::ProcessSample(float InputSample)
{
    if (!Kernel.IsValid() || InputBlock.IsEmpty())
    {
        return 0.0f;
    }

    const float Output = OutputReadIndex < OutputBlock.Num() ? OutputBlock[OutputReadIndex++] : 0.0f;
    InputBlock[InputFill++] = InputSample;
    if (InputFill == InputBlock.Num())
    {
        ProcessBlock();
    }
    return Output;
}

void FUERayTracingAudioPartitionedConvolver::Reset()
{
    InputHistory.Reset();
    FrequencyOutput.Reset();
    InputBlock.Reset();
    OutputBlock.Reset();
    Overlap.Reset();
    InputFill = 0;
    OutputReadIndex = 0;
    HistoryWriteIndex = 0;
}

bool FUERayTracingAudioPartitionedConvolver::HasOutput() const
{
    return Kernel.IsValid() && !InputBlock.IsEmpty();
}

int32 FUERayTracingAudioPartitionedConvolver::GetBlockSize() const
{
    return Kernel.IsValid() ? Kernel->GetBlockSize() : 0;
}

uint64 FUERayTracingAudioPartitionedConvolver::GetAllocatedBytes() const
{
    uint64 AllocatedBytes =
        static_cast<uint64>(InputHistory.GetAllocatedSize())
        + static_cast<uint64>(FrequencyOutput.GetAllocatedSize())
        + static_cast<uint64>(InputBlock.GetAllocatedSize())
        + static_cast<uint64>(OutputBlock.GetAllocatedSize())
        + static_cast<uint64>(Overlap.GetAllocatedSize());
    for (const TArray<FUERayTracingAudioComplexSample>& History
        : InputHistory)
    {
        AllocatedBytes +=
            static_cast<uint64>(History.GetAllocatedSize());
    }
    return AllocatedBytes;
}

#if WITH_DEV_AUTOMATION_TESTS
uint64 FUERayTracingAudioPartitionedConvolver::
    GetStorageFingerprintForTesting() const
{
    uint64 Fingerprint = 1469598103934665603ull;
    auto Mix = [&Fingerprint](const uint64 Value)
    {
        Fingerprint ^= Value;
        Fingerprint *= 1099511628211ull;
    };
    auto MixArray = [&Mix](const auto& Array)
    {
        Mix(static_cast<uint64>(
            reinterpret_cast<UPTRINT>(Array.GetData())));
        Mix(static_cast<uint64>(Array.Num()));
        Mix(static_cast<uint64>(Array.Max()));
        Mix(static_cast<uint64>(Array.GetAllocatedSize()));
    };

    Mix(static_cast<uint64>(
        reinterpret_cast<UPTRINT>(Kernel.Get())));
    MixArray(InputHistory);
    for (const TArray<FUERayTracingAudioComplexSample>& History
        : InputHistory)
    {
        MixArray(History);
    }
    MixArray(FrequencyOutput);
    MixArray(InputBlock);
    MixArray(OutputBlock);
    MixArray(Overlap);
    return Fingerprint;
}
#endif

void FUERayTracingAudioPartitionedConvolver::ProcessBlock()
{
    const int32 BlockSize = Kernel->BlockSize;
    const int32 TransformSize = BlockSize * 2;
    TArray<FUERayTracingAudioComplexSample>& CurrentInput = InputHistory[HistoryWriteIndex];
    for (int32 Index = 0; Index < TransformSize; ++Index)
    {
        CurrentInput[Index] = FUERayTracingAudioComplexSample();
        FrequencyOutput[Index] = FUERayTracingAudioComplexSample();
    }
    for (int32 Index = 0; Index < BlockSize; ++Index)
    {
        CurrentInput[Index].Real = InputBlock[Index];
    }
    Transform(CurrentInput, false);

    for (int32 PartitionIndex = 0; PartitionIndex < Kernel->FrequencyPartitions.Num(); ++PartitionIndex)
    {
        int32 HistoryIndex = HistoryWriteIndex - PartitionIndex;
        if (HistoryIndex < 0)
        {
            HistoryIndex += InputHistory.Num();
        }
        const TArray<FUERayTracingAudioComplexSample>& History = InputHistory[HistoryIndex];
        const TArray<FUERayTracingAudioComplexSample>& Impulse = Kernel->FrequencyPartitions[PartitionIndex];
        for (int32 BinIndex = 0; BinIndex < TransformSize; ++BinIndex)
        {
            FrequencyOutput[BinIndex] += History[BinIndex] * Impulse[BinIndex];
        }
    }
    Transform(FrequencyOutput, true);

    for (int32 Index = 0; Index < BlockSize; ++Index)
    {
        OutputBlock[Index] = FrequencyOutput[Index].Real + Overlap[Index];
        Overlap[Index] = FrequencyOutput[Index + BlockSize].Real;
        InputBlock[Index] = 0.0f;
    }
    InputFill = 0;
    OutputReadIndex = 0;
    HistoryWriteIndex = (HistoryWriteIndex + 1) % InputHistory.Num();
}

void FUERayTracingAudioPreparedConvolverState::Prepare(
    FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel,
    const uint64 InRevision,
    const uint64 InOwnerKey)
{
    // A recycled state may target the same immutable kernel but still contain
    // history from an older voice. Force all cleanup/reinitialization here on
    // the control path; the audio path never calls SetKernel or Reset.
    Convolver.SetKernel(nullptr);
    Convolver.SetKernel(MoveTemp(InKernel));
    Revision = InRevision;
    OwnerKey = InOwnerKey;
}

float FUERayTracingAudioPreparedConvolverState::ProcessSample(
    const float InputSample)
{
    return Convolver.ProcessSample(InputSample);
}

bool FUERayTracingAudioPreparedConvolverState::HasOutput() const
{
    return Convolver.HasOutput();
}

int32 FUERayTracingAudioPreparedConvolverState::GetBlockSize() const
{
    return Convolver.GetBlockSize();
}

uint64 FUERayTracingAudioPreparedConvolverState::GetRevision() const
{
    return Revision;
}

uint64 FUERayTracingAudioPreparedConvolverState::GetOwnerKey() const
{
    return OwnerKey;
}

uint64 FUERayTracingAudioPreparedConvolverState::GetAllocatedBytes() const
{
    return Convolver.GetAllocatedBytes();
}

#if WITH_DEV_AUTOMATION_TESTS
uint64 FUERayTracingAudioPreparedConvolverState::
    GetStorageFingerprintForTesting() const
{
    return Convolver.GetStorageFingerprintForTesting();
}
#endif

bool FUERayTracingAudioPreparedCrossfadingConvolver::
    CanAcceptTransition() const
{
    return WarmupSamplesRemaining == 0
        && CrossfadeSamplesRemaining == 0
        && Previous == nullptr
        && Retired == nullptr;
}

bool FUERayTracingAudioPreparedCrossfadingConvolver::StartTransition(
    FUERayTracingAudioPreparedConvolverState* InState,
    const uint64 InRevision,
    const uint64 InOwnerKey,
    const int32 InCrossfadeSamples)
{
    if (!CanAcceptTransition()
        || (InState == Current
            && InRevision == CurrentRevision
            && InOwnerKey == CurrentOwnerKey)
        || (InState != nullptr && !InState->HasOutput()))
    {
        return false;
    }

    Previous = Current;
    Current = InState;
    CurrentRevision = InRevision;
    CurrentOwnerKey = InOwnerKey;
    CrossfadeSamples = FMath::Max(InCrossfadeSamples, 1);
    CrossfadeSamplesRemaining = CrossfadeSamples;
    WarmupSamplesRemaining =
        Current ? Current->GetBlockSize() : 0;
    return true;
}

bool FUERayTracingAudioPreparedCrossfadingConvolver::AdoptPreparedState(
    FUERayTracingAudioPreparedConvolverState* InState,
    const int32 InCrossfadeSamples)
{
    return InState
        && StartTransition(
            InState,
            InState->GetRevision(),
            InState->GetOwnerKey(),
            InCrossfadeSamples);
}

bool FUERayTracingAudioPreparedCrossfadingConvolver::AdoptSilence(
    const uint64 InRevision,
    const uint64 InOwnerKey,
    const int32 InCrossfadeSamples)
{
    return StartTransition(
        nullptr,
        InRevision,
        InOwnerKey,
        InCrossfadeSamples);
}

float FUERayTracingAudioPreparedCrossfadingConvolver::ProcessSample(
    const float InputSample)
{
    const float CurrentOutput =
        Current ? Current->ProcessSample(InputSample) : 0.0f;
    const float PreviousOutput =
        Previous ? Previous->ProcessSample(InputSample) : 0.0f;

    if (WarmupSamplesRemaining > 0)
    {
        --WarmupSamplesRemaining;
        return PreviousOutput;
    }

    if (CrossfadeSamplesRemaining > 0)
    {
        const float Alpha = 1.0f
            - (static_cast<float>(CrossfadeSamplesRemaining)
                / static_cast<float>(CrossfadeSamples));
        --CrossfadeSamplesRemaining;
        const float Output =
            FMath::Lerp(PreviousOutput, CurrentOutput, Alpha);
        if (CrossfadeSamplesRemaining == 0 && Previous)
        {
            // CanAcceptTransition prevents a second transition while an old
            // lease awaits collection, so this assignment is bounded and
            // cannot overwrite an unreturned state.
            Retired = Previous;
            Previous = nullptr;
        }
        return Output;
    }

    return CurrentOutput;
}

FUERayTracingAudioPreparedConvolverState*
FUERayTracingAudioPreparedCrossfadingConvolver::PeekRetiredState() const
{
    return Retired;
}

FUERayTracingAudioPreparedConvolverState*
FUERayTracingAudioPreparedCrossfadingConvolver::TakeRetiredState()
{
    FUERayTracingAudioPreparedConvolverState* Result = Retired;
    Retired = nullptr;
    return Result;
}

int32 FUERayTracingAudioPreparedCrossfadingConvolver::DetachAllStates(
    FUERayTracingAudioPreparedConvolverState** OutStates,
    const int32 MaxOutStates)
{
    FUERayTracingAudioPreparedConvolverState* UniqueStates[3] =
    {
        nullptr,
        nullptr,
        nullptr
    };
    int32 NumUniqueStates = 0;
    auto AddUnique =
        [&UniqueStates, &NumUniqueStates](
            FUERayTracingAudioPreparedConvolverState* State)
        {
            if (!State)
            {
                return;
            }
            for (int32 Index = 0; Index < NumUniqueStates; ++Index)
            {
                if (UniqueStates[Index] == State)
                {
                    return;
                }
            }
            UniqueStates[NumUniqueStates++] = State;
        };
    AddUnique(Current);
    AddUnique(Previous);
    AddUnique(Retired);
    if (!OutStates || MaxOutStates < NumUniqueStates)
    {
        return 0;
    }

    for (int32 Index = 0; Index < NumUniqueStates; ++Index)
    {
        OutStates[Index] = UniqueStates[Index];
    }
    Current = nullptr;
    Previous = nullptr;
    Retired = nullptr;
    CurrentRevision = 0;
    CurrentOwnerKey = 0;
    CrossfadeSamples = 0;
    CrossfadeSamplesRemaining = 0;
    WarmupSamplesRemaining = 0;
    return NumUniqueStates;
}

bool FUERayTracingAudioPreparedCrossfadingConvolver::HasOutput() const
{
    return Current != nullptr || Previous != nullptr;
}

uint64 FUERayTracingAudioPreparedCrossfadingConvolver::
    GetCurrentRevision() const
{
    return CurrentRevision;
}

uint64 FUERayTracingAudioPreparedCrossfadingConvolver::
    GetCurrentOwnerKey() const
{
    return CurrentOwnerKey;
}

void FUERayTracingAudioCrossfadingConvolver::SetKernel(
    FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel,
    const int32 InCrossfadeSamples)
{
    if (CurrentKernel == InKernel)
    {
        // SetKernel is called for every audio buffer. If the producer returns
        // to the active kernel before a queued update starts, the active
        // kernel is once again the latest requested state.
        PendingKernel.Reset();
        PendingCrossfadeSamples = 0;
        bHasPendingKernel = false;
        return;
    }

    const int32 RequestedCrossfadeSamples = FMath::Max(
        InCrossfadeSamples,
        1);
    if (WarmupSamplesRemaining > 0 || CrossfadeSamplesRemaining > 0)
    {
        // Realtime ray tracing can publish a new IR faster than one
        // partition can warm up. Replacing Current here would repeatedly
        // discard its history before it produces a sample. Keep one bounded,
        // last-wins request instead; the audible Current/Previous pair keeps
        // processing the source and retaining its tail.
        PendingKernel = MoveTemp(InKernel);
        PendingCrossfadeSamples = RequestedCrossfadeSamples;
        bHasPendingKernel = true;
        return;
    }

    StartTransition(
        MoveTemp(InKernel),
        RequestedCrossfadeSamples);
}

void FUERayTracingAudioCrossfadingConvolver::StartTransition(
    FUERayTracingAudioConvolutionKernel::FKernelPtr InKernel,
    const int32 InCrossfadeSamples)
{
    PreviousKernel = MoveTemp(CurrentKernel);
    Previous = MoveTemp(Current);
    Current = FUERayTracingAudioPartitionedConvolver();
    CurrentKernel = MoveTemp(InKernel);
    Current.SetKernel(CurrentKernel);

    CrossfadeSamples = FMath::Max(InCrossfadeSamples, 1);
    CrossfadeSamplesRemaining = CrossfadeSamples;
    WarmupSamplesRemaining = CurrentKernel.IsValid() ? CurrentKernel->GetBlockSize() : 0;
}

void FUERayTracingAudioCrossfadingConvolver::StartPendingTransition()
{
    if (!bHasPendingKernel)
    {
        return;
    }

    FUERayTracingAudioConvolutionKernel::FKernelPtr NextKernel =
        MoveTemp(PendingKernel);
    const int32 NextCrossfadeSamples = FMath::Max(
        PendingCrossfadeSamples,
        1);
    PendingCrossfadeSamples = 0;
    bHasPendingKernel = false;
    if (CurrentKernel != NextKernel)
    {
        StartTransition(
            MoveTemp(NextKernel),
            NextCrossfadeSamples);
    }
}

float FUERayTracingAudioCrossfadingConvolver::ProcessSample(const float InputSample)
{
    const float CurrentOutput = Current.ProcessSample(InputSample);
    const float PreviousOutput = Previous.ProcessSample(InputSample);

    if (WarmupSamplesRemaining > 0)
    {
        --WarmupSamplesRemaining;
        return PreviousKernel.IsValid() ? PreviousOutput : 0.0f;
    }

    if (CrossfadeSamplesRemaining > 0)
    {
        const float Alpha = 1.0f
            - (static_cast<float>(CrossfadeSamplesRemaining) / static_cast<float>(CrossfadeSamples));
        --CrossfadeSamplesRemaining;
        const float Output = FMath::Lerp(PreviousOutput, CurrentOutput, Alpha);
        if (CrossfadeSamplesRemaining == 0)
        {
            Previous.SetKernel(nullptr);
            PreviousKernel.Reset();
            StartPendingTransition();
        }
        return Output;
    }

    return CurrentOutput;
}

void FUERayTracingAudioCrossfadingConvolver::Reset()
{
    CurrentKernel.Reset();
    PreviousKernel.Reset();
    PendingKernel.Reset();
    Current.SetKernel(nullptr);
    Previous.SetKernel(nullptr);
    CrossfadeSamples = 0;
    CrossfadeSamplesRemaining = 0;
    WarmupSamplesRemaining = 0;
    PendingCrossfadeSamples = 0;
    bHasPendingKernel = false;
}

bool FUERayTracingAudioCrossfadingConvolver::HasOutput() const
{
    return CurrentKernel.IsValid()
        || PreviousKernel.IsValid()
        || (bHasPendingKernel && PendingKernel.IsValid());
}
