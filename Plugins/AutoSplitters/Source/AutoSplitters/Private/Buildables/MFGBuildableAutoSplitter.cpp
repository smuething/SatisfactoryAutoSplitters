// ILikeBanas

#include "Buildables/MFGBuildableAutoSplitter.h"

#include <numeric>

#include <Modules/ModuleManager.h>
#include "AutoSplittersLog.h"
#include "AutoSplittersModule.h"
#include "FGFactoryConnectionComponent.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "AutoSplitters_ConfigStruct.h"
#include "Chaos/AABB.h"
#include "Chaos/AABB.h"

#if AUTO_SPLITTERS_DEBUG
#define DEBUG_THIS_SPLITTER mDebug
#define DEBUG_SPLITTER(splitter) ((splitter).mDebug)
#else
#define DEBUG_THIS_SPLITTER false
#define DEBUG_SPLITTER(splitter) false
#endif

template<std::size_t n, typename T>
constexpr auto make_array(T value) -> std::array<T,n>
{
    std::array<T,n> r{};
    for (auto& v : r)
        v = value;
    return r;
}

template<std::size_t n, typename T>
constexpr auto MakeTArray(const T& Value) -> TArray<T,TFixedAllocator<n>>
{
    TArray<T,TFixedAllocator<n>> Result;
    Result.Init(Value,n);
    return Result;
}

AMFGBuildableAutoSplitter::AMFGBuildableAutoSplitter()
    : mTransientState(0)
    , mOutputStates(MakeTArray<NUM_OUTPUTS>(Flag(EOutputState::Automatic)))
    , mRemainingItems(MakeTArray<NUM_OUTPUTS>(0))
    , mPersistentState(0) // Do the setup in BeginPlay(), otherwise we cannot detect version changes during loading
    , mTargetInputRate(0)
    , mIntegralOutputRates(MakeTArray<NUM_OUTPUTS>(FRACTIONAL_RATE_MULTIPLIER))
    , mRootSplitter(nullptr)
    , mItemsPerCycle(MakeTArray<NUM_OUTPUTS>(0))
    , mLeftInCycle(0)
    , mDebug(false)
    , mCycleLength(0)
    , mCachedInventoryItemCount(0)
    , mItemRate(0.0f)
    , mBlockedFor(make_array<NUM_OUTPUTS>(0.0f))
    , mAssignedItems(make_array<NUM_OUTPUTS>(0))
    , mGrabbedItems(make_array<NUM_OUTPUTS>(0))
    , mPriorityStepSize(make_array<NUM_OUTPUTS>(0.0f))
    , mBalancingRequired(true)
    , mNeedsInitialDistributionSetup(true)
    , mCycleTime(0.0f)
    , mReallyGrabbed(0)
{}

void AMFGBuildableAutoSplitter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mTransientState);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mOutputStates);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mPersistentState);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mTargetInputRate);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mIntegralOutputRates);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mLeftInCycle);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mCycleLength);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mCachedInventoryItemCount);
    DOREPLIFETIME(AMFGBuildableAutoSplitter,mItemRate);
}

void AMFGBuildableAutoSplitter::PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker)
{
    Super::PreReplication(ChangedPropertyTracker);
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mOutputStates,IsTransientFlagSet(IS_REPLICATION_ENABLED));
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mPersistentState,IsTransientFlagSet(IS_REPLICATION_ENABLED));
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mTargetInputRate,IsTransientFlagSet(IS_REPLICATION_ENABLED));
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mIntegralOutputRates,IsTransientFlagSet(IS_REPLICATION_ENABLED));
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mLeftInCycle,IsTransientFlagSet(IS_REPLICATION_ENABLED));
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mCycleLength,IsTransientFlagSet(IS_REPLICATION_ENABLED));
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mCachedInventoryItemCount,IsTransientFlagSet(IS_REPLICATION_ENABLED));
    DOREPLIFETIME_ACTIVE_OVERRIDE(AMFGBuildableAutoSplitter,mItemRate,IsTransientFlagSet(IS_REPLICATION_ENABLED));
}

void AMFGBuildableAutoSplitter::Factory_Tick(float dt)
{
    if (!HasAuthority())
        return;

    // keep outputs from pulling while we're in here
    mNextInventorySlot = make_array<NUM_OUTPUTS>(MAX_INVENTORY_SIZE);

    // skip direct splitter base class, it doesn't do anything useful for us
    AFGBuildableConveyorAttachment::Factory_Tick(dt);

    if (DEBUG_THIS_SPLITTER)
    {
        UE_LOG(LogAutoSplitters,Display,TEXT("transient=%d persistent=%d cycleLength=%d leftInCycle=%d outputstates=(%d %d %d) remaining=(%d %d %d)"),
            mTransientState,mPersistentState,
            mCycleLength,mLeftInCycle,
            mOutputStates[0],mOutputStates[1],mOutputStates[2],
            mRemainingItems[0],mRemainingItems[1],mRemainingItems[2]
            );
        UE_LOG(LogAutoSplitters,Display,TEXT("targetInput=%d outputRates=(%d %d %d) itemsPerCycle=(%d %d %d)"),
            mTargetInputRate,
            mIntegralOutputRates[0],mIntegralOutputRates[1],mIntegralOutputRates[2],
            mItemsPerCycle[0],mItemsPerCycle[1],mItemsPerCycle[2]
            );
    }

    if (mNeedsInitialDistributionSetup)
    {
        SetupInitialDistributionState();
    }

    if (mBalancingRequired)
    {
        auto [valid,_] = Server_BalanceNetwork(this,true);
        if (!valid)
        {
            // bail out for this tick
            return;
        }
    }

    for (int i = 0 ; i < NUM_OUTPUTS ; ++i)
    {
        mLeftInCycle -= mGrabbedItems[i];
        mGrabbedItems[i] = 0;
        mAssignedItems[i] = 0;
    }
    mInventorySlotEnd = make_array<NUM_OUTPUTS>(0);
    mAssignedOutputs = make_array<MAX_INVENTORY_SIZE>(-1);

    if (mTargetInputRate == 0 && mInputs[0]->IsConnected())
    {
        auto [_,Rate,Ready] = FindAutoSplitterAndMaxBeltRate(mInputs[0],false);
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
        if (!Ready)
            return;
#endif
        mTargetInputRate = Rate;
    }

    int32 Connections = 0;
    bool NeedsBalancing = false;
    for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
    {
        const bool Connected = IsSet(mOutputStates[i],EOutputState::Connected);
        Connections += Connected;
        if (Connected != mOutputs[i]->IsConnected())
        {
            if (DEBUG_THIS_SPLITTER)
            {
                UE_LOG(LogAutoSplitters,Display,TEXT("Connection change in output %d"),i);
            }
            NeedsBalancing = true;
        }
    }

    if (NeedsBalancing)
    {
        mBalancingRequired = true;
        // bail out for this tick
        return;
    }

    if (IsPersistentFlagSet(NEEDS_DISTRIBUTION_SETUP))
    {
        SetupDistribution();
    }

    mCachedInventoryItemCount = 0;
    auto PopulatedInventorySlots = make_array<MAX_INVENTORY_SIZE>(-1);
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
    auto AssignableItems = make_array<NUM_OUTPUTS>(0);

    auto NextInventorySlot = make_array<NUM_OUTPUTS>(MAX_INVENTORY_SIZE);

    for (int32 ActiveSlot = 0 ; ActiveSlot < mCachedInventoryItemCount ; ++ActiveSlot)
    {
        if (DEBUG_THIS_SPLITTER)
        {
            UE_LOG(LogAutoSplitters,Display,TEXT("slot=%d"),ActiveSlot);
        }
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
            if (DEBUG_THIS_SPLITTER)
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
            if (DEBUG_THIS_SPLITTER)
            {
                UE_LOG(LogAutoSplitters,Display,TEXT("Slot %d -> actual slot %d"),ActiveSlot,Slot);
            }
            mAssignedOutputs[Slot] = Next;
            if (NextInventorySlot[Next] == MAX_INVENTORY_SIZE)
                NextInventorySlot[Next] = Slot;
            mInventorySlotEnd[Next] = Slot + 1;
            ++mAssignedItems[Next];
        }
        else if (DEBUG_THIS_SPLITTER) {
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

    if (DEBUG_THIS_SPLITTER)
    {
        UE_LOG(LogAutoSplitters,Display,TEXT("Assigned items (jammed): 0=%d (%f) 1=%d (%f) 2=%d (%f)"),
            mAssignedItems[0],mBlockedFor[0],
            mAssignedItems[1],mBlockedFor[1],
            mAssignedItems[2],mBlockedFor[2]
            );
    }

    // make new items available to outputs
    mNextInventorySlot = NextInventorySlot;
}


void AMFGBuildableAutoSplitter::PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion)
{
    Super::PostLoadGame_Implementation(saveVersion,gameVersion);

    if (!HasAuthority())
    {
        UE_LOG(LogAutoSplitters,Fatal,TEXT("PostLoadGame_Implementation() was called without authority"));
    }

    mLeftInCycle = std::accumulate(mRemainingItems.begin(),mRemainingItems.end(),0);
    mCycleLength = std::accumulate(mItemsPerCycle.begin(),mItemsPerCycle.end(),0);
    mCycleTime = -100000.0; // this delays item rate calculation to the first full cycle when loading the game

    if (GetSplitterVersion() == 0)
    {
        UE_LOG(LogAutoSplitters,Display,TEXT("Upgrading saved Auto Splitter from version 0 to 1"));

#if AUTO_SPLITTERS_DEBUG

        TInlineComponentArray<UFGFactoryConnectionComponent*,6> Connections;

        GetComponents(Connections);

        UE_LOG(LogAutoSplitters,Display,TEXT("Splitter %p: connections=%d outputrates_deprecated=%d outputstates=(%d %d %d)"),
            this,
            Connections.Num(),
            mOutputRates_DEPRECATED.Num(),
            mOutputStates[0],mOutputStates[1],mOutputStates[2]
            );

#endif

        for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
        {
            mIntegralOutputRates[i] = FRACTIONAL_RATE_MULTIPLIER;
            mOutputStates[i] = Flag(EOutputState::Automatic);
            mRemainingItems[i] = 0;
            mItemsPerCycle[i] = 0;
        }
        mLeftInCycle = 0;
        mCycleLength = 0;

        mOutputRates_DEPRECATED.Empty();

        SetPersistentFlag(NEEDS_CONNECTIONS_FIXUP);
        SetSplitterVersion(1);
    }

    if (!IsPersistentFlagSet(NEEDS_CONNECTIONS_FIXUP))
    {
        SetupDistribution(true);
        mNeedsInitialDistributionSetup = false;
    }
}

void AMFGBuildableAutoSplitter::BeginPlay()
{

    // we need to fix the connection wiring before calling into our parent class
    if (HasAuthority())
    {
        if (IsPersistentFlagSet(NEEDS_CONNECTIONS_FIXUP))
        {
            FixupConnections();
        }

        Super::BeginPlay();
        SetSplitterVersion(VERSION);
        mBalancingRequired = true;
    }
    else
    {
        Super::BeginPlay();
    }
}


void AMFGBuildableAutoSplitter::FillDistributionTable(float dt)
{
    // we are doing our own distribution management, as we need to track
    // whether assigned items were actually picked up by the outputs
}

bool AMFGBuildableAutoSplitter::Factory_GrabOutput_Implementation(UFGFactoryConnectionComponent* connection,
    FInventoryItem& out_item, float& out_OffsetBeyond, TSubclassOf<UFGItemDescriptor> type)
{
    if (!HasAuthority())
    {
        UE_LOG(LogAutoSplitters, Fatal, TEXT("Factory_GrabOutput_Implementation() was called without authority"));
    }

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
        if (!IsSet(mOutputStates[Output],EOutputState::Connected))
        {
            mBalancingRequired = true;
        }
        return false;
    }

    for(int32 Slot = mNextInventorySlot[Output] ; Slot < mInventorySlotEnd[Output] ; ++Slot)
    {
        if (mAssignedOutputs[Slot] == Output)
        {
            if (Slot > 8)
            {
                UE_LOG(LogAutoSplitters,Error,TEXT("Hit invalid slot %d for output %d"),Slot,Output);
            }
            FInventoryStack Stack;
            mBufferInventory->GetStackFromIndex(Slot,Stack);
            mBufferInventory->RemoveAllFromIndex(Slot);
            out_item = Stack.Item;
            out_OffsetBeyond = mGrabbedItems[Output] * AFGBuildableConveyorBase::ITEM_SPACING;
            ++mGrabbedItems[Output];
            --mRemainingItems[Output];
            ++mReallyGrabbed;
            mNextInventorySlot[Output] = Slot + 1;

            if (DEBUG_THIS_SPLITTER)
            {
                UE_LOG(LogAutoSplitters,Display,TEXT("Sent item out of output %d"),Output);
            }

            return true;
        }
    }

    UE_LOG(LogAutoSplitters,Warning,TEXT("Output %d: No valid output found, this should not happen!"),Output);

    if (!IsSet(mOutputStates[Output],EOutputState::Connected))
    {
        mBalancingRequired = true;
    }

    return false;
}

void AMFGBuildableAutoSplitter::SetupDistribution(bool LoadingSave)
{

    if (DEBUG_THIS_SPLITTER)
    {
        UE_LOG(
            LogAutoSplitters,
            Display,
            TEXT("SetupDistribution() input=%d outputs=(%d %d %d)"),
            mTargetInputRate,
            mIntegralOutputRates[0],
            mIntegralOutputRates[1],
            mIntegralOutputRates[2]
            );
    }

    if (!LoadingSave)
    {
        for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
        {
            mOutputStates[i] = SetFlag(mOutputStates[i],EOutputState::Connected,mOutputs[i]->IsConnected());
        }
    }

    if (std::none_of(mOutputStates.begin(),mOutputStates.end(),[](auto State) { return IsSet(State,EOutputState::Connected); }))
    {
        mIntegralOutputRates.Init(FRACTIONAL_RATE_MULTIPLIER,NUM_OUTPUTS);
        mItemsPerCycle.Init(0,NUM_OUTPUTS);
        return;
    }

    // calculate item counts per cycle
    for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
    {
        mItemsPerCycle[i] = IsSet(mOutputStates[i], EOutputState::Connected) * mIntegralOutputRates[i];
    }

#if UE_BUILD_SHIPPING
    const auto GCD = std::accumulate(
        mItemsPerCycle.begin()+1,
        mItemsPerCycle.end(),
        mItemsPerCycle[0],
        [](auto a, auto b) { return std::gcd(a,b);}
        );
#else
    // The checked iterator generated here for begin() cannot be advanced with +1, we don't care, this code won't ever
    // run anyway
    const auto GCD = std::accumulate(
        mItemsPerCycle.begin(),
        mItemsPerCycle.end(),
        mItemsPerCycle[0],
        [](auto a, auto b) { return std::gcd(a,b);}
        );
#endif

    if (GCD == 0)
    {
        if (DEBUG_THIS_SPLITTER)
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

    ClearPersistentFlag(NEEDS_DISTRIBUTION_SETUP);
}

void AMFGBuildableAutoSplitter::PrepareCycle(const bool AllowCycleExtension, const bool Reset)
{
    if (DEBUG_THIS_SPLITTER)
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
            if (DEBUG_THIS_SPLITTER)
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
                if (DEBUG_THIS_SPLITTER)
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
            if (IsSet(mOutputStates[i],EOutputState::Connected) && mIntegralOutputRates[i] > 0)
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
            if (IsSet(mOutputStates[i],EOutputState::Connected) && mIntegralOutputRates[i] > 0)
                mRemainingItems[i] += mItemsPerCycle[i];
            else
                mRemainingItems[i] = 0;
        }
    }
}

void AMFGBuildableAutoSplitter::Server_EnableReplication(float Duration)
{
    if (!HasAuthority())
    {
        UE_LOG(LogAutoSplitters,Fatal,TEXT("AMFGBuildableAutoSplitter::Server_EnableReplication() may only be called on server"));
    }

    UE_LOG(LogAutoSplitters,Display,TEXT("Enabling full data replication for Auto Splitter %p"),this);

    SetTransientFlag(IS_REPLICATION_ENABLED);
    GetWorldTimerManager().SetTimer(mReplicationTimer,this,&AMFGBuildableAutoSplitter::Server_ReplicationEnabledTimeout,Duration,false);
}

bool AMFGBuildableAutoSplitter::Server_SetTargetRateAutomatic(bool Automatic)
{
    if (Automatic == !IsPersistentFlagSet(MANUAL_INPUT_RATE))
        return true;

    SetPersistentFlag(MANUAL_INPUT_RATE,!Automatic);
    auto [valid, _] = Server_BalanceNetwork(this);
    if (!valid)
    {
        SetPersistentFlag(MANUAL_INPUT_RATE,Automatic);
        return false;
    }
    return true;
}

float AMFGBuildableAutoSplitter::GetTargetInputRate() const
{
    return mTargetInputRate * INV_FRACTIONAL_RATE_MULTIPLIER;
}

bool AMFGBuildableAutoSplitter::Server_SetTargetInputRate(float Rate)
{
    if (Rate < 0)
        return false;

    if (IsTargetRateAutomatic())
        return false;

    int32 IntRate = static_cast<int32>(Rate * FRACTIONAL_RATE_MULTIPLIER);

    bool Changed = mTargetInputRate != IntRate;
    mTargetInputRate = IntRate;

    if (Changed)
        Server_BalanceNetwork(this);

    return true;
}

float AMFGBuildableAutoSplitter::GetOutputRate(int32 Output) const
{
    if (Output < 0 || Output > NUM_OUTPUTS -1)
        return NAN;

    return static_cast<float>(mIntegralOutputRates[Output]) * INV_FRACTIONAL_RATE_MULTIPLIER;
}

bool AMFGBuildableAutoSplitter::Server_SetOutputRate(const int32 Output, const float Rate)
{
    if (Output < 0 || Output > NUM_OUTPUTS - 1)
    {
        UE_LOG(
            LogAutoSplitters,
            Error,
            TEXT("Invalid output index: %d"),
            Output
            );
        return false;
    }

    auto IntRate = static_cast<int32>(Rate * FRACTIONAL_RATE_MULTIPLIER);

    if (IntRate < 0 || IntRate > 780 * FRACTIONAL_RATE_MULTIPLIER)
    {
        UE_LOG(
            LogAutoSplitters,
            Error,
            TEXT("Invalid output rate: %f (must be between 0 and 780)"),
            Output
            );
        return false;
    }

    if (IsSet(mOutputStates[Output],EOutputState::Automatic))
    {
        UE_LOG(
            LogAutoSplitters,
            Display,
            TEXT("Output %d is automatic, ignoring rate value"),
            Output
            );
        return false;
    }

    if (mIntegralOutputRates[Output] == Rate)
        return true;

    int32 OldRate = mIntegralOutputRates[Output];
    mIntegralOutputRates[Output] = IntRate;

    auto [DownstreamAutoSplitter,_,Ready] = FindAutoSplitterAndMaxBeltRate(mOutputs[Output],true);

    bool OldManualInputRate = false;
    int32 OldTargetInputRate = 0;
    if (DownstreamAutoSplitter)
    {
        OldManualInputRate = DownstreamAutoSplitter->IsPersistentFlagSet(MANUAL_INPUT_RATE);
        OldTargetInputRate = DownstreamAutoSplitter->mTargetInputRate;
        DownstreamAutoSplitter->SetPersistentFlag(MANUAL_INPUT_RATE);
        DownstreamAutoSplitter->mTargetInputRate = IntRate;
    }

    auto [valid,_2] = Server_BalanceNetwork(this);

    if (!valid)
    {
        mIntegralOutputRates[Output] = OldRate;
        if (DownstreamAutoSplitter)
        {
            DownstreamAutoSplitter->SetPersistentFlag(MANUAL_INPUT_RATE,OldManualInputRate);
            DownstreamAutoSplitter->mTargetInputRate = OldTargetInputRate;
        }
    }

    return valid;
}

bool AMFGBuildableAutoSplitter::Server_SetOutputAutomatic(int32 Output, bool Automatic)
{

    if (Output < 0 || Output > NUM_OUTPUTS - 1)
        return false;

    if (Automatic == IsSet(mOutputStates[Output],EOutputState::Automatic))
        return true;

    auto [DownstreamAutoSplitter,_,Ready] = FindAutoSplitterAndMaxBeltRate(mOutputs[Output],true);
    if (DownstreamAutoSplitter)
    {
        DownstreamAutoSplitter->SetPersistentFlag(MANUAL_INPUT_RATE,!Automatic);
    }
    else
    {
        mOutputStates[Output] = SetFlag(mOutputStates[Output],EOutputState::Automatic,Automatic);
    }

    auto [valid,_2] = Server_BalanceNetwork(this);
    if (!valid)
    {
        mOutputStates[Output] = SetFlag(mOutputStates[Output], EOutputState::Automatic,!Automatic);
        if (DownstreamAutoSplitter)
        {
            DownstreamAutoSplitter->SetPersistentFlag(MANUAL_INPUT_RATE,Automatic);
        }
        UE_LOG(
            LogAutoSplitters,
            Warning,
            TEXT("Failed to set output %d to %s"),
            Output,
            Automatic ? TEXT("automatic") : TEXT("manual")
        );
    }
    else
    {
        UE_LOG(
            LogAutoSplitters,
            Display,
            TEXT("Set output %d to %s"),
            Output,
            Automatic ? TEXT("automatic") : TEXT("manual")
        );
    }
    return valid;
}

void AMFGBuildableAutoSplitter::Server_ReplicationEnabledTimeout()
{
    if (!HasAuthority())
    {
        UE_LOG(LogAutoSplitters,Fatal,TEXT("AMFGBuildableAutoSplitter::Server_ReplicationEnabledTimeout() may only be called on server"));
    }
    UE_LOG(LogAutoSplitters,Display,TEXT("Disabling full data replication for Auto Splitter %p"),this);
    ClearTransientFlag(IS_REPLICATION_ENABLED);
}


void AMFGBuildableAutoSplitter::FixupConnections()
{

    auto Module = FModuleManager::GetModulePtr<FAutoSplittersModule>("AutoSplitters");

    TInlineComponentArray<UFGFactoryConnectionComponent*, 6> Connections;
    GetComponents(Connections);

    UE_LOG(LogAutoSplitters, Display, TEXT("Fixing up Auto Splitter connections for 0.3.0 upgrade"));

#if AUTO_SPLITTERS_DEBUG

    UE_LOG(LogAutoSplitters,Display,TEXT("Splitter %p: connections=%d outputrates_deprecated=%d outputstates=(%d %d %d)"),
        this,
        Connections.Num(),
        mOutputRates_DEPRECATED.Num(),
        mOutputStates[0],mOutputStates[1],mOutputStates[2]
        );

    TInlineComponentArray<UFGFactoryConnectionComponent*, 6> Partners;
    int32 PartnerCount = 0;

    int32 ii = 0;
    for (auto& c : Connections)
    {
        UFGFactoryConnectionComponent* Partner = c->IsConnected() ? c->GetConnection() : nullptr;
        Partners.Add(Partner);
        PartnerCount += Partner != nullptr;

        auto Pos = this->GetTransform().InverseTransformPosition(c->GetComponentLocation());
        auto Rot = this->GetTransform().InverseTransformRotation(c->GetComponentRotation().Quaternion());

        UE_LOG(LogAutoSplitters,Display,
            TEXT("Splitter %p: component %d (%s) - %p connected=%s direction=%s global=%s pos=%s rot=%s"),
            this,
            ii,
            *c->GetName(),
            c,
            c->IsConnected() ? TEXT("true") : TEXT("false"),
            c->GetDirection() == EFactoryConnectionDirection::FCD_INPUT ? TEXT("Input") : TEXT("Output"),
            *c->GetComponentLocation().ToString(),
            *Pos.ToString(),
            *Rot.ToString()
            );

        if (Partner)
        {
            Pos = this->GetTransform().InverseTransformPosition(Partner->GetComponentLocation());
            Rot = this->GetTransform().InverseTransformRotation(Partner->GetComponentRotation().Quaternion());
            UE_LOG(LogAutoSplitters,Display,
                TEXT("Splitter %p: component %d partner (%s) - %p connected=%s direction=%s global=%s pos=%s rot=%s"),
                this,
                ii,
                *Partner->GetName(),
                Partner,
                Partner->IsConnected() ? TEXT("true") : TEXT("false"),
                Partner->GetDirection() == EFactoryConnectionDirection::FCD_INPUT ? TEXT("Input") : TEXT("Output"),
                *Partner->GetComponentLocation().ToString(),
                *Pos.ToString(),
                *Rot.ToString()
                );

            auto Belt = Cast<AFGBuildableConveyorBase>(Partner->GetOuterBuildable());
            if (!Belt)
            {
                UE_LOG(LogAutoSplitters,Error,
                    TEXT("Splitter %p: component %d partner is no belt, but a %s"),
                    this,
                    ii,
                    *Partner->GetOuterBuildable()->StaticClass()->GetName()
                    );
            }
            else
            {
                UE_LOG(LogAutoSplitters,Display,
                    TEXT("Splitter %p: belt connection0=%p connection1=%p"),
                    this,
                    Belt->GetConnection0(),
                    Belt->GetConnection1()
                    );
            }
        }

        ++ii;
    }

#endif

    auto& [This,OldBluePrintConnections,ConveyorConnections] = Module->mPreUpgradeSplitters.Add_GetRef({this,{},{}});

    for (auto Connection : Connections)
    {

        if (Connection->GetName() == TEXT("Output0") || Connection->GetName() == TEXT("Input0"))
        {
            UE_LOG(LogAutoSplitters,Display,TEXT("Detaching component %s and scheduling for destruction"),*Connection->GetName());
            RemoveOwnedComponent(Connection);
            OldBluePrintConnections.Emplace(Connection);
        }

        if (Connection->IsConnected())
        {
            UE_LOG(LogAutoSplitters,Display,TEXT("Recording existing connection"));
            ConveyorConnections.Emplace(Connection->GetConnection());
        }
    }

    ClearPersistentFlag(NEEDS_CONNECTIONS_FIXUP);

}

void AMFGBuildableAutoSplitter::SetupInitialDistributionState()
{
    auto [InputSplitter,MaxInputRate,Ready] = FindAutoSplitterAndMaxBeltRate(mInputs[0], false);
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
    if (!Ready)
    {
#if AUTO_SPLITTERS_DEBUG
        UE_LOG(LogAutoSplitters,Display,TEXT("Splitter %p: Aborting because not ready"),this);
#endif
        return;
    }
#endif
    mTargetInputRate = MaxInputRate;
    for (int32 i = 0; i < NUM_OUTPUTS; ++i)
    {
        auto [OutputSplitter,MaxRate,Ready2] = FindAutoSplitterAndMaxBeltRate(mOutputs[i], true);
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
        if (!Ready2)
        {
#if AUTO_SPLITTERS_DEBUG
            UE_LOG(LogAutoSplitters,Display,TEXT("Splitter %p: Aborting because not ready"),this);
#endif
            return;
        }
#endif
        if (MaxRate > 0)
        {
            mIntegralOutputRates[i] = FRACTIONAL_RATE_MULTIPLIER;
            mOutputStates[i] = SetFlag(mOutputStates[i], EOutputState::Connected);
        }
        else
        {
            mIntegralOutputRates[i] = 0;
            mOutputStates[i] = ClearFlag(mOutputStates[i], EOutputState::Connected);
        }
        mOutputStates[i] = SetFlag(mOutputStates[i], EOutputState::AutoSplitter, OutputSplitter != nullptr);
    }
    mNeedsInitialDistributionSetup = false;
    mBalancingRequired = true;
}

std::tuple<bool,int32> AMFGBuildableAutoSplitter::Server_BalanceNetwork(AMFGBuildableAutoSplitter* ForSplitter, bool RootOnly)
{
    if (!ForSplitter)
    {
        UE_LOG(
            LogAutoSplitters,
            Error,
            TEXT("BalanceNetwork() must be called with a valid ForSplitter argument, aborting!")
            );
        return {false,-1};
    }

    if(ForSplitter->IsPersistentFlagSet(NEEDS_CONNECTIONS_FIXUP) || !ForSplitter->HasActorBegunPlay())
    {
        return {false,-1};
    }

    TSet<AMFGBuildableAutoSplitter*> SplitterSet;
    // start by going upstream
    auto Root = ForSplitter;
    SplitterSet.Add(Root);
    for (
        auto [Current,Rate,Ready] = FindAutoSplitterAndMaxBeltRate(Root->mInputs[0],false) ;
        Current ;
        std::tie(Current,Rate,Ready) = FindAutoSplitterAndMaxBeltRate(Current->mInputs[0],false)
        )
    {
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
        if (!Ready)
        {
#if AUTO_SPLITTERS_DEBUG
            UE_LOG(LogAutoSplitters,Display,TEXT("Splitter %p: Aborting because not ready"),Current);
#endif
            return {false,-1};
        }
#endif
        if (Current->IsPersistentFlagSet(NEEDS_CONNECTIONS_FIXUP) || !Current->HasActorBegunPlay())
            return {false,-1};
        if (SplitterSet.Contains(Current))
        {
            UE_LOG(LogAutoSplitters,Warning,TEXT("Cycle in auto splitter network detected, bailing out"));
            return {false,-1};
        }
        SplitterSet.Add(Current);
        Root = Current;
    }

    if (RootOnly && ForSplitter != Root)
    {
        Root->mBalancingRequired = true;
        return {false,-1};
    }

    const auto Config = FAutoSplitters_ConfigStruct::GetActiveConfig();

    // Now walk the tree to discover the whole network
    TArray<TArray<FNetworkNode>> Network;
    int32 SplitterCount = 0;
    if (!DiscoverHierarchy(Network,Root,0,nullptr, INT32_MAX,Root,Config.Features.RespectOverclocking))
    {
        Root->mBalancingRequired = true;
        return {false,-1};
    }

    UE_LOG(LogAutoSplitters,Display,TEXT("Starting BalanceNetwork() algorithm for root splitter %p (%s)"),Root,*Root->GetName());

    for (int32 Level = Network.Num() - 1 ; Level >= 0 ; --Level)
    {
        for (auto& Node: Network[Level])
        {
            ++SplitterCount;

            auto& Splitter = *Node.Splitter;
            Splitter.mBalancingRequired = false;

            for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
            {
                if (Node.MaxOutputRates[i] == 0)
                {
                    if (IsSet(Splitter.mOutputStates[i],EOutputState::Connected))
                    {
                        Splitter.mOutputStates[i] = ClearFlag(Splitter.mOutputStates[i],EOutputState::Connected);
                        Node.ConnectionStateChanged = true;
                    }
                    if (IsSet(Splitter.mOutputStates[i], EOutputState::AutoSplitter))
                    {
                        Splitter.mOutputStates[i] = ClearFlag(Splitter.mOutputStates[i],EOutputState::AutoSplitter);
                        Node.ConnectionStateChanged = true;
                    }
                    continue;
                }

                if (!IsSet(Splitter.mOutputStates[i], EOutputState::Connected))
                {
                    Splitter.mOutputStates[i] = SetFlag(Splitter.mOutputStates[i], EOutputState::Connected);
                    Node.ConnectionStateChanged = true;
                }

                if (Node.Outputs[i])
                {
                    if (!IsSet(Splitter.mOutputStates[i], EOutputState::AutoSplitter))
                    {
                        Splitter.mOutputStates[i] = SetFlag(Splitter.mOutputStates[i],EOutputState::AutoSplitter);
                        Node.ConnectionStateChanged = true;
                    }
                    auto& OutputNode = *Node.Outputs[i];
                    auto& OutputSplitter = *OutputNode.Splitter;
                    if (OutputSplitter.IsPersistentFlagSet(MANUAL_INPUT_RATE))
                    {
                        Splitter.mOutputStates[i] = ClearFlag(Splitter.mOutputStates[i],EOutputState::Automatic);
                        Node.FixedDemand += OutputSplitter.mTargetInputRate;
                    }
                    else
                    {
                        Splitter.mOutputStates[i] = SetFlag(Splitter.mOutputStates[i],EOutputState::Automatic);
                        Node.Shares += OutputNode.Shares;
                        Node.FixedDemand += OutputNode.FixedDemand;
                    }
                }
                else
                {
                    if (IsSet(Splitter.mOutputStates[i], EOutputState::AutoSplitter))
                    {
                        Splitter.mOutputStates[i] = ClearFlag(Splitter.mOutputStates[i],EOutputState::AutoSplitter);
                        Node.ConnectionStateChanged = true;
                    }
                    if (IsSet(Splitter.mOutputStates[i],EOutputState::Automatic))
                    {
                        Node.Shares += Node.PotentialShares[i];
                    }
                    else
                    {
                        Node.FixedDemand += Splitter.mIntegralOutputRates[i];
                    }
                }
            }
        }
    }

    // Ok, now for the hard part: distribute the available items

    Network[0][0].AllocatedInputRate = Root->mTargetInputRate;
    bool Valid = true;
    for (auto& Level : Network)
    {
        if (!Valid)
            break;
        for (auto& Node : Level)
        {
            auto& Splitter = *Node.Splitter;
            if (Node.MaxInputRate < Node.FixedDemand)
            {
                UE_LOG(
                    LogAutoSplitters,
                    Warning,
                    TEXT("Max input rate is not sufficient to satisfy fixed demand: %d < %d"),
                    Node.MaxInputRate,
                    Node.FixedDemand
                    );
                Valid = false;
                break;
            }

            int32 AvailableForShares = Node.AllocatedInputRate - Node.FixedDemand;

            if (AvailableForShares < 0)
            {
                UE_LOG(
                    LogAutoSplitters,
                    Warning,
                    TEXT("Not enough available input for requested fixed output rates: demand=%d available=%d"),
                    Node.FixedDemand,
                    Node.AllocatedInputRate
                    );
                Valid = false;
                break;
            }

            // Avoid division by zero
            auto [RatePerShare,Remainder] = Node.Shares > 0 ? std::div(static_cast<int64>(AvailableForShares) * FRACTIONAL_SHARE_MULTIPLIER, Node.Shares) : std::lldiv_t{0,0};

            if (Remainder != 0)
            {
                UE_LOG(
                    LogAutoSplitters,
                    Warning,
                    TEXT("Could not evenly distribute rate among shares: available=%d shares=%lld rate=%lld remainder=%lld"),
                    AvailableForShares,
                    Node.Shares,
                    RatePerShare,
                    Remainder
                );
                // Valid = false;
                // break;
            }

            if (DEBUG_SPLITTER(Splitter))
            {
                UE_LOG(
                    LogAutoSplitters,
                    Display,
                    TEXT("distribution setup: input=%d fixedDemand=%d ratePerShare=%lld shares=%lld remainder=%lld"),
                    Node.AllocatedInputRate,
                    Node.FixedDemand,
                    RatePerShare,
                    Node.Shares,
                    Remainder
                    );
            }

            int64 UndistributedShares = 0;
            int64 UndistributedRate = 0;

            for (int32 i = 0; i < NUM_OUTPUTS; ++i)
            {
                if (Node.Outputs[i])
                {
                    if (Node.Outputs[i]->Splitter->IsPersistentFlagSet(MANUAL_INPUT_RATE))
                    {
                        Node.AllocatedOutputRates[i] = Node.Outputs[i]->Splitter->mTargetInputRate;
                    }
                    else
                    {
                        int64 Rate = RatePerShare * Node.Outputs[i]->Shares;
                        if (Remainder > 0)
                        {
                            auto [ExtraRate,NewUndistributedShares] = std::div(UndistributedShares + RatePerShare * Node.Outputs[i]->Shares,Remainder);
                            UE_LOG(
                                LogAutoSplitters,
                                Display,
                                TEXT("Output %d: increasing rate from %lld to %lld, newUndistributedShares=%lld"),
                                i,
                                Rate,
                                Rate + ExtraRate,
                                NewUndistributedShares
                                );
                            UndistributedShares = NewUndistributedShares;
                            Rate += ExtraRate;
                        }

                        auto [ShareBasedRate,OutputRemainder] = std::div(Rate,FRACTIONAL_SHARE_MULTIPLIER);
                        if (OutputRemainder != 0)
                        {
                            UE_LOG(
                                LogAutoSplitters,
                                Warning,
                                TEXT("Could not calculate fixed precision output rate for output %d (autosplitter): RatePerShare=%lld Shares=%lld rate=%lld remainder=%lld"),
                                i,
                                RatePerShare,
                                Node.Outputs[i]->Shares,
                                ShareBasedRate,
                                OutputRemainder
                            );
                            UndistributedRate += OutputRemainder;
                        }
                        Node.AllocatedOutputRates[i] = Node.Outputs[i]->FixedDemand + ShareBasedRate;
                    }
                    Node.Outputs[i]->AllocatedInputRate = Node.AllocatedOutputRates[i];
                }
                else
                {
                    if (IsSet(Splitter.mOutputStates[i], EOutputState::Connected))
                    {
                        if (IsSet(Splitter.mOutputStates[i], EOutputState::Automatic))
                        {
                            int64 Rate = RatePerShare * Node.PotentialShares[i];
                            if (Remainder > 0)
                            {
                                auto [ExtraRate,NewUndistributedShares] = std::div(UndistributedShares + RatePerShare * Node.PotentialShares[i],Remainder);
                                UE_LOG(
                                    LogAutoSplitters,
                                    Display,
                                    TEXT("Output %d: increasing rate from %lld to %lld, newUndistributedShares=%lld"),
                                    i,
                                    Rate,
                                    Rate + ExtraRate,
                                    NewUndistributedShares
                                    );
                                UndistributedShares = NewUndistributedShares;
                                Rate += ExtraRate;
                            }

                            auto [ShareBasedRate,OutputRemainder] = std::div(Rate,FRACTIONAL_SHARE_MULTIPLIER);
                            if (OutputRemainder != 0)
                            {
                                UE_LOG(
                                    LogAutoSplitters,
                                    Warning,
                                    TEXT("Could not calculate fixed precision output rate for output %d: RatePerShare=%lld PotentialShares=%lld rate=%lld remainder=%lld"),
                                    i,
                                    RatePerShare,
                                    Node.PotentialShares[i],
                                    ShareBasedRate,
                                    OutputRemainder
                                );
                                UndistributedRate += OutputRemainder;
                            }
                            Node.AllocatedOutputRates[i] = ShareBasedRate;
                        }
                        else
                        {
                            Node.AllocatedOutputRates[i] = Splitter.mIntegralOutputRates[i];
                        }
                    }
                }
            }

            if (UndistributedRate > 0)
            {
                UE_LOG(LogAutoSplitters, Warning, TEXT("%lld units of unallocated distribution rate"),UndistributedRate);
            }

            if (true)
            {
                UE_LOG(
                    LogAutoSplitters,
                    Display,
                    TEXT("allocated output rates: %d %d %d"),
                    Node.AllocatedOutputRates[0],
                    Node.AllocatedOutputRates[1],
                    Node.AllocatedOutputRates[2]
                    );
            }
        }
    }

    if (!Valid)
    {
        UE_LOG(
            LogAutoSplitters,
            Warning,
            TEXT("Invalid network configuration, aborting network balancing")
            );
        return {false,SplitterCount};
    }

    // we have a consistent new network setup, now switch the network to the new settings
    for (auto& Level: Network)
    {
        for (auto& Node : Level)
        {
            auto& Splitter = *Node.Splitter;
            bool NeedsSetupDistribution = Node.ConnectionStateChanged;

            if (Splitter.mTargetInputRate != Node.AllocatedInputRate)
            {
                NeedsSetupDistribution = true;
                Splitter.mTargetInputRate = Node.AllocatedInputRate;
            }

            for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
            {
                if (IsSet(Splitter.mOutputStates[i],EOutputState::Connected) && Splitter.mIntegralOutputRates[i] != Node.AllocatedOutputRates[i])
                {
                    NeedsSetupDistribution = true;
                    Splitter.mIntegralOutputRates[i] = Node.AllocatedOutputRates[i];
                }
            }

            if (NeedsSetupDistribution)
            {
                Splitter.SetPersistentFlag(NEEDS_DISTRIBUTION_SETUP);
            }
        }
    }

    return {true,SplitterCount};
}

std::tuple<AMFGBuildableAutoSplitter*, int32, bool> AMFGBuildableAutoSplitter::FindAutoSplitterAndMaxBeltRate(
    UFGFactoryConnectionComponent* Connection, bool Forward)
{
    int32 Rate = INT32_MAX;
    while (Connection->IsConnected())
    {
        Connection = Connection->GetConnection();
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
        if (!Connection->GetOuterBuildable()->HasActorBegunPlay())
        {
#if AUTO_SPLITTERS_DEBUG
            UE_LOG(LogAutoSplitters,Display,TEXT("Encountered not-ready actor %p of type %s"),
                Connection->GetOuterBuildable(),
                *Connection->GetOuterBuildable()->StaticClass()->GetName()
                );
#endif

            return {0,0,false};
        }
#endif
        const auto Belt = Cast<AFGBuildableConveyorBase>(Connection->GetOuterBuildable());
        if (Belt)
        {
            Connection = Forward ? Belt->GetConnection1() : Belt->GetConnection0();
            Rate = std::min(Rate,static_cast<int32>(Belt->GetSpeed()) * (FRACTIONAL_RATE_MULTIPLIER / 2));
            continue;
        }
        return {Cast<AMFGBuildableAutoSplitter>(Connection->GetOuterBuildable()),Rate,true};
    }
    return {nullptr,0,true};
}

std::tuple<AFGBuildableFactory*, int32, bool> AMFGBuildableAutoSplitter::FindFactoryAndMaxBeltRate(
    UFGFactoryConnectionComponent* Connection, bool Forward)
{
    int32 Rate = INT32_MAX;
    while (Connection->IsConnected())
    {
        Connection = Connection->GetConnection();
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
        if (!Connection->GetOuterBuildable()->HasActorBegunPlay())
        {
#if AUTO_SPLITTERS_DEBUG
            UE_LOG(LogAutoSplitters,Display,TEXT("Encountered not-ready actor %p of type %s"),
                Connection->GetOuterBuildable(),
                *Connection->GetOuterBuildable()->StaticClass()->GetName()
                );
#endif

            return {0,0,false};
        }
#endif
        const auto Belt = Cast<AFGBuildableConveyorBase>(Connection->GetOuterBuildable());
        if (Belt)
        {
            Connection = Forward ? Belt->GetConnection1() : Belt->GetConnection0();
            Rate = std::min(Rate,static_cast<int32>(Belt->GetSpeed()) * (FRACTIONAL_RATE_MULTIPLIER / 2));
            continue;
        }
        return {Cast<AFGBuildableFactory>(Connection->GetOuterBuildable()),Rate,true};
    }
    return {nullptr,0,true};
}

bool AMFGBuildableAutoSplitter::DiscoverHierarchy(
    TArray<TArray<FNetworkNode>>& Nodes,
    AMFGBuildableAutoSplitter* Splitter,
    const int32 Level,
    FNetworkNode* InputNode,
    const int32 ChildInParent,
    AMFGBuildableAutoSplitter* Root,
    bool ExtractPotentialShares
)
{
    if (!Splitter->HasActorBegunPlay())
        return false;
    if (!Nodes.IsValidIndex(Level))
    {
        Nodes.Emplace();
    }
    Splitter->mRootSplitter = Root;
    auto& Node = Nodes[Level][Nodes[Level].Emplace(Splitter,InputNode)];
    if (InputNode)
    {
        InputNode->Outputs[ChildInParent] = &Node;
        Node.MaxInputRate = InputNode->MaxOutputRates[ChildInParent];
    }
    else
    {
        auto [_,MaxRate,Ready] = FindAutoSplitterAndMaxBeltRate(Splitter->mInputs[0],false);
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
        if (!Ready)
        {
#if AUTO_SPLITTERS_DEBUG
            UE_LOG(LogAutoSplitters,Display,TEXT("Splitter %p: Aborting because not ready"),Splitter);
#endif
            return false;
        }
#endif
        Node.MaxInputRate = MaxRate;
    }
    for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
    {
        const auto [Downstream,MaxRate,Ready] = FindFactoryAndMaxBeltRate(Splitter->mOutputs[i], true);
#if AUTO_SPLITTERS_DELAY_UNTIL_READY
        if (!Ready)
        {
#if AUTO_SPLITTERS_DEBUG
            UE_LOG(LogAutoSplitters,Display,TEXT("Splitter %p: Aborting because not ready"),Splitter);
#endif
            return false;
        }
#endif
        Node.MaxOutputRates[i] = MaxRate;
        if (Downstream)
        {
            const auto DownstreamAutoSplitter = Cast<AMFGBuildableAutoSplitter>(Downstream);
            if (DownstreamAutoSplitter)
            {
                if (!DiscoverHierarchy(Nodes, DownstreamAutoSplitter, Level + 1, &Node, i, Root, ExtractPotentialShares))
                {
                    return false;
                }
            }
            else
            {
                if (ExtractPotentialShares)
                    Node.PotentialShares[i] = static_cast<int32>(Downstream->GetPendingPotential() * FRACTIONAL_SHARE_MULTIPLIER);
                else
                    Node.PotentialShares[i] = FRACTIONAL_SHARE_MULTIPLIER;
            }
        }
    }
    return true;
}

void AMFGBuildableAutoSplitter::SetSplitterVersion(uint32 Version)
{
    if (Version < 1 || Version > 254)
    {
        UE_LOG(LogAutoSplitters,Fatal,TEXT("Invalid Auto Splitter version: %d"),Version);
    }
    if (Version < GetSplitterVersion())
    {
        UE_LOG(LogAutoSplitters,Fatal,TEXT("Cannot downgrade Auto Splitter from version %d to %d"),GetSplitterVersion(),Version);
    }
    mPersistentState = (mPersistentState & ~0xFFu) | (Version & 0xFFu);
}
