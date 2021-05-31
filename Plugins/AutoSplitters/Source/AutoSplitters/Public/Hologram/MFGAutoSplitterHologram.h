// ILikeBanas

#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGAttachmentSplitterHologram.h"
#include "MFGAutoSplitterHologram.generated.h"

/**
 * 
 */
UCLASS()
class AUTOSPLITTERS_API AMFGAutoSplitterHologram : public AFGAttachmentSplitterHologram
{
	GENERATED_BODY()

protected:

	virtual void ConfigureComponents( class AFGBuildable* inBuildable ) const override;
	
};
