using UnrealBuildTool;

public class UERayTracingAudio : ModuleRules
{
    public UERayTracingAudio(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDefinitions.Add(
            $"WITH_UERAYTRACINGAUDIO_VALIDATION={(Target.Configuration != UnrealTargetConfiguration.Shipping ? 1 : 0)}");

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
            "InputCore",
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
