// ILikeBanas

#pragma once

#include <array>

#include "FGPlayerController.h"
#include "FGFactoryConnectionComponent.h"
#include "Buildables/FGBuildableAttachmentSplitter.h"
#include "Buildables/FGBuildableConveyorBase.h"

#include "AutoSplittersModule.h"
#include "AutoSplittersRCO.h"
#include "util/BitField.h"

#include "MFGBuildableAutoSplitter.generated.h"

UENUM(BlueprintType, Meta = (BitFlags))
enum class EOutputState : uint8
{
    Automatic UMETA(DisplayName = "Automatic"),
    Connected UMETA(DisplayName = "Connected"),
    AutoSplitter UMETA(DisplayName = "AutoSplitter"),
};

template <>
struct is_enum_bitfield<EOutputState> : std::true_type {};


enum class EAutoSplitterPersistentFlags : uint32
{
    // first eight bits reserved for version
    ManualInputRate        =  8,
    NeedsConnectionsFixup  =  9,
    NeedsDistributionSetup = 10,
};

template<>
struct is_enum_bitfield<EAutoSplitterPersistentFlags> : std::true_type{};

enum class EAutoSplitterTransientFlags : uint32
{
    // first eight bits reserved for error code

    // replication is currently turned on
    IsReplicationEnabled          =  8,

    // splitter was loaded from save game and needs to be processed accordingly in BeginPlay()
    NeedsLoadedSplitterProcessing =  9,

    // splitter was deemed incompatible after load process, remove in Invoke_BeginPlay() hook
    DismantleAfterLoading         = 10,

};

template<>
struct is_enum_bitfield<EAutoSplitterTransientFlags> : std::true_type{};


/**
 *
 */
UCLASS()
class AUTOSPLITTERS_API AMFGBuildableAutoSplitter : public AFGBuildableAttachmentSplitter
{
    GENERATED_BODY()

    friend class FAutoSplittersModule;
    friend class AMFGAutoSplitterHologram;
    friend class UAutoSplittersRCO;
    friend class AMFGReplicationDetailActor_BuildableAutoSplitter;

public:

    // shorter names to avoid crazy amounts of typing
    using EPersistent = EAutoSplitterPersistentFlags;
    using ETransient  = EAutoSplitterTransientFlags;

    static constexpr uint32 VERSION = 1;

    static constexpr int32 MAX_INVENTORY_SIZE = 10;
    static constexpr float EXPONENTIAL_AVERAGE_WEIGHT = 0.5f;
    static constexpr int32 NUM_OUTPUTS = 3;
    static constexpr float BLOCK_DETECTION_THRESHOLD = 0.5f;

    static constexpr int32 FRACTIONAL_RATE_DIGITS = 3;
    static constexpr int32 FRACTIONAL_RATE_MULTIPLIER = Pow_Constexpr(10,FRACTIONAL_RATE_DIGITS);
    static constexpr float INV_FRACTIONAL_RATE_MULTIPLIER = 1.0f / FRACTIONAL_RATE_MULTIPLIER;

    static constexpr int32 FRACTIONAL_SHARE_DIGITS = 5;
    static constexpr int64 FRACTIONAL_SHARE_MULTIPLIER = Pow_Constexpr(10,FRACTIONAL_SHARE_DIGITS);
    static constexpr float INV_FRACTIONAL_SHARE_MULTIPLIER = 1.0f / FRACTIONAL_SHARE_MULTIPLIER;

    static constexpr float UPGRADE_POSITION_REQUIRED_DELTA = 100.0f;

public:

    AMFGBuildableAutoSplitter();
    virtual void GetLifetimeReplicatedProps( TArray< FLifetimeProperty >& OutLifetimeProps ) const override;
    virtual void PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker) override;;

    virtual void BeginPlay() override;
    virtual void PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override;

    virtual UClass* GetReplicationDetailActorClass() const override;

protected:

    virtual void Factory_Tick(float dt) override;
    virtual bool Factory_GrabOutput_Implementation( UFGFactoryConnectionComponent* connection, FInventoryItem& out_item, float& out_OffsetBeyond, TSubclassOf< UFGItemDescriptor > type ) override;
    virtual void FillDistributionTable(float dt) override;

    UAutoSplittersRCO* RCO() const
    {
        return UAutoSplittersRCO::Get(GetWorld());
    }

    void Server_EnableReplication(float Duration);

    bool Server_SetTargetRateAutomatic(bool Automatic);

    bool Server_SetTargetInputRate(float Rate);

    bool Server_SetOutputRate(int32 Output, float Rate);

    bool Server_SetOutputAutomatic(int32 Output, bool Automatic);

    void Server_ReplicationEnabledTimeout();

private:

    void SetupDistribution(bool LoadingSave = false);
    void PrepareCycle(bool AllowCycleExtension, bool Reset = false);

    bool IsOutputBlocked(int32 Output) const
    {
        return mBlockedFor[Output] > BLOCK_DETECTION_THRESHOLD;
    }

protected:

    UPROPERTY(Transient)
    uint32 mTransientState;

    UPROPERTY(SaveGame, Meta = (DeprecatedProperty,NoAutoJson))
    TArray<float> mOutputRates_DEPRECATED;

    UPROPERTY(SaveGame, BlueprintReadOnly, Meta = (NoAutoJson))
    TArray<int32> mOutputStates;

    UPROPERTY(SaveGame, BlueprintReadOnly, Meta = (NoAutoJson))
    TArray<int32> mRemainingItems;

    UPROPERTY(SaveGame, Meta = (NoAutoJson))
    uint32 mPersistentState;

    UPROPERTY(SaveGame, Meta = (NoAutoJson))
    int32 mTargetInputRate;

    UPROPERTY(SaveGame, Meta = (NoAutoJson))
    TArray<int32> mIntegralOutputRates;

    UPROPERTY(Transient, BlueprintReadOnly)
    AMFGBuildableAutoSplitter* mRootSplitter;

    UPROPERTY(Transient, BlueprintReadOnly)
    TArray<int32> mItemsPerCycle;

    UPROPERTY(Transient, BlueprintReadOnly)
    int32 mLeftInCycle;

    UPROPERTY(Transient, BlueprintReadWrite)
    bool mDebug;

    UPROPERTY(Transient, BlueprintReadOnly)
    int32 mCycleLength;

    UPROPERTY(Transient, BlueprintReadOnly)
    int32 mCachedInventoryItemCount;

    UPROPERTY(Transient, BlueprintReadOnly)
    float mItemRate;

private:

    std::array<float,NUM_OUTPUTS> mBlockedFor;
    std::array<int32,NUM_OUTPUTS> mAssignedItems;
    std::array<int32,NUM_OUTPUTS> mGrabbedItems;
    std::array<float,NUM_OUTPUTS> mPriorityStepSize;
    std::array<int32,MAX_INVENTORY_SIZE> mAssignedOutputs;
    std::array<int32,NUM_OUTPUTS> mNextInventorySlot;
    std::array<int32,NUM_OUTPUTS> mInventorySlotEnd;

    bool mBalancingRequired;
    bool mNeedsInitialDistributionSetup;
    float mCycleTime;
    int32 mReallyGrabbed;

    FTimerHandle mReplicationTimer;

public:

    UFUNCTION(BlueprintPure)
    static int32 GetFractionalRateDigits()
    {
        return FRACTIONAL_RATE_DIGITS;
    }

    UFUNCTION(BlueprintPure)
    bool IsReplicationEnabled() const
    {
        return IsSplitterFlagSet(ETransient::IsReplicationEnabled);
    }

    UFUNCTION(BlueprintCallable)
    void EnableReplication(float Duration)
    {
        if (HasAuthority())
            Server_EnableReplication(Duration);
        else
            RCO()->EnableReplication(this,Duration);
    }

    UFUNCTION(BlueprintCallable,BlueprintPure)
    bool IsTargetRateAutomatic() const
    {
        return !IsSplitterFlagSet(EPersistent::ManualInputRate);
    }

    UFUNCTION(BlueprintCallable)
    void SetTargetRateAutomatic(bool Automatic)
    {
        if (HasAuthority())
            Server_SetTargetRateAutomatic(Automatic);
        else
            RCO()->SetTargetRateAutomatic(this,Automatic);
    }


    UFUNCTION(BlueprintPure)
    float GetTargetInputRate() const;

    UFUNCTION(BlueprintCallable)
    void SetTargetInputRate(float Rate)
    {
        if (HasAuthority())
            Server_SetTargetInputRate(Rate);
        else
            RCO()->SetTargetInputRate(this,Rate);
    }

    UFUNCTION(BlueprintPure)
    float GetOutputRate(int32 Output) const;

    UFUNCTION(BlueprintCallable)
    void SetOutputRate(int32 Output, float Rate)
    {
        if (HasAuthority())
            Server_SetOutputRate(Output,Rate);
        else
            RCO()->SetOutputRate(this,Output,Rate);
    }

    UFUNCTION(BlueprintCallable)
    void SetOutputAutomatic(int32 Output, bool Automatic)
    {
        if (HasAuthority())
            Server_SetOutputAutomatic(Output,Automatic);
        else
            RCO()->SetOutputAutomatic(this,Output,Automatic);
    }

    UFUNCTION(BlueprintCallable)
    void BalanceNetwork(bool RootOnly = true)
    {
        if (HasAuthority())
            Server_BalanceNetwork(this,RootOnly);
        else
            RCO()->BalanceNetwork(this,RootOnly);
    }

    uint32 GetSplitterVersion() const
    {
        return mPersistentState & 0xFFu;
    }

    UFUNCTION(BlueprintPure)
    int32 GetInventorySize() const
    {
        return mCachedInventoryItemCount;
    }

    UFUNCTION(BlueprintPure)
    float GetItemRate() const
    {
        return mItemRate;
    }

    UFUNCTION(BluePrintPure)
    static bool IsDebugSupported()
    {
#if AUTO_SPLITTERS_DEBUG
        return true;
#else
        return false;
#endif
    }

    UFUNCTION(BluePrintPure)
    bool HasCurrentData() const
    {
        return HasAuthority() || IsReplicationEnabled();
    }

    UFUNCTION(BlueprintPure)
    int32 GetError() const
    {
        return mTransientState & 0xFFu;
    }

    struct FNetworkNode
    {
        AMFGBuildableAutoSplitter* Splitter;
        FNetworkNode* Input;
        int32 MaxInputRate;
        std::array<FNetworkNode*,NUM_OUTPUTS> Outputs;
        std::array<int64,NUM_OUTPUTS> PotentialShares;
        std::array<int32,NUM_OUTPUTS> MaxOutputRates;
        int32 FixedDemand;
        int64 Shares;
        int32 AllocatedInputRate;
        std::array<int32,NUM_OUTPUTS> AllocatedOutputRates;
        bool ConnectionStateChanged;

        explicit FNetworkNode(AMFGBuildableAutoSplitter* Splitter, FNetworkNode* Input = nullptr)
            : Splitter(Splitter)
            , Input(Input)
            , MaxInputRate(0)
            , Outputs({nullptr})
            , PotentialShares({0})
            , MaxOutputRates({0})
            , FixedDemand(0)
            , Shares(0)
            , AllocatedInputRate(0)
            , AllocatedOutputRates({0})
            , ConnectionStateChanged(false)
        {}
    };

private:

    void SetError(uint8 Error)
    {
        mTransientState = (mTransientState & ~0xFFu) | static_cast<uint32>(Error);
    }

    void ClearError()
    {
        mTransientState &= ~0xFFu;;
    }

    void FixupConnections();
    void SetupInitialDistributionState();

    static std::tuple<bool,int32> Server_BalanceNetwork(AMFGBuildableAutoSplitter* ForSplitter, bool RootOnly = false);

    static std::tuple<AMFGBuildableAutoSplitter*, int32, bool>
    FindAutoSplitterAndMaxBeltRate(UFGFactoryConnectionComponent* Connection, bool Forward);

    static std::tuple<AFGBuildableFactory*, int32, bool>
    FindFactoryAndMaxBeltRate(UFGFactoryConnectionComponent* Connection, bool Forward);

    static bool DiscoverHierarchy(
        TArray<TArray<FNetworkNode>>& Nodes,
        AMFGBuildableAutoSplitter* Splitter,
        const int32 Level,
        FNetworkNode* InputNode,
        const int32 ChildInParent,
        AMFGBuildableAutoSplitter* Root, bool ExtractPotentialShares
    );

    void SetSplitterVersion(uint32 Version);

    FORCEINLINE bool IsSplitterFlagSet(EPersistent Flag) const
    {
        return IsSet(mPersistentState,Flag);
    }

    FORCEINLINE void SetSplitterFlag(EPersistent Flag, bool Value)
    {
        mPersistentState = SetFlag(mPersistentState,Flag,Value);
    }

    FORCEINLINE void SetSplitterFlag(EPersistent Flag)
    {
        mPersistentState = SetFlag(mPersistentState,Flag);
    }

    FORCEINLINE void ClearSplitterFlag(EPersistent Flag)
    {
        mPersistentState = ClearFlag(mPersistentState,Flag);
    }

    FORCEINLINE void ToggleSplitterFlag(EPersistent Flag)
    {
        mPersistentState = ToggleFlag(mPersistentState,Flag);
    }

    FORCEINLINE bool IsSplitterFlagSet(ETransient Flag) const
    {
        return IsSet(mTransientState,Flag);
    }

    FORCEINLINE void SetSplitterFlag(ETransient Flag, bool Value)
    {
        mTransientState = SetFlag(mTransientState,Flag,Value);
    }

    FORCEINLINE void SetSplitterFlag(ETransient Flag)
    {
        mTransientState = SetFlag(mTransientState,Flag);
    }

    FORCEINLINE void ClearSplitterFlag(ETransient Flag)
    {
        mTransientState = ClearFlag(mTransientState,Flag);
    }

    FORCEINLINE void ToggleSplitterFlag(ETransient Flag)
    {
        mTransientState = ToggleFlag(mTransientState,Flag);
    }

};
