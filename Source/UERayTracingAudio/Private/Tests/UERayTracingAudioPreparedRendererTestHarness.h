#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "Audio/UERayTracingAudioIndirectAudioBridge.h"
#include "Audio/UERayTracingAudioIndirectRenderer.h"
#include "Audio/UERayTracingAudioSimulationSnapshot.h"

// Drives the same prepared-state handoff used by an audio device without
// reintroducing owning convolution state into the renderer. Tests call
// Configure on their simulated audio callback; this harness performs the
// intervening game-thread preparation synchronously and then lets the second
// callback-facing ConfigurePrepared call adopt the result.
class FUERayTracingAudioPreparedRendererTestHarness
{
public:
    explicit FUERayTracingAudioPreparedRendererTestHarness(
        const int32 SampleRate)
    {
        Bridge.Initialize(
            1,
            1,
            SampleRate);
    }

    ~FUERayTracingAudioPreparedRendererTestHarness()
    {
        ReleaseBoundRenderer();
        Bridge.RemoveConvolutionTargets(AudioComponentId);
        Bridge.ServiceConvolutionGameThread(MAX_int32);
    }

    FUERayTracingAudioPreparedRendererTestHarness(
        const FUERayTracingAudioPreparedRendererTestHarness&) = delete;
    FUERayTracingAudioPreparedRendererTestHarness& operator=(
        const FUERayTracingAudioPreparedRendererTestHarness&) = delete;

    void Configure(
        FUERayTracingAudioIndirectRenderer& Renderer,
        const FUERayTracingAudioSimulationSnapshot* Snapshot)
    {
        if (BoundRenderer != &Renderer)
        {
            ReleaseBoundRenderer();
            BoundRenderer = &Renderer;
        }

        if (!Snapshot)
        {
            UpdateKernelRevisions(nullptr);
            Bridge.RemoveConvolutionTargets(AudioComponentId);
            Renderer.ConfigurePrepared(
                Bridge,
                SourceId,
                AudioComponentId,
                nullptr);
            Bridge.ServiceConvolutionGameThread(MAX_int32);
            Renderer.ConfigurePrepared(
                Bridge,
                SourceId,
                AudioComponentId,
                nullptr);
            return;
        }

        FUERayTracingAudioSimulationSnapshot PreparedSnapshot =
            *Snapshot;
        UpdateKernelRevisions(Snapshot);
        PreparedSnapshot.ConvolutionRevisions = Revisions;

        Bridge.PublishConvolutionTargets(
            AudioComponentId,
            Revisions,
            Snapshot->BakedConvolutionKernel,
            Snapshot->BakedConvolutionKernelRight,
            Snapshot->RealtimeConvolutionKernelLeft,
            Snapshot->RealtimeConvolutionKernelRight);
        Renderer.ConfigurePrepared(
            Bridge,
            SourceId,
            AudioComponentId,
            &PreparedSnapshot);
        Bridge.ServiceConvolutionGameThread(MAX_int32);
        Renderer.ConfigurePrepared(
            Bridge,
            SourceId,
            AudioComponentId,
            &PreparedSnapshot);
    }

private:
    using FKernelPtr =
        FUERayTracingAudioConvolutionKernel::FKernelPtr;

    static void UpdateLaneRevision(
        const FKernelPtr& Kernel,
        const FUERayTracingAudioConvolutionKernel*&
            PreviousIdentity,
        uint64& Revision)
    {
        if (PreviousIdentity == Kernel.Get())
        {
            return;
        }

        PreviousIdentity = Kernel.Get();
        ++Revision;
        if (Revision == 0)
        {
            ++Revision;
        }
    }

    void UpdateKernelRevisions(
        const FUERayTracingAudioSimulationSnapshot* Snapshot)
    {
        const FKernelPtr EmptyKernel;
        const FKernelPtr& BakedLeft = Snapshot
            ? Snapshot->BakedConvolutionKernel
            : EmptyKernel;
        const FKernelPtr& BakedRight =
            Snapshot
            && Snapshot->BakedConvolutionKernelRight.IsValid()
                ? Snapshot->BakedConvolutionKernelRight
                : BakedLeft;
        const FKernelPtr& RealtimeLeft = Snapshot
            ? Snapshot->RealtimeConvolutionKernelLeft
            : EmptyKernel;
        const FKernelPtr& RealtimeRight =
            Snapshot
            && Snapshot->RealtimeConvolutionKernelRight.IsValid()
                ? Snapshot->RealtimeConvolutionKernelRight
                : RealtimeLeft;

        UpdateLaneRevision(
            BakedLeft,
            BakedLeftIdentity,
            Revisions.BakedLeft);
        UpdateLaneRevision(
            BakedRight,
            BakedRightIdentity,
            Revisions.BakedRight);
        UpdateLaneRevision(
            RealtimeLeft,
            RealtimeLeftIdentity,
            Revisions.RealtimeLeft);
        UpdateLaneRevision(
            RealtimeRight,
            RealtimeRightIdentity,
            Revisions.RealtimeRight);
    }

    void ReleaseBoundRenderer()
    {
        if (!BoundRenderer)
        {
            return;
        }

        BoundRenderer->ReleasePreparedStates(
            Bridge,
            SourceId);
        Bridge.ServiceConvolutionGameThread(MAX_int32);
        BoundRenderer = nullptr;
    }

    static constexpr int32 SourceId = 0;
    static constexpr uint64 AudioComponentId =
        0xA170'0000'0000'0001ULL;

    FUERayTracingAudioIndirectAudioBridge Bridge;
    FUERayTracingAudioIndirectRenderer* BoundRenderer = nullptr;
    FUERayTracingAudioConvolutionRevisions Revisions;
    const FUERayTracingAudioConvolutionKernel*
        BakedLeftIdentity = nullptr;
    const FUERayTracingAudioConvolutionKernel*
        BakedRightIdentity = nullptr;
    const FUERayTracingAudioConvolutionKernel*
        RealtimeLeftIdentity = nullptr;
    const FUERayTracingAudioConvolutionKernel*
        RealtimeRightIdentity = nullptr;
};

#endif
