// ILikeBanas

#pragma once

#include <array>

#include "AkBankManager.h"
#include "FGFactoryConnectionComponent.h"
#include "Buildables/FGBuildableAttachmentSplitter.h"
#include "Buildables/FGBuildableConveyorBase.h"

#include "AutoSplittersModule.h"

#include "MFGBuildableAutoSplitter.generated.h"

UENUM(BlueprintType, Meta = (BitFlags))
enum class EOutputState : uint8
{
    Automatic UMETA(DisplayName = "Automatic"),
    Connected UMETA(DisplayName = "Connected"),
    AutoSplitter UMETA(DisplayName = "AutoSplitter")
};

constexpr int32 Flag(EOutputState flag)
{
    return 1 << static_cast<int32>(flag);
}

constexpr bool IsSet(int32 BitField, EOutputState flag)
{
    return BitField & Flag(flag);
}

constexpr int32 SetFlag(int32 BitField, EOutputState flag)
{
    return BitField | Flag(flag);
}

constexpr int32 ClearFlag(int32 BitField, EOutputState flag)
{
    return BitField & ~Flag(flag);
}

constexpr int32 SetFlag(int32 BitField, EOutputState flag, bool Enabled)
{
    return (BitField & ~Flag(flag)) | (Enabled * Flag(flag));
}

constexpr static int32 Pow_Constexpr(int32 Base, int32 Exponent)
{
    int32 Result = 1;
    while (Exponent-- > 0)
        Result *= Base;
    return Result;
}

/**
 *
 */
UCLASS()
class AUTOSPLITTERS_API AMFGBuildableAutoSplitter : public AFGBuildableAttachmentSplitter
{
    GENERATED_BODY()

    friend class FAutoSplittersModule;
    friend class AMFGAutoSplitterHologram;

    static constexpr uint32 MANUAL_INPUT_RATE       = 1 <<  8;
    static constexpr uint32 NEEDS_CONNECTIONS_FIXUP = 1 <<  9;

    static constexpr uint32 VERSION = 1;

    static constexpr int32 MAX_INVENTORY_SIZE = 10;
    static constexpr float EXPONENTIAL_AVERAGE_WEIGHT = 0.5f;
    static constexpr int32 NUM_OUTPUTS = 3;
    static constexpr float BLOCK_DETECTION_THRESHOLD = 0.5f;

    static constexpr int32 FRACTIONAL_RATE_DIGITS = 3;
    static constexpr int32 FRACTIONAL_RATE_MULTIPLIER = Pow_Constexpr(10,FRACTIONAL_RATE_DIGITS);
    static constexpr float INV_FRACTIONAL_RATE_MULTIPLIER = 1.0f / FRACTIONAL_RATE_MULTIPLIER;

public:

    AMFGBuildableAutoSplitter();

    virtual void BeginPlay() override;
    virtual void PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override;

protected:

    virtual void Factory_Tick(float dt) override;
    virtual bool Factory_GrabOutput_Implementation( UFGFactoryConnectionComponent* connection, FInventoryItem& out_item, float& out_OffsetBeyond, TSubclassOf< UFGItemDescriptor > type ) override;
    virtual void FillDistributionTable(float dt) override;

private:

    void SetupDistribution(bool LoadingSave = false);
    void PrepareCycle(bool AllowCycleExtension, bool Reset = false);

    bool IsOutputBlocked(int32 Output) const
    {
        return mBlockedFor[Output] > BLOCK_DETECTION_THRESHOLD;
    }

public:

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

    UPROPERTY(Transient,BlueprintReadWrite)
    bool mDebug;

    UPROPERTY(Transient,BlueprintReadOnly)
    int32 mCycleLength;

    UFUNCTION(BlueprintCallable,BlueprintPure)
    static int32 GetFractionalRateDigits()
    {
        return FRACTIONAL_RATE_DIGITS;
    }

    UFUNCTION(BlueprintCallable,BlueprintPure)
    bool IsTargetRateAutomatic() const
    {
        return !IsPersistentFlagSet(MANUAL_INPUT_RATE);
    }

    UFUNCTION(BlueprintCallable)
    bool SetTargetRateAutomatic(bool Automatic);

    UFUNCTION(BlueprintCallable,BlueprintPure)
    float GetTargetInputRate() const;

    UFUNCTION(BlueprintCallable)
    bool SetTargetInputRate(float Rate);

    UFUNCTION(BlueprintCallable,BlueprintPure)
    float GetOutputRate(int32 Output) const;

    UFUNCTION(BlueprintCallable)
    bool SetOutputRate(int32 Output, float Rate);

    UFUNCTION(BlueprintCallable)
    bool SetOutputAutomatic(int32 Output, bool Automatic);

    UFUNCTION(BlueprintCallable)
    int32 BalanceNetwork(int32& SplitterCount_Out)
    {
        bool Result;
        std::tie(Result,SplitterCount_Out) = BalanceNetwork_Internal(this);
        return Result;
    }

    uint32 GetSplitterVersion() const
    {
        return mPersistentState & 0xFFu;
    }

    bool IsPersistentFlagSet(uint32 Flag) const
    {
        return !!(mPersistentState & Flag);
    }

    UFUNCTION(BlueprintCallable,BlueprintPure)
    int32 GetInventorySize() const
    {
        return mCachedInventoryItemCount;
    }

    UFUNCTION(BlueprintCallable,BlueprintPure)
    float GetItemRate() const
    {
        return mItemRate;
    }

    UFUNCTION(BlueprintCallable,BluePrintPure)
    bool IsDebugSupported() const
    {
#if AUTO_SPLITTERS_DEBUG
        return true;
#else
        return false;
#endif
    }

    struct FNetworkNode
    {
        AMFGBuildableAutoSplitter* Splitter;
        FNetworkNode* Input;
        int32 MaxInputRate;
        std::array<FNetworkNode*,NUM_OUTPUTS> Outputs;
        std::array<int32,NUM_OUTPUTS> MaxOutputRates;
        int32 FixedDemand;
        int32 Shares;
        int32 AllocatedInputRate;
        std::array<int32,NUM_OUTPUTS> AllocatedOutputRates;
        bool ConnectionStateChanged;

        explicit FNetworkNode(AMFGBuildableAutoSplitter* Splitter, FNetworkNode* Input = nullptr)
            : Splitter(Splitter)
            , Input(Input)
            , MaxInputRate(0)
            , Outputs({nullptr})
            , MaxOutputRates({0})
            , FixedDemand(0)
            , Shares(0)
            , AllocatedInputRate(0)
            , AllocatedOutputRates({0})
            , ConnectionStateChanged(false)
        {}
    };

private:

    void FixupConnections();
    void SetupInitialDistributionState();

    static std::tuple<bool,int32> BalanceNetwork_Internal(AMFGBuildableAutoSplitter* ForSplitter, bool RootOnly = false);

    static std::tuple<AMFGBuildableAutoSplitter*,int32>
    FindAutoSplitterAndMaxBeltRate(UFGFactoryConnectionComponent* Connection, bool Forward);

    static bool DiscoverHierarchy(
        TArray<TArray<FNetworkNode>>& Nodes,
        AMFGBuildableAutoSplitter* Splitter,
        const int32 Level,
        FNetworkNode* InputNode,
        const int32 ChildInParent,
        AMFGBuildableAutoSplitter* Root
    );

    void SetSplitterVersion(uint32 Version);

    FORCEINLINE void SetPersistentFlag(int32 Flag, bool Value = true)
    {
        mPersistentState = (mPersistentState & ~Flag) | (Value * Flag);
    }

    FORCEINLINE void ClearPersistentFlag(int32 Flag)
    {
        mPersistentState &= ~Flag;
    }

    FORCEINLINE void TogglePersistentFlag(int32 Flag)
    {
        mPersistentState ^= Flag;
    }

    // void RescaleOutputRates();

    std::array<float,NUM_OUTPUTS> mBlockedFor;
    std::array<int32,NUM_OUTPUTS> mAssignedItems;
    std::array<int32,NUM_OUTPUTS> mGrabbedItems;
    std::array<float,NUM_OUTPUTS> mPriorityStepSize;
    std::array<int32,MAX_INVENTORY_SIZE> mAssignedOutputs;
    std::array<int32,NUM_OUTPUTS> mNextInventorySlot;
    std::array<int32,NUM_OUTPUTS> mInventorySlotEnd;

    bool mBalancingRequired;
    bool mNeedsInitialDistributionSetup;
    int32 mCachedInventoryItemCount;
    float mItemRate;
    float mCycleTime;
    int32 mReallyGrabbed;
};
