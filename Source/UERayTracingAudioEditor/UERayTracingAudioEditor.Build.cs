using UnrealBuildTool;

public class UERayTracingAudioEditor : ModuleRules
{
    public UERayTracingAudioEditor(ReadOnlyTargetRules Target) : base(Target)
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
            "InputCore",
            "LevelEditor",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "UERayTracingAudio",
            "UERayTracingAudioSDK",
            "UnrealEd",
            "WorkspaceMenuStructure"
        });
    }
}
