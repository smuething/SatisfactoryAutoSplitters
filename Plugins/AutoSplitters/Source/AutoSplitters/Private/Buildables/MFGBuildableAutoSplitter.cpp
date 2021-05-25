// ILikeBanas

#include "Buildables/MFGBuildableAutoSplitter.h"

#include <numeric>

#include "AutoSplittersLog.h"
#include "FGFactoryConnectionComponent.h"
#include "Buildables/FGBuildableConveyorBase.h"

#if AUTO_SPLITTERS_DEBUG
#define DEBUG_SPLITTER mDebug
#else
#define DEBUG_SPLITTER false
#endif

AMFGBuildableAutoSplitter::AMFGBuildableAutoSplitter()
	: mOutputRates({1.0,1.0,1.0})
	, mOutputStates({Flag(EOutputState::Automatic),Flag(EOutputState::Automatic),Flag(EOutputState::Automatic)})
	, mRemainingItems({0,0,0})
	, mItemsPerCycle({0,0,0})
	, mLeftInCycle(0)
    , mDebug(false)
	, mCycleLength(0)
	, mBlockedFor({0,0,0})
	, mAssignedItems({0,0,0})
	, mGrabbedItems({0,0,0})
	, mPriorityStepSize({0.0,0.0,0.0})
	, mBalancingRequired(true)
    , mCachedInventoryItemCount(0)
    , mItemRate(0.0)
	, mCycleTime(0.0)
	, mReallyGrabbed(0)
{}

template<typename T, std::size_t n>
constexpr auto make_array(T value) -> std::array<T,n>
{
	std::array<T,n> r{};
	for (auto& v : r)
		v = value;
	return r;
}

void AMFGBuildableAutoSplitter::Factory_Tick(float dt)
{
	Super::Factory_Tick(dt);

	if (mBalancingRequired)
	{
		BalanceNetwork(true);
	}

	for (int i = 0 ; i < NUM_OUTPUTS ; ++i)
	{
		mLeftInCycle -= mGrabbedItems[i];
		mGrabbedItems[i] = 0;
		mAssignedItems[i] = 0;
	}
	mNextInventorySlot = {MAX_INVENTORY_SIZE,MAX_INVENTORY_SIZE,MAX_INVENTORY_SIZE};
	mInventorySlotEnd = {0,0,0};
	std::fill(mAssignedOutputs.begin(),mAssignedOutputs.end(),-1);
	
	int32 Connections = 0;
	bool NeedsBalancing = false;
	for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
	{
		const bool Connected = IsSet(mOutputStates[i],EOutputState::Connected);
		Connections += Connected;
		if (Connected != mOutputs[i]->IsConnected())
		{
			NeedsBalancing = true;
		}
	}

	if (NeedsBalancing)
		BalanceNetwork();
	
	mCachedInventoryItemCount = 0;
	auto PopulatedInventorySlots = make_array<int32,MAX_INVENTORY_SIZE>(-1);
	for (int32 i = 0 ; i < mInventorySizeX ; ++i)
	{
		if(mBufferInventory->IsSomethingOnIndex(i))
		{
			PopulatedInventorySlots[mCachedInventoryItemCount++] = i;
		}
	}

	if (Connections == 0 || mCachedInventoryItemCount == 0)
	{
		mCycleTime += dt;
		return;
	}
	
	if (mLeftInCycle < -40)
	{
		UE_LOG(LogAutoSplitters,Warning,TEXT("mLeftInCycle too negative (%d), resetting"),mLeftInCycle);
		PrepareCycle(false,true);
	}
	else if (mLeftInCycle <= 0)
	{
		PrepareCycle(true);
	}

	mCycleTime += dt;
	std::array<int32,NUM_OUTPUTS> AssignableItems = {0};

	for (int32 ActiveSlot = 0 ; ActiveSlot < mCachedInventoryItemCount ; ++ActiveSlot)
	{
		int32 Next = -1;
		float Priority = -INFINITY;
		for (int32 i = 0; i < NUM_OUTPUTS; ++i)
		{
			// Adding the grabbed items in the next line de-skews the algorithm if the output has been
			// penalized for an earlier inventory slot
			AssignableItems[i] = mRemainingItems[i] - mAssignedItems[i] + mGrabbedItems[i];
			const auto ItemPriority = AssignableItems[i] * mPriorityStepSize[i];
			if (AssignableItems[i] > 0 && ItemPriority > Priority)
			{
				Next = i;
				Priority = ItemPriority;
			}
		}

		if (Next < 0)
		{
			break;
		}

		std::array<bool,NUM_OUTPUTS> Penalized = {false,false,false};
		while (IsOutputBlocked(Next) && Next >= 0)
		{
			if (DEBUG_SPLITTER)
			{
				UE_LOG(LogAutoSplitters,Display,TEXT("Output %d is blocked, reassigning item and penalizing output"),Next);
			}
			Penalized[Next] = true;
			--mRemainingItems[Next];
			++mAssignedItems[Next];
			++mGrabbedItems[Next]; // this is a blatant lie, but it will cause the correct update of mLeftInCycle during the next tick
			--AssignableItems[Next];
			Priority = -INFINITY;
			Next = -1;
			for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
			{
				if (Penalized[i] || AssignableItems[i] <= 0)
					continue;

				const auto ItemPriority = AssignableItems[i] * mPriorityStepSize[i];
				if (ItemPriority > Priority)
				{
					Next = i;
					Priority = ItemPriority;
				}
			}
		}

		if (Next >= 0)
		{
			const auto Slot = PopulatedInventorySlots[ActiveSlot];
			mAssignedOutputs[Slot] = Next;
			if (mNextInventorySlot[Next] == MAX_INVENTORY_SIZE)
				mNextInventorySlot[Next] = Slot;
			mInventorySlotEnd[Next] = Slot + 1;
			++mAssignedItems[Next];
		}
		else if (DEBUG_SPLITTER) {
			UE_LOG(LogAutoSplitters,Warning,TEXT("All eligible outputs blocked, cannot assign item!"))
		}

	}

	for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
	{
		// Checking for mGrabbedItems seems weird, but that catches stuck outputs
		// that have been penalized
		if ((mAssignedItems[i] > 0 || mGrabbedItems[i] > 0))
		{
			mBlockedFor[i] += dt;
		}
	}

	if (DEBUG_SPLITTER)
	{
		UE_LOG(LogAutoSplitters,Display,TEXT("Assigned items (jammed): 0=%d (%f) 1=%d (%f) 2=%d (%f)"),
			mAssignedItems[0],mBlockedFor[0],
			mAssignedItems[1],mBlockedFor[1],
			mAssignedItems[2],mBlockedFor[2]
			);
	}
}

void AMFGBuildableAutoSplitter::PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion)
{
	Super::PostLoadGame_Implementation(saveVersion,gameVersion);
	mLeftInCycle = std::accumulate(mRemainingItems.begin(),mRemainingItems.end(),0);
	mCycleLength = std::accumulate(mItemsPerCycle.begin(),mItemsPerCycle.end(),0);
	mCycleTime = -100000.0; // this delays item rate calculation to the first full cycle when loading the game
	SetupDistribution(true);
}

void AMFGBuildableAutoSplitter::BeginPlay()
{
	Super::BeginPlay();
	mBalancingRequired = true;
}


void AMFGBuildableAutoSplitter::FillDistributionTable(float dt)
{
	// we are doing our own distribution management, as we need to track
	// whether assigned items were actually picked up by the outputs
}

bool AMFGBuildableAutoSplitter::Factory_GrabOutput_Implementation(UFGFactoryConnectionComponent* connection,
	FInventoryItem& out_item, float& out_OffsetBeyond, TSubclassOf<UFGItemDescriptor> type)
{
	int32 Output = -1;
	for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
	{
		if (connection == mOutputs[i])
		{
			Output = i;
			break;
		}
	}
	if (Output < 0)
	{
		UE_LOG(LogAutoSplitters,Error,TEXT("Could not find connection!"));
		return false;
	}

	mBlockedFor[Output] = 0.0;	

	if (mAssignedItems[Output] <= mGrabbedItems[Output])
	{
		return false;
	}

	for(int32 Slot = mNextInventorySlot[Output] ; Slot < mInventorySlotEnd[Output] ; ++Slot)
	{
		if (mAssignedOutputs[Slot] == Output)
		{
			FInventoryStack Stack;
			mBufferInventory->GetStackFromIndex(Slot,Stack);
			mBufferInventory->RemoveAllFromIndex(Slot);
			out_item = Stack.Item;
			out_OffsetBeyond = mGrabbedItems[Output] * AFGBuildableConveyorBase::ITEM_SPACING;
			++mGrabbedItems[Output];
			--mRemainingItems[Output];
			++mReallyGrabbed;
			mNextInventorySlot[Output] = Slot + 1;

			if (DEBUG_SPLITTER)
			{
				UE_LOG(LogAutoSplitters,Display,TEXT("Sent item out of output %d"),Output);
			}
	
			return true;
		}
	}

	UE_LOG(LogAutoSplitters,Warning,TEXT("Output %d: No valid output found, this should not happen!"),Output);
	
	return false;
}

void AMFGBuildableAutoSplitter::SetupDistribution(bool LoadingSave)
{

	if (!LoadingSave)
	{
		for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
		{
			mOutputStates[i] = SetFlag(mOutputStates[i],EOutputState::Connected,mOutputs[i]->IsConnected());
		}
	}
		
	if (!(
		IsSet(mOutputStates[0],EOutputState::Connected) ||
		IsSet(mOutputStates[1],EOutputState::Connected) ||
		IsSet(mOutputStates[2],EOutputState::Connected)))
	{
		return;		
	}
	
	// calculate item counts per cycle
	for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
	{
		mItemsPerCycle[i] = static_cast<int32>(std::round(IsSet(mOutputStates[i], EOutputState::Connected) * mOutputRates[i] * 10000));
	}

	auto GCD = std::gcd(std::gcd(mItemsPerCycle[0],mItemsPerCycle[1]),mItemsPerCycle[2]);

	if (GCD == 0)
	{
		if (DEBUG_SPLITTER)
		{
			UE_LOG(LogAutoSplitters,Display,TEXT("Nothing connected, chilling"));
		}
		return;
	}

	for (auto& Item : mItemsPerCycle)
		Item /= GCD;

	mCycleLength = 0;
    bool Changed = false;
	for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
	{
		if (IsSet(mOutputStates[i],EOutputState::Connected))
		{
			mCycleLength += mItemsPerCycle[i];
			float StepSize = 0.0f;
			if (mItemsPerCycle[i] > 0)
			{
				StepSize = 1.0f/mItemsPerCycle[i];
			}
			if (mPriorityStepSize[i] != StepSize)
			{
				mPriorityStepSize[i] = StepSize;
				Changed = true;
			}
		}
		else
		{
			// disable output
			if (mPriorityStepSize[i] != 0)
			{
				mPriorityStepSize[i] = 0;
				Changed = true;
			}
		}
	}
		
	if (Changed && !LoadingSave)
	{
		mRemainingItems = {0,0,0};
		mLeftInCycle = 0;
		PrepareCycle(false);
	}
}

void AMFGBuildableAutoSplitter::PrepareCycle(const bool AllowCycleExtension, const bool Reset)
{
	if (DEBUG_SPLITTER)
	{
		UE_LOG(LogAutoSplitters,Display,TEXT("PrepareCycle(%s,%s) cycleTime=%f grabbed=%d"),
			AllowCycleExtension ? TEXT("true") : TEXT("false"),
			Reset ? TEXT("true") : TEXT("false"),
			mCycleTime,
			mReallyGrabbed
			);
	}

	if (!Reset && mCycleTime > 0.0)
	{
		// update statistics
		if (mItemRate > 0.0)
		{
			mItemRate = EXPONENTIAL_AVERAGE_WEIGHT * 60 * mReallyGrabbed / mCycleTime + (1.0-EXPONENTIAL_AVERAGE_WEIGHT) * mItemRate;
		}
		else
		{
			// bootstrap
			mItemRate = 60.0 * mReallyGrabbed / mCycleTime;
		}
		
		if (AllowCycleExtension && mCycleTime < 2.0)
		{
			if (DEBUG_SPLITTER)
			{
				UE_LOG(LogAutoSplitters,Display,TEXT("Cycle time too short (%f), doubling cycle length to %d"),mCycleTime,2*mCycleLength);
			}
			mCycleLength *= 2;
			for (int i = 0 ; i < NUM_OUTPUTS ; ++i)
				mItemsPerCycle[i] *= 2;
		}
		else if (mCycleTime > 10.0)
		{
			bool CanShortenCycle = !(mCycleLength & 1);
			for (int i = 0 ; i < NUM_OUTPUTS ; ++i)
				CanShortenCycle = CanShortenCycle && !(mItemsPerCycle[i] & 1);

			if (CanShortenCycle)
			{
				if (DEBUG_SPLITTER)
				{
					UE_LOG(LogAutoSplitters,Display,TEXT("Cycle time too long (%f), halving cycle length to %d"),mCycleTime,mCycleLength/2);
				}
				mCycleLength /= 2;
				for (int i = 0 ; i < NUM_OUTPUTS ; ++i)
					mItemsPerCycle[i] /= 2;
			}
		}
	}

	mCycleTime = 0.0;
	mReallyGrabbed = 0;	

	if (Reset)
	{
		mLeftInCycle = mCycleLength;

		for (int i = 0; i < NUM_OUTPUTS ; ++i)
		{
			if (IsSet(mOutputStates[i],EOutputState::Connected) && mOutputRates[i] > 0)
				mRemainingItems[i] = mItemsPerCycle[i];
			else
				mRemainingItems[i] = 0;
		}
	}
	else
	{
		mLeftInCycle += mCycleLength;

		for (int i = 0; i < NUM_OUTPUTS ; ++i)
		{
			if (IsSet(mOutputStates[i],EOutputState::Connected) && mOutputRates[i] > 0)
				mRemainingItems[i] += mItemsPerCycle[i];
			else
				mRemainingItems[i] = 0;
		}
	}
}

bool AMFGBuildableAutoSplitter::SetOutputRate(const int32 Output, const float Rate)
{
	if (Output < 0 || Output > NUM_OUTPUTS - 1)
		return false;
	
	if (Rate < 0.0 || Rate > 780.0)
		return false;

	if (mOutputRates[Output] == Rate)
		return true;

	mOutputRates[Output] = Rate;

    SetupDistribution();
	BalanceNetwork();

	return true;
}

bool AMFGBuildableAutoSplitter::SetOutputAutomatic(int32 Output, bool Automatic)
{

	if (Output < 0 || Output > NUM_OUTPUTS - 1)
		return false;

	if (Automatic == IsSet(mOutputStates[Output],EOutputState::Automatic))
		return true;
	
	if (Automatic)
	{
		mOutputStates[Output] = SetFlag(mOutputStates[Output],EOutputState::Automatic);
		BalanceNetwork();
	}
	else
	{
		mOutputStates[Output] = ClearFlag(mOutputStates[Output],EOutputState::Automatic);
		if (IsSet(mOutputStates[Output],EOutputState::Connected))
		{
			if (mOutputRates[Output] != 1.0)
			{
				mOutputRates[Output] = 1.0;
				SetupDistribution();
				BalanceNetwork();
			}
		}
	}

	return true;
}

int32 AMFGBuildableAutoSplitter::BalanceNetwork(bool RootOnly)
{
	mBalancingRequired = false;
	TSet<AMFGBuildableAutoSplitter*> SplitterSet;
	// start by going upstream
	auto Root = this;
	for (auto Current = this ; Current ; Current = FindAutoSplitterAfterBelt(Current->mInputs[0],false))
	{
		if (SplitterSet.Contains(Current))
		{
			UE_LOG(LogAutoSplitters,Warning,TEXT("Cycle in auto splitter network detected, bailing out"));
			return -1;
		}
		SplitterSet.Add(Current);
		Root = Current;
	}

	if (RootOnly && this != Root)
	{
		Root->mBalancingRequired = true;
		return -1;
	}

	// Now walk the tree to discover the whole network
	TArray<TArray<FConnections>> SplitterHierarchy;
	DiscoverHierarchy(SplitterHierarchy,Root,0);


	// We have found all connected AutoSplitters, now let's re-balance
	TMap<AMFGBuildableAutoSplitter*,float> InputRates;

	int32 SplitterCount = 0;
	for (int32 Level = SplitterHierarchy.Num() - 1 ; Level >= 0 ; --Level)
	{
		for (auto& c : SplitterHierarchy[Level])
		{
			++SplitterCount;
			bool Changed = false;
			float InputRate = 0;
			for (int32 i = 0; i < NUM_OUTPUTS; ++i)
			{
				auto& OutputState = c.Splitter->mOutputStates[i];
				const bool IsConnected = c.Splitter->mOutputs[i]->IsConnected();

				if (c.Outputs[i])
				{
					OutputState = SetFlag(OutputState,EOutputState::AutoSplitter);
					if (IsSet(OutputState,EOutputState::Automatic))
					{
						const auto OutputRate = InputRates[c.Outputs[i]];
						if (OutputRate != c.Splitter->mOutputRates[i])
						{
							c.Splitter->mOutputRates[i] = OutputRate;
							Changed = true;
						}
					}
				}
				else
				{
					OutputState = ClearFlag(OutputState,EOutputState::AutoSplitter);	
					if (IsSet(OutputState,EOutputState::Automatic))
					{
						if (c.Splitter->mOutputRates[i] != 1.0)
						{
							c.Splitter->mOutputRates[i] = 1.0;
							Changed = true;
						}
					}
					if (IsConnected != IsSet(OutputState,EOutputState::Connected))
					{
						OutputState = SetFlag(OutputState,EOutputState::Connected,IsConnected);
						Changed = true;
					}
				}

				// only count automatic outputs that have a belt connected
				if (!IsSet(OutputState,EOutputState::Automatic) || IsConnected)
					InputRate += c.Splitter->mOutputRates[i];
			}
			if (Changed)
				c.Splitter->SetupDistribution();

			InputRates.Add(c.Splitter,InputRate);
		}
	}
	return SplitterCount;
}

AMFGBuildableAutoSplitter* AMFGBuildableAutoSplitter::FindAutoSplitterAfterBelt(
	UFGFactoryConnectionComponent* Connection, bool Forward)
{
	while (Connection->IsConnected())
	{
		Connection = Connection->GetConnection();
		const auto Belt = Cast<AFGBuildableConveyorBase>(Connection->GetOuterBuildable());
		if (Belt)
		{			
			Connection = Forward ? Belt->GetConnection1() : Belt->GetConnection0();
			continue;
		}
		return Cast<AMFGBuildableAutoSplitter>(Connection->GetOuterBuildable());
	}
	return nullptr;
}

void AMFGBuildableAutoSplitter::DiscoverHierarchy(TArray<TArray<FConnections>>& Splitters,
                                                  AMFGBuildableAutoSplitter* Splitter, const int32 Level)
{
	if (!Splitters.IsValidIndex(Level))
	{
		Splitters.Emplace();
	}
	auto& Connections = Splitters[Level][Splitters[Level].Emplace(Splitter)];
	int32 i = 0;
	for (auto Connection : Splitter->mOutputs)
	{
		const auto Downstream = FindAutoSplitterAfterBelt(Connection, true);
		Connections.Outputs[i++] = Downstream;
		if (Downstream)
			DiscoverHierarchy(Splitters, Downstream, Level + 1);
	}
}
