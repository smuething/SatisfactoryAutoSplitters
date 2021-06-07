// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/MFGReplicationDetailActor_BuildableAutoSplitter.h"

void AMFGReplicationDetailActor_BuildableAutoSplitter::GetLifetimeReplicatedProps(
    TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mTransientState);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mOutputStates);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mPersistentState);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mTargetInputRate);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mIntegralOutputRates);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mLeftInCycle);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mCycleLength);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mCachedInventoryItemCount);
    DOREPLIFETIME(AMFGReplicationDetailActor_BuildableAutoSplitter,mItemRate);
}

void AMFGReplicationDetailActor_BuildableAutoSplitter::UpdateInternalReplicatedValues()
{
    Super::UpdateInternalReplicatedValues();
    mTransientState = GetSplitter()->mTransientState;
    mOutputStates = GetSplitter()->mOutputStates;
    mPersistentState = GetSplitter()->mPersistentState;
    mTargetInputRate = GetSplitter()->mTargetInputRate;
    mIntegralOutputRates = GetSplitter()->mIntegralOutputRates;
    mLeftInCycle = GetSplitter()->mLeftInCycle;
    mCycleLength = GetSplitter()->mCycleLength;
    mCachedInventoryItemCount = GetSplitter()->mCachedInventoryItemCount;
    mItemRate = GetSplitter()->mItemRate;
}

void AMFGReplicationDetailActor_BuildableAutoSplitter::FlushReplicationActorStateToOwner()
{
    Super::FlushReplicationActorStateToOwner();
    /*
    GetSplitter()->mTransientState = mTransientState;
    GetSplitter()->mOutputStates = mOutputStates;
    GetSplitter()->mPersistentState = mPersistentState;
    GetSplitter()->mTargetInputRate = mTargetInputRate;
    GetSplitter()->mIntegralOutputRates = mIntegralOutputRates;
    GetSplitter()->mLeftInCycle = mLeftInCycle;
    GetSplitter()->mCycleLength = mCycleLength;
    GetSplitter()->mCachedInventoryItemCount = mCachedInventoryItemCount;
    GetSplitter()->mItemRate = mItemRate;
    */
}
