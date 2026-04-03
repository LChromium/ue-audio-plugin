using UnrealBuildTool;

public class UERayTracingAudioSDK : ModuleRules
{
    public UERayTracingAudioSDK(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "CoreUObject",
            "Engine",
            "Renderer",
            "RenderCore",
            "RHI"
        });
    }
}
