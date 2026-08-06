#include "Listening/UERayTracingAudioHumanAcceptance.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
    constexpr const TCHAR* AcceptanceRequirement =
        TEXT("Direct and Full retain recognizable source content; Wet is spatial tail only and audibly distinct; moving occlusion and runtime mode switching have no click/pop, dropout, noise, or timing jump; environment differences are reasonable.");

    bool TryReadRequiredString(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* Field,
        FString& OutValue,
        FString& OutError)
    {
        if (!Object.IsValid()
            || !Object->TryGetStringField(Field, OutValue)
            || OutValue.TrimStartAndEnd().IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("Comparison manifest requires non-empty %s."),
                Field);
            return false;
        }
        return true;
    }

    bool TryReadRequiredBool(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* Field,
        bool& OutValue,
        FString& OutError)
    {
        if (!Object.IsValid() || !Object->TryGetBoolField(Field, OutValue))
        {
            OutError = FString::Printf(
                TEXT("Comparison manifest requires boolean %s."),
                Field);
            return false;
        }
        return true;
    }

    bool TryReadRequiredNumber(
        const TSharedPtr<FJsonObject>& Object,
        const TCHAR* Field,
        double& OutValue,
        FString& OutError)
    {
        if (!Object.IsValid() || !Object->TryGetNumberField(Field, OutValue))
        {
            OutError = FString::Printf(
                TEXT("Comparison manifest requires numeric %s."),
                Field);
            return false;
        }
        return true;
    }

    void AddPreviewMode(
        const bool bPreviewed,
        const TCHAR* Name,
        TArray<TSharedPtr<FJsonValue>>& OutModes)
    {
        if (bPreviewed)
        {
            OutModes.Add(MakeShared<FJsonValueString>(Name));
        }
    }

    bool ContainsPreviewMode(
        const TArray<TSharedPtr<FJsonValue>>& Modes,
        const FString& Name)
    {
        return Modes.ContainsByPredicate(
            [&Name](const TSharedPtr<FJsonValue>& Value)
            {
                FString Mode;
                return Value.IsValid()
                    && Value->TryGetString(Mode)
                    && Mode == Name;
            });
    }
}

bool FUERayTracingAudioHumanAcceptance::BuildRecordJson(
    const FString& ComparisonManifestFilename,
    const FString& ComparisonManifestJson,
    const FUERayTracingAudioHumanAcceptanceRequest& Request,
    FString& OutRecordJson,
    FString& OutError)
{
    OutRecordJson.Reset();
    OutError.Reset();

    FString TargetDevice = Request.TargetListeningDevice;
    TargetDevice.TrimStartAndEndInline();
    if (TargetDevice.IsEmpty())
    {
        OutError = TEXT("Enter the target headphones or speakers before recording Human Pass/Fail.");
        return false;
    }
    if (Request.RecordedAtUtc.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("Human acceptance requires a UTC recording timestamp.");
        return false;
    }
    if (ComparisonManifestFilename.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("Human acceptance requires a comparison manifest filename.");
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> PreviewedModes;
    AddPreviewMode(Request.bPreviewedReference, TEXT("Reference"), PreviewedModes);
    AddPreviewMode(Request.bPreviewedDirect, TEXT("Direct"), PreviewedModes);
    AddPreviewMode(Request.bPreviewedWet, TEXT("Wet"), PreviewedModes);
    AddPreviewMode(Request.bPreviewedFull, TEXT("Full"), PreviewedModes);
    if (PreviewedModes.IsEmpty())
    {
        OutError = TEXT("Preview at least one comparison mode before recording Human Pass/Fail.");
        return false;
    }
    if (Request.bHumanListeningPassed
        && (!Request.bPreviewedReference
            || !Request.bPreviewedDirect
            || !Request.bPreviewedWet
            || !Request.bPreviewedFull))
    {
        TArray<FString> MissingModes;
        if (!Request.bPreviewedReference)
        {
            MissingModes.Add(TEXT("Reference"));
        }
        if (!Request.bPreviewedDirect)
        {
            MissingModes.Add(TEXT("Direct"));
        }
        if (!Request.bPreviewedWet)
        {
            MissingModes.Add(TEXT("Wet"));
        }
        if (!Request.bPreviewedFull)
        {
            MissingModes.Add(TEXT("Full"));
        }
        OutError = FString::Printf(
            TEXT("Human Pass requires previewing all four modes; missing: %s."),
            *FString::Join(MissingModes, TEXT(", ")));
        return false;
    }
    if (Request.bHumanListeningPassed
        && (!Request.bRecognizableDirectConfirmed
            || !Request.bAudibleWetFullDifferenceConfirmed
            || !Request.bMovingOcclusionContinuityConfirmed
            || !Request.bModeSwitchingContinuityConfirmed
            || !Request.bEnvironmentDifferenceConfirmed))
    {
        TArray<FString> MissingConfirmations;
        if (!Request.bRecognizableDirectConfirmed)
        {
            MissingConfirmations.Add(TEXT("recognizable_direct"));
        }
        if (!Request.bAudibleWetFullDifferenceConfirmed)
        {
            MissingConfirmations.Add(TEXT("audible_wet_full_difference"));
        }
        if (!Request.bMovingOcclusionContinuityConfirmed)
        {
            MissingConfirmations.Add(TEXT("moving_occlusion_continuity"));
        }
        if (!Request.bModeSwitchingContinuityConfirmed)
        {
            MissingConfirmations.Add(TEXT("mode_switching_continuity"));
        }
        if (!Request.bEnvironmentDifferenceConfirmed)
        {
            MissingConfirmations.Add(TEXT("environment_difference"));
        }
        OutError = FString::Printf(
            TEXT("Human Pass requires every structured human confirmation; missing: %s."),
            *FString::Join(MissingConfirmations, TEXT(", ")));
        return false;
    }
    if (Request.LastPreviewedMode.TrimStartAndEnd().IsEmpty()
        || !ContainsPreviewMode(PreviewedModes, Request.LastPreviewedMode))
    {
        OutError = TEXT("The last previewed mode must be present in previewed_modes.");
        return false;
    }

    TSharedPtr<FJsonObject> Manifest;
    const TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(ComparisonManifestJson);
    if (!FJsonSerializer::Deserialize(Reader, Manifest) || !Manifest.IsValid())
    {
        OutError = TEXT("Comparison manifest is not a valid JSON object.");
        return false;
    }

    FString InputAsset;
    FString SourceActor;
    FString ListenerActor;
    FString SceneSignature;
    FString DirectPreset;
    FString ReflectionEnvironment;
    bool bAutomaticChecksPassed = false;
    bool bModesAreDistinct = false;
    double DirectToReferenceRmsRatio = 0.0;
    double WetToReferenceRmsRatio = 0.0;
    double DirectWetNormalizedDifference = 0.0;
    if (!TryReadRequiredString(Manifest, TEXT("input_asset"), InputAsset, OutError)
        || !TryReadRequiredString(Manifest, TEXT("source_actor"), SourceActor, OutError)
        || !TryReadRequiredString(Manifest, TEXT("listener_actor"), ListenerActor, OutError)
        || !TryReadRequiredString(Manifest, TEXT("scene_signature"), SceneSignature, OutError)
        || !TryReadRequiredString(Manifest, TEXT("direct_preset"), DirectPreset, OutError)
        || !TryReadRequiredString(Manifest, TEXT("reflection_environment"), ReflectionEnvironment, OutError)
        || !TryReadRequiredBool(Manifest, TEXT("automatic_checks_passed"), bAutomaticChecksPassed, OutError)
        || !TryReadRequiredBool(Manifest, TEXT("modes_are_distinct"), bModesAreDistinct, OutError)
        || !TryReadRequiredNumber(Manifest, TEXT("direct_to_reference_rms_ratio"), DirectToReferenceRmsRatio, OutError)
        || !TryReadRequiredNumber(Manifest, TEXT("wet_to_reference_rms_ratio"), WetToReferenceRmsRatio, OutError)
        || !TryReadRequiredNumber(Manifest, TEXT("direct_wet_normalized_difference"), DirectWetNormalizedDifference, OutError))
    {
        return false;
    }

    const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schema_version"), SchemaVersion);
    Root->SetStringField(TEXT("recorded_at_utc"), Request.RecordedAtUtc);
    Root->SetStringField(
        TEXT("comparison_manifest"),
        FPaths::ConvertRelativePathToFull(ComparisonManifestFilename));
    Root->SetStringField(TEXT("target_listening_device"), TargetDevice);
    Root->SetStringField(TEXT("listening_notes"), Request.ListeningNotes);
    Root->SetStringField(TEXT("input_asset"), InputAsset);
    Root->SetStringField(TEXT("source_actor"), SourceActor);
    Root->SetStringField(TEXT("listener_actor"), ListenerActor);
    Root->SetStringField(TEXT("scene_signature"), SceneSignature);
    Root->SetStringField(TEXT("direct_preset"), DirectPreset);
    Root->SetStringField(TEXT("reflection_environment"), ReflectionEnvironment);
    Root->SetBoolField(TEXT("automatic_checks_passed"), bAutomaticChecksPassed);
    Root->SetBoolField(TEXT("human_listening_passed"), Request.bHumanListeningPassed);
    Root->SetArrayField(TEXT("previewed_modes"), PreviewedModes);
    Root->SetStringField(TEXT("last_previewed_mode"), Request.LastPreviewedMode);
    const TSharedRef<FJsonObject> HumanConfirmations = MakeShared<FJsonObject>();
    HumanConfirmations->SetBoolField(
        TEXT("recognizable_direct"),
        Request.bRecognizableDirectConfirmed);
    HumanConfirmations->SetBoolField(
        TEXT("audible_wet_full_difference"),
        Request.bAudibleWetFullDifferenceConfirmed);
    HumanConfirmations->SetBoolField(
        TEXT("moving_occlusion_continuity"),
        Request.bMovingOcclusionContinuityConfirmed);
    HumanConfirmations->SetBoolField(
        TEXT("mode_switching_continuity"),
        Request.bModeSwitchingContinuityConfirmed);
    HumanConfirmations->SetBoolField(
        TEXT("environment_difference"),
        Request.bEnvironmentDifferenceConfirmed);
    Root->SetObjectField(TEXT("human_confirmations"), HumanConfirmations);
    Root->SetNumberField(
        TEXT("direct_to_reference_rms_ratio"),
        DirectToReferenceRmsRatio);
    Root->SetNumberField(
        TEXT("wet_to_reference_rms_ratio"),
        WetToReferenceRmsRatio);
    Root->SetNumberField(
        TEXT("direct_wet_normalized_difference"),
        DirectWetNormalizedDifference);
    Root->SetBoolField(TEXT("modes_are_distinct"), bModesAreDistinct);
    Root->SetStringField(TEXT("requirement"), AcceptanceRequirement);

    const TSharedRef<TJsonWriter<>> Writer =
        TJsonWriterFactory<>::Create(&OutRecordJson);
    if (!FJsonSerializer::Serialize(Root, Writer))
    {
        OutRecordJson.Reset();
        OutError = TEXT("Could not serialize the human acceptance record.");
        return false;
    }
    return true;
}

bool FUERayTracingAudioHumanAcceptance::SaveRecord(
    const FString& ComparisonManifestFilename,
    const FUERayTracingAudioHumanAcceptanceRequest& Request,
    FString& OutRecordFilename,
    FString& OutError)
{
    OutRecordFilename.Reset();
    OutError.Reset();

    FString ManifestJson;
    if (!FFileHelper::LoadFileToString(
            ManifestJson,
            *ComparisonManifestFilename))
    {
        OutError = FString::Printf(
            TEXT("Could not read comparison manifest: %s"),
            *ComparisonManifestFilename);
        return false;
    }

    FString RecordJson;
    if (!BuildRecordJson(
            ComparisonManifestFilename,
            ManifestJson,
            Request,
            RecordJson,
            OutError))
    {
        return false;
    }

    const FString RecordFilename = FPaths::ChangeExtension(
        ComparisonManifestFilename,
        TEXT("HumanAcceptance.json"));
    const FString TemporaryFilename = RecordFilename + TEXT(".tmp");
    IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
    if (!FFileHelper::SaveStringToFile(
            RecordJson,
            *TemporaryFilename,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = FString::Printf(
            TEXT("Could not write temporary human acceptance record: %s"),
            *TemporaryFilename);
        return false;
    }
    if (!IFileManager::Get().Move(
            *RecordFilename,
            *TemporaryFilename,
            true,
            true))
    {
        IFileManager::Get().Delete(*TemporaryFilename, false, true, true);
        OutError = FString::Printf(
            TEXT("Could not publish human acceptance record: %s"),
            *RecordFilename);
        return false;
    }

    OutRecordFilename = RecordFilename;
    return true;
}
