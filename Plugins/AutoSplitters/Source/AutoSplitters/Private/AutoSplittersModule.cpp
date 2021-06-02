#include "AutoSplittersModule.h"

#include "Patching/NativeHookManager.h"

#include "FGWorldSettings.h"
#include "UI/FGPopupWidget.h"
#include "FGPlayerController.h"
#include "FGBlueprintFunctionLibrary.h"
#include "Buildables/MFGBuildableAutoSplitter.h"
#include "AutoSplittersRCO.h"
#include "AutoSplittersLog.h"

#include "AutoSplittersLog.h"
#include "AutoSplitters_ConfigStruct.h"

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

		//UE_LOG(LogAutoSplitters,Display,TEXT("Calling UpgradeFromSplitter()"));
		//Target->UpgradeFromSplitter(*Source);

		UE_LOG(LogAutoSplitters,Display,TEXT("Cancelling original call"));
		Call.Cancel();
	};

	SUBSCRIBE_METHOD(IFGDismantleInterface::Execute_Upgrade,UpgradeHook);

	auto NotifyBeginPlayHook = [&](AFGWorldSettings* WorldSettings)
	{
		if (!WorldSettings->HasAuthority())
			return;

		if (mUpgradedSplitters == 0)
			return;

		const auto Config = FAutoSplitters_ConfigStruct::GetActiveConfig();

		UE_LOG(LogAutoSplitters,Display,TEXT("Upgraded %d AutoSplitters while loading savegame"),mUpgradedSplitters);

		if (Config.Upgrade.RemoveAllConveyors)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("User has chosen nuclear upgrade option of removing all conveyors attached to Auto Splitters"));
		}

		UE_LOG(LogAutoSplitters,Display,TEXT("Disconnecting %d factory connection pairs"),mBrokenConnectionPairs.Num());

		for (auto [first,second] : mBrokenConnectionPairs)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("Disconnecting %p (%s) and %p (%s) distance=%f"),
				first,*first->GetComponentLocation().ToString(),
				second,*second->GetComponentLocation().ToString(),
				FVector::Dist(first->GetComponentLocation(),second->GetComponentLocation())
				);

			second->ClearConnection();
		}

		UE_LOG(LogAutoSplitters,Display,TEXT("Disconnected %d factory connection pairs"),mBrokenConnectionPairs.Num());

		if (!Config.Upgrade.RemoveAllConveyors)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("Reconnecting %d factory connection pairs"),mPendingConnectionPairs.Num());

			TMap<UFGFactoryConnectionComponent*,std::pair<UFGFactoryConnectionComponent*,UFGFactoryConnectionComponent*>> Components;

			for (auto [first,second] : mPendingConnectionPairs)
			{
				UE_LOG(LogAutoSplitters,Display,TEXT("Reconnecting %p (%s) and %p (%s) distance=%f"),
					first,*first->GetComponentLocation().ToString(),
					second,*second->GetComponentLocation().ToString(),
					FVector::Dist(first->GetComponentLocation(),second->GetComponentLocation())
					);

				if (auto Pair = Components.Find(first); Pair != nullptr)
				{
					UE_LOG(LogAutoSplitters,Error,TEXT("Component %p also connects to (%p,%p)"),first,Pair->first,Pair->second);
				}
				Components.FindOrAdd(first) = {first,second};

				if (auto Pair = Components.Find(second); Pair != nullptr)
				{
					UE_LOG(LogAutoSplitters,Error,TEXT("Component %p also connects to (%p,%p)"),second,Pair->first,Pair->second);
				}
				Components.FindOrAdd(second) = {first,second};

				if (!((first->GetDirection() == EFactoryConnectionDirection::FCD_INPUT && second->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT) ||
					(first->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT && second->GetDirection() == EFactoryConnectionDirection::FCD_INPUT)))
				{
					auto FirstDirection = EnumToFString("EFactoryConnectionDirection",static_cast<int32>(first->GetDirection()));
					auto SecondDirection = EnumToFString("EFactoryConnectionDirection",static_cast<int32>(second->GetDirection()));
					UE_LOG(LogAutoSplitters,Error,TEXT("Components have inconsistent directions: %s and %s"),*FirstDirection,*SecondDirection);
				}

				first->SetConnection(second);
			}

			UE_LOG(LogAutoSplitters,Display,TEXT("Reconnected %d factory connection pairs"),mPendingConnectionPairs.Num());
		}

		UE_LOG(LogAutoSplitters,Display,TEXT("Destroying %d connection components left over from old blueprints"),mOldBlueprintConnections.Num());

		for (auto Connection : mOldBlueprintConnections)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("Destroying old component %s of splitter %p at %s"),
				*Connection->GetName(),
				Connection->GetAttachParent(),
				*Connection->GetComponentLocation().ToString()
				);

			Connection->DestroyComponent(false);
		}

		UE_LOG(LogAutoSplitters,Display,TEXT("Destroyed %d connection components left over from old blueprints"),mOldBlueprintConnections.Num());


		if (mBrokenConveyors.Num() > 0)
		{

			UE_LOG(LogAutoSplitters,Warning,TEXT("AutoSplitters Mod Upgrade: Dismantling %d Conveyors"),mBrokenConveyors.Num());

			for (auto Conveyor : mBrokenConveyors)
			{
				UE_LOG(LogAutoSplitters,Display,TEXT("Dismantling conveyor %p (%s) at %s"),Conveyor,*Conveyor->GetName(),*Conveyor->GetActorLocation().ToString());
				IFGDismantleInterface::Execute_Dismantle(Conveyor);
			}

			UE_LOG(LogAutoSplitters,Warning,TEXT("AutoSplitters Mod Upgrade: Dismantled %d Conveyors"),mBrokenConveyors.Num());

			if (Config.Upgrade.ShowWarningMessage)
			{
				FString Str;

				if (Config.Upgrade.RemoveAllConveyors)
				{
					Str = FString::Printf(TEXT("Your savegame contained Auto Splitters created with versions of the mod older than 0.3.0, "\
					"which connect to the attached conveyors in a wrong way. The mod has upgraded these Auto Splitters, but because you have selected the "\
					"mod configuration option \"Remove Conveyors\", all conveyors attached to Auto Splitters have been dismantled. "\
					"\n\nA total of %d conveyors have been removed."),mBrokenConveyors.Num());
				}
				else
				{
					Str = FString::Printf(TEXT("Your savegame contained Auto Splitters created with versions of the mod older than 0.3.0, "\
					"which connect to the attached conveyors in a wrong way. The mod has upgraded these Auto Splitters, but some connections could "\
					"not be repaired.\n\n WARNING: Those conveyors have been dismantled to make it easy for you to find the broken splitters.\n\nA total "\
					"of %d conveyors have been removed."),mBrokenConveyors.Num());
				}

				AFGPlayerController* LocalController = UFGBlueprintFunctionLibrary::GetLocalPlayerController(WorldSettings->GetWorld());

				FPopupClosed CloseDelegate;

				UFGBlueprintFunctionLibrary::AddPopupWithCloseDelegate(LocalController,FText::FromString("Savegame upgraded to AutoSplitters 0.3.x"),FText::FromString(Str),CloseDelegate);
			}
		}

		mUpgradedSplitters = 0;
		mBrokenConveyors.Empty();
		mOldBlueprintConnections.Empty();
		mBrokenConnectionPairs.Empty();
		mPendingConnectionPairs.Empty();
	};

	void* SampleInstance = GetMutableDefault<AFGWorldSettings>();

	SUBSCRIBE_METHOD_VIRTUAL_AFTER(AFGWorldSettings::NotifyBeginPlay,SampleInstance,NotifyBeginPlayHook);

#endif // UE_BUILD_SHIPPING

}

// #pragma optimize( "", on )

IMPLEMENT_GAME_MODULE(FAutoSplittersModule,AutoSplitters);
