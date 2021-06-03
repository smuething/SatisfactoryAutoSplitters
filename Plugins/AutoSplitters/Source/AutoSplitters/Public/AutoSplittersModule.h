#pragma once

// define to 1 to get more debug output to console when the debug flag is set in the splitter UI
#define AUTO_SPLITTERS_DEBUG 1
#define AUTO_SPLITTERS_DELAY_UNTIL_READY 0

#include <FGFactoryConnectionComponent.h>
#include "Buildables/MFGBuildableAutoSplitter.h"

class FAutoSplittersModule : public IModuleInterface
{
	friend class AMFGBuildableAutoSplitter;

	TArray<
		std::tuple<
			AMFGBuildableAutoSplitter*,
			TInlineComponentArray<UFGFactoryConnectionComponent*, 2>,
			TInlineComponentArray<UFGFactoryConnectionComponent*, 4>
		>
	> mPreUpgradeSplitters;

public:
	virtual void StartupModule() override;
};
