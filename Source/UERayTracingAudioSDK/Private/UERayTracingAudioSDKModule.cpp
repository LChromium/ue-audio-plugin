#include "UERayTracingAudioSDKModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogUERayTracingAudioSDK);

IMPLEMENT_MODULE(FUERayTracingAudioSDKModule, UERayTracingAudioSDK)

void FUERayTracingAudioSDKModule::StartupModule()
{
    if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UERayTracingAudio")))
    {
        AddShaderSourceDirectoryMapping(TEXT("/Plugin/UERayTracingAudio"), FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders")));
    }
}

void FUERayTracingAudioSDKModule::ShutdownModule()
{
}
