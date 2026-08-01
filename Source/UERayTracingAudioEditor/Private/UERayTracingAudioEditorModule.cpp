#include "UERayTracingAudioEditorModule.h"

#include "Audio.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Async/Async.h"
#include "Assets/UERayTracingAudioImpulseResponseAsset.h"
#include "Bake/UERayTracingAudioBakeJob.h"
#include "Bake/UERayTracingAudioOfflineRenderer.h"
#include "Bake/UERayTracingAudioEditorBakeAdmission.h"
#include "Components/AudioComponent.h"
#include "Components/UERayTracingAudioGeometryComponent.h"
#include "Components/UERayTracingAudioListenerComponent.h"
#include "Components/UERayTracingAudioSourceComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "HAL/PlatformTime.h"
#include "LevelEditor.h"
#include "Managers/UERayTracingAudioManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "Sound/SoundWave.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ScopedTransaction.h"
#include "Rendering/DrawElements.h"
#include "ToolMenus.h"
#include "UERayTracingAudioModule.h"
#include "Validation/UERayTracingAudioEditorValidationScene.h"
#include "Validation/UERayTracingAudioEditorArtifactRunner.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

static const FName UERayTracingAudioBakeTabName(TEXT("UERayTracingAudioBake"));

namespace
{
    constexpr int32 ComparisonWaveformBinCount = 512;

    int32 GetValidationReflectionBounces()
    {
        int32 ReflectionBounces = 8;
        FParse::Value(
            FCommandLine::Get(),
            TEXT("UERayTracingAudioValidationReflectionBounces="),
            ReflectionBounces);
        return FMath::Clamp(ReflectionBounces, 1, 64);
    }

    bool LoadComparisonWaveform(
        const FString& Filename,
        TArray<float>& OutPeakEnvelope,
        FString& OutError)
    {
        TArray<uint8> WaveBytes;
        if (!FFileHelper::LoadFileToArray(WaveBytes, *Filename))
        {
            OutError = FString::Printf(TEXT("could not read %s"), *Filename);
            return false;
        }

        FWaveModInfo WaveInfo;
        FString WaveError;
        if (!WaveInfo.ReadWaveInfo(WaveBytes.GetData(), WaveBytes.Num(), &WaveError)
            || WaveInfo.pFormatTag == nullptr
            || WaveInfo.pBitsPerSample == nullptr
            || WaveInfo.pChannels == nullptr
            || WaveInfo.SampleDataStart == nullptr)
        {
            OutError = FString::Printf(TEXT("invalid WAV %s (%s)"), *Filename, *WaveError);
            return false;
        }

        if (*WaveInfo.pFormatTag != FWaveModInfo::WAVE_INFO_FORMAT_PCM
            || *WaveInfo.pBitsPerSample != 16
            || *WaveInfo.pChannels == 0)
        {
            OutError = FString::Printf(
                TEXT("waveform preview requires PCM16 WAV data (%s is format %u, %u-bit, %u channels)"),
                *Filename,
                static_cast<uint32>(*WaveInfo.pFormatTag),
                static_cast<uint32>(*WaveInfo.pBitsPerSample),
                static_cast<uint32>(*WaveInfo.pChannels));
            return false;
        }

        const int32 NumChannels = static_cast<int32>(*WaveInfo.pChannels);
        const int32 FrameStrideBytes = NumChannels * static_cast<int32>(sizeof(int16));
        const int32 NumFrames = FrameStrideBytes > 0
            ? static_cast<int32>(WaveInfo.SampleDataSize) / FrameStrideBytes
            : 0;
        if (NumFrames <= 0)
        {
            OutError = FString::Printf(TEXT("WAV contains no PCM frames: %s"), *Filename);
            return false;
        }

        const int16* Samples = reinterpret_cast<const int16*>(WaveInfo.SampleDataStart);
        OutPeakEnvelope.SetNumZeroed(ComparisonWaveformBinCount);
        for (int32 BinIndex = 0; BinIndex < ComparisonWaveformBinCount; ++BinIndex)
        {
            const int32 FirstFrame = static_cast<int32>(
                static_cast<int64>(BinIndex) * NumFrames / ComparisonWaveformBinCount);
            const int32 EndFrame = FMath::Max(
                FirstFrame + 1,
                static_cast<int32>(
                    static_cast<int64>(BinIndex + 1) * NumFrames / ComparisonWaveformBinCount));

            float Peak = 0.0f;
            for (int32 FrameIndex = FirstFrame; FrameIndex < FMath::Min(EndFrame, NumFrames); ++FrameIndex)
            {
                for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
                {
                    const int32 Sample = static_cast<int32>(Samples[FrameIndex * NumChannels + ChannelIndex]);
                    Peak = FMath::Max(Peak, FMath::Abs(static_cast<float>(Sample) / 32768.0f));
                }
            }
            OutPeakEnvelope[BinIndex] = FMath::Clamp(Peak, 0.0f, 1.0f);
        }

        return true;
    }

    class SUERayTracingAudioComparisonWaveforms final : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SUERayTracingAudioComparisonWaveforms) {}
        SLATE_END_ARGS()

        void Construct(const FArguments&)
        {
        }

        void SetWaveforms(
            TArray<float>&& InReference,
            TArray<float>&& InDirect,
            TArray<float>&& InWet,
            TArray<float>&& InFull)
        {
            Waveforms[0] = MoveTemp(InReference);
            Waveforms[1] = MoveTemp(InDirect);
            Waveforms[2] = MoveTemp(InWet);
            Waveforms[3] = MoveTemp(InFull);
            Invalidate(EInvalidateWidgetReason::Paint);
        }

        void ResetWaveforms()
        {
            for (TArray<float>& Waveform : Waveforms)
            {
                Waveform.Reset();
            }
            Invalidate(EInvalidateWidgetReason::Paint);
        }

    private:
        virtual FVector2D ComputeDesiredSize(float) const override
        {
            return FVector2D(720.0f, 240.0f);
        }

        virtual int32 OnPaint(
            const FPaintArgs& Args,
            const FGeometry& AllottedGeometry,
            const FSlateRect& MyCullingRect,
            FSlateWindowElementList& OutDrawElements,
            int32 LayerId,
            const FWidgetStyle& InWidgetStyle,
            bool bParentEnabled) const override
        {
            const FVector2D Size = AllottedGeometry.GetLocalSize();
            const float LaneHeight = Size.Y / 4.0f;
            const FLinearColor Colors[4] = {
                FLinearColor(0.82f, 0.82f, 0.82f, 1.0f),
                FLinearColor(0.20f, 0.72f, 1.0f, 1.0f),
                FLinearColor(0.72f, 0.35f, 1.0f, 1.0f),
                FLinearColor(0.20f, 0.92f, 0.46f, 1.0f)
            };

            for (int32 LaneIndex = 0; LaneIndex < 4; ++LaneIndex)
            {
                const float CenterY = LaneHeight * (static_cast<float>(LaneIndex) + 0.5f);
                const TArray<FVector2D> Baseline = {
                    FVector2D(0.0f, CenterY),
                    FVector2D(Size.X, CenterY)
                };
                FSlateDrawElement::MakeLines(
                    OutDrawElements,
                    LayerId,
                    AllottedGeometry.ToPaintGeometry(),
                    Baseline,
                    ESlateDrawEffect::None,
                    FLinearColor(0.25f, 0.25f, 0.25f, 1.0f),
                    false,
                    1.0f);

                const TArray<float>& Waveform = Waveforms[LaneIndex];
                if (Waveform.Num() < 2)
                {
                    continue;
                }

                TArray<FVector2D> UpperPoints;
                TArray<FVector2D> LowerPoints;
                UpperPoints.Reserve(Waveform.Num());
                LowerPoints.Reserve(Waveform.Num());
                const float Amplitude = LaneHeight * 0.42f;
                for (int32 BinIndex = 0; BinIndex < Waveform.Num(); ++BinIndex)
                {
                    const float X = Size.X * static_cast<float>(BinIndex)
                        / static_cast<float>(Waveform.Num() - 1);
                    const float Peak = FMath::Clamp(Waveform[BinIndex], 0.0f, 1.0f) * Amplitude;
                    UpperPoints.Add(FVector2D(X, CenterY - Peak));
                    LowerPoints.Add(FVector2D(X, CenterY + Peak));
                }

                FSlateDrawElement::MakeLines(
                    OutDrawElements,
                    LayerId + 1,
                    AllottedGeometry.ToPaintGeometry(),
                    UpperPoints,
                    ESlateDrawEffect::None,
                    Colors[LaneIndex],
                    true,
                    1.25f);
                FSlateDrawElement::MakeLines(
                    OutDrawElements,
                    LayerId + 1,
                    AllottedGeometry.ToPaintGeometry(),
                    LowerPoints,
                    ESlateDrawEffect::None,
                    Colors[LaneIndex],
                    true,
                    1.25f);
            }

            return LayerId + 1;
        }

        TArray<float> Waveforms[4];
    };

    enum class EUERayTracingAudioListeningMode : uint8
    {
        None,
        Reference,
        Direct,
        Wet,
        Full
    };

    bool ConfigureComparisonSoundWaveForPreview(USoundWave* SoundWave)
    {
        if (!IsValid(SoundWave))
        {
            return false;
        }

        // Comparison assets are dedicated Editor-listening artifacts. Keep all
        // four modes running until the user switches mode or presses Stop.
        SoundWave->bLooping = true;
        return true;
    }

    USoundWave* FindRecommendedProjectSoundWave()
    {
        TArray<FAssetData> SoundWaveAssets;
        FAssetRegistryModule::GetRegistry().GetAssetsByClass(
            USoundWave::StaticClass()->GetClassPathName(),
            SoundWaveAssets,
            true);
        USoundWave* BestSoundWave = nullptr;
        float BestScore = TNumericLimits<float>::Max();
        for (const FAssetData& AssetData : SoundWaveAssets)
        {
            const FString PackageName = AssetData.PackageName.ToString();
            if (!PackageName.StartsWith(TEXT("/Game/"))
                || PackageName.StartsWith(TEXT("/Game/UERayTracingAudio/Validation")))
            {
                continue;
            }

            USoundWave* Candidate = Cast<USoundWave>(AssetData.GetAsset());
            if (!IsValid(Candidate) || Candidate->GetDuration() <= 0.0f)
            {
                continue;
            }

            const float Duration = Candidate->GetDuration();
            const float Score = FMath::Abs(Duration - 15.0f)
                + ((Duration >= 10.0f && Duration <= 20.0f) ? 0.0f : 1000.0f);
            if (Score < BestScore)
            {
                BestScore = Score;
                BestSoundWave = Candidate;
            }
        }
        return BestSoundWave;
    }

    class SUERayTracingAudioBakePanel final : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SUERayTracingAudioBakePanel) {}
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs)
        {
            InitializeValidationSelections();
            ChildSlot
            [
                SNew(SBorder)
                .Padding(16.0f)
                [
                    SNew(SScrollBox)
                    + SScrollBox::Slot()
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("Choose a real SoundWave and level actors, then bake once to create sample-aligned Reference / Direct / Wet / Full assets. All four WAVs use one recorded safety scale; no mode-specific gain compensation is applied.")))
                            .AutoWrapText(true)
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
                        [
                            SNew(SBorder)
                            .Padding(8.0f)
                            [
                                BuildListeningControls()
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
                        [
                            SNew(SButton)
                            .Text(FText::FromString(TEXT("Create / Select Actual A/B Validation Scene")))
                            .ToolTipText(FText::FromString(TEXT("Creates a visible enclosed acoustic room with Source, Listener, center occlusion wall, seven geometry components, lights, and a camera. Repeated clicks reuse the same tagged scene.")))
                            .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                            .OnClicked_Lambda([this]()
                            {
                                return CreateValidationScene(EUERayTracingAudioEditorDirectPreset::Clear);
                            })
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Clear Path (2 m)")))
                                .ToolTipText(FText::FromString(TEXT("Places Source and Listener 2 m apart on the same side of the center wall.")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return CreateValidationScene(EUERayTracingAudioEditorDirectPreset::Clear);
                                })
                            ]
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Soft Occluded (2 m)")))
                                .ToolTipText(FText::FromString(TEXT("Moves the Source behind the center wall at the same 2 m distance and preserves an explicit 0.35 acoustic direct floor.")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return CreateValidationScene(EUERayTracingAudioEditorDirectPreset::SoftOccluded);
                                })
                            ]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Hard Occluded (2 m)")))
                                .ToolTipText(FText::FromString(TEXT("Moves the Source behind the center wall at the same 2 m distance with no soft direct floor.")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return CreateValidationScene(EUERayTracingAudioEditorDirectPreset::HardOccluded);
                                })
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(TEXT("Validation Distance:")))
                            ]
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Clear 1 m")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]() { return SetValidationDistanceCm(100.0f); })
                            ]
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Clear 2 m")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]() { return SetValidationDistanceCm(200.0f); })
                            ]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Clear 4 m")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]() { return SetValidationDistanceCm(400.0f); })
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(TEXT("Validation Air Absorption:")))
                            ]
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Off")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return SetValidationAirAbsorptionProfile(
                                        EUERayTracingAudioEditorAirAbsorptionProfile::Off);
                                })
                            ]
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Default")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return SetValidationAirAbsorptionProfile(
                                        EUERayTracingAudioEditorAirAbsorptionProfile::Default);
                                })
                            ]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Stress")))
                                .IsEnabled_Lambda([this]() { return CanMutateValidationFixture(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return SetValidationAirAbsorptionProfile(
                                        EUERayTracingAudioEditorAirAbsorptionProfile::Stress);
                                })
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]()
                            {
                                return FText::FromString(
                                    GetValidationFixtureSummary());
                            })
                            .AutoWrapText(true)
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
                        [
                            BuildSceneSelectionGrid()
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("Runtime IR Data Source")))
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text_Lambda([this]()
                                {
                                    return FText::FromString(GetDataSourceButtonLabel(
                                        EUERayTracingAudioIndirectDataSource::Realtime,
                                        TEXT("Realtime IR")));
                                })
                                .ToolTipText(FText::FromString(TEXT("Uses the current hardware ray-traced impulse response.")))
                                .IsEnabled_Lambda([this]() { return SelectedSource.IsValid(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return SetRuntimeDataSource(EUERayTracingAudioIndirectDataSource::Realtime);
                                })
                            ]
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text_Lambda([this]()
                                {
                                    return FText::FromString(GetDataSourceButtonLabel(
                                        EUERayTracingAudioIndirectDataSource::Baked,
                                        TEXT("Baked IR")));
                                })
                                .ToolTipText(FText::FromString(TEXT("Uses both channels of the saved hardware-baked IR.")))
                                .IsEnabled_Lambda([this]() { return HasSelectedBakedAsset(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return SetRuntimeDataSource(EUERayTracingAudioIndirectDataSource::Baked);
                                })
                            ]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SButton)
                                .Text_Lambda([this]()
                                {
                                    return FText::FromString(GetDataSourceButtonLabel(
                                        EUERayTracingAudioIndirectDataSource::Hybrid,
                                        TEXT("Hybrid")));
                                })
                                .ToolTipText(FText::FromString(TEXT("Uses the baked early field and the realtime ray-traced tail with a complementary crossfade.")))
                                .IsEnabled_Lambda([this]() { return HasSelectedBakedAsset(); })
                                .OnClicked_Lambda([this]()
                                {
                                    return SetRuntimeDataSource(EUERayTracingAudioIndirectDataSource::Hybrid);
                                })
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]() { return FText::FromString(GetRuntimeDataSourceSummary()); })
                            .AutoWrapText(true)
                        ]
                        + SVerticalBox::Slot().AutoHeight()
                        [
                            BuildSettingsGrid()
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
                        [
                            SNew(SHorizontalBox)
                            + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Bake Selected Source")))
                                .IsEnabled_Lambda([this]() { return !IsJobRunning() && !bOfflineRenderActive; })
                                .OnClicked(this, &SUERayTracingAudioBakePanel::StartBake)
                            ]
                            + SHorizontalBox::Slot().AutoWidth()
                            [
                                SNew(SButton)
                                .Text(FText::FromString(TEXT("Cancel")))
                                .IsEnabled_Lambda([this]() { return IsJobRunning(); })
                                .OnClicked(this, &SUERayTracingAudioBakePanel::CancelBake)
                            ]
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
                        [
                            SNew(SProgressBar)
                            .Percent_Lambda([this]()
                            {
                                if (bOfflineRenderActive)
                                {
                                    return TOptional<float>();
                                }
                                return Job.IsValid() ? TOptional<float>(Job->GetProgress()) : TOptional<float>(0.0f);
                            })
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]() { return FText::FromString(GetStatusText()); })
                            .AutoWrapText(true)
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 16.0f, 0.0f, 12.0f)
                        [
                            SNew(SSeparator)
                        ]
                        + SVerticalBox::Slot().AutoHeight()
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("Detailed Comparison Metrics")))
                        ]
                        + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]() { return FText::FromString(GetListeningSummary()); })
                            .AutoWrapText(true)
                        ]
                    ]
                ]
            ];
        }

        virtual ~SUERayTracingAudioBakePanel() override
        {
            StopListeningPreviewAudioIfOwned();
        }

        virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
        {
            SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
            RefreshListeningPreviewState();
            if (bOfflineRenderActive && OfflineRenderFuture.IsReady())
            {
                bOfflineRenderActive = false;
                FinishOfflineRender(OfflineRenderFuture.Get());
            }
            if (!Job.IsValid() || bHandledTerminalState)
            {
                return;
            }

            if (Job->GetState() == EUERayTracingAudioBakeJobState::Completed)
            {
                bHandledTerminalState = true;
                SaveCompletedBake();
            }
            else if (Job->GetState() == EUERayTracingAudioBakeJobState::Failed)
            {
                bHandledTerminalState = true;
                LastStatus = FString::Printf(TEXT("Bake failed: %s"), *Job->GetError());
            }
            else if (Job->GetState() == EUERayTracingAudioBakeJobState::Cancelled)
            {
                bHandledTerminalState = true;
                LastStatus = TEXT("Bake cancelled.");
            }
        }

        bool AdoptValidationArtifactResult(
            const FUERayTracingAudioEditorArtifactResult& ArtifactResult)
        {
            if (!ArtifactResult.bSucceeded
                || ArtifactResult.ImportedComparisonAssetPaths.Num() != 4
                || !ArtifactResult.OfflineRender.bSucceeded)
            {
                return false;
            }

            auto LoadComparisonSound = [](const FString& ObjectPath) -> USoundWave*
            {
                return LoadObject<USoundWave>(nullptr, *ObjectPath);
            };

            USoundWave* Reference =
                LoadComparisonSound(ArtifactResult.ImportedComparisonAssetPaths[0]);
            USoundWave* Direct =
                LoadComparisonSound(ArtifactResult.ImportedComparisonAssetPaths[1]);
            USoundWave* Wet =
                LoadComparisonSound(ArtifactResult.ImportedComparisonAssetPaths[2]);
            USoundWave* Full =
                LoadComparisonSound(ArtifactResult.ImportedComparisonAssetPaths[3]);
            if (!IsValid(Reference)
                || !IsValid(Direct)
                || !IsValid(Wet)
                || !IsValid(Full))
            {
                return false;
            }

            StopListeningPreviewAudioIfOwned();
            ConfigureComparisonSoundWaveForPreview(Reference);
            ConfigureComparisonSoundWaveForPreview(Direct);
            ConfigureComparisonSoundWaveForPreview(Wet);
            ConfigureComparisonSoundWaveForPreview(Full);
            ComparisonReference = Reference;
            ComparisonDirect = Direct;
            ComparisonWet = Wet;
            ComparisonFull = Full;
            InputSoundWave = LoadObject<USoundWave>(
                nullptr,
                *ArtifactResult.InputAssetPath);
            LastComparisonRender = ArtifactResult.OfflineRender;
            CurrentListeningMode = EUERayTracingAudioListeningMode::None;
            LastHumanVerdict = TEXT("not recorded");
            LastStatus = FString::Printf(
                TEXT("Hardware A/B artifacts are loaded into the listening controls. Reference, Direct, Wet, and Full loop from sample zero until switched or stopped. Manifest: %s"),
                *LastComparisonRender.ManifestFilename);
            return HasComparisonAudio();
        }

    private:
        TSharedRef<SWidget> BuildSceneSelectionGrid()
        {
            return SNew(SGridPanel)
                + SGridPanel::Slot(0, 0).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Source Actor"))) ]
                + SGridPanel::Slot(1, 0).Padding(0.0f, 3.0f, 8.0f, 3.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        return FText::FromString(SelectedSource.IsValid()
                            ? SelectedSource->GetOwner()->GetActorLabel()
                            : TEXT("Not selected"));
                    })
                ]
                + SGridPanel::Slot(2, 0).Padding(0.0f, 3.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Use Selected Actor")))
                    .OnClicked(this, &SUERayTracingAudioBakePanel::UseSelectedSource)
                ]
                + SGridPanel::Slot(0, 1).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Listener Actor"))) ]
                + SGridPanel::Slot(1, 1).Padding(0.0f, 3.0f, 8.0f, 3.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        return FText::FromString(SelectedListener.IsValid()
                            ? SelectedListener->GetOwner()->GetActorLabel()
                            : TEXT("Not selected"));
                    })
                ]
                + SGridPanel::Slot(2, 1).Padding(0.0f, 3.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("Use Selected Actor")))
                    .OnClicked(this, &SUERayTracingAudioBakePanel::UseSelectedListener)
                ]
                + SGridPanel::Slot(0, 2).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Geometry"))) ]
                + SGridPanel::Slot(1, 2).ColumnSpan(2).Padding(0.0f, 3.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]() { return FText::FromString(GetGeometrySummary()); })
                ];
        }

        TSharedRef<SWidget> BuildSettingsGrid()
        {
            return SNew(SGridPanel)
                + SGridPanel::Slot(0, 0).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Asset Package"))) ]
                + SGridPanel::Slot(1, 0).Padding(0.0f, 3.0f)
                [
                    SAssignNew(AssetPackageTextBox, SEditableTextBox)
                    .Text(FText::FromString(TEXT("/Game/UERayTracingAudio/BakedIR")))
                    .MinDesiredWidth(360.0f)
                ]
                + SGridPanel::Slot(0, 1).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Input SoundWave (required for A/B)"))) ]
                + SGridPanel::Slot(1, 1).Padding(0.0f, 3.0f)
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(USoundWave::StaticClass())
                    .ObjectPath_Lambda([this]() { return InputSoundWave.ToSoftObjectPath().ToString(); })
                    .OnObjectChanged_Lambda([this](const FAssetData& AssetData)
                    {
                        InputSoundWave = Cast<USoundWave>(AssetData.GetAsset());
                    })
                ]
                + SGridPanel::Slot(0, 2).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Baked Audio Folder"))) ]
                + SGridPanel::Slot(1, 2).Padding(0.0f, 3.0f)
                [
                    SAssignNew(BakedAudioDestinationTextBox, SEditableTextBox)
                    .Text(FText::FromString(TEXT("/Game/UERayTracingAudio/BakedAudio")))
                ]
                + SGridPanel::Slot(0, 3).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Rays"))) ]
                + SGridPanel::Slot(1, 3).Padding(0.0f, 3.0f)
                [
                    SNew(SNumericEntryBox<int32>)
                    .Value_Lambda([this]() { return TOptional<int32>(BakeSettings.NumRays); })
                    .MinValue(1).MaxValue(1048576)
                    .OnValueChanged_Lambda([this](int32 Value) { BakeSettings.NumRays = Value; })
                ]
                + SGridPanel::Slot(0, 4).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Bounces"))) ]
                + SGridPanel::Slot(1, 4).Padding(0.0f, 3.0f)
                [
                    SNew(SNumericEntryBox<int32>)
                    .Value_Lambda([this]() { return TOptional<int32>(BakeSettings.MaxBounces); })
                    .MinValue(1).MaxValue(64)
                    .OnValueChanged_Lambda([this](int32 Value) { BakeSettings.MaxBounces = Value; })
                ]
                + SGridPanel::Slot(0, 5).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Duration (s)"))) ]
                + SGridPanel::Slot(1, 5).Padding(0.0f, 3.0f)
                [
                    SNew(SNumericEntryBox<float>)
                    .Value_Lambda([this]() { return TOptional<float>(BakeSettings.DurationSeconds); })
                    .MinValue(0.05f).MaxValue(30.0f)
                    .OnValueChanged_Lambda([this](float Value) { BakeSettings.DurationSeconds = Value; })
                ]
                + SGridPanel::Slot(0, 6).Padding(0.0f, 3.0f, 12.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("Sample Rate"))) ]
                + SGridPanel::Slot(1, 6).Padding(0.0f, 3.0f)
                [
                    SNew(SNumericEntryBox<int32>)
                    .Value_Lambda([this]() { return TOptional<int32>(BakeSettings.SampleRate); })
                    .MinValue(8000).MaxValue(192000)
                    .OnValueChanged_Lambda([this](int32 Value) { BakeSettings.SampleRate = Value; })
                ];
        }

        TSharedRef<SWidget> BuildListeningControls()
        {
            return SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Listening Acceptance")))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]() { return FText::FromString(GetListeningToolbarSummary()); })
                    .AutoWrapText(true)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Aligned waveforms, shared full-scale axis: Reference (gray) | Direct (blue) | Wet (purple) | Full (green)")))
                    .AutoWrapText(true)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SNew(SBorder)
                    .Padding(4.0f)
                    [
                        SNew(SBox)
                        .HeightOverride(240.0f)
                        [
                            SAssignNew(WaveformPanel, SUERayTracingAudioComparisonWaveforms)
                        ]
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Reference")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return PlayListeningMode(EUERayTracingAudioListeningMode::Reference); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Direct")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return PlayListeningMode(EUERayTracingAudioListeningMode::Direct); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Wet")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return PlayListeningMode(EUERayTracingAudioListeningMode::Wet); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Full")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return PlayListeningMode(EUERayTracingAudioListeningMode::Full); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Stop")))
                        .OnClicked(this, &SUERayTracingAudioBakePanel::StopPreview)
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("A/B Reference ↔ Direct")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return ToggleAB(EUERayTracingAudioListeningMode::Direct); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("A/B Reference ↔ Full")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return ToggleAB(EUERayTracingAudioListeningMode::Full); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Replay")))
                        .IsEnabled_Lambda([this]() { return CurrentListeningMode != EUERayTracingAudioListeningMode::None; })
                        .OnClicked_Lambda([this]() { return PlayListeningMode(CurrentListeningMode); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Human Pass")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return RecordListeningVerdict(true); })
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Human Fail")))
                        .IsEnabled_Lambda([this]() { return HasComparisonAudio(); })
                        .OnClicked_Lambda([this]() { return RecordListeningVerdict(false); })
                    ]
                ];
        }

        bool IsJobRunning() const
        {
            return Job.IsValid()
                && (Job->GetState() == EUERayTracingAudioBakeJobState::Pending
                    || Job->GetState() == EUERayTracingAudioBakeJobState::Running);
        }

        bool CanMutateValidationFixture() const
        {
            return !IsJobRunning() && !bOfflineRenderActive;
        }

        FString GetValidationFixtureSummary() const
        {
            return FString::Printf(
                TEXT("Validation fixture only | Effective distance: %.0f cm | Air absorption (%s): (%.6f, %.6f, %.6f)"),
                EffectiveValidationDistanceCm,
                FUERayTracingAudioEditorValidationScene::
                    GetAirAbsorptionProfileName(ValidationAirProfile),
                EffectiveValidationAirAbsorption.X,
                EffectiveValidationAirAbsorption.Y,
                EffectiveValidationAirAbsorption.Z);
        }

        bool HasSelectedBakedAsset() const
        {
            const UUERayTracingAudioSourceComponent* Source = SelectedSource.Get();
            return IsValid(Source) && IsValid(Source->BakedImpulseResponseAsset);
        }

        FString GetDataSourceButtonLabel(
            const EUERayTracingAudioIndirectDataSource DataSource,
            const TCHAR* Label) const
        {
            const UUERayTracingAudioSourceComponent* Source = SelectedSource.Get();
            return IsValid(Source) && Source->IndirectDataSource == DataSource
                ? FString::Printf(TEXT("[Active] %s"), Label)
                : FString(Label);
        }

        FString GetRuntimeDataSourceSummary() const
        {
            const UUERayTracingAudioSourceComponent* Source = SelectedSource.Get();
            if (!IsValid(Source))
            {
                return TEXT("Select a Source Actor to choose its runtime IR data source.");
            }

            const TCHAR* ModeName = TEXT("Realtime IR");
            if (Source->IndirectDataSource == EUERayTracingAudioIndirectDataSource::Baked)
            {
                ModeName = TEXT("Baked IR");
            }
            else if (Source->IndirectDataSource == EUERayTracingAudioIndirectDataSource::Hybrid)
            {
                ModeName = TEXT("Hybrid: baked early field + realtime tail");
            }
            return FString::Printf(
                TEXT("Active: %s | Baked asset: %s"),
                ModeName,
                IsValid(Source->BakedImpulseResponseAsset)
                    ? *Source->BakedImpulseResponseAsset->GetPathName()
                    : TEXT("none (run Bake Selected Source first)"));
        }

        FReply SetRuntimeDataSource(const EUERayTracingAudioIndirectDataSource DataSource)
        {
            UUERayTracingAudioSourceComponent* Source = SelectedSource.Get();
            if (!IsValid(Source))
            {
                LastStatus = TEXT("Select a Source Actor before changing the runtime IR data source.");
                return FReply::Handled();
            }
            if (DataSource != EUERayTracingAudioIndirectDataSource::Realtime
                && !IsValid(Source->BakedImpulseResponseAsset))
            {
                LastStatus = TEXT("Baked IR and Hybrid require a saved baked asset. Run Bake Selected Source first.");
                return FReply::Handled();
            }

            const FScopedTransaction Transaction(
                FText::FromString(TEXT("Change UE Ray Tracing Audio Runtime IR Data Source")));
            Source->Modify();
            Source->SetIndirectDataSource(DataSource);
            Source->GetOwner()->MarkPackageDirty();
            LastStatus = GetRuntimeDataSourceSummary();
            return FReply::Handled();
        }

        UWorld* GetEditorWorld() const
        {
            return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        }

        void InitializeValidationSelections()
        {
            UWorld* World = GetEditorWorld();
            SelectedSource =
                FUERayTracingAudioEditorValidationScene::FindTaggedSource(
                    World);
            SelectedListener = FindListener(World);

            FString AirProfileValue;
            FParse::Value(
                FCommandLine::Get(),
                TEXT("UERayTracingAudioValidationAirAbsorptionProfile="),
                AirProfileValue);
            ValidationAirProfile =
                FUERayTracingAudioEditorValidationScene::
                    ParseAirAbsorptionProfile(AirProfileValue);
            FParse::Value(
                FCommandLine::Get(),
                TEXT("UERayTracingAudioValidationDistanceCm="),
                ValidationDistanceCm);
            if (!FMath::IsNearlyEqual(ValidationDistanceCm, 100.0f)
                && !FMath::IsNearlyEqual(ValidationDistanceCm, 400.0f))
            {
                ValidationDistanceCm = 200.0f;
            }

            FString ReflectionEnvironmentValue;
            FParse::Value(
                FCommandLine::Get(),
                TEXT("UERayTracingAudioValidationReflectionEnvironment="),
                ReflectionEnvironmentValue);
            ValidationReflectionEnvironment =
                FUERayTracingAudioEditorValidationScene::
                    ParseReflectionEnvironment(ReflectionEnvironmentValue);

            FString DirectPresetValue;
            FParse::Value(
                FCommandLine::Get(),
                TEXT("UERayTracingAudioValidationDirectPreset="),
                DirectPresetValue);
            ValidationDirectPreset =
                FUERayTracingAudioEditorValidationScene::ParseDirectPreset(
                    DirectPresetValue);

            const AActor* SourceActor = SelectedSource.IsValid()
                ? SelectedSource->GetOwner()
                : nullptr;
            if (IsValid(SourceActor)
                && SourceActor->ActorHasTag(
                    FName(TEXT("VRTA_EditorValidationScene")))
                && SelectedListener.IsValid()
                && IsValid(SelectedListener->GetOwner())
                && SelectedListener->GetOwner()->ActorHasTag(
                    FName(TEXT("VRTA_EditorValidationScene"))))
            {
                EffectiveValidationDistanceCm = FVector::Distance(
                    SourceActor->GetActorLocation(),
                    SelectedListener->GetOwner()->GetActorLocation());
                EffectiveValidationAirAbsorption =
                    SelectedSource->GetAirAbsorptionPerMeter();
            }

            if (!FParse::Param(FCommandLine::Get(), TEXT("UERayTracingAudioValidationScenario")))
            {
                return;
            }

            USoundWave* BestSoundWave = FindRecommendedProjectSoundWave();
            InputSoundWave = BestSoundWave;
            if (IsValid(BestSoundWave))
            {
                LastStatus = FString::Printf(
                    TEXT("Validation startup selected real project SoundWave: %s (%.2fs)."),
                    *BestSoundWave->GetPathName(),
                    BestSoundWave->GetDuration());
            }
        }

        FReply CreateValidationScene(const EUERayTracingAudioEditorDirectPreset DirectPreset)
        {
            UWorld* World = GetEditorWorld();
            if (!IsValid(World))
            {
                LastStatus = TEXT("No active editor world is available for the A/B validation scene.");
                return FReply::Handled();
            }

            const FUERayTracingAudioEditorValidationSceneResult SceneResult =
                FUERayTracingAudioEditorValidationScene::EnsureScene(
                    *World,
                    EUERayTracingAudioEditorValidationSceneMode::Persistent,
                    DirectPreset,
                    ValidationReflectionEnvironment,
                    ValidationDistanceCm,
                    ValidationAirProfile,
                    GetValidationReflectionBounces());
            ValidationDirectPreset = DirectPreset;
            EffectiveValidationDistanceCm =
                SceneResult.SourceListenerDistanceCm;
            EffectiveValidationAirAbsorption =
                SceneResult.AirAbsorptionPerMeter;
            SelectedSource = SceneResult.Source;
            SelectedListener = SceneResult.Listener;
            if (GEditor && SelectedSource.IsValid() && SelectedSource->GetOwner())
            {
                GEditor->SelectNone(false, true, false);
                GEditor->SelectActor(SelectedSource->GetOwner(), true, true, true);
            }
            LastStatus = SceneResult.Message;
            return FReply::Handled();
        }

        FReply SetValidationDistanceCm(const float DistanceCm)
        {
            if (!CanMutateValidationFixture())
            {
                return FReply::Handled();
            }
            ValidationDistanceCm = DistanceCm;
            return CreateValidationScene(
                EUERayTracingAudioEditorDirectPreset::Clear);
        }

        FReply SetValidationAirAbsorptionProfile(
            const EUERayTracingAudioEditorAirAbsorptionProfile AirProfile)
        {
            if (!CanMutateValidationFixture())
            {
                return FReply::Handled();
            }
            ValidationAirProfile = AirProfile;
            return CreateValidationScene(ValidationDirectPreset);
        }

        UUERayTracingAudioSourceComponent* FindSelectedSource() const
        {
            if (!GEditor || !GEditor->GetSelectedActors())
            {
                return nullptr;
            }

            for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
            {
                if (AActor* Actor = Cast<AActor>(*It))
                {
                    if (UUERayTracingAudioSourceComponent* Source = Actor->FindComponentByClass<UUERayTracingAudioSourceComponent>())
                    {
                        return Source;
                    }
                }
            }
            return nullptr;
        }

        UUERayTracingAudioListenerComponent* FindSelectedListener() const
        {
            if (!GEditor || !GEditor->GetSelectedActors())
            {
                return nullptr;
            }

            for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
            {
                if (AActor* Actor = Cast<AActor>(*It))
                {
                    if (UUERayTracingAudioListenerComponent* Listener =
                        Actor->FindComponentByClass<UUERayTracingAudioListenerComponent>())
                    {
                        return Listener;
                    }
                }
            }
            return nullptr;
        }

        FReply UseSelectedSource()
        {
            SelectedSource = FindSelectedSource();
            LastStatus = SelectedSource.IsValid()
                ? FString::Printf(TEXT("Selected source: %s"), *SelectedSource->GetOwner()->GetActorLabel())
                : TEXT("The selected Actor has no UERayTracingAudioSourceComponent.");
            return FReply::Handled();
        }

        FReply UseSelectedListener()
        {
            SelectedListener = FindSelectedListener();
            LastStatus = SelectedListener.IsValid()
                ? FString::Printf(TEXT("Selected listener: %s"), *SelectedListener->GetOwner()->GetActorLabel())
                : TEXT("The selected Actor has no UERayTracingAudioListenerComponent.");
            return FReply::Handled();
        }

        FString GetGeometrySummary() const
        {
            UWorld* World = GetEditorWorld();
            int32 NumGeometryComponents = 0;
            if (IsValid(World))
            {
                for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
                {
                    TInlineComponentArray<UUERayTracingAudioGeometryComponent*> GeometryComponents(*ActorIt);
                    NumGeometryComponents += GeometryComponents.Num();
                }
            }
            return FString::Printf(
                TEXT("All %d UERayTracingAudioGeometryComponent instances in the current level"),
                NumGeometryComponents);
        }

        UUERayTracingAudioListenerComponent* FindListener(UWorld* World) const
        {
            if (!IsValid(World))
            {
                return nullptr;
            }

            UUERayTracingAudioListenerComponent* FirstListener = nullptr;
            for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
            {
                if (UUERayTracingAudioListenerComponent* Listener = ActorIt->FindComponentByClass<UUERayTracingAudioListenerComponent>())
                {
                    if (ActorIt->ActorHasTag(FName(TEXT("VRTA_EditorValidationScene"))))
                    {
                        return Listener;
                    }
                    FirstListener = FirstListener ? FirstListener : Listener;
                }
            }
            return FirstListener;
        }

        FReply StartBake()
        {
            UWorld* World = GetEditorWorld();
            UUERayTracingAudioSourceComponent* Source = SelectedSource.Get();
            UUERayTracingAudioListenerComponent* Listener = SelectedListener.Get();
            if (!IsValid(Listener))
            {
                Listener = FindListener(World);
            }
            FString AdmissionError;
            if (!FUERayTracingAudioEditorBakeAdmission::Validate(
                    World,
                    Source,
                    Listener,
                    InputSoundWave.Get(),
                    AdmissionError))
            {
                LastStatus = MoveTemp(AdmissionError);
                return FReply::Handled();
            }

            SelectedSource = Source;
            SelectedListener = Listener;
            ActiveBakeSource = Source;
            ActiveBakeInputSoundWave = InputSoundWave;
            ActiveBakeWetSend = FMath::Clamp(
                Source->GetIndirectMix(),
                0.0f,
                4.0f);
            ActiveBakeSourceActorPath = Source->GetOwner()
                ? Source->GetOwner()->GetPathName()
                : FString();
            ActiveBakeListenerActorPath = Listener->GetOwner()
                ? Listener->GetOwner()->GetPathName()
                : FString();

            BakeSettings.bRequireHardwareRayTracing = true;
            Job = FUERayTracingAudioModule::GetManager().StartImpulseResponseBake(Source, Listener, BakeSettings);
            bHandledTerminalState = false;
            LastStatus = Job.IsValid() ? Job->GetStatusText() : TEXT("Failed to create bake job.");
            return FReply::Handled();
        }

        FReply CancelBake()
        {
            if (Job.IsValid())
            {
                Job->Cancel();
            }
            return FReply::Handled();
        }

        FString GetStatusText() const
        {
            if (bOfflineRenderActive)
            {
                return TEXT("Rendering selected SoundWave with the baked impulse response...");
            }
            if (!Job.IsValid() || bHandledTerminalState)
            {
                return LastStatus;
            }
            if (Job->GetState() == EUERayTracingAudioBakeJobState::Failed)
            {
                return FString::Printf(TEXT("Bake failed: %s"), *Job->GetError());
            }
            return Job->GetStatusText();
        }

        void SaveCompletedBake()
        {
            FUERayTracingAudioBakeResult BakeResult;
            if (!Job->GetResult(BakeResult))
            {
                LastStatus = TEXT("Bake completed without a readable result.");
                return;
            }

            FString RequestedPackageName = AssetPackageTextBox.IsValid()
                ? AssetPackageTextBox->GetText().ToString().TrimStartAndEnd()
                : FString();
            FText PackageError;
            if (!FPackageName::IsValidLongPackageName(RequestedPackageName, true, &PackageError))
            {
                LastStatus = FString::Printf(TEXT("Invalid asset package path: %s"), *PackageError.ToString());
                return;
            }

            FString UniquePackageName;
            FString UniqueAssetName;
            FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(
                RequestedPackageName,
                TEXT(""),
                UniquePackageName,
                UniqueAssetName);

            UPackage* Package = CreatePackage(*UniquePackageName);
            if (!Package)
            {
                LastStatus = TEXT("Could not create the impulse response asset package.");
                return;
            }

            UUERayTracingAudioImpulseResponseAsset* Asset = NewObject<UUERayTracingAudioImpulseResponseAsset>(
                Package,
                *UniqueAssetName,
                RF_Public | RF_Standalone | RF_Transactional);
            TArray<float> OfflineImpulseResponse = BakeResult.Samples;
            const FString OfflineSceneSignature = BakeResult.SceneSignature;
            Asset->Initialize(
                BakeResult.SourceWorld,
                BakeResult.SceneVersion,
                MoveTemp(BakeResult.SceneSignature),
                BakeResult.SourceLocation,
                BakeResult.ListenerLocation,
                BakeResult.BakeSettings,
                BakeResult.ChannelFormat,
                BakeResult.NumChannels,
                BakeResult.BinDurationSeconds,
                MoveTemp(BakeResult.Samples));

            FString ValidationError;
            if (!Asset->Validate(ValidationError))
            {
                LastStatus = FString::Printf(TEXT("Baked asset validation failed: %s"), *ValidationError);
                return;
            }

            FAssetRegistryModule::AssetCreated(Asset);
            Package->MarkPackageDirty();

            const FString Filename = FPackageName::LongPackageNameToFilename(
                UniquePackageName,
                FPackageName::GetAssetPackageExtension());
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            if (!UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
            {
                LastStatus = FString::Printf(TEXT("Bake completed, but saving failed: %s"), *Filename);
                return;
            }

            if (UUERayTracingAudioSourceComponent* Source = ActiveBakeSource.Get())
            {
                const FScopedTransaction Transaction(
                    FText::FromString(TEXT("Assign UE Ray Tracing Audio Baked IR")));
                Source->Modify();
                Source->SetBakedImpulseResponseAsset(Asset);
                Source->SetIndirectDataSource(
                    EUERayTracingAudioIndirectDataSource::Hybrid);
                if (AActor* SourceOwner = Source->GetOwner())
                {
                    SourceOwner->MarkPackageDirty();
                }
            }
            LastStatus = FString::Printf(TEXT("Saved impulse response asset: %s.%s"), *UniquePackageName, *UniqueAssetName);
            StartOfflineRenderIfRequested(
                OfflineImpulseResponse,
                UniqueAssetName,
                BakeResult,
                OfflineSceneSignature);
        }

        void StartOfflineRenderIfRequested(
            const TArray<float>& ImpulseResponse,
            const FString& ImpulseResponseAssetName,
            const FUERayTracingAudioBakeResult& BakeResult,
            const FString& SceneSignature)
        {
            USoundWave* SoundWave = ActiveBakeInputSoundWave.Get();
            if (!IsValid(SoundWave))
            {
                return;
            }

            TArray<uint8> RawPcmBytes;
            uint32 InputSampleRate = 0;
            uint16 NumChannels = 0;
            if (!SoundWave->GetImportedSoundWaveData(RawPcmBytes, InputSampleRate, NumChannels)
                || RawPcmBytes.IsEmpty()
                || RawPcmBytes.Num() % sizeof(int16) != 0)
            {
                LastStatus = TEXT("IR asset was saved, but the selected SoundWave has no readable imported PCM16 data.");
                return;
            }

            FString DestinationPath = BakedAudioDestinationTextBox.IsValid()
                ? BakedAudioDestinationTextBox->GetText().ToString().TrimStartAndEnd()
                : FString();
            FText DestinationError;
            if (!FPackageName::IsValidLongPackageName(DestinationPath, true, &DestinationError))
            {
                LastStatus = FString::Printf(TEXT("IR asset was saved, but baked audio folder is invalid: %s"), *DestinationError.ToString());
                return;
            }

            FUERayTracingAudioOfflineRenderRequest Request;
            Request.InputInterleavedPcm.SetNumUninitialized(RawPcmBytes.Num() / sizeof(int16));
            FMemory::Memcpy(Request.InputInterleavedPcm.GetData(), RawPcmBytes.GetData(), RawPcmBytes.Num());
            Request.InputSampleRate = static_cast<int32>(InputSampleRate);
            Request.NumChannels = static_cast<int32>(NumChannels);
            Request.ImpulseResponse = ImpulseResponse;
            Request.ImpulseResponseNumChannels = BakeResult.NumChannels;
            Request.OutputSampleRate = BakeResult.BakeSettings.SampleRate;
            Request.DirectGain = BakeResult.DirectResult.OverallGain;
            Request.WetMix = ActiveBakeWetSend;
            Request.DirectPreset = TEXT("custom");
            Request.DirectDistanceCm = BakeResult.DirectResult.DistanceCm;
            Request.DirectVisibility = BakeResult.DirectResult.DirectVisibility;
            Request.DirectOcclusion = BakeResult.DirectResult.Occlusion;
            Request.DirectDistanceAttenuation = BakeResult.DirectResult.DistanceAttenuation;
            Request.DirectAirAbsorptionAverage = (
                BakeResult.DirectResult.AirAbsorption.X
                + BakeResult.DirectResult.AirAbsorption.Y
                + BakeResult.DirectResult.AirAbsorption.Z) / 3.0f;
            Request.ReflectionRayCount = BakeResult.BakeSettings.NumRays;
            Request.ReflectionBounceCount = BakeResult.BakeSettings.MaxBounces;
            Request.HardwareIndirectValidPaths = BakeResult.IndirectValidPathCount;
            Request.HardwareIndirectGain = BakeResult.IndirectGain;
            Request.HardwareEarlyReflectionGain = BakeResult.EarlyReflectionGain;
            Request.HardwareLateReverbGain = BakeResult.LateReverbGain;
            Request.HardwareEarliestArrivalSeconds = BakeResult.EarliestArrivalSeconds;
            Request.HardwareAverageDelaySeconds = BakeResult.AverageDelaySeconds;
            Request.HardwareReverbTimes = BakeResult.ReverbTimes;
            Request.HardwareDominantArrivalDirection = BakeResult.DominantArrivalDirection;
            Request.HardwareDirectionalEnergyRatio = BakeResult.DirectionalEnergyRatio;
            Request.HardwareDirectionalBinCount = BakeResult.DirectionalBinCount;
            Request.HardwareImpulseResponseEnergy = BakeResult.ImpulseResponseEnergy;
            Request.bHasCpuReference = BakeResult.bHasCpuReference;
            Request.CpuReferenceIndirectValidPaths = BakeResult.CpuReferenceValidPathCount;
            Request.CpuReferenceIndirectGain = BakeResult.CpuReferenceIndirectGain;
            Request.CpuReferenceEarlyReflectionGain = BakeResult.CpuReferenceEarlyReflectionGain;
            Request.CpuReferenceLateReverbGain = BakeResult.CpuReferenceLateReverbGain;
            Request.CpuReferenceDominantArrivalDirection = BakeResult.CpuReferenceDominantArrivalDirection;
            Request.CpuReferenceDirectionalEnergyRatio = BakeResult.CpuReferenceDirectionalEnergyRatio;
            Request.CpuReferenceDirectionalBinCount = BakeResult.CpuReferenceDirectionalBinCount;
            Request.CpuReferenceImpulseResponseEnergy = BakeResult.CpuReferenceImpulseResponseEnergy;
            const FString SessionId = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
            Request.OutputDirectory = FPaths::Combine(
                FPaths::ProjectSavedDir(),
                TEXT("UERayTracingAudio"),
                TEXT("ListeningAcceptance"),
                SessionId);
            Request.OutputFilenamePrefix = FString::Printf(
                TEXT("%s_%s"),
                *SoundWave->GetName(),
                *ImpulseResponseAssetName);
            Request.InputAssetPath = SoundWave->GetPathName();
            Request.SourceActorPath = ActiveBakeSourceActorPath;
            Request.ListenerActorPath = ActiveBakeListenerActorPath;
            Request.SceneSignature = SceneSignature;
            Request.bUsedHardwareRayTracing = BakeResult.bUsedHardwareRayTracing;

            PendingImportDestination = MoveTemp(DestinationPath);
            bOfflineRenderActive = true;
            OfflineRenderFuture = Async(
                EAsyncExecution::ThreadPool,
                [Request = MoveTemp(Request)]() mutable
                {
                    return FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(MoveTemp(Request));
                });
            LastStatus = TEXT("IR asset saved. Rendering sample-aligned Reference / Direct / Wet / Full WAVs in the background...");
        }

        void FinishOfflineRender(const FUERayTracingAudioOfflineRenderResult& RenderResult)
        {
            if (!RenderResult.bSucceeded)
            {
                LastStatus = FString::Printf(TEXT("Offline audio render failed: %s"), *RenderResult.Error);
                return;
            }

            auto ImportSoundWave = [this](
                const FString& Filename,
                TSoftObjectPtr<USoundWave>& OutSoundWave) -> bool
            {
                const TArray<UObject*> ImportedAssets = FAssetToolsModule::GetModule().Get().ImportAssets(
                    TArray<FString>{ Filename },
                    PendingImportDestination);
                USoundWave* ImportedSoundWave = ImportedAssets.IsEmpty()
                    ? nullptr
                    : Cast<USoundWave>(ImportedAssets[0]);
                ConfigureComparisonSoundWaveForPreview(ImportedSoundWave);
                OutSoundWave = ImportedSoundWave;
                return IsValid(ImportedSoundWave);
            };

            StopListeningPreviewAudioIfOwned();
            ComparisonReference.Reset();
            ComparisonDirect.Reset();
            ComparisonWet.Reset();
            ComparisonFull.Reset();
            bComparisonWaveformsLoaded = false;
            WaveformLoadError = TEXT("waiting for comparison render");
            if (WaveformPanel.IsValid())
            {
                WaveformPanel->ResetWaveforms();
            }
            if (!ImportSoundWave(RenderResult.ReferenceWaveFilename, ComparisonReference)
                || !ImportSoundWave(RenderResult.DirectWaveFilename, ComparisonDirect)
                || !ImportSoundWave(RenderResult.WetWaveFilename, ComparisonWet)
                || !ImportSoundWave(RenderResult.FullWaveFilename, ComparisonFull))
            {
                LastStatus = FString::Printf(
                    TEXT("Comparison WAVs were written, but one or more could not be imported. Manifest: %s"),
                    *RenderResult.ManifestFilename);
                return;
            }

            LastComparisonRender = RenderResult;
            CurrentListeningMode = EUERayTracingAudioListeningMode::None;
            LastHumanVerdict = TEXT("not recorded");
            bComparisonWaveformsLoaded = LoadComparisonWaveforms(RenderResult);
            LastStatus = FString::Printf(
                TEXT("Imported four aligned comparison assets. Auto checks: %s. Waveforms: %s. Direct correlation %.4f, Direct level %.4f, Wet level %.4f, Direct/Wet difference %.4f, Full correlation %.4f, common scale %.6f. Manifest: %s"),
                RenderResult.bAutomaticChecksPassed ? TEXT("PASS") : TEXT("FAIL"),
                bComparisonWaveformsLoaded ? TEXT("ready") : *WaveformLoadError,
                RenderResult.DirectDryCorrelation,
                RenderResult.DirectToReferenceRmsRatio,
                RenderResult.WetToReferenceRmsRatio,
                RenderResult.DirectWetNormalizedDifference,
                RenderResult.FullDryCorrelation,
                RenderResult.CommonOutputScale,
                *RenderResult.ManifestFilename);
        }

        bool LoadComparisonWaveforms(const FUERayTracingAudioOfflineRenderResult& RenderResult)
        {
            TArray<float> ReferenceEnvelope;
            TArray<float> DirectEnvelope;
            TArray<float> WetEnvelope;
            TArray<float> FullEnvelope;
            WaveformLoadError.Reset();

            if (!LoadComparisonWaveform(RenderResult.ReferenceWaveFilename, ReferenceEnvelope, WaveformLoadError)
                || !LoadComparisonWaveform(RenderResult.DirectWaveFilename, DirectEnvelope, WaveformLoadError)
                || !LoadComparisonWaveform(RenderResult.WetWaveFilename, WetEnvelope, WaveformLoadError)
                || !LoadComparisonWaveform(RenderResult.FullWaveFilename, FullEnvelope, WaveformLoadError))
            {
                if (WaveformPanel.IsValid())
                {
                    WaveformPanel->ResetWaveforms();
                }
                return false;
            }

            if (!WaveformPanel.IsValid())
            {
                WaveformLoadError = TEXT("waveform panel is unavailable");
                return false;
            }

            WaveformPanel->SetWaveforms(
                MoveTemp(ReferenceEnvelope),
                MoveTemp(DirectEnvelope),
                MoveTemp(WetEnvelope),
                MoveTemp(FullEnvelope));
            return true;
        }

        bool HasComparisonAudio() const
        {
            return ComparisonReference.IsValid()
                && ComparisonDirect.IsValid()
                && ComparisonWet.IsValid()
                && ComparisonFull.IsValid();
        }

        USoundWave* GetListeningSound(EUERayTracingAudioListeningMode Mode) const
        {
            switch (Mode)
            {
            case EUERayTracingAudioListeningMode::Reference:
                return ComparisonReference.Get();
            case EUERayTracingAudioListeningMode::Direct:
                return ComparisonDirect.Get();
            case EUERayTracingAudioListeningMode::Wet:
                return ComparisonWet.Get();
            case EUERayTracingAudioListeningMode::Full:
                return ComparisonFull.Get();
            default:
                return nullptr;
            }
        }

        static const TCHAR* GetListeningModeName(EUERayTracingAudioListeningMode Mode)
        {
            switch (Mode)
            {
            case EUERayTracingAudioListeningMode::Reference:
                return TEXT("Reference");
            case EUERayTracingAudioListeningMode::Direct:
                return TEXT("Direct");
            case EUERayTracingAudioListeningMode::Wet:
                return TEXT("Wet");
            case EUERayTracingAudioListeningMode::Full:
                return TEXT("Full");
            default:
                return TEXT("None");
            }
        }

        void ResetListeningPreviewState()
        {
            ListeningPreviewAudioComponent.Reset();
            CurrentListeningMode = EUERayTracingAudioListeningMode::None;
        }

        void StopListeningPreviewAudioIfOwned()
        {
            UAudioComponent* PreviewAudio = ListeningPreviewAudioComponent.Get();
            if (GEditor
                && IsValid(PreviewAudio)
                && GEditor->GetPreviewAudioComponent() == PreviewAudio)
            {
                GEditor->ResetPreviewAudioComponent();
            }
            ResetListeningPreviewState();
        }

        void RefreshListeningPreviewState()
        {
            if (CurrentListeningMode == EUERayTracingAudioListeningMode::None)
            {
                return;
            }

            UAudioComponent* PreviewAudio = ListeningPreviewAudioComponent.Get();
            if (GEditor
                && IsValid(PreviewAudio)
                && GEditor->GetPreviewAudioComponent() == PreviewAudio
                && PreviewAudio->IsPlaying())
            {
                return;
            }

            const FString FinishedMode = GetListeningModeName(CurrentListeningMode);
            if (GEditor
                && IsValid(PreviewAudio)
                && GEditor->GetPreviewAudioComponent() == PreviewAudio
                && ListeningPreviewRestartCount == 0)
            {
                // bLooping is the primary mechanism. If the shared Editor
                // preview component nevertheless reports a terminal stop,
                // recover the same comparison mode from sample zero rather
                // than leaving a stale "playing" UI with no sound.
                USoundWave* SoundWave = GetListeningSound(CurrentListeningMode);
                if (ConfigureComparisonSoundWaveForPreview(SoundWave))
                {
                    UAudioComponent* RestartedPreview =
                        GEditor->PlayPreviewSound(SoundWave);
                    if (IsValid(RestartedPreview)
                        && RestartedPreview->IsPlaying())
                    {
                        ListeningPreviewAudioComponent = RestartedPreview;
                        ++ListeningPreviewRestartCount;
                        LastStatus = FString::Printf(
                            TEXT("Recovered stopped %s comparison preview from sample zero (restart %d); playback remains looping."),
                            *FinishedMode,
                            ListeningPreviewRestartCount);
                        return;
                    }
                }
            }

            ResetListeningPreviewState();
            LastStatus = FString::Printf(
                TEXT("%s comparison preview is no longer playing."),
                *FinishedMode);
        }

        FReply PlayListeningMode(EUERayTracingAudioListeningMode Mode)
        {
            USoundWave* SoundWave = GetListeningSound(Mode);
            if (!GEditor || !ConfigureComparisonSoundWaveForPreview(SoundWave))
            {
                ResetListeningPreviewState();
                LastStatus = TEXT("The requested comparison SoundWave is not available.");
                return FReply::Handled();
            }

            UAudioComponent* PreviewAudio = GEditor->PlayPreviewSound(SoundWave);
            if (!IsValid(PreviewAudio) || !PreviewAudio->IsPlaying())
            {
                GEditor->ResetPreviewAudioComponent();
                ResetListeningPreviewState();
                LastStatus = FString::Printf(
                    TEXT("Could not start the %s comparison preview."),
                    GetListeningModeName(Mode));
                return FReply::Handled();
            }

            ListeningPreviewAudioComponent = PreviewAudio;
            CurrentListeningMode = Mode;
            ListeningPreviewRestartCount = 0;
            LastStatus = FString::Printf(
                TEXT("Looping %s from sample zero at the shared comparison scale %.6f; press Stop to end playback."),
                GetListeningModeName(Mode),
                LastComparisonRender.CommonOutputScale);
            return FReply::Handled();
        }

        FReply ToggleAB(EUERayTracingAudioListeningMode ProcessedMode)
        {
            const EUERayTracingAudioListeningMode NextMode =
                CurrentListeningMode == EUERayTracingAudioListeningMode::Reference
                ? ProcessedMode
                : EUERayTracingAudioListeningMode::Reference;
            return PlayListeningMode(NextMode);
        }

        FReply StopPreview()
        {
            if (GEditor)
            {
                GEditor->ResetPreviewAudioComponent();
            }
            ResetListeningPreviewState();
            LastStatus = TEXT("Comparison preview stopped.");
            return FReply::Handled();
        }

        FString GetListeningToolbarSummary() const
        {
            if (!HasComparisonAudio())
            {
                return TEXT("A/B controls are waiting for a complete Reference / Direct / Wet / Full set.");
            }

            return FString::Printf(
                TEXT("Ready | Auto=%s | Hardware=%s | Waveforms=%s | Common scale %.6f | Current=%s | Human=%s"),
                LastComparisonRender.bAutomaticChecksPassed ? TEXT("PASS") : TEXT("FAIL"),
                LastComparisonRender.bUsedHardwareRayTracing ? TEXT("yes") : TEXT("no"),
                bComparisonWaveformsLoaded ? TEXT("ready") : *WaveformLoadError,
                LastComparisonRender.CommonOutputScale,
                GetListeningModeName(CurrentListeningMode),
                *LastHumanVerdict);
        }

        FString GetListeningSummary() const
        {
            if (!HasComparisonAudio())
            {
                return TEXT("No comparison set is loaded. Select a SoundWave, Source, and Listener, then run Bake Selected Source.");
            }

            return FString::Printf(
                TEXT("Auto=%s | hardware=%s | input %.2fs%s | direct gain %.6f | wet send %.3f | dry correlation direct %.4f / wet %.4f / full %.4f | RMS ratio direct %.4f / wet %.4f / full %.4f | direct-wet difference %.4f | distinct=%s | common scale %.6f | current=%s | human=%s"),
                LastComparisonRender.bAutomaticChecksPassed ? TEXT("PASS") : TEXT("FAIL"),
                LastComparisonRender.bUsedHardwareRayTracing ? TEXT("yes") : TEXT("no"),
                LastComparisonRender.DurationSeconds,
                LastComparisonRender.bRecommendedInputDuration ? TEXT("") : TEXT(" (recommended 10-20s)"),
                LastComparisonRender.DirectGain,
                LastComparisonRender.WetMix,
                LastComparisonRender.DirectDryCorrelation,
                LastComparisonRender.WetDryCorrelation,
                LastComparisonRender.FullDryCorrelation,
                LastComparisonRender.DirectToReferenceRmsRatio,
                LastComparisonRender.WetToReferenceRmsRatio,
                LastComparisonRender.FullToReferenceRmsRatio,
                LastComparisonRender.DirectWetNormalizedDifference,
                LastComparisonRender.bModesAreDistinct ? TEXT("yes") : TEXT("no"),
                LastComparisonRender.CommonOutputScale,
                GetListeningModeName(CurrentListeningMode),
                *LastHumanVerdict);
        }

        FReply RecordListeningVerdict(bool bPassed)
        {
            if (!HasComparisonAudio() || LastComparisonRender.ManifestFilename.IsEmpty())
            {
                LastStatus = TEXT("No complete comparison set is available to record.");
                return FReply::Handled();
            }

            TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
            Root->SetStringField(TEXT("recorded_at_utc"), FDateTime::UtcNow().ToIso8601());
            Root->SetStringField(TEXT("comparison_manifest"), LastComparisonRender.ManifestFilename);
            Root->SetBoolField(TEXT("automatic_checks_passed"), LastComparisonRender.bAutomaticChecksPassed);
            Root->SetBoolField(TEXT("human_listening_passed"), bPassed);
            Root->SetNumberField(TEXT("direct_to_reference_rms_ratio"), LastComparisonRender.DirectToReferenceRmsRatio);
            Root->SetNumberField(TEXT("wet_to_reference_rms_ratio"), LastComparisonRender.WetToReferenceRmsRatio);
            Root->SetNumberField(TEXT("direct_wet_normalized_difference"), LastComparisonRender.DirectWetNormalizedDifference);
            Root->SetBoolField(TEXT("modes_are_distinct"), LastComparisonRender.bModesAreDistinct);
            Root->SetStringField(TEXT("last_previewed_mode"), GetListeningModeName(CurrentListeningMode));
            Root->SetStringField(TEXT("requirement"), TEXT("Direct and Full retain recognizable source content; Wet is spatial tail only; no clipping, dropout, noise, or timing jump."));

            FString JsonText;
            TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
            const FString VerdictFilename = FPaths::ChangeExtension(
                LastComparisonRender.ManifestFilename,
                TEXT("HumanAcceptance.json"));
            if (!FJsonSerializer::Serialize(Root, Writer)
                || !FFileHelper::SaveStringToFile(JsonText, *VerdictFilename))
            {
                LastStatus = FString::Printf(TEXT("Could not save human listening record: %s"), *VerdictFilename);
                return FReply::Handled();
            }

            LastHumanVerdict = bPassed ? TEXT("PASS") : TEXT("FAIL");
            LastStatus = FString::Printf(
                TEXT("Recorded human listening %s: %s"),
                *LastHumanVerdict,
                *VerdictFilename);
            return FReply::Handled();
        }

        FUERayTracingAudioBakeSettings BakeSettings;
        TSharedPtr<FUERayTracingAudioBakeJob> Job;
        TSharedPtr<SEditableTextBox> AssetPackageTextBox;
        TSharedPtr<SEditableTextBox> BakedAudioDestinationTextBox;
        TSharedPtr<SUERayTracingAudioComparisonWaveforms> WaveformPanel;
        TSoftObjectPtr<USoundWave> InputSoundWave;
        TWeakObjectPtr<UUERayTracingAudioSourceComponent> SelectedSource;
        TWeakObjectPtr<UUERayTracingAudioListenerComponent> SelectedListener;
        TSoftObjectPtr<USoundWave> ActiveBakeInputSoundWave;
        TWeakObjectPtr<UUERayTracingAudioSourceComponent> ActiveBakeSource;
        FString ActiveBakeSourceActorPath;
        FString ActiveBakeListenerActorPath;
        float ActiveBakeWetSend = 1.0f;
        float ValidationDistanceCm = 200.0f;
        float EffectiveValidationDistanceCm = 200.0f;
        FVector EffectiveValidationAirAbsorption =
            FVector(0.0002f, 0.0006f, 0.0012f);
        EUERayTracingAudioEditorDirectPreset ValidationDirectPreset =
            EUERayTracingAudioEditorDirectPreset::Clear;
        EUERayTracingAudioEditorReflectionEnvironment
            ValidationReflectionEnvironment =
                EUERayTracingAudioEditorReflectionEnvironment::Enclosed;
        EUERayTracingAudioEditorAirAbsorptionProfile ValidationAirProfile =
            EUERayTracingAudioEditorAirAbsorptionProfile::Default;
        TSoftObjectPtr<USoundWave> ComparisonReference;
        TSoftObjectPtr<USoundWave> ComparisonDirect;
        TSoftObjectPtr<USoundWave> ComparisonWet;
        TSoftObjectPtr<USoundWave> ComparisonFull;
        TWeakObjectPtr<UAudioComponent> ListeningPreviewAudioComponent;
        TFuture<FUERayTracingAudioOfflineRenderResult> OfflineRenderFuture;
        FUERayTracingAudioOfflineRenderResult LastComparisonRender;
        EUERayTracingAudioListeningMode CurrentListeningMode = EUERayTracingAudioListeningMode::None;
        int32 ListeningPreviewRestartCount = 0;
        FString PendingImportDestination;
        FString LastStatus = TEXT("Ready.");
        FString LastHumanVerdict = TEXT("not recorded");
        FString WaveformLoadError = TEXT("waiting for comparison render");
        bool bOfflineRenderActive = false;
        bool bHandledTerminalState = false;
        bool bComparisonWaveformsLoaded = false;
    };

    TWeakPtr<SUERayTracingAudioBakePanel> GActiveBakePanel;
}

IMPLEMENT_MODULE(FUERayTracingAudioEditorModule, UERayTracingAudioEditor)

void FUERayTracingAudioEditorModule::StartupModule()
{
    UE_LOG(LogTemp, Display, TEXT("UERayTracingAudioEditor module initialized."));
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        UERayTracingAudioBakeTabName,
        FOnSpawnTab::CreateRaw(this, &FUERayTracingAudioEditorModule::SpawnBakeTab))
        .SetDisplayName(FText::FromString(TEXT("UE Ray Tracing Audio Bake")))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUERayTracingAudioEditorModule::RegisterMenus));

    if (FParse::Param(FCommandLine::Get(), TEXT("UERayTracingAudioValidationScenario"))
        && FParse::Param(FCommandLine::Get(), TEXT("UERayTracingAudioInteractiveValidation")))
    {
        UE_LOG(
            LogTemp,
            Display,
            TEXT("UERayTracingAudioEditor interactive validation ready: static_bake_fixture=0 press_play=1 movement=WASD+Mouse mode_hotkeys=F1,F2,F5 ab_hotkey=F3."));
    }
    else if (FParse::Param(FCommandLine::Get(), TEXT("UERayTracingAudioValidationScenario")))
    {
        ValidationSceneStartSeconds = FPlatformTime::Seconds();
        ValidationSceneTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateRaw(this, &FUERayTracingAudioEditorModule::TickValidationScene),
            0.25f);
    }
}

void FUERayTracingAudioEditorModule::ShutdownModule()
{
    GActiveBakePanel.Reset();
    ValidationArtifactRunner.Reset();
    if (ValidationSceneTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ValidationSceneTickerHandle);
        ValidationSceneTickerHandle.Reset();
    }

    UToolMenus::UnRegisterStartupCallback(this);

    if (UToolMenus::TryGet())
    {
        UToolMenus::UnregisterOwner(this);
    }

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UERayTracingAudioBakeTabName);
}

bool FUERayTracingAudioEditorModule::TickValidationScene(const float DeltaTime)
{
    static_cast<void>(DeltaTime);
    if (ValidationArtifactRunner.IsValid())
    {
        ValidationArtifactRunner->Tick();
        if (!ValidationArtifactRunner->IsComplete())
        {
            return true;
        }

        const FUERayTracingAudioEditorArtifactResult& ArtifactResult =
            ValidationArtifactRunner->GetResult();
        if (!ArtifactResult.bSucceeded)
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("UERayTracingAudioEditor A/B artifacts failed: %s"),
                *ArtifactResult.Error);
            ValidationArtifactRunner.Reset();
            ValidationSceneTickerHandle.Reset();
            return false;
        }

        const FUERayTracingAudioOfflineRenderResult& Offline = ArtifactResult.OfflineRender;
        const TSharedPtr<SUERayTracingAudioBakePanel> BakePanel = GActiveBakePanel.Pin();
        if (!BakePanel.IsValid()
            || !BakePanel->AdoptValidationArtifactResult(ArtifactResult))
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("UERayTracingAudioEditor A/B artifacts failed: generated comparison assets could not be loaded into the visible listening acceptance controls."));
            ValidationArtifactRunner.Reset();
            ValidationSceneTickerHandle.Reset();
            return false;
        }
        UE_LOG(
            LogTemp,
            Display,
            TEXT("UERayTracingAudioEditor listening acceptance ready: controls=1 imported_assets=4 manifest=\"%s\" human_verdict=enabled."),
            *Offline.ManifestFilename);
        UE_LOG(
            LogTemp,
            Display,
            TEXT("UERayTracingAudioEditor A/B artifacts ready: hardware=%d auto_checks=%d distinct=%d input=\"%s\" direct_preset=\"%s\" reflection_environment=\"%s\" distance_cm=%.3f visibility=%.6f occlusion=%.6f distance_attenuation=%.6f ir_asset=\"%s\" imported_assets=%d reference=\"%s\" direct=\"%s\" wet=\"%s\" full=\"%s\" manifest=\"%s\" reflection_rays=%d reflection_bounces=%d hw_paths=%d hw_gain=%.9f hw_early=%.9f hw_late=%.9f hw_directional_ratio=%.6f hw_directional_bins=%d hw_ir_energy=%.9f cpu_reference=%d cpu_paths=%d cpu_gain=%.9f cpu_early=%.9f cpu_late=%.9f cpu_directional_ratio=%.6f cpu_directional_bins=%d cpu_ir_energy=%.9f direct_level=%.6f wet_level=%.6f full_level=%.6f wet_correlation=%.6f direct_wet_difference=%.6f wet_stereo_difference=%.6f directional_wet=%d common_scale=%.6f."),
            Offline.bUsedHardwareRayTracing ? 1 : 0,
            Offline.bAutomaticChecksPassed ? 1 : 0,
            Offline.bModesAreDistinct ? 1 : 0,
            *ArtifactResult.InputAssetPath,
            *Offline.DirectPreset,
            *Offline.ReflectionEnvironment,
            Offline.DirectDistanceCm,
            Offline.DirectVisibility,
            Offline.DirectOcclusion,
            Offline.DirectDistanceAttenuation,
            *ArtifactResult.ImpulseResponseAssetPath,
            ArtifactResult.ImportedComparisonAssetPaths.Num(),
            *Offline.ReferenceWaveFilename,
            *Offline.DirectWaveFilename,
            *Offline.WetWaveFilename,
            *Offline.FullWaveFilename,
            *Offline.ManifestFilename,
            Offline.ReflectionRayCount,
            Offline.ReflectionBounceCount,
            Offline.HardwareIndirectValidPaths,
            Offline.HardwareIndirectGain,
            Offline.HardwareEarlyReflectionGain,
            Offline.HardwareLateReverbGain,
            Offline.HardwareDirectionalEnergyRatio,
            Offline.HardwareDirectionalBinCount,
            Offline.HardwareImpulseResponseEnergy,
            Offline.bHasCpuReference ? 1 : 0,
            Offline.CpuReferenceIndirectValidPaths,
            Offline.CpuReferenceIndirectGain,
            Offline.CpuReferenceEarlyReflectionGain,
            Offline.CpuReferenceLateReverbGain,
            Offline.CpuReferenceDirectionalEnergyRatio,
            Offline.CpuReferenceDirectionalBinCount,
            Offline.CpuReferenceImpulseResponseEnergy,
            Offline.DirectToReferenceRmsRatio,
            Offline.WetToReferenceRmsRatio,
            Offline.FullToReferenceRmsRatio,
            Offline.WetDryCorrelation,
            Offline.DirectWetNormalizedDifference,
            Offline.WetStereoNormalizedDifference,
            Offline.bDirectionalWetIsDistinct ? 1 : 0,
            Offline.CommonOutputScale);
        ValidationArtifactRunner.Reset();
        ValidationSceneTickerHandle.Reset();
        return false;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!IsValid(World) || World->IsGameWorld())
    {
        if (FPlatformTime::Seconds() - ValidationSceneStartSeconds < 30.0)
        {
            return true;
        }

        UE_LOG(LogTemp, Error, TEXT("UERayTracingAudioEditor validation scene timed out waiting for an editor world."));
        ValidationSceneTickerHandle.Reset();
        return false;
    }

    FString ReflectionEnvironmentValue;
    FParse::Value(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioValidationReflectionEnvironment="),
        ReflectionEnvironmentValue);
    float DistanceCmOverride = -1.0f;
    FParse::Value(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioValidationDistanceCm="),
        DistanceCmOverride);
    FString AirAbsorptionProfileValue;
    FParse::Value(
        FCommandLine::Get(),
        TEXT("UERayTracingAudioValidationAirAbsorptionProfile="),
        AirAbsorptionProfileValue);
    const int32 ReflectionBounces = GetValidationReflectionBounces();

    const FUERayTracingAudioEditorValidationSceneResult SceneResult =
        FUERayTracingAudioEditorValidationScene::EnsureScene(
            *World,
            EUERayTracingAudioEditorValidationSceneMode::Transient,
            [&]()
            {
                FString DirectPresetValue;
                FParse::Value(
                    FCommandLine::Get(),
                    TEXT("UERayTracingAudioValidationDirectPreset="),
                    DirectPresetValue);
                return FUERayTracingAudioEditorValidationScene::ParseDirectPreset(DirectPresetValue);
            }(),
            FUERayTracingAudioEditorValidationScene::ParseReflectionEnvironment(ReflectionEnvironmentValue),
            DistanceCmOverride,
            FUERayTracingAudioEditorValidationScene::
                ParseAirAbsorptionProfile(AirAbsorptionProfileValue),
            ReflectionBounces);
    if (!SceneResult.bSucceeded)
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("UERayTracingAudioEditor validation scene failed: %s"),
            *SceneResult.Message);
        ValidationSceneTickerHandle.Reset();
        return false;
    }

    FGlobalTabmanager::Get()->TryInvokeTab(UERayTracingAudioBakeTabName);
    UE_LOG(
        LogTemp,
        Display,
        TEXT("UERayTracingAudioEditor validation scene ready: source=1 listener=1 geometry=%d lighting=1 bake_ui=1 direct_preset=%s reflection_environment=%s reflection_bounces=%d source_listener_distance_cm=%.2f air_absorption_profile=%s air_absorption_per_meter=(%.6f,%.6f,%.6f)."),
        SceneResult.GeometryCount,
        FUERayTracingAudioEditorValidationScene::GetDirectPresetName(SceneResult.DirectPreset),
        FUERayTracingAudioEditorValidationScene::GetReflectionEnvironmentName(SceneResult.ReflectionEnvironment),
        SceneResult.ReflectionBounces,
        SceneResult.SourceListenerDistanceCm,
        FUERayTracingAudioEditorValidationScene::GetAirAbsorptionProfileName(
            SceneResult.AirAbsorptionProfile),
        SceneResult.AirAbsorptionPerMeter.X,
        SceneResult.AirAbsorptionPerMeter.Y,
        SceneResult.AirAbsorptionPerMeter.Z);

    if (FParse::Param(FCommandLine::Get(), TEXT("UERayTracingAudioValidationEditorBake")))
    {
        USoundWave* SoundWave = FindRecommendedProjectSoundWave();
        UUERayTracingAudioSourceComponent* Source = SceneResult.Source.Get();
        UUERayTracingAudioListenerComponent* Listener = SceneResult.Listener.Get();
        if (!IsValid(SoundWave) || !IsValid(Source) || !IsValid(Listener))
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("UERayTracingAudioEditor A/B artifacts failed: no valid project SoundWave, Source, or Listener."));
            ValidationSceneTickerHandle.Reset();
            return false;
        }

        ValidationArtifactRunner = MakeShared<FUERayTracingAudioEditorArtifactRunner>();
        FString StartError;
        if (!ValidationArtifactRunner->Start(
                *Source,
                *Listener,
                *SoundWave,
                FUERayTracingAudioEditorValidationScene::GetDirectPresetName(SceneResult.DirectPreset),
                SceneResult.ReflectionBounces,
                StartError,
                FUERayTracingAudioEditorValidationScene::GetReflectionEnvironmentName(SceneResult.ReflectionEnvironment)))
        {
            UE_LOG(
                LogTemp,
                Error,
                TEXT("UERayTracingAudioEditor A/B artifacts failed: %s"),
                *StartError);
            ValidationArtifactRunner.Reset();
            ValidationSceneTickerHandle.Reset();
            return false;
        }
        return true;
    }

    ValidationSceneTickerHandle.Reset();
    return false;
}

void FUERayTracingAudioEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
    FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
    Section.AddMenuEntry(
        "UERayTracingAudioBakeWindow",
        FText::FromString(TEXT("UE Ray Tracing Audio Bake")),
        FText::FromString(TEXT("Open the bake workflow for UE Ray Tracing Audio.")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FGlobalTabmanager::Get()->TryInvokeTab(UERayTracingAudioBakeTabName);
        })));
}

TSharedRef<SDockTab> FUERayTracingAudioEditorModule::SpawnBakeTab(const FSpawnTabArgs& SpawnTabArgs)
{
    const TSharedRef<SUERayTracingAudioBakePanel> BakePanel =
        SNew(SUERayTracingAudioBakePanel);
    GActiveBakePanel = BakePanel;
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            BakePanel
        ];
}
