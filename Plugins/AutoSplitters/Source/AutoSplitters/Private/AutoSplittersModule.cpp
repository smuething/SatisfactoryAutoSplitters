#include "AutoSplittersModule.h"

#include "Patching/NativeHookManager.h"

#include "Buildables/FGBuildableAttachmentSplitter.h"
#include "Buildables/MFGBuildableAutoSplitter.h"

#include "AutoSplittersLog.h"

#pragma optimize( "", off )

void FAutoSplittersModule::StartupModule()
{

#if UE_BUILD_SHIPPING

	void* Instance = GetMutableDefault<AFGBuildableAttachmentSplitter>();
	
	UE_LOG(LogAutoSplitters,Display,TEXT("Instance ptr is %d"),Instance);

	/*
	SUBSCRIBE_METHOD_VIRTUAL(AFGBuildableAttachmentSplitter::Upgrade_Implementation,Instance,[](auto& Call, AFGBuildableAttachmentSplitter* self, AActor* newActor)
	{
		UE_LOG(LogAutoSplitters,Display,TEXT("Hook was hit"));
		Call(self,newActor);
	});
	*/

	/*
	SUBSCRIBE_METHOD_VIRTUAL(AFGBuildableAttachmentSplitter::FillDistributionTable,Instance,[](auto& Call, AFGBuildableAttachmentSplitter* self, float dt)
	{
		UE_LOG(LogAutoSplitters,Display,TEXT("Hook was hit"));
		Call(self,newActor);
	});
	*/

#endif // UE_BUILD_SHIPPING

}

#pragma optimize( "", on )

IMPLEMENT_GAME_MODULE(FAutoSplittersModule,AutoSplitters);