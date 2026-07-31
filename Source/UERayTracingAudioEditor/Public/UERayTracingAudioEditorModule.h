#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"

class FUERayTracingAudioEditorArtifactRunner;

class UERAYTRACINGAUDIOEDITOR_API FUERayTracingAudioEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    bool TickValidationScene(float DeltaTime);
    TSharedRef<class SDockTab> SpawnBakeTab(const class FSpawnTabArgs& SpawnTabArgs);

    FTSTicker::FDelegateHandle ValidationSceneTickerHandle;
    TSharedPtr<FUERayTracingAudioEditorArtifactRunner> ValidationArtifactRunner;
    double ValidationSceneStartSeconds = 0.0;
};
