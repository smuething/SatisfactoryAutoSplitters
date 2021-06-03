#include "AutoSplittersModule.h"

#include "Patching/NativeHookManager.h"

#include "FGWorldSettings.h"
#include "UI/FGPopupWidget.h"
#include "FGPlayerController.h"
#include "FGBlueprintFunctionLibrary.h"
#include "FGBuildableSubsystem.h"
#include "Buildables/MFGBuildableAutoSplitter.h"
#include "Hologram/MFGAutoSplitterHologram.h"
#include "AutoSplittersLog.h"
#include "AutoSplitters_ConfigStruct.h"
#include "Registry/ModContentRegistry.h"
#include "Resources/FGBuildingDescriptor.h"

// #pragma optimize( "", off )

void FAutoSplittersModule::StartupModule()
{

#if UE_BUILD_SHIPPING

	auto UpgradeHook = [](auto& Call, UObject* self, AActor* newActor)
	{

		if (!newActor->HasAuthority())
			return;

		UE_LOG(LogAutoSplitters,Display,TEXT("Entered hook for IFGDismantleInterface::Execute_Upgrade()"));

		AMFGBuildableAutoSplitter* Target = Cast<AMFGBuildableAutoSplitter>(newActor);
		if (!Target)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("Target is not an AMFGBuildableAutoSplitter, bailing out"));
			return;
		}
		AFGBuildableAttachmentSplitter* Source = Cast<AFGBuildableAttachmentSplitter>(self);
		if (!Source)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("self is not an AFGBuildableAttachmentSplitter, bailing out"));
			return;
		}

		UE_LOG(LogAutoSplitters,Display,TEXT("Cancelling original call"));
		Call.Cancel();
	};

	SUBSCRIBE_METHOD(IFGDismantleInterface::Execute_Upgrade,UpgradeHook);


	auto NotifyBeginPlayHook = [&](AFGWorldSettings* WorldSettings)
	{
		if (!WorldSettings->HasAuthority())
			return;

		if (mPreUpgradeSplitters.Num() == 0)
			return;

		const auto Config = FAutoSplitters_ConfigStruct::GetActiveConfig();

		UE_LOG(LogAutoSplitters,Display,TEXT("Found %d pre-upgrade AutoSplitters while loading savegame"),mPreUpgradeSplitters.Num());

		if (Config.Upgrade.RemoveAllConveyors)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("User has chosen nuclear upgrade option of removing all conveyors attached to Auto Splitters"));
		}

		auto World = WorldSettings->GetWorld();

		auto BuildableSubSystem = AFGBuildableSubsystem::Get(World);

		auto ModContentRegistry = AModContentRegistry::Get(World);

		UFGRecipe* AutoSplitterRecipe = nullptr;
		for (auto& RecipeInfo : ModContentRegistry->GetRegisteredRecipes())
		{
			if (RecipeInfo.OwnedByModReference != FName("AutoSplitters"))
				continue;
			auto Recipe = Cast<UFGRecipe>(RecipeInfo.RegisteredObject->GetDefaultObject());
			if (Recipe->mProduct.Num() != 1)
				continue;
			auto BuildingDescriptor = Recipe->mProduct[0].ItemClass->GetDefaultObject<UFGBuildingDescriptor>();
			if (!BuildingDescriptor)
				continue;

			UE_LOG(LogAutoSplitters,Display,TEXT("Found building descriptor: %s"),*BuildingDescriptor->GetClass()->GetName());

			if (UFGBuildingDescriptor::GetBuildableClass(BuildingDescriptor->GetClass())->IsChildOf(AMFGBuildableAutoSplitter::StaticClass()))
			{
				UE_LOG(LogAutoSplitters,Display,TEXT("Found AutoSplitter recipe to use for rebuilt splitters"));
				AutoSplitterRecipe = Recipe;
				break;
			}
		}

		if (!AutoSplitterRecipe)
		{
			UE_LOG(LogAutoSplitters,Fatal,TEXT("Error: Could not find AutoSplitter recipe, unable to upgrade old Autosplitters"));
		}

		TSet<AFGBuildableConveyorBase*> Conveyors;
		for (auto& [Splitter,PreUpgradeComponents,ConveyorConnections] : mPreUpgradeSplitters)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("Replacing AutoSplitter %s"),*Splitter->GetName());

			auto Location = Splitter->GetActorLocation();
			auto Transform = Splitter->GetTransform();
			IFGDismantleInterface::Execute_Dismantle(Splitter);

			for (auto Component : PreUpgradeComponents)
			{
				Component->DestroyComponent();
			}

			UE_LOG(LogAutoSplitters,Display,TEXT("Creating and setting up hologram"),);

			auto Hologram = Cast<AMFGAutoSplitterHologram>(
				AFGHologram::SpawnHologramFromRecipe(
					AutoSplitterRecipe->GetClass(),
					World->GetFirstPlayerController(),
					Location
				)
			);

			Hologram->SetActorTransform(Transform);
			if (Config.Upgrade.RemoveAllConveyors)
			{
				for (auto Connection : ConveyorConnections)
				{
					auto Conveyor = Cast<AFGBuildableConveyorBase>(Connection->GetOuterBuildable());
					if (!Conveyor)
					{
						UE_LOG(LogAutoSplitters,Warning,TEXT("Found something connected to a splitter that is not a conveyor: %s"),
							*Connection->GetOuterBuildable()->GetClass()->GetName())
						break;
					}
					Conveyors.Add(Conveyor);
				}
			}
			else
			{
				Hologram->mPreUpgradeConnections = ConveyorConnections;
			}

			UE_LOG(LogAutoSplitters,Display,TEXT("Spawning Splitter through hologram"));
			TArray<AActor*> Children;
			auto Actor = Hologram->Construct(Children,BuildableSubSystem->GetNewNetConstructionID());
			UE_LOG(LogAutoSplitters,Display,TEXT("Destroying Hologram"));
			Hologram->Destroy();

		}

		if (Config.Upgrade.RemoveAllConveyors)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("Dismantling %d attached conveyors"),Conveyors.Num());
			for (auto Conveyor : Conveyors)
			{
				IFGDismantleInterface::Execute_Dismantle(Conveyor);
			}
		}

		if (Config.Upgrade.ShowWarningMessage)
		{
			FString Str;

			if (Config.Upgrade.RemoveAllConveyors)
			{
				Str = FString::Printf(TEXT(
					"Your savegame contained %d Auto Splitters created with versions of the mod older than 0.3.0, "
					"which connect to the attached conveyors in a wrong way. The mod has replaced these Auto Splitters with new ones, but because "
					"you have selected the mod configuration option \"Remove Conveyors\", all conveyors attached to Auto Splitters have been dismantled. "
					"\n\nAll replaced splitters have been reset to fully automatic mode."
					"\n\nA total of %d conveyors have been removed."), mPreUpgradeSplitters.Num(), Conveyors.Num());
			}
			else
			{
				Str = FString::Printf(TEXT(
					"Your savegame contained %d Auto Splitters created with versions of the mod older than 0.3.0, "
					"which connect to the attached conveyors in a wrong way. The mod has replaced these Auto Splitters with new ones. "
					"\n\nUnfortunately, it is not possible to carry over any manual settings for those splitters, and they are now all in fully automatic mode"),
				                      mPreUpgradeSplitters.Num());
			}

			AFGPlayerController* LocalController = UFGBlueprintFunctionLibrary::GetLocalPlayerController(
				WorldSettings->GetWorld());

			FPopupClosed CloseDelegate;

			UFGBlueprintFunctionLibrary::AddPopupWithCloseDelegate(LocalController,
			                                                       FText::FromString(
				                                                       "Savegame upgraded to AutoSplitters 0.3.x"),
			                                                       FText::FromString(Str), CloseDelegate);
		}


		mPreUpgradeSplitters.Empty();
	};


	void* SampleInstance = GetMutableDefault<AFGWorldSettings>();

	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGWorldSettings::NotifyBeginPlay,SampleInstance,NotifyBeginPlayHook);

#endif // UE_BUILD_SHIPPING

}

// #pragma optimize( "", on )

IMPLEMENT_GAME_MODULE(FAutoSplittersModule,AutoSplitters);
