// ILikeBanas

#include "Buildables/MFGBuildableAutoSplitter.h"

#include <numeric>

#include "AutoSplittersLog.h"
#include "FGFactoryConnectionComponent.h"
#include "Buildables/FGBuildableConveyorBase.h"

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
    : mOutputStates(MakeTArray<NUM_OUTPUTS>(Flag(EOutputState::Automatic)))
    , mRemainingItems(MakeTArray<NUM_OUTPUTS>(0))
    , mPersistentState(0) // Do the setup in BeginPlay(), otherwise we cannot detect version changes during loading
    , mTargetInputRate(0)
    , mIntegralOutputRates(MakeTArray<NUM_OUTPUTS>(FRACTIONAL_RATE_MULTIPLIER))
    , mRootSplitter(nullptr)
    , mItemsPerCycle(MakeTArray<NUM_OUTPUTS>(0))
    , mLeftInCycle(0)
    , mDebug(false)
    , mCycleLength(0)
    , mBlockedFor(make_array<NUM_OUTPUTS>(0.0f))
    , mAssignedItems(make_array<NUM_OUTPUTS>(0))
    , mGrabbedItems(make_array<NUM_OUTPUTS>(0))
    , mPriorityStepSize(make_array<NUM_OUTPUTS>(0.0f))
    , mBalancingRequired(true)
    , mNeedsInitialDistributionSetup(true)
    , mCachedInventoryItemCount(0)
    , mItemRate(0.0f)
    , mCycleTime(0.0f)
    , mReallyGrabbed(0)
{}

void AMFGBuildableAutoSplitter::Factory_Tick(float dt)
{

    // skip direct splitter base class, it doesn't do anything useful for us
    AFGBuildableConveyorAttachment::Factory_Tick(dt);

    if (mNeedsInitialDistributionSetup)
    {
        SetupInitialDistributionState();
    }

    if (mBalancingRequired)
    {
        auto [valid,_] = BalanceNetwork_Internal(this,true);
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
    mNextInventorySlot = {MAX_INVENTORY_SIZE,MAX_INVENTORY_SIZE,MAX_INVENTORY_SIZE};
    mInventorySlotEnd = {0,0,0};
    std::fill(mAssignedOutputs.begin(),mAssignedOutputs.end(),-1);

    if (mTargetInputRate == 0 && mInputs[0]->IsConnected())
    {
        auto [_,Rate] = FindAutoSplitterAndMaxBeltRate(mInputs[0],false);
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
            mAssignedOutputs[Slot] = Next;
            if (mNextInventorySlot[Next] == MAX_INVENTORY_SIZE)
                mNextInventorySlot[Next] = Slot;
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
}

constexpr std::array<int32,4> MAPPED_COMPONENTS = {0,1,2,3};

void AMFGBuildableAutoSplitter::PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion)
{
    Super::PostLoadGame_Implementation(saveVersion,gameVersion);
    mLeftInCycle = std::accumulate(mRemainingItems.begin(),mRemainingItems.end(),0);
    mCycleLength = std::accumulate(mItemsPerCycle.begin(),mItemsPerCycle.end(),0);
    mCycleTime = -100000.0; // this delays item rate calculation to the first full cycle when loading the game

    if (GetSplitterVersion() == 0)
    {
        UE_LOG(LogAutoSplitters,Display,TEXT("Upgrading saved Auto Splitter from version 0 to 1"));

        for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
        {
            mIntegralOutputRates[i] = FRACTIONAL_RATE_MULTIPLIER;
            mOutputStates[i] = Flag(EOutputState::Automatic);
        }
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
    if (IsPersistentFlagSet(NEEDS_CONNECTIONS_FIXUP))
    {
        FixupConnections();
    }

    Super::BeginPlay();
    SetSplitterVersion(VERSION);
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

            if (DEBUG_THIS_SPLITTER)
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

    if (!(
        IsSet(mOutputStates[0],EOutputState::Connected) ||
        IsSet(mOutputStates[1],EOutputState::Connected) ||
        IsSet(mOutputStates[2],EOutputState::Connected)))
    {
        mIntegralOutputRates[0] = mIntegralOutputRates[1] = mIntegralOutputRates[2] = FRACTIONAL_RATE_MULTIPLIER;
        mItemsPerCycle[0] = mItemsPerCycle[1] = mItemsPerCycle[2] = 4;
        return;
    }

    // calculate item counts per cycle
    for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
    {
        mItemsPerCycle[i] = IsSet(mOutputStates[i], EOutputState::Connected) * mIntegralOutputRates[i];
    }

    auto GCD = std::gcd(std::gcd(mItemsPerCycle[0],mItemsPerCycle[1]),mItemsPerCycle[2]);

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

bool AMFGBuildableAutoSplitter::SetTargetRateAutomatic(bool Automatic)
{
    SetPersistentFlag(MANUAL_INPUT_RATE,!Automatic);
    if (!Automatic)
        mTargetInputRate = 0; // will trigger an update during the next factory tick
    return true;
}

float AMFGBuildableAutoSplitter::GetTargetInputRate() const
{
    return mTargetInputRate * INV_FRACTIONAL_RATE_MULTIPLIER;
}

bool AMFGBuildableAutoSplitter::SetTargetInputRate(float Rate)
{
    if (Rate < 0)
        return false;

    if (IsTargetRateAutomatic())
        return false;

    int32 IntRate = static_cast<int32>(Rate * FRACTIONAL_RATE_MULTIPLIER);

    bool Changed = mTargetInputRate != IntRate;
    mTargetInputRate = IntRate;

    if (Changed)
        BalanceNetwork_Internal(this);

    return true;
}

float AMFGBuildableAutoSplitter::GetOutputRate(int32 Output) const
{
    if (Output < 0 || Output > NUM_OUTPUTS -1)
        return NAN;

    return static_cast<float>(mIntegralOutputRates[Output]) * INV_FRACTIONAL_RATE_MULTIPLIER;
}

bool AMFGBuildableAutoSplitter::SetOutputRate(const int32 Output, const float Rate)
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

    auto [DownstreamAutoSplitter,_] = FindAutoSplitterAndMaxBeltRate(mOutputs[Output],true);
    bool OldManualInputRate = false;
    int32 OldTargetInputRate = 0;
    if (DownstreamAutoSplitter)
    {
        OldManualInputRate = DownstreamAutoSplitter->IsPersistentFlagSet(MANUAL_INPUT_RATE);
        OldTargetInputRate = DownstreamAutoSplitter->mTargetInputRate;
        DownstreamAutoSplitter->SetPersistentFlag(MANUAL_INPUT_RATE);
        DownstreamAutoSplitter->mTargetInputRate = IntRate;
    }

    auto [valid,_2] = BalanceNetwork_Internal(this);

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

bool AMFGBuildableAutoSplitter::SetOutputAutomatic(int32 Output, bool Automatic)
{

    if (Output < 0 || Output > NUM_OUTPUTS - 1)
        return false;

    if (Automatic == IsSet(mOutputStates[Output],EOutputState::Automatic))
        return true;

    auto [DownstreamAutoSplitter,_] = FindAutoSplitterAndMaxBeltRate(mOutputs[Output],true);
    if (DownstreamAutoSplitter)
    {
        DownstreamAutoSplitter->SetPersistentFlag(MANUAL_INPUT_RATE,!Automatic);
    }
    else
    {
        mOutputStates[Output] = SetFlag(mOutputStates[Output],EOutputState::Automatic,Automatic);
    }

    auto [valid,_2] = BalanceNetwork_Internal(this);
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

constexpr std::array<int32,4> GPartner_Map = {0,1,3,5};

void AMFGBuildableAutoSplitter::FixupConnections()
{
    TInlineComponentArray<UFGFactoryConnectionComponent*, 6> Connections;
    GetComponents(Connections);

    UE_LOG(LogAutoSplitters, Display, TEXT("Fixing up Auto Splitter connections for 0.3.0 upgrade"), Connections.Num());

    TInlineComponentArray<UFGFactoryConnectionComponent*, 6> Partners;

    for (auto& c : Connections)
    {
        UFGFactoryConnectionComponent* Partner = c->IsConnected() ? c->GetConnection() : nullptr;
        Partners.Add(Partner);
        auto Pos = this->GetTransform().InverseTransformPosition(c->GetComponentLocation());
        auto Rot = this->GetTransform().InverseTransformRotation(c->GetComponentRotation().Quaternion());
        if (Partner)
        {
            Partner->ClearConnection();
        }

        if (c->GetName() == TEXT("Output0") || c->GetName() == TEXT("Input0"))
        {
            c->DestroyComponent(false);
            c = nullptr;
        }
    }

    for (int32 i = 0; i < 4; ++i)
    {
        if (Partners[GPartner_Map[i]])
        {
            Connections[i]->SetConnection(Partners[GPartner_Map[i]]);
        }
    }

    ClearPersistentFlag(NEEDS_CONNECTIONS_FIXUP);
    mNeedsInitialDistributionSetup = true;
}

void AMFGBuildableAutoSplitter::SetupInitialDistributionState()
{
    auto [InputSplitter,MaxInputRate] = FindAutoSplitterAndMaxBeltRate(mInputs[0], false);
    mTargetInputRate = MaxInputRate;
    for (int32 i = 0; i < NUM_OUTPUTS; ++i)
    {
        auto [OutputSplitter,MaxRate] = FindAutoSplitterAndMaxBeltRate(mOutputs[i], true);
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

std::tuple<bool,int32> AMFGBuildableAutoSplitter::BalanceNetwork_Internal(AMFGBuildableAutoSplitter* ForSplitter, bool RootOnly)
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

    if(!ForSplitter->HasActorBegunPlay())
    {
        return {false,-1};
    }

    TSet<AMFGBuildableAutoSplitter*> SplitterSet;
    // start by going upstream
    auto Root = ForSplitter;
    SplitterSet.Add(Root);
    for (
        auto [Current,Rate] = FindAutoSplitterAndMaxBeltRate(Root->mInputs[0],false) ;
        Current ;
        std::tie(Current,Rate) = FindAutoSplitterAndMaxBeltRate(Current->mInputs[0],false)
        )
    {
        if (!Current->HasActorBegunPlay())
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

    // Now walk the tree to discover the whole network
    TArray<TArray<FNetworkNode>> Network;
    int32 SplitterCount = 0;
    if (!DiscoverHierarchy(Network,Root,0,nullptr,INT32_MAX,Root))
    {
        Root->mBalancingRequired = true;
        return {false,-1};
    }

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
                    continue;
                }

                if (!IsSet(Splitter.mOutputStates[i], EOutputState::Connected))
                {
                    Splitter.mOutputStates[i] = SetFlag(Splitter.mOutputStates[i], EOutputState::Connected);
                    Node.ConnectionStateChanged = true;
                }

                Splitter.mOutputStates[i] = SetFlag(Splitter.mOutputStates[i],EOutputState::AutoSplitter,Node.Outputs[i] != nullptr);
                if (Node.Outputs[i])
                {
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
                    if (IsSet(Splitter.mOutputStates[i],EOutputState::Automatic))
                    {
                        ++Node.Shares;
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
            auto [RatePerShare,Remainder] = Node.Shares > 0 ? std::div(AvailableForShares, Node.Shares) : std::div_t{0,0};

            if (Remainder != 0)
            {
                UE_LOG(
                    LogAutoSplitters,
                    Warning,
                    TEXT("Could not evenly distribute rate among shares: available=%d shares=%d rate=%d remainder=%d"),
                    AvailableForShares,
                    Node.Shares,
                    RatePerShare,
                    Remainder
                );
                Valid = false;
                break;
            }

            if (DEBUG_SPLITTER(Splitter))
            {
                UE_LOG(
                    LogAutoSplitters,
                    Display,
                    TEXT("distribution setup: input=%d fixedDemand=%d ratePerShare=%d shares=%d"),
                    Node.AllocatedInputRate,
                    Node.FixedDemand,
                    RatePerShare,
                    Node.Shares
                    );
            }

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
                        Node.AllocatedOutputRates[i] = Node.Outputs[i]->FixedDemand + Node.Outputs[i]->Shares *
                            RatePerShare;
                    }
                    Node.Outputs[i]->AllocatedInputRate = Node.AllocatedOutputRates[i];
                }
                else
                {
                    if (IsSet(Splitter.mOutputStates[i], EOutputState::Connected))
                    {
                        if (IsSet(Splitter.mOutputStates[i], EOutputState::Automatic))
                        {
                            Node.AllocatedOutputRates[i] = RatePerShare;
                        }
                        else
                        {
                            Node.AllocatedOutputRates[i] = Splitter.mIntegralOutputRates[i];
                        }
                    }
                }
            }
            if (DEBUG_SPLITTER(Splitter))
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
            bool NeedsSetupDistribution = false;

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
                Splitter.SetupDistribution();
            }
        }
    }

    return {true,SplitterCount};
}

std::tuple<AMFGBuildableAutoSplitter*,int32> AMFGBuildableAutoSplitter::FindAutoSplitterAndMaxBeltRate(
    UFGFactoryConnectionComponent* Connection, bool Forward)
{
    int32 Rate = INT32_MAX;
    while (Connection->IsConnected())
    {
        Connection = Connection->GetConnection();
        const auto Belt = Cast<AFGBuildableConveyorBase>(Connection->GetOuterBuildable());
        if (Belt)
        {
            Connection = Forward ? Belt->GetConnection1() : Belt->GetConnection0();
            Rate = std::min(Rate,static_cast<int32>(Belt->GetSpeed()) * (FRACTIONAL_RATE_MULTIPLIER / 2));
            continue;
        }
        return {Cast<AMFGBuildableAutoSplitter>(Connection->GetOuterBuildable()),Rate};
    }
    return {nullptr,0};
}

bool AMFGBuildableAutoSplitter::DiscoverHierarchy(
    TArray<TArray<FNetworkNode>>& Nodes,
    AMFGBuildableAutoSplitter* Splitter,
    const int32 Level,
    FNetworkNode* InputNode,
    const int32 ChildInParent,
    AMFGBuildableAutoSplitter* Root
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
        auto [_,MaxRate] = FindAutoSplitterAndMaxBeltRate(Splitter->mInputs[0],false);
        Node.MaxInputRate = MaxRate;
    }
    for (int32 i = 0 ; i < NUM_OUTPUTS ; ++i)
    {
        const auto [Downstream,MaxRate] = FindAutoSplitterAndMaxBeltRate(Splitter->mOutputs[i], true);
        Node.MaxOutputRates[i] = MaxRate;
        if (Downstream)
        {
            if (!DiscoverHierarchy(Nodes, Downstream, Level + 1, &Node, i, Root))
            {
                return false;
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
