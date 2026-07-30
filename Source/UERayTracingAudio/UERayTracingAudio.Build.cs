using UnrealBuildTool;

public class UERayTracingAudio : ModuleRules
{
    public UERayTracingAudio(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "AudioExtensions",
            "DeveloperSettings",
            "UERayTracingAudioSDK"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AudioMixer",
            "CoreUObject",
            "Engine",
            "Projects",
            "RHI",
            "RenderCore",
            "Slate",
            "SlateCore"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }
    }
}
