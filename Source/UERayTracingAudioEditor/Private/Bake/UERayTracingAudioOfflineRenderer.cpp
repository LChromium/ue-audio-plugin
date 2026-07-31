#include "Bake/UERayTracingAudioOfflineRenderer.h"

#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    // Below -26 dB RMS, a non-zero wet file can still be effectively
    // indistinguishable under the dry signal. This is an audibility gate, not
    // merely a numerical non-silence check.
    constexpr float MinimumAudibleWetToReferenceRmsRatio = 0.05f;

    struct FComplexSample
    {
        float Real = 0.0f;
        float Imag = 0.0f;

        FComplexSample operator+(const FComplexSample& Other) const { return { Real + Other.Real, Imag + Other.Imag }; }
        FComplexSample operator-(const FComplexSample& Other) const { return { Real - Other.Real, Imag - Other.Imag }; }
        FComplexSample operator*(const FComplexSample& Other) const
        {
            return { (Real * Other.Real) - (Imag * Other.Imag), (Real * Other.Imag) + (Imag * Other.Real) };
        }
        FComplexSample& operator+=(const FComplexSample& Other)
        {
            Real += Other.Real;
            Imag += Other.Imag;
            return *this;
        }
    };

    void Transform(TArray<FComplexSample>& Data, bool bInverse)
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
            const FComplexSample Root{ FMath::Cos(Angle), FMath::Sin(Angle) };
            for (int32 Start = 0; Start < Num; Start += Length)
            {
                FComplexSample Weight{ 1.0f, 0.0f };
                for (int32 Offset = 0; Offset < Length / 2; ++Offset)
                {
                    const FComplexSample Even = Data[Start + Offset];
                    const FComplexSample Odd = Data[Start + Offset + (Length / 2)] * Weight;
                    Data[Start + Offset] = Even + Odd;
                    Data[Start + Offset + (Length / 2)] = Even - Odd;
                    Weight = Weight * Root;
                }
            }
        }

        if (bInverse)
        {
            const float Scale = 1.0f / static_cast<float>(Num);
            for (FComplexSample& Sample : Data)
            {
                Sample.Real *= Scale;
                Sample.Imag *= Scale;
            }
        }
    }

    TArray<float> ResampleChannel(
        const TArray<int16>& InterleavedInput,
        int32 NumChannels,
        int32 ChannelIndex,
        int32 InputSampleRate,
        int32 OutputSampleRate)
    {
        const int32 InputFrames = InterleavedInput.Num() / NumChannels;
        const int32 OutputFrames = FMath::Max(
            FMath::RoundToInt(static_cast<double>(InputFrames) * static_cast<double>(OutputSampleRate) / static_cast<double>(InputSampleRate)),
            1);
        TArray<float> Output;
        Output.SetNumUninitialized(OutputFrames);

        const double InputPerOutput = static_cast<double>(InputSampleRate) / static_cast<double>(OutputSampleRate);
        for (int32 OutputFrame = 0; OutputFrame < OutputFrames; ++OutputFrame)
        {
            const double SourcePosition = static_cast<double>(OutputFrame) * InputPerOutput;
            const int32 FrameA = FMath::Clamp(FMath::FloorToInt(SourcePosition), 0, InputFrames - 1);
            const int32 FrameB = FMath::Min(FrameA + 1, InputFrames - 1);
            const float Alpha = static_cast<float>(SourcePosition - static_cast<double>(FrameA));
            const float SampleA = static_cast<float>(InterleavedInput[(FrameA * NumChannels) + ChannelIndex]) / 32768.0f;
            const float SampleB = static_cast<float>(InterleavedInput[(FrameB * NumChannels) + ChannelIndex]) / 32768.0f;
            Output[OutputFrame] = FMath::Lerp(SampleA, SampleB, Alpha);
        }
        return Output;
    }

    TArray<float> ExtractImpulseResponseChannel(
        const TArray<float>& InterleavedImpulseResponse,
        int32 NumChannels,
        int32 ChannelIndex)
    {
        const int32 NumFrames = InterleavedImpulseResponse.Num() / NumChannels;
        TArray<float> Output;
        Output.SetNumUninitialized(NumFrames);
        for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
        {
            Output[FrameIndex] = InterleavedImpulseResponse[(FrameIndex * NumChannels) + ChannelIndex];
        }
        return Output;
    }

    TArray<float> PartitionedConvolve(const TArray<float>& Input, const TArray<float>& ImpulseResponse)
    {
        constexpr int32 BlockSize = 8192;
        constexpr int32 TransformSize = BlockSize * 2;
        const int32 NumPartitions = FMath::DivideAndRoundUp(ImpulseResponse.Num(), BlockSize);
        const int32 NumInputBlocks = FMath::DivideAndRoundUp(Input.Num(), BlockSize);
        const int32 NumOutputBlocks = NumInputBlocks + NumPartitions - 1;
        const int32 OutputSamples = Input.Num() + ImpulseResponse.Num() - 1;

        TArray<TArray<FComplexSample>> ImpulsePartitions;
        ImpulsePartitions.SetNum(NumPartitions);
        for (int32 PartitionIndex = 0; PartitionIndex < NumPartitions; ++PartitionIndex)
        {
            TArray<FComplexSample>& Partition = ImpulsePartitions[PartitionIndex];
            Partition.SetNumZeroed(TransformSize);
            const int32 SourceOffset = PartitionIndex * BlockSize;
            const int32 CopyCount = FMath::Min(BlockSize, ImpulseResponse.Num() - SourceOffset);
            for (int32 Index = 0; Index < CopyCount; ++Index)
            {
                Partition[Index].Real = ImpulseResponse[SourceOffset + Index];
            }
            Transform(Partition, false);
        }

        TArray<TArray<FComplexSample>> InputHistory;
        InputHistory.SetNum(NumPartitions);
        for (TArray<FComplexSample>& History : InputHistory)
        {
            History.SetNumZeroed(TransformSize);
        }

        TArray<float> Output;
        Output.SetNumZeroed(OutputSamples);
        TArray<float> Overlap;
        Overlap.SetNumZeroed(BlockSize);

        for (int32 BlockIndex = 0; BlockIndex < NumOutputBlocks; ++BlockIndex)
        {
            TArray<FComplexSample>& CurrentInput = InputHistory[BlockIndex % NumPartitions];
            for (FComplexSample& Sample : CurrentInput)
            {
                Sample = FComplexSample();
            }

            if (BlockIndex < NumInputBlocks)
            {
                const int32 SourceOffset = BlockIndex * BlockSize;
                const int32 CopyCount = FMath::Min(BlockSize, Input.Num() - SourceOffset);
                for (int32 Index = 0; Index < CopyCount; ++Index)
                {
                    CurrentInput[Index].Real = Input[SourceOffset + Index];
                }
            }
            Transform(CurrentInput, false);

            TArray<FComplexSample> FrequencyOutput;
            FrequencyOutput.SetNumZeroed(TransformSize);
            const int32 ActivePartitions = FMath::Min(NumPartitions, BlockIndex + 1);
            for (int32 PartitionIndex = 0; PartitionIndex < ActivePartitions; ++PartitionIndex)
            {
                int32 HistoryIndex = (BlockIndex - PartitionIndex) % NumPartitions;
                if (HistoryIndex < 0)
                {
                    HistoryIndex += NumPartitions;
                }
                const TArray<FComplexSample>& History = InputHistory[HistoryIndex];
                const TArray<FComplexSample>& Impulse = ImpulsePartitions[PartitionIndex];
                for (int32 BinIndex = 0; BinIndex < TransformSize; ++BinIndex)
                {
                    FrequencyOutput[BinIndex] += History[BinIndex] * Impulse[BinIndex];
                }
            }

            Transform(FrequencyOutput, true);
            const int32 DestinationOffset = BlockIndex * BlockSize;
            for (int32 Index = 0; Index < BlockSize; ++Index)
            {
                const int32 DestinationIndex = DestinationOffset + Index;
                if (DestinationIndex < Output.Num())
                {
                    Output[DestinationIndex] = FrequencyOutput[Index].Real + Overlap[Index];
                }
                Overlap[Index] = FrequencyOutput[Index + BlockSize].Real;
            }
        }
        return Output;
    }

    void AppendUInt16(TArray<uint8>& Bytes, uint16 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xff));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
    }

    void AppendUInt32(TArray<uint8>& Bytes, uint32 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xff));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
        Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
        Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
    }

    TArray<uint8> BuildWaveFile(
        const TArray<TArray<float>>& Channels,
        int32 SampleRate,
        float CommonOutputScale)
    {
        const int32 NumChannels = Channels.Num();
        const int32 NumFrames = NumChannels > 0 ? Channels[0].Num() : 0;
        const uint32 DataSize = static_cast<uint32>(NumFrames * NumChannels * sizeof(int16));
        TArray<uint8> Bytes;
        Bytes.Reserve(44 + DataSize);
        const ANSICHAR Riff[] = "RIFF";
        const ANSICHAR Wave[] = "WAVE";
        const ANSICHAR Fmt[] = "fmt ";
        const ANSICHAR Data[] = "data";
        Bytes.Append(reinterpret_cast<const uint8*>(Riff), 4);
        AppendUInt32(Bytes, 36 + DataSize);
        Bytes.Append(reinterpret_cast<const uint8*>(Wave), 4);
        Bytes.Append(reinterpret_cast<const uint8*>(Fmt), 4);
        AppendUInt32(Bytes, 16);
        AppendUInt16(Bytes, 1);
        AppendUInt16(Bytes, static_cast<uint16>(NumChannels));
        AppendUInt32(Bytes, static_cast<uint32>(SampleRate));
        AppendUInt32(Bytes, static_cast<uint32>(SampleRate * NumChannels * sizeof(int16)));
        AppendUInt16(Bytes, static_cast<uint16>(NumChannels * sizeof(int16)));
        AppendUInt16(Bytes, 16);
        Bytes.Append(reinterpret_cast<const uint8*>(Data), 4);
        AppendUInt32(Bytes, DataSize);

        for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
        {
            for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
            {
                const float Sample = FMath::Clamp(
                    Channels[ChannelIndex][FrameIndex] * CommonOutputScale,
                    -1.0f,
                    1.0f);
                AppendUInt16(Bytes, static_cast<uint16>(static_cast<int16>(FMath::RoundToInt(Sample * 32767.0f))));
            }
        }
        return Bytes;
    }

    float CalculatePeak(const TArray<TArray<float>>& Channels)
    {
        float Peak = 0.0f;
        for (const TArray<float>& Channel : Channels)
        {
            for (const float Sample : Channel)
            {
                Peak = FMath::Max(Peak, FMath::Abs(Sample));
            }
        }
        return Peak;
    }

    float CalculateRms(const TArray<TArray<float>>& Channels)
    {
        double SumSquares = 0.0;
        int64 NumSamples = 0;
        for (const TArray<float>& Channel : Channels)
        {
            for (const float Sample : Channel)
            {
                SumSquares += static_cast<double>(Sample) * static_cast<double>(Sample);
                ++NumSamples;
            }
        }
        return NumSamples > 0 ? static_cast<float>(FMath::Sqrt(SumSquares / static_cast<double>(NumSamples))) : 0.0f;
    }

    float CalculateCorrelation(
        const TArray<TArray<float>>& Reference,
        const TArray<TArray<float>>& Candidate,
        int32 NumFrames)
    {
        double Dot = 0.0;
        double ReferenceEnergy = 0.0;
        double CandidateEnergy = 0.0;
        const int32 NumChannels = FMath::Min(Reference.Num(), Candidate.Num());
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 ChannelFrames = FMath::Min3(
                NumFrames,
                Reference[ChannelIndex].Num(),
                Candidate[ChannelIndex].Num());
            for (int32 FrameIndex = 0; FrameIndex < ChannelFrames; ++FrameIndex)
            {
                const double ReferenceSample = Reference[ChannelIndex][FrameIndex];
                const double CandidateSample = Candidate[ChannelIndex][FrameIndex];
                Dot += ReferenceSample * CandidateSample;
                ReferenceEnergy += ReferenceSample * ReferenceSample;
                CandidateEnergy += CandidateSample * CandidateSample;
            }
        }

        const double Denominator = FMath::Sqrt(ReferenceEnergy * CandidateEnergy);
        return Denominator > UE_SMALL_NUMBER ? static_cast<float>(Dot / Denominator) : 0.0f;
    }

    float CalculateNormalizedDifference(
        const TArray<TArray<float>>& First,
        const TArray<TArray<float>>& Second,
        int32 NumFrames)
    {
        double DifferenceEnergy = 0.0;
        double FirstEnergy = 0.0;
        double SecondEnergy = 0.0;
        const int32 NumChannels = FMath::Min(First.Num(), Second.Num());
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 ChannelFrames = FMath::Min3(
                NumFrames,
                First[ChannelIndex].Num(),
                Second[ChannelIndex].Num());
            for (int32 FrameIndex = 0; FrameIndex < ChannelFrames; ++FrameIndex)
            {
                const double FirstSample = First[ChannelIndex][FrameIndex];
                const double SecondSample = Second[ChannelIndex][FrameIndex];
                const double Difference = FirstSample - SecondSample;
                DifferenceEnergy += Difference * Difference;
                FirstEnergy += FirstSample * FirstSample;
                SecondEnergy += SecondSample * SecondSample;
            }
        }

        const double Denominator = FMath::Max(FirstEnergy, SecondEnergy);
        return Denominator > UE_SMALL_NUMBER
            ? static_cast<float>(FMath::Sqrt(DifferenceEnergy / Denominator))
            : 0.0f;
    }

    float CalculateStereoNormalizedDifference(const TArray<TArray<float>>& Channels)
    {
        if (Channels.Num() != 2)
        {
            return 0.0f;
        }
        double DifferenceEnergy = 0.0;
        double LeftEnergy = 0.0;
        double RightEnergy = 0.0;
        const int32 NumFrames = FMath::Min(Channels[0].Num(), Channels[1].Num());
        for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
        {
            const double Left = Channels[0][FrameIndex];
            const double Right = Channels[1][FrameIndex];
            const double Difference = Left - Right;
            DifferenceEnergy += Difference * Difference;
            LeftEnergy += Left * Left;
            RightEnergy += Right * Right;
        }
        const double Denominator = FMath::Max(LeftEnergy, RightEnergy);
        return Denominator > UE_SMALL_NUMBER
            ? static_cast<float>(FMath::Sqrt(DifferenceEnergy / Denominator))
            : 0.0f;
    }

    bool AreSamplesFinite(const TArray<TArray<float>>& Channels)
    {
        for (const TArray<float>& Channel : Channels)
        {
            for (const float Sample : Channel)
            {
                if (!FMath::IsFinite(Sample))
                {
                    return false;
                }
            }
        }
        return true;
    }

    int64 CountClippedSamples(const TArray<TArray<float>>& Channels, float OutputScale)
    {
        int64 Count = 0;
        for (const TArray<float>& Channel : Channels)
        {
            for (const float Sample : Channel)
            {
                if (FMath::Abs(Sample * OutputScale) > 1.0f)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    float CalculateDirectModelResidualRms(
        const TArray<TArray<float>>& Reference,
        const TArray<TArray<float>>& Direct,
        float DirectGain)
    {
        double ResidualEnergy = 0.0;
        int64 NumSamples = 0;
        const int32 NumChannels = FMath::Min(Reference.Num(), Direct.Num());
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 NumFrames = FMath::Min(Reference[ChannelIndex].Num(), Direct[ChannelIndex].Num());
            for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
            {
                const double Residual = static_cast<double>(Direct[ChannelIndex][FrameIndex])
                    - (static_cast<double>(Reference[ChannelIndex][FrameIndex]) * static_cast<double>(DirectGain));
                ResidualEnergy += Residual * Residual;
                ++NumSamples;
            }
        }
        return NumSamples > 0
            ? static_cast<float>(FMath::Sqrt(ResidualEnergy / static_cast<double>(NumSamples)))
            : 0.0f;
    }

    float CalculateFullMixResidualRms(
        const TArray<TArray<float>>& Direct,
        const TArray<TArray<float>>& Wet,
        const TArray<TArray<float>>& Full)
    {
        double ResidualEnergy = 0.0;
        int64 NumSamples = 0;
        const int32 NumChannels = FMath::Min3(Direct.Num(), Wet.Num(), Full.Num());
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 NumFrames = FMath::Min3(
                Direct[ChannelIndex].Num(),
                Wet[ChannelIndex].Num(),
                Full[ChannelIndex].Num());
            for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex)
            {
                const double Residual = static_cast<double>(Full[ChannelIndex][FrameIndex])
                    - static_cast<double>(Direct[ChannelIndex][FrameIndex])
                    - static_cast<double>(Wet[ChannelIndex][FrameIndex]);
                ResidualEnergy += Residual * Residual;
                ++NumSamples;
            }
        }
        return NumSamples > 0
            ? static_cast<float>(FMath::Sqrt(ResidualEnergy / static_cast<double>(NumSamples)))
            : 0.0f;
    }

    float CalculateMaxDirectDiscontinuityResidual(
        const TArray<TArray<float>>& Reference,
        const TArray<TArray<float>>& Direct,
        float DirectGain)
    {
        float MaxResidual = 0.0f;
        const int32 NumChannels = FMath::Min(Reference.Num(), Direct.Num());
        for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
        {
            const int32 NumFrames = FMath::Min(Reference[ChannelIndex].Num(), Direct[ChannelIndex].Num());
            for (int32 FrameIndex = 1; FrameIndex < NumFrames; ++FrameIndex)
            {
                const float DirectDelta = Direct[ChannelIndex][FrameIndex] - Direct[ChannelIndex][FrameIndex - 1];
                const float ExpectedDelta =
                    (Reference[ChannelIndex][FrameIndex] - Reference[ChannelIndex][FrameIndex - 1]) * DirectGain;
                MaxResidual = FMath::Max(MaxResidual, FMath::Abs(DirectDelta - ExpectedDelta));
            }
        }
        return MaxResidual;
    }

    void CountDirectDropoutWindows(
        const TArray<TArray<float>>& Reference,
        const TArray<TArray<float>>& Direct,
        int32 InputFrames,
        int32 SampleRate,
        float ReferenceRms,
        float DirectGain,
        int32& OutActiveWindowCount,
        int32& OutDropoutWindowCount)
    {
        OutActiveWindowCount = 0;
        OutDropoutWindowCount = 0;
        const int32 WindowFrames = FMath::Max(FMath::RoundToInt(static_cast<float>(SampleRate) * 0.02f), 1);
        const float ActiveRmsThreshold = FMath::Max(ReferenceRms * 0.1f, 1.0e-4f);
        const double MinimumEnergyRatio = 0.25;
        const int32 NumChannels = FMath::Min(Reference.Num(), Direct.Num());
        for (int32 WindowStart = 0; WindowStart < InputFrames; WindowStart += WindowFrames)
        {
            const int32 WindowEnd = FMath::Min(WindowStart + WindowFrames, InputFrames);
            double ReferenceEnergy = 0.0;
            double DirectEnergy = 0.0;
            int64 NumSamples = 0;
            for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ++ChannelIndex)
            {
                const int32 ChannelEnd = FMath::Min3(
                    WindowEnd,
                    Reference[ChannelIndex].Num(),
                    Direct[ChannelIndex].Num());
                for (int32 FrameIndex = WindowStart; FrameIndex < ChannelEnd; ++FrameIndex)
                {
                    const double ReferenceSample = Reference[ChannelIndex][FrameIndex];
                    const double DirectSample = Direct[ChannelIndex][FrameIndex];
                    ReferenceEnergy += ReferenceSample * ReferenceSample;
                    DirectEnergy += DirectSample * DirectSample;
                    ++NumSamples;
                }
            }

            const double ActiveThresholdEnergy = static_cast<double>(ActiveRmsThreshold)
                * static_cast<double>(ActiveRmsThreshold)
                * static_cast<double>(NumSamples);
            if (NumSamples > 0 && ReferenceEnergy >= ActiveThresholdEnergy)
            {
                ++OutActiveWindowCount;
                const double ExpectedDirectEnergy = ReferenceEnergy
                    * static_cast<double>(DirectGain)
                    * static_cast<double>(DirectGain);
                if (DirectEnergy < ExpectedDirectEnergy * MinimumEnergyRatio)
                {
                    ++OutDropoutWindowCount;
                }
            }
        }
    }

    bool SaveWave(
        const TArray<TArray<float>>& Channels,
        int32 SampleRate,
        float CommonOutputScale,
        const FString& Filename)
    {
        return FFileHelper::SaveArrayToFile(
            BuildWaveFile(Channels, SampleRate, CommonOutputScale),
            *Filename);
    }

    bool SaveManifest(
        const FUERayTracingAudioOfflineRenderRequest& Request,
        const FUERayTracingAudioOfflineRenderResult& Result)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("input_asset"), Request.InputAssetPath);
        Root->SetStringField(TEXT("source_actor"), Request.SourceActorPath);
        Root->SetStringField(TEXT("listener_actor"), Request.ListenerActorPath);
        Root->SetStringField(TEXT("scene_signature"), Request.SceneSignature);
        Root->SetStringField(TEXT("direct_preset"), Result.DirectPreset);
        Root->SetStringField(TEXT("reflection_environment"), Result.ReflectionEnvironment);
        Root->SetNumberField(TEXT("direct_distance_cm"), Result.DirectDistanceCm);
        Root->SetNumberField(TEXT("direct_visibility"), Result.DirectVisibility);
        Root->SetNumberField(TEXT("direct_occlusion"), Result.DirectOcclusion);
        Root->SetNumberField(TEXT("direct_distance_attenuation"), Result.DirectDistanceAttenuation);
        Root->SetNumberField(TEXT("direct_air_absorption_average"), Result.DirectAirAbsorptionAverage);
        Root->SetNumberField(TEXT("reflection_ray_count"), Result.ReflectionRayCount);
        Root->SetNumberField(TEXT("reflection_bounce_count"), Result.ReflectionBounceCount);
        Root->SetNumberField(TEXT("hardware_indirect_valid_paths"), Result.HardwareIndirectValidPaths);
        Root->SetNumberField(TEXT("hardware_indirect_gain"), Result.HardwareIndirectGain);
        Root->SetNumberField(TEXT("hardware_early_reflection_gain"), Result.HardwareEarlyReflectionGain);
        Root->SetNumberField(TEXT("hardware_late_reverb_gain"), Result.HardwareLateReverbGain);
        Root->SetNumberField(TEXT("hardware_earliest_arrival_seconds"), Result.HardwareEarliestArrivalSeconds);
        Root->SetNumberField(TEXT("hardware_average_delay_seconds"), Result.HardwareAverageDelaySeconds);
        Root->SetNumberField(TEXT("hardware_reverb_time_low_seconds"), Result.HardwareReverbTimes.X);
        Root->SetNumberField(TEXT("hardware_reverb_time_mid_seconds"), Result.HardwareReverbTimes.Y);
        Root->SetNumberField(TEXT("hardware_reverb_time_high_seconds"), Result.HardwareReverbTimes.Z);
        Root->SetNumberField(TEXT("hardware_dominant_arrival_direction_x"), Result.HardwareDominantArrivalDirection.X);
        Root->SetNumberField(TEXT("hardware_dominant_arrival_direction_y"), Result.HardwareDominantArrivalDirection.Y);
        Root->SetNumberField(TEXT("hardware_dominant_arrival_direction_z"), Result.HardwareDominantArrivalDirection.Z);
        Root->SetNumberField(TEXT("hardware_directional_energy_ratio"), Result.HardwareDirectionalEnergyRatio);
        Root->SetNumberField(TEXT("hardware_directional_bin_count"), Result.HardwareDirectionalBinCount);
        Root->SetNumberField(TEXT("hardware_impulse_response_energy"), Result.HardwareImpulseResponseEnergy);
        Root->SetBoolField(TEXT("has_cpu_reference"), Result.bHasCpuReference);
        Root->SetNumberField(TEXT("cpu_reference_indirect_valid_paths"), Result.CpuReferenceIndirectValidPaths);
        Root->SetNumberField(TEXT("cpu_reference_indirect_gain"), Result.CpuReferenceIndirectGain);
        Root->SetNumberField(TEXT("cpu_reference_early_reflection_gain"), Result.CpuReferenceEarlyReflectionGain);
        Root->SetNumberField(TEXT("cpu_reference_late_reverb_gain"), Result.CpuReferenceLateReverbGain);
        Root->SetNumberField(TEXT("cpu_reference_dominant_arrival_direction_x"), Result.CpuReferenceDominantArrivalDirection.X);
        Root->SetNumberField(TEXT("cpu_reference_dominant_arrival_direction_y"), Result.CpuReferenceDominantArrivalDirection.Y);
        Root->SetNumberField(TEXT("cpu_reference_dominant_arrival_direction_z"), Result.CpuReferenceDominantArrivalDirection.Z);
        Root->SetNumberField(TEXT("cpu_reference_directional_energy_ratio"), Result.CpuReferenceDirectionalEnergyRatio);
        Root->SetNumberField(TEXT("cpu_reference_directional_bin_count"), Result.CpuReferenceDirectionalBinCount);
        Root->SetNumberField(TEXT("cpu_reference_impulse_response_energy"), Result.CpuReferenceImpulseResponseEnergy);
        Root->SetBoolField(TEXT("hardware_ray_tracing"), Result.bUsedHardwareRayTracing);
        Root->SetNumberField(TEXT("sample_rate"), Result.SampleRate);
        Root->SetNumberField(TEXT("channels"), Result.NumChannels);
        Root->SetNumberField(TEXT("impulse_response_channels"), Result.ImpulseResponseNumChannels);
        Root->SetNumberField(TEXT("frames"), Result.NumFrames);
        Root->SetNumberField(TEXT("duration_seconds"), Result.DurationSeconds);
        Root->SetNumberField(TEXT("direct_gain"), Result.DirectGain);
        Root->SetNumberField(TEXT("wet_mix"), Result.WetMix);
        Root->SetNumberField(TEXT("common_output_scale"), Result.CommonOutputScale);
        Root->SetNumberField(TEXT("peak_before_scale"), Result.PeakBeforeScale);
        Root->SetNumberField(TEXT("reference_rms"), Result.ReferenceRms);
        Root->SetNumberField(TEXT("direct_rms"), Result.DirectRms);
        Root->SetNumberField(TEXT("wet_rms"), Result.WetRms);
        Root->SetNumberField(TEXT("full_rms"), Result.FullRms);
        Root->SetNumberField(TEXT("direct_dry_correlation"), Result.DirectDryCorrelation);
        Root->SetNumberField(TEXT("full_dry_correlation"), Result.FullDryCorrelation);
        Root->SetNumberField(TEXT("wet_dry_correlation"), Result.WetDryCorrelation);
        Root->SetNumberField(TEXT("direct_to_reference_rms_ratio"), Result.DirectToReferenceRmsRatio);
        Root->SetNumberField(TEXT("wet_to_reference_rms_ratio"), Result.WetToReferenceRmsRatio);
        Root->SetNumberField(TEXT("full_to_reference_rms_ratio"), Result.FullToReferenceRmsRatio);
        Root->SetNumberField(TEXT("direct_wet_normalized_difference"), Result.DirectWetNormalizedDifference);
        Root->SetNumberField(TEXT("wet_stereo_normalized_difference"), Result.WetStereoNormalizedDifference);
        Root->SetBoolField(TEXT("directional_wet_is_distinct"), Result.bDirectionalWetIsDistinct);
        Root->SetNumberField(TEXT("post_scale_peak"), Result.PostScalePeak);
        Root->SetNumberField(TEXT("clipped_sample_count"), Result.ClippedSampleCount);
        Root->SetNumberField(TEXT("direct_active_window_count"), Result.DirectActiveWindowCount);
        Root->SetNumberField(TEXT("direct_dropout_window_count"), Result.DirectDropoutWindowCount);
        Root->SetNumberField(TEXT("direct_model_residual_rms"), Result.DirectModelResidualRms);
        Root->SetNumberField(TEXT("full_mix_residual_rms"), Result.FullMixResidualRms);
        Root->SetNumberField(TEXT("max_direct_discontinuity_residual"), Result.MaxDirectDiscontinuityResidual);
        Root->SetBoolField(TEXT("samples_finite"), Result.bSamplesFinite);
        Root->SetBoolField(TEXT("direct_dropout_check_applicable"), Result.bDirectDropoutCheckApplicable);
        Root->SetBoolField(TEXT("audio_safety_checks_passed"), Result.bAudioSafetyChecksPassed);
        Root->SetBoolField(TEXT("direct_semantics_passed"), Result.bDirectSemanticsPassed);
        Root->SetBoolField(TEXT("modes_are_distinct"), Result.bModesAreDistinct);
        Root->SetBoolField(TEXT("recommended_input_duration"), Result.bRecommendedInputDuration);
        Root->SetBoolField(TEXT("automatic_checks_passed"), Result.bAutomaticChecksPassed);
        Root->SetStringField(TEXT("reference_wav"), Result.ReferenceWaveFilename);
        Root->SetStringField(TEXT("direct_wav"), Result.DirectWaveFilename);
        Root->SetStringField(TEXT("wet_wav"), Result.WetWaveFilename);
        Root->SetStringField(TEXT("full_wav"), Result.FullWaveFilename);

        FString JsonText;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
        if (!FJsonSerializer::Serialize(Root, Writer))
        {
            return false;
        }
        return FFileHelper::SaveStringToFile(JsonText, *Result.ManifestFilename);
    }
}

FUERayTracingAudioOfflineRenderResult FUERayTracingAudioOfflineRenderer::RenderComparisonToWaveFiles(
    FUERayTracingAudioOfflineRenderRequest&& Request)
{
    FUERayTracingAudioOfflineRenderResult Result;
    Result.SampleRate = Request.OutputSampleRate;
    Result.ImpulseResponseNumChannels = Request.ImpulseResponseNumChannels;
    Result.DirectGain = FMath::Clamp(Request.DirectGain, 0.0f, 1.0f);
    Result.WetMix = FMath::Clamp(Request.WetMix, 0.0f, 4.0f);
    Result.DirectPreset = Request.DirectPreset.IsEmpty() ? TEXT("custom") : Request.DirectPreset;
    Result.ReflectionEnvironment = Request.ReflectionEnvironment.IsEmpty() ? TEXT("enclosed") : Request.ReflectionEnvironment;
    Result.DirectDistanceCm = FMath::Max(Request.DirectDistanceCm, 0.0f);
    Result.DirectVisibility = FMath::Clamp(Request.DirectVisibility, 0.0f, 1.0f);
    Result.DirectOcclusion = FMath::Clamp(Request.DirectOcclusion, 0.0f, 1.0f);
    Result.DirectDistanceAttenuation = FMath::Clamp(Request.DirectDistanceAttenuation, 0.0f, 1.0f);
    Result.DirectAirAbsorptionAverage = FMath::Clamp(Request.DirectAirAbsorptionAverage, 0.0f, 1.0f);
    Result.ReflectionRayCount = FMath::Max(Request.ReflectionRayCount, 0);
    Result.ReflectionBounceCount = FMath::Max(Request.ReflectionBounceCount, 0);
    Result.HardwareIndirectValidPaths = Request.HardwareIndirectValidPaths;
    Result.HardwareIndirectGain = Request.HardwareIndirectGain;
    Result.HardwareEarlyReflectionGain = Request.HardwareEarlyReflectionGain;
    Result.HardwareLateReverbGain = Request.HardwareLateReverbGain;
    Result.HardwareEarliestArrivalSeconds = Request.HardwareEarliestArrivalSeconds;
    Result.HardwareAverageDelaySeconds = Request.HardwareAverageDelaySeconds;
    Result.HardwareReverbTimes = Request.HardwareReverbTimes;
    Result.HardwareDominantArrivalDirection = Request.HardwareDominantArrivalDirection;
    Result.HardwareDirectionalEnergyRatio = Request.HardwareDirectionalEnergyRatio;
    Result.HardwareDirectionalBinCount = Request.HardwareDirectionalBinCount;
    Result.HardwareImpulseResponseEnergy = Request.HardwareImpulseResponseEnergy;
    Result.bHasCpuReference = Request.bHasCpuReference;
    Result.CpuReferenceIndirectValidPaths = Request.CpuReferenceIndirectValidPaths;
    Result.CpuReferenceIndirectGain = Request.CpuReferenceIndirectGain;
    Result.CpuReferenceEarlyReflectionGain = Request.CpuReferenceEarlyReflectionGain;
    Result.CpuReferenceLateReverbGain = Request.CpuReferenceLateReverbGain;
    Result.CpuReferenceDominantArrivalDirection = Request.CpuReferenceDominantArrivalDirection;
    Result.CpuReferenceDirectionalEnergyRatio = Request.CpuReferenceDirectionalEnergyRatio;
    Result.CpuReferenceDirectionalBinCount = Request.CpuReferenceDirectionalBinCount;
    Result.CpuReferenceImpulseResponseEnergy = Request.CpuReferenceImpulseResponseEnergy;
    Result.bUsedHardwareRayTracing = Request.bUsedHardwareRayTracing;

    if (Request.InputSampleRate <= 0 || Request.OutputSampleRate <= 0 || Request.NumChannels <= 0)
    {
        Result.Error = TEXT("Input or output audio format is invalid.");
        return Result;
    }
    if (Request.InputInterleavedPcm.IsEmpty() || Request.InputInterleavedPcm.Num() % Request.NumChannels != 0)
    {
        Result.Error = TEXT("Input PCM data does not match the channel count.");
        return Result;
    }
    if (Request.ImpulseResponse.IsEmpty())
    {
        Result.Error = TEXT("Impulse response is empty.");
        return Result;
    }
    if ((Request.ImpulseResponseNumChannels != 1 && Request.ImpulseResponseNumChannels != 2)
        || Request.ImpulseResponse.Num() % Request.ImpulseResponseNumChannels != 0)
    {
        Result.Error = TEXT("Impulse response must use a valid mono or stereo interleaved layout.");
        return Result;
    }
    if (Request.ImpulseResponseNumChannels == 2 && Request.NumChannels > 2)
    {
        Result.Error = TEXT("Directional stereo bake currently supports mono or stereo input SoundWaves.");
        return Result;
    }

    TArray<TArray<float>> ReferenceChannels;
    TArray<TArray<float>> WetChannels;
    ReferenceChannels.SetNum(Request.NumChannels);
    for (int32 ChannelIndex = 0; ChannelIndex < Request.NumChannels; ++ChannelIndex)
    {
        ReferenceChannels[ChannelIndex] = ResampleChannel(
            Request.InputInterleavedPcm,
            Request.NumChannels,
            ChannelIndex,
            Request.InputSampleRate,
            Request.OutputSampleRate);
    }

    if (Request.ImpulseResponseNumChannels == 2)
    {
        const int32 InputFrames = ReferenceChannels.IsEmpty() ? 0 : ReferenceChannels[0].Num();
        TArray<float> MonoInput;
        MonoInput.SetNumZeroed(InputFrames);
        for (const TArray<float>& Channel : ReferenceChannels)
        {
            for (int32 FrameIndex = 0; FrameIndex < InputFrames && FrameIndex < Channel.Num(); ++FrameIndex)
            {
                MonoInput[FrameIndex] += Channel[FrameIndex] / static_cast<float>(ReferenceChannels.Num());
            }
        }
        if (ReferenceChannels.Num() == 1)
        {
            TArray<float> RightReference = ReferenceChannels[0];
            ReferenceChannels.Add(MoveTemp(RightReference));
        }
        WetChannels.SetNum(2);
        for (int32 ChannelIndex = 0; ChannelIndex < 2; ++ChannelIndex)
        {
            WetChannels[ChannelIndex] = PartitionedConvolve(
                MonoInput,
                ExtractImpulseResponseChannel(
                    Request.ImpulseResponse,
                    Request.ImpulseResponseNumChannels,
                    ChannelIndex));
        }
    }
    else
    {
        WetChannels.SetNum(Request.NumChannels);
        for (int32 ChannelIndex = 0; ChannelIndex < Request.NumChannels; ++ChannelIndex)
        {
            WetChannels[ChannelIndex] = PartitionedConvolve(
                ReferenceChannels[ChannelIndex],
                Request.ImpulseResponse);
        }
    }
    for (TArray<float>& Channel : WetChannels)
    {
        for (float& Sample : Channel)
        {
            Sample *= Result.WetMix;
        }
    }

    Result.NumChannels = ReferenceChannels.Num();

    const int32 InputFrames = ReferenceChannels.IsEmpty() ? 0 : ReferenceChannels[0].Num();
    const int32 OutputFrames = WetChannels.IsEmpty() ? 0 : WetChannels[0].Num();
    if (InputFrames <= 0 || OutputFrames <= 0)
    {
        Result.Error = TEXT("Offline comparison produced no audio frames.");
        return Result;
    }

    TArray<TArray<float>> DirectChannels;
    TArray<TArray<float>> FullChannels;
    DirectChannels.SetNum(Result.NumChannels);
    FullChannels.SetNum(Result.NumChannels);
    for (int32 ChannelIndex = 0; ChannelIndex < Result.NumChannels; ++ChannelIndex)
    {
        ReferenceChannels[ChannelIndex].SetNumZeroed(OutputFrames);
        DirectChannels[ChannelIndex].SetNumZeroed(OutputFrames);
        FullChannels[ChannelIndex].SetNumZeroed(OutputFrames);
        for (int32 FrameIndex = 0; FrameIndex < InputFrames; ++FrameIndex)
        {
            DirectChannels[ChannelIndex][FrameIndex] =
                ReferenceChannels[ChannelIndex][FrameIndex] * Result.DirectGain;
        }
        for (int32 FrameIndex = 0; FrameIndex < OutputFrames; ++FrameIndex)
        {
            FullChannels[ChannelIndex][FrameIndex] =
                DirectChannels[ChannelIndex][FrameIndex] + WetChannels[ChannelIndex][FrameIndex];
        }
    }

    Result.NumFrames = OutputFrames;
    Result.DurationSeconds = static_cast<float>(InputFrames) / static_cast<float>(Request.OutputSampleRate);
    Result.ReferenceRms = CalculateRms(ReferenceChannels);
    Result.DirectRms = CalculateRms(DirectChannels);
    Result.WetRms = CalculateRms(WetChannels);
    Result.FullRms = CalculateRms(FullChannels);
    Result.DirectDryCorrelation = CalculateCorrelation(ReferenceChannels, DirectChannels, InputFrames);
    Result.FullDryCorrelation = CalculateCorrelation(ReferenceChannels, FullChannels, InputFrames);
    Result.WetDryCorrelation = CalculateCorrelation(ReferenceChannels, WetChannels, InputFrames);
    if (Result.ReferenceRms > UE_SMALL_NUMBER)
    {
        Result.DirectToReferenceRmsRatio = Result.DirectRms / Result.ReferenceRms;
        Result.WetToReferenceRmsRatio = Result.WetRms / Result.ReferenceRms;
        Result.FullToReferenceRmsRatio = Result.FullRms / Result.ReferenceRms;
    }
    Result.DirectWetNormalizedDifference = CalculateNormalizedDifference(
        DirectChannels,
        WetChannels,
        OutputFrames);
    Result.WetStereoNormalizedDifference = CalculateStereoNormalizedDifference(WetChannels);
    Result.bDirectionalWetIsDistinct = Result.ImpulseResponseNumChannels == 2
        && Result.WetStereoNormalizedDifference >= 0.01f;
    Result.bModesAreDistinct =
        Result.WetToReferenceRmsRatio
            >= MinimumAudibleWetToReferenceRmsRatio
        && FMath::Abs(Result.WetDryCorrelation) <= 0.98f
        && Result.DirectWetNormalizedDifference >= 0.05f;
    Result.bRecommendedInputDuration = Result.DurationSeconds >= 10.0f && Result.DurationSeconds <= 20.0f;
    Result.bSamplesFinite = AreSamplesFinite(ReferenceChannels)
        && AreSamplesFinite(DirectChannels)
        && AreSamplesFinite(WetChannels)
        && AreSamplesFinite(FullChannels);
    if (!Result.bSamplesFinite)
    {
        Result.Error = TEXT("Offline comparison produced a non-finite audio sample.");
        return Result;
    }
    Result.PeakBeforeScale = FMath::Max(
        FMath::Max(CalculatePeak(ReferenceChannels), CalculatePeak(DirectChannels)),
        FMath::Max(CalculatePeak(WetChannels), CalculatePeak(FullChannels)));
    Result.CommonOutputScale = Result.PeakBeforeScale > 0.99f
        ? 0.99f / Result.PeakBeforeScale
        : 1.0f;
    Result.PostScalePeak = Result.PeakBeforeScale * Result.CommonOutputScale;
    Result.ClippedSampleCount = CountClippedSamples(ReferenceChannels, Result.CommonOutputScale)
        + CountClippedSamples(DirectChannels, Result.CommonOutputScale)
        + CountClippedSamples(WetChannels, Result.CommonOutputScale)
        + CountClippedSamples(FullChannels, Result.CommonOutputScale);
    Result.bDirectDropoutCheckApplicable = Result.DirectGain > 1.0e-4f;
    if (Result.bDirectDropoutCheckApplicable)
    {
        CountDirectDropoutWindows(
            ReferenceChannels,
            DirectChannels,
            InputFrames,
            Request.OutputSampleRate,
            Result.ReferenceRms,
            Result.DirectGain,
            Result.DirectActiveWindowCount,
            Result.DirectDropoutWindowCount);
    }
    Result.DirectModelResidualRms = CalculateDirectModelResidualRms(
        ReferenceChannels,
        DirectChannels,
        Result.DirectGain);
    Result.FullMixResidualRms = CalculateFullMixResidualRms(
        DirectChannels,
        WetChannels,
        FullChannels);
    Result.MaxDirectDiscontinuityResidual = CalculateMaxDirectDiscontinuityResidual(
        ReferenceChannels,
        DirectChannels,
        Result.DirectGain);
    Result.bAudioSafetyChecksPassed = Result.bSamplesFinite
        && Result.ClippedSampleCount == 0
        && Result.PostScalePeak <= 0.99001f
        && (!Result.bDirectDropoutCheckApplicable || Result.DirectDropoutWindowCount == 0)
        && Result.DirectModelResidualRms <= 1.0e-6f
        && Result.FullMixResidualRms <= 1.0e-6f
        && Result.MaxDirectDiscontinuityResidual <= 1.0e-5f;

    const bool bHardOccluded = Result.DirectPreset.Equals(TEXT("hard_occluded"), ESearchCase::IgnoreCase);
    Result.bDirectSemanticsPassed = bHardOccluded
        ? Result.DirectGain <= 1.0e-4f
            && Result.DirectToReferenceRmsRatio <= 1.0e-4f
            && Result.DirectVisibility <= 0.05f
            && Result.DirectOcclusion <= 0.01f
        : Result.DirectDryCorrelation >= 0.99f
            && Result.DirectToReferenceRmsRatio >= 0.05f;
    const bool bFullSemanticsPassed = bHardOccluded
        ? Result.FullToReferenceRmsRatio >= 0.001f
        : Result.FullDryCorrelation >= 0.35f
            && Result.FullToReferenceRmsRatio >= 0.05f;
    Result.bAutomaticChecksPassed = Result.ReferenceRms > 1.0e-4f
        && Result.bDirectSemanticsPassed
        && bFullSemanticsPassed
        && Result.bModesAreDistinct
        && Result.bAudioSafetyChecksPassed
        && FMath::IsFinite(Result.PeakBeforeScale)
        && Result.PeakBeforeScale > 0.0f;

    if (!IFileManager::Get().MakeDirectory(*Request.OutputDirectory, true))
    {
        Result.Error = FString::Printf(TEXT("Could not create output directory: %s"), *Request.OutputDirectory);
        return Result;
    }

    const FString SafePrefix = FPaths::MakeValidFileName(Request.OutputFilenamePrefix);
    Result.ReferenceWaveFilename = FPaths::Combine(Request.OutputDirectory, SafePrefix + TEXT("_Reference.wav"));
    Result.DirectWaveFilename = FPaths::Combine(Request.OutputDirectory, SafePrefix + TEXT("_Direct.wav"));
    Result.WetWaveFilename = FPaths::Combine(Request.OutputDirectory, SafePrefix + TEXT("_Wet.wav"));
    Result.FullWaveFilename = FPaths::Combine(Request.OutputDirectory, SafePrefix + TEXT("_Full.wav"));
    Result.ManifestFilename = FPaths::Combine(Request.OutputDirectory, SafePrefix + TEXT("_Manifest.json"));

    if (!SaveWave(ReferenceChannels, Request.OutputSampleRate, Result.CommonOutputScale, Result.ReferenceWaveFilename)
        || !SaveWave(DirectChannels, Request.OutputSampleRate, Result.CommonOutputScale, Result.DirectWaveFilename)
        || !SaveWave(WetChannels, Request.OutputSampleRate, Result.CommonOutputScale, Result.WetWaveFilename)
        || !SaveWave(FullChannels, Request.OutputSampleRate, Result.CommonOutputScale, Result.FullWaveFilename))
    {
        Result.Error = FString::Printf(TEXT("Could not write one or more comparison WAV files to: %s"), *Request.OutputDirectory);
        return Result;
    }

    if (!SaveManifest(Request, Result))
    {
        Result.Error = FString::Printf(TEXT("Could not write comparison manifest: %s"), *Result.ManifestFilename);
        return Result;
    }

    Result.bSucceeded = true;
    return Result;
}
