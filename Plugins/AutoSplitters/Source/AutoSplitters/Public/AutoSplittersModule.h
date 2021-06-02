#pragma once

#include <Buildables/FGBuildableConveyorBase.h>
#include <FGFactoryConnectionComponent.h>

// define to 1 to get more debug output to console when the debug flag is set in the splitter UI
#define AUTO_SPLITTERS_DEBUG 1
#define AUTO_SPLITTERS_DELAY_UNTIL_READY 1

class FAutoSplittersModule : public IModuleInterface
{
	friend class AMFGBuildableAutoSplitter;

	int32 mUpgradedSplitters = 0;
	TSet<AFGBuildableConveyorBase*> mBrokenConveyors;
	TArray<UFGFactoryConnectionComponent*> mOldBlueprintConnections;
	TArray<std::pair<UFGFactoryConnectionComponent*,UFGFactoryConnectionComponent*>> mBrokenConnectionPairs;
	TArray<std::pair<UFGFactoryConnectionComponent*,UFGFactoryConnectionComponent*>> mPendingConnectionPairs;

public:
	virtual void StartupModule() override;
};
