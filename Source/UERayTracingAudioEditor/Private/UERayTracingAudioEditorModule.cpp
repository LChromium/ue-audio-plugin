#include "UERayTracingAudioEditorModule.h"

#include "LevelEditor.h"
#include "Misc/MessageDialog.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

static const FName UERayTracingAudioBakeTabName(TEXT("UERayTracingAudioBake"));

IMPLEMENT_MODULE(FUERayTracingAudioEditorModule, UERayTracingAudioEditor)

void FUERayTracingAudioEditorModule::StartupModule()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        UERayTracingAudioBakeTabName,
        FOnSpawnTab::CreateRaw(this, &FUERayTracingAudioEditorModule::SpawnBakeTab))
        .SetDisplayName(FText::FromString(TEXT("UE Ray Tracing Audio Bake")))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory());

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUERayTracingAudioEditorModule::RegisterMenus));
}

void FUERayTracingAudioEditorModule::ShutdownModule()
{
    UToolMenus::UnRegisterStartupCallback(this);

    if (UToolMenus::TryGet())
    {
        UToolMenus::UnregisterOwner(this);
    }

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UERayTracingAudioBakeTabName);
}

void FUERayTracingAudioEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
    FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
    Section.AddMenuEntry(
        "UERayTracingAudioBakeWindow",
        FText::FromString(TEXT("UE Ray Tracing Audio Bake")),
        FText::FromString(TEXT("Open the bake workflow for UE Ray Tracing Audio.")),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FGlobalTabmanager::Get()->TryInvokeTab(UERayTracingAudioBakeTabName);
        })));
}

TSharedRef<SDockTab> FUERayTracingAudioEditorModule::SpawnBakeTab(const FSpawnTabArgs& SpawnTabArgs)
{
    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SBorder)
            .Padding(16.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(TEXT("Phase 1 bake window skeleton is ready. Phase 3 and 4 can extend this tab with scene export, probe generation, and bake task controls.")))
                .AutoWrapText(true)
            ]
        ];
}
