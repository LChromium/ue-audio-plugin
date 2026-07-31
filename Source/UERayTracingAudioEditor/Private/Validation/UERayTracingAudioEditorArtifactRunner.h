#pragma once

#include "Bake/UERayTracingAudioOfflineRenderer.h"
#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class FUERayTracingAudioBakeJob;
class USoundWave;
class UUERayTracingAudioListenerComponent;
class UUERayTracingAudioSourceComponent;

struct FUERayTracingAudioEditorArtifactResult
{
    bool bSucceeded = false;
    FString Error;
    FString InputAssetPath;
    FString ImpulseResponseAssetPath;
    TArray<FString> ImportedComparisonAssetPaths;
    FUERayTracingAudioOfflineRenderResult OfflineRender;
};

class FUERayTracingAudioEditorArtifactRunner
{
public:
    bool Start(
        UUERayTracingAudioSourceComponent& Source,
        UUERayTracingAudioListenerComponent& Listener,
        USoundWave& InputSoundWave,
        const FString& DirectPreset,
        int32 ReflectionBounces,
        FString& OutError,
        const FString& ReflectionEnvironment = TEXT("enclosed"));

    void Tick();
    bool IsComplete() const;
    const FUERayTracingAudioEditorArtifactResult& GetResult() const;

private:
    void FinishBake();
    void SetFailed(FString Error);

    TSharedPtr<FUERayTracingAudioBakeJob> BakeJob;
    TWeakObjectPtr<UUERayTracingAudioSourceComponent> Source;
    TWeakObjectPtr<UUERayTracingAudioListenerComponent> Listener;
    TWeakObjectPtr<USoundWave> InputSoundWave;
    FString DirectPreset;
    FString ReflectionEnvironment;
    FUERayTracingAudioEditorArtifactResult Result;
    bool bComplete = false;
};
