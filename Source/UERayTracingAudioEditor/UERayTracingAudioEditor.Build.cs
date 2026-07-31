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
            "AssetRegistry",
            "AssetTools",
            "CoreUObject",
            "Engine",
            "InputCore",
            "Json",
            "LevelEditor",
            "PropertyEditor",
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
