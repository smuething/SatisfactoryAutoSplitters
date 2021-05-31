#include "AutoSplittersModule.h"

#include "Patching/NativeHookManager.h"

//#include "FGDismantleInterface.h"
//#include "Buildables/FGBuildableAttachmentSplitter.h"
#include "Buildables/MFGBuildableAutoSplitter.h"

#include "AutoSplittersLog.h"

#pragma optimize( "", off )

void FAutoSplittersModule::StartupModule()
{

#if UE_BUILD_SHIPPING
	
	auto UpgradeHook = [](auto& Call, UObject* self, AActor* newActor)
	{

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

#endif // UE_BUILD_SHIPPING

}

#pragma optimize( "", on )

IMPLEMENT_GAME_MODULE(FAutoSplittersModule,AutoSplitters);