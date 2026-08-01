#include "Audio/UERayTracingAudioIndirectRenderer.h"

#include "Audio/UERayTracingAudioAudioDiagnostics.h"
#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"

void FUERayTracingAudioEarlyReflectionRenderer::
    ReleasePreparedStates(
        FUERayTracingAudioIndirectAudioBridge& Bridge,
        const int32 SourceId)
{
    Bridge.ReleaseConvolver(
        SourceId,
        EUERayTracingAudioConvolutionLane::BakedLeft,
        BakedLeftConvolver);
    Bridge.ReleaseConvolver(
        SourceId,
        EUERayTracingAudioConvolutionLane::BakedRight,
        BakedRightConvolver);
    Bridge.ReleaseConvolver(
        SourceId,
        EUERayTracingAudioConvolutionLane::RealtimeLeft,
        RealtimeLeftConvolver);
    Bridge.ReleaseConvolver(
        SourceId,
        EUERayTracingAudioConvolutionLane::RealtimeRight,
        RealtimeRightConvolver);
}

void FUERayTracingAudioEarlyReflectionRenderer::
    ConfigurePrepared(
        FUERayTracingAudioIndirectAudioBridge& Bridge,
        const int32 SourceId,
        const uint64 AudioComponentId,
        const FUERayTracingAudioSimulationSnapshot* Snapshot,
        const int32 SampleRate)
{
    const bool bLegacyUniformRevision =
        Snapshot
        && Snapshot->ConvolutionRevision != 0
        && Snapshot->ConvolutionRevisions.BakedLeft == 0
        && Snapshot->ConvolutionRevisions.BakedRight == 0
        && Snapshot->ConvolutionRevisions.RealtimeLeft == 0
        && Snapshot->ConvolutionRevisions.RealtimeRight == 0;
    const auto ResolveRevision = [
        Snapshot,
        bLegacyUniformRevision](
        const uint64 LaneRevision)
    {
        if (!Snapshot)
        {
            return static_cast<uint64>(0);
        }
        return bLegacyUniformRevision
            ? Snapshot->ConvolutionRevision
            : LaneRevision;
    };
    const bool bHasBakedLeft =
        Snapshot
        && Snapshot->BakedConvolutionKernel.IsValid()
        && Snapshot->BakedConvolutionKernel->GetSampleRate()
            == SampleRate;
    const bool bHasDedicatedBakedRight =
        Snapshot
        && Snapshot->BakedConvolutionKernelRight.IsValid()
        && Snapshot->BakedConvolutionKernelRight->
            GetSampleRate() == SampleRate;
    const bool bHasBakedRight =
        bHasDedicatedBakedRight || bHasBakedLeft;
    const bool bHasRealtimeLeft =
        Snapshot
        && Snapshot->RealtimeConvolutionKernelLeft.IsValid()
        && Snapshot->RealtimeConvolutionKernelLeft->
            GetSampleRate() == SampleRate;
    const bool bHasDedicatedRealtimeRight =
        Snapshot
        && Snapshot->RealtimeConvolutionKernelRight.IsValid()
        && Snapshot->RealtimeConvolutionKernelRight->
            GetSampleRate() == SampleRate;
    const bool bHasRealtimeRight =
        bHasDedicatedRealtimeRight || bHasRealtimeLeft;
    const uint64 BakedLeftRevision = ResolveRevision(
        Snapshot
            ? Snapshot->ConvolutionRevisions.BakedLeft
            : 0);
    uint64 BakedRightRevision = ResolveRevision(
        Snapshot
            ? Snapshot->ConvolutionRevisions.BakedRight
            : 0);
    if (BakedRightRevision == 0
        && !bHasDedicatedBakedRight)
    {
        BakedRightRevision = BakedLeftRevision;
    }
    const uint64 RealtimeLeftRevision = ResolveRevision(
        Snapshot
            ? Snapshot->ConvolutionRevisions.RealtimeLeft
            : 0);
    uint64 RealtimeRightRevision = ResolveRevision(
        Snapshot
            ? Snapshot->ConvolutionRevisions.RealtimeRight
            : 0);
    if (RealtimeRightRevision == 0
        && !bHasDedicatedRealtimeRight)
    {
        RealtimeRightRevision = RealtimeLeftRevision;
    }

    Bridge.ConfigureConvolver(
        SourceId,
        AudioComponentId,
        BakedLeftRevision,
        bHasBakedLeft,
        EUERayTracingAudioConvolutionLane::BakedLeft,
        BakedLeftConvolver);
    Bridge.ConfigureConvolver(
        SourceId,
        AudioComponentId,
        BakedRightRevision,
        bHasBakedRight,
        EUERayTracingAudioConvolutionLane::BakedRight,
        BakedRightConvolver);
    Bridge.ConfigureConvolver(
        SourceId,
        AudioComponentId,
        RealtimeLeftRevision,
        bHasRealtimeLeft,
        EUERayTracingAudioConvolutionLane::RealtimeLeft,
        RealtimeLeftConvolver);
    Bridge.ConfigureConvolver(
        SourceId,
        AudioComponentId,
        RealtimeRightRevision,
        bHasRealtimeRight,
        EUERayTracingAudioConvolutionLane::RealtimeRight,
        RealtimeRightConvolver);
}

FVector2f FUERayTracingAudioEarlyReflectionRenderer::ProcessSample(const float MonoInput)
{
    return FVector2f(
        BakedLeftConvolver.ProcessSample(MonoInput)
            + RealtimeLeftConvolver.ProcessSample(MonoInput),
        BakedRightConvolver.ProcessSample(MonoInput)
            + RealtimeRightConvolver.ProcessSample(MonoInput));
}

bool FUERayTracingAudioEarlyReflectionRenderer::HasOutput() const
{
    return BakedLeftConvolver.HasOutput()
        || BakedRightConvolver.HasOutput()
        || RealtimeLeftConvolver.HasOutput()
        || RealtimeRightConvolver.HasOutput();
}

void FUERayTracingAudioLateReverbRenderer::Initialize(
    const int32 InSampleRate,
    const float MaxPreDelaySeconds)
{
    const int32 PreparedSampleRate = FMath::Max(InSampleRate, 8000);
    const float SanitizedMaxPreDelaySeconds =
        FMath::IsFinite(MaxPreDelaySeconds)
        ? FMath::Max(MaxPreDelaySeconds, 0.0f)
        : 0.0f;
    const int32 RequiredDelaySamples = FMath::Max(
        512,
        FMath::CeilToInt(
            SanitizedMaxPreDelaySeconds
            * static_cast<float>(PreparedSampleRate))
            + 256);
    const int32 CombSizes[] =
    {
        FMath::Max(
            64,
            FMath::RoundToInt(
                static_cast<float>(PreparedSampleRate) * 0.031f)),
        FMath::Max(
            64,
            FMath::RoundToInt(
                static_cast<float>(PreparedSampleRate) * 0.047f)),
        FMath::Max(
            64,
            FMath::RoundToInt(
                static_cast<float>(PreparedSampleRate) * 0.071f))
    };

    const bool bAlreadyPrepared =
        bCapacityPrepared
        && SampleRate == PreparedSampleRate
        && DelayBuffer.Num() >= RequiredDelaySamples
        && CombBuffers.Num() == UE_ARRAY_COUNT(CombSizes)
        && CombBuffers[0].Num() == CombSizes[0]
        && CombBuffers[1].Num() == CombSizes[1]
        && CombBuffers[2].Num() == CombSizes[2];
    if (!bAlreadyPrepared)
    {
        SampleRate = PreparedSampleRate;
        DelayBuffer.SetNumUninitialized(RequiredDelaySamples);
        CombBuffers.SetNum(UE_ARRAY_COUNT(CombSizes));
        for (int32 CombIndex = 0;
            CombIndex < UE_ARRAY_COUNT(CombSizes);
            ++CombIndex)
        {
            CombBuffers[CombIndex].SetNumUninitialized(
                CombSizes[CombIndex]);
        }
        CapacityOverflowCount.Store(0);
        bCapacityPrepared = true;
    }
    Reset(PreparedSampleRate);
}

void FUERayTracingAudioLateReverbRenderer::Reset(const int32 InSampleRate)
{
    const int32 RequestedSampleRate = FMath::Max(InSampleRate, 8000);
    if (RequestedSampleRate != SampleRate)
    {
        // A mismatched stream configuration must be prepared by Initialize on
        // the control path. Keep the old storage owned here, but render no tail
        // rather than resizing it from an audio callback.
        SampleRate = RequestedSampleRate;
        bCapacityPrepared = false;
    }
    DelayWriteIndex = 0;
    DelaySamplesWritten = 0;
    for (int32 CombIndex = 0; CombIndex < UE_ARRAY_COUNT(CombWriteIndices); ++CombIndex)
    {
        CombWriteIndices[CombIndex] = 0;
        CombSamplesWritten[CombIndex] = 0;
    }
    bCapacityOverflowActive = false;
    bHasCurrentPaths = false;
    bUseCurrentParametricTail = false;
    CurrentLateReverbGain = 0.0f;
    CurrentParametricDelaySeconds = 0.0f;
    CurrentReverbTimes = FVector::ZeroVector;
    CurrentParametricEq = FVector::OneVector;
    ClearCachedState(false);
}

float FUERayTracingAudioLateReverbRenderer::ReadDelayedSample(const int32 DelaySamples) const
{
    if (DelayBuffer.IsEmpty()
        || DelaySamples <= 0
        || DelaySamples >= DelaySamplesWritten)
    {
        return 0.0f;
    }

    const int32 ClampedDelaySamples = FMath::Clamp(DelaySamples, 1, DelayBuffer.Num() - 1);
    int32 ReadIndex = DelayWriteIndex - ClampedDelaySamples;
    if (ReadIndex < 0)
    {
        ReadIndex += DelayBuffer.Num() * (1 + (-ReadIndex / DelayBuffer.Num()));
    }
    return DelayBuffer[ReadIndex % DelayBuffer.Num()];
}

void FUERayTracingAudioLateReverbRenderer::ClearCachedState(const bool bClearCombBuffers)
{
    bHasCachedReverbState = false;
    CachedLateReverbGain = 0.0f;
    CachedParametricDelaySeconds = 0.0f;
    CachedReverbTimes = FVector::ZeroVector;
    CachedParametricEq = FVector::OneVector;
    CachedReverbTailSamplesRemaining = 0;
    if (bClearCombBuffers)
    {
        for (int32 CombIndex = 0;
            CombIndex < UE_ARRAY_COUNT(CombWriteIndices);
            ++CombIndex)
        {
            CombWriteIndices[CombIndex] = 0;
            CombSamplesWritten[CombIndex] = 0;
        }
    }
}

void FUERayTracingAudioLateReverbRenderer::Configure(
    const FUERayTracingAudioIndirectSimulationResult* IndirectResult,
    const float DurationSeconds)
{
    if (IndirectResult)
    {
        bHasCurrentPaths = IndirectResult->bHasValidPaths;
        const bool bRequestedParametricTail =
            bHasCurrentPaths
            && IndirectResult->bUsedParametricTail;
        const float RequestedPreDelaySeconds =
            IndirectResult->ParametricDelaySeconds;
        const bool bHasValidPreDelay =
            FMath::IsFinite(RequestedPreDelaySeconds)
            && RequestedPreDelaySeconds >= 0.0f;
        const double RequestedPreDelaySamplesExact =
            bHasValidPreDelay
            ? static_cast<double>(RequestedPreDelaySeconds)
                * static_cast<double>(SampleRate)
            : -1.0;
        const bool bHasPreparedCapacity =
            bCapacityPrepared
            && DelayBuffer.Num() > 1
            && bHasValidPreDelay
            && RequestedPreDelaySamplesExact
                <= static_cast<double>(DelayBuffer.Num() - 1)
            && CombBuffers.Num() == UE_ARRAY_COUNT(CombWriteIndices);
        const bool bCapacityOverflow =
            bRequestedParametricTail
            && !bHasPreparedCapacity;
        if (bCapacityOverflow && !bCapacityOverflowActive)
        {
            ++CapacityOverflowCount;
            FUERayTracingAudioAudioDiagnostics::
                RecordHardRealtimeCapacityMiss();
        }
        bCapacityOverflowActive = bCapacityOverflow;
        bUseCurrentParametricTail =
            bRequestedParametricTail
            && bHasPreparedCapacity;
        CurrentLateReverbGain =
            FMath::IsFinite(IndirectResult->LateReverbGain)
            ? FMath::Max(IndirectResult->LateReverbGain, 0.0f)
            : 0.0f;
        CurrentParametricDelaySeconds = bHasPreparedCapacity
            ? RequestedPreDelaySeconds
            : 0.0f;
        CurrentReverbTimes = FVector(
            FMath::IsFinite(IndirectResult->ReverbTimes.X)
                ? FMath::Max(IndirectResult->ReverbTimes.X, 0.0f)
                : 0.0f,
            FMath::IsFinite(IndirectResult->ReverbTimes.Y)
                ? FMath::Max(IndirectResult->ReverbTimes.Y, 0.0f)
                : 0.0f,
            FMath::IsFinite(IndirectResult->ReverbTimes.Z)
                ? FMath::Max(IndirectResult->ReverbTimes.Z, 0.0f)
                : 0.0f);
        CurrentParametricEq = FVector(
            FMath::IsFinite(IndirectResult->ParametricEq.X)
                ? IndirectResult->ParametricEq.X
                : 0.0f,
            FMath::IsFinite(IndirectResult->ParametricEq.Y)
                ? IndirectResult->ParametricEq.Y
                : 0.0f,
            FMath::IsFinite(IndirectResult->ParametricEq.Z)
                ? IndirectResult->ParametricEq.Z
                : 0.0f);

        if (bHasCurrentPaths && bUseCurrentParametricTail)
        {
            bHasCachedReverbState = true;
            CachedLateReverbGain = CurrentLateReverbGain;
            CachedParametricDelaySeconds = CurrentParametricDelaySeconds;
            CachedReverbTimes = CurrentReverbTimes;
            CachedParametricEq = CurrentParametricEq;
            const float MaxReverbTimeSeconds = FMath::Max3(
                CurrentReverbTimes.X,
                CurrentReverbTimes.Y,
                CurrentReverbTimes.Z);
            CachedReverbTailSamplesRemaining = FMath::Clamp(
                FMath::CeilToInt(FMath::Max(MaxReverbTimeSeconds * 1.25f, 0.1f)
                    * static_cast<float>(SampleRate)),
                1,
                SampleRate * 30);
        }
        else if (bHasCurrentPaths)
        {
            // A switch to pure convolution must not leave an inaudible old comb
            // state ready to reappear when parametric rendering is enabled again.
            ClearCachedState(true);
        }
        (void)DurationSeconds;
        return;
    }

    bHasCurrentPaths = false;
    bUseCurrentParametricTail = false;
    bCapacityOverflowActive = false;
    CurrentLateReverbGain = 0.0f;
    CurrentParametricDelaySeconds = 0.0f;
    CurrentReverbTimes = FVector::ZeroVector;
    CurrentParametricEq = FVector::OneVector;
}

float FUERayTracingAudioLateReverbRenderer::ProcessSample(const float MonoInput)
{
    if (!bCapacityPrepared || DelayBuffer.IsEmpty())
    {
        return 0.0f;
    }

    const float SafeInput = FMath::IsFinite(MonoInput)
        ? MonoInput
        : 0.0f;
    DelayBuffer[DelayWriteIndex] = SafeInput;
    DelaySamplesWritten = FMath::Min(
        DelaySamplesWritten + 1,
        DelayBuffer.Num());
    const bool bCanRenderLate = (bHasCurrentPaths && bUseCurrentParametricTail)
        || (!bHasCurrentPaths && bHasCachedReverbState);
    float LateWet = 0.0f;
    if (bCanRenderLate && CombBuffers.Num() == 3)
    {
        const bool bUseCurrentState = bHasCurrentPaths && bUseCurrentParametricTail;
        const float EffectiveLateReverbEnergy = bUseCurrentState
            ? CurrentLateReverbGain
            : CachedLateReverbGain;
        // The simulator publishes accumulated reflected energy. Convert it to
        // linear amplitude at the renderer boundary; using energy directly as
        // an amplitude applied the attenuation twice for low-energy tails.
        const float EffectiveLateReverbAmplitude = FMath::Sqrt(
            FMath::Max(EffectiveLateReverbEnergy, 0.0f));
        const float EffectivePreDelay = bUseCurrentState
            ? CurrentParametricDelaySeconds
            : CachedParametricDelaySeconds;
        const FVector EffectiveReverbTimes = bUseCurrentState
            ? CurrentReverbTimes
            : CachedReverbTimes;
        const FVector EffectiveEq = bUseCurrentState
            ? CurrentParametricEq
            : CachedParametricEq;
        const int32 PreDelaySamples = FMath::Clamp(
            FMath::RoundToInt(EffectivePreDelay * static_cast<float>(SampleRate)),
            1,
            DelayBuffer.Num() - 1);
        const float PreDelayedInput = bUseCurrentState ? ReadDelayedSample(PreDelaySamples) : 0.0f;

        for (int32 CombIndex = 0; CombIndex < CombBuffers.Num(); ++CombIndex)
        {
            TArray<float>& CombBuffer = CombBuffers[CombIndex];
            int32& WriteIndex = CombWriteIndices[CombIndex];
            const float BandRT60 = FMath::Max(EffectiveReverbTimes[CombIndex], 0.1f);
            const float BandEq = EffectiveEq[CombIndex];
            const float DelaySeconds = static_cast<float>(CombBuffer.Num()) / static_cast<float>(SampleRate);
            const float Feedback = FMath::Clamp(
                FMath::Pow(0.001f, DelaySeconds / BandRT60),
                0.0f,
                0.97f);
            const float DelayedSample =
                CombSamplesWritten[CombIndex] >= CombBuffer.Num()
                ? CombBuffer[WriteIndex]
                : 0.0f;
            CombBuffer[WriteIndex] = (PreDelayedInput * BandEq) + (DelayedSample * Feedback);
            WriteIndex = (WriteIndex + 1) % CombBuffer.Num();
            CombSamplesWritten[CombIndex] = FMath::Min(
                CombSamplesWritten[CombIndex] + 1,
                CombBuffer.Num());
            LateWet += DelayedSample * BandEq;
        }
        // The decorrelated comb bands add in energy, not coherently in
        // amplitude. Normalize by sqrt(N) so the published late-field energy
        // survives diffusion; dividing by N attenuated a three-band tail by
        // another sqrt(3), making a valid realtime Wet path nearly inaudible.
        LateWet = (
            LateWet
            / FMath::Sqrt(static_cast<float>(CombBuffers.Num())))
            * EffectiveLateReverbAmplitude;
    }

    if (!bHasCurrentPaths && bHasCachedReverbState)
    {
        --CachedReverbTailSamplesRemaining;
        if (CachedReverbTailSamplesRemaining <= 0)
        {
            ClearCachedState(true);
        }
    }

    DelayWriteIndex = (DelayWriteIndex + 1) % DelayBuffer.Num();
    return FMath::IsFinite(LateWet) ? LateWet : 0.0f;
}

bool FUERayTracingAudioLateReverbRenderer::HasOutput() const
{
    return (bHasCurrentPaths && bUseCurrentParametricTail) || bHasCachedReverbState;
}

uint64 FUERayTracingAudioLateReverbRenderer::GetCapacityOverflowCount() const
{
    return CapacityOverflowCount.Load();
}

#if WITH_DEV_AUTOMATION_TESTS
int32 FUERayTracingAudioLateReverbRenderer::GetDelayCapacityForTesting() const
{
    return DelayBuffer.Num();
}
#endif

void FUERayTracingAudioIndirectRenderer::Initialize(
    const int32 InSampleRate,
    const float MaxPreDelaySeconds)
{
    SampleRate = FMath::Max(InSampleRate, 8000);
    LateReverb.Initialize(SampleRate, MaxPreDelaySeconds);
    Reset(SampleRate);
}

void FUERayTracingAudioIndirectRenderer::Reset(const int32 InSampleRate)
{
    SampleRate = FMath::Max(InSampleRate, 8000);
    IndirectMix = 0.0f;
    IndirectMixRampStart = 0.0f;
    TargetIndirectMix = 0.0f;
    IndirectMixRampSamplesRemaining = 0;
    LateReverb.Reset(SampleRate);
}

void FUERayTracingAudioIndirectRenderer::ReleasePreparedStates(
    FUERayTracingAudioIndirectAudioBridge& Bridge,
    const int32 SourceId)
{
    EarlyReflections.ReleasePreparedStates(
        Bridge,
        SourceId);
}

void FUERayTracingAudioIndirectRenderer::ConfigurePrepared(
    FUERayTracingAudioIndirectAudioBridge& Bridge,
    const int32 SourceId,
    const uint64 AudioComponentId,
    const FUERayTracingAudioSimulationSnapshot* Snapshot)
{
    EarlyReflections.ConfigurePrepared(
        Bridge,
        SourceId,
        AudioComponentId,
        Snapshot,
        SampleRate);
    ConfigureAcousticMix(Snapshot);
}

void FUERayTracingAudioIndirectRenderer::ConfigureAcousticMix(
    const FUERayTracingAudioSimulationSnapshot* Snapshot)
{
    LateReverb.Configure(
        Snapshot ? &Snapshot->IndirectResult : nullptr,
        Snapshot ? Snapshot->IndirectDurationSeconds : 0.0f);
    const float NewTargetIndirectMix = Snapshot && FMath::IsFinite(Snapshot->IndirectMix)
        ? FMath::Clamp(Snapshot->IndirectMix, 0.0f, 4.0f)
        : 0.0f;
    if (!FMath::IsNearlyEqual(NewTargetIndirectMix, TargetIndirectMix, 1.0e-6f))
    {
        IndirectMixRampStart = IndirectMix;
        TargetIndirectMix = NewTargetIndirectMix;
        IndirectMixRampSamplesRemaining = MixCrossfadeSamples;
    }
}

FVector2f FUERayTracingAudioIndirectRenderer::ProcessSample(const float MonoInput)
{
    if (IndirectMixRampSamplesRemaining > 0)
    {
        const float Alpha = 1.0f
            - (static_cast<float>(IndirectMixRampSamplesRemaining)
                / static_cast<float>(MixCrossfadeSamples));
        IndirectMix = FMath::Lerp(IndirectMixRampStart, TargetIndirectMix, Alpha);
        --IndirectMixRampSamplesRemaining;
        if (IndirectMixRampSamplesRemaining == 0)
        {
            IndirectMix = TargetIndirectMix;
        }
    }
    const float SafeInput = FMath::IsFinite(MonoInput)
        ? MonoInput
        : 0.0f;
    const FVector2f EarlyWet = EarlyReflections.ProcessSample(SafeInput);
    const float LateWet = LateReverb.ProcessSample(SafeInput);
    const FVector2f Wet(
        (EarlyWet.X + LateWet) * IndirectMix,
        (EarlyWet.Y + LateWet) * IndirectMix);
    return FVector2f(
        FMath::IsFinite(Wet.X) ? Wet.X : 0.0f,
        FMath::IsFinite(Wet.Y) ? Wet.Y : 0.0f);
}

bool FUERayTracingAudioIndirectRenderer::HasOutput() const
{
    return (IndirectMix > 0.0f || TargetIndirectMix > 0.0f)
        && (EarlyReflections.HasOutput() || LateReverb.HasOutput());
}
