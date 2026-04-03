#pragma once

#include "Modules/ModuleManager.h"

class UERAYTRACINGAUDIOEDITOR_API FUERayTracingAudioEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenus();
    TSharedRef<class SDockTab> SpawnBakeTab(const class FSpawnTabArgs& SpawnTabArgs);
};
