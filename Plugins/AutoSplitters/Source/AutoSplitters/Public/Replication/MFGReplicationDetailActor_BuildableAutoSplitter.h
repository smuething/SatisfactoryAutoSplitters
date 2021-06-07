// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/FGReplicationDetailActor_BuildableFactory.h"
#include "Buildables/MFGBuildableAutoSplitter.h"

#include "MFGReplicationDetailActor_BuildableAutoSplitter.generated.h"

/**
 *
 */
UCLASS()
class AUTOSPLITTERS_API AMFGReplicationDetailActor_BuildableAutoSplitter : public AFGReplicationDetailActor_BuildableFactory
{
    GENERATED_BODY()

protected:

    UPROPERTY(Replicated)
    uint32 mTransientState;

    UPROPERTY(Replicated)
    TArray<int32> mOutputStates;

    UPROPERTY(Replicated)
    uint32 mPersistentState;

    UPROPERTY(Replicated)
    int32 mTargetInputRate;

    UPROPERTY(Replicated)
    TArray<int32> mIntegralOutputRates;

    UPROPERTY(Replicated)
    int32 mLeftInCycle;

    UPROPERTY(Replicated)
    int32 mCycleLength;

    UPROPERTY(Replicated)
    int32 mCachedInventoryItemCount;

    UPROPERTY(Replicated)
    float mItemRate;

    FORCEINLINE AMFGBuildableAutoSplitter* GetSplitter() const
    {
        return CastChecked<AMFGBuildableAutoSplitter>(mOwningBuildable);
    }

public:

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void UpdateInternalReplicatedValues() override;
    virtual void FlushReplicationActorStateToOwner() override;
};
