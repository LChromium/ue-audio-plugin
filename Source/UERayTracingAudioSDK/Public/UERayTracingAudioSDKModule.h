#pragma once

#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUERayTracingAudioSDK, Log, All);

class UERAYTRACINGAUDIOSDK_API FUERayTracingAudioSDKModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
