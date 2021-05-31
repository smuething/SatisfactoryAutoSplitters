// ILikeBanas

#include "Hologram/MFGAutoSplitterHologram.h"
#include "Buildables/MFGBuildableAutoSplitter.h"
#include "Buildables/FGBuildable.h"

#include "AutoSplittersLog.h"

void AMFGAutoSplitterHologram::ConfigureComponents(AFGBuildable* inBuildable) const
{

	AMFGBuildableAutoSplitter* Splitter = Cast<AMFGBuildableAutoSplitter>(inBuildable);

	if (IsUpgrade())
	{
		UE_LOG(LogAutoSplitters,Display,TEXT("Upgrading, working around broken connection handling of upstream hologram"));
		std::array<UFGFactoryConnectionComponent*,4> SnappedConnections;
		for (int i = 0 ; i < 4 ; ++i)
		{
			SnappedConnections[i] = mSnappedConnectionComponents[i];
			if (mSnappedConnectionComponents[i])
			{
				mSnappedConnectionComponents[i]->ClearConnection();
			}
			const_cast<AMFGAutoSplitterHologram*>(this)->mSnappedConnectionComponents[i] = nullptr;
		}

		// Skip AFGAttachmentSplitterHologram and AFGConveyorAttachmentHologram, those two completely
		// break in the update case
		AFGFactoryHologram::ConfigureComponents(inBuildable);

		TInlineComponentArray<UFGFactoryConnectionComponent*,4> Connections;
		Splitter->GetComponents(Connections);

		if (Connections.Num() != 4)
		{
			UE_LOG(LogAutoSplitters,Error,TEXT("Unexpected number of connections: %d"),Connections.Num());
			return;
		}

		for (int i = 0 ; i < 4 ; ++i)
		{
			if (Connections[i]->IsConnected())
			{
				UE_LOG(LogAutoSplitters,Warning,TEXT("Connection %d is connected but should not be"),i);
				Connections[i]->ClearConnection();
			}

			if (SnappedConnections[i])
			{
				Connections[i]->SetConnection(SnappedConnections[i]);
			}
		}		
	}
	else
	{
		UE_LOG(LogAutoSplitters,Display,TEXT("Calling original implementation"));
		Super::ConfigureComponents(inBuildable);
	}
}
