// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "HeapCensusDemoObject.generated.h"

/**
 * The object Heap.Leak and Heap.Churn create.
 *
 * It exists so a buyer can watch the growth table react without first building a leak into their own game.
 * There is nothing staged about what it produces: these are ordinary UObjects, counted by the same walk over
 * the object array that counts everything else, and the class appears in the table under its own name like
 * any other. The only difference between the healthy demonstration and the leaking one is whether anything
 * keeps a reference.
 *
 * It carries a payload because objects are what HeapCensus counts, and a demonstration whose objects weigh
 * nothing would move this plugin's numbers and nothing else. With a payload, a leak shown here also shows up
 * in Task Manager and in Memory Insights - so a sceptical buyer can check this plugin against another one.
 */
UCLASS(NotBlueprintable, DisplayName = "HeapCensus Demo Object")
class HEAPCENSUS_API UHeapCensusDemoObject : public UObject
{
	GENERATED_BODY()

public:
	/** Give the object some weight. Called once, right after construction. */
	void SetPayloadKilobytes(int32 Kilobytes);

	/** How many bytes this object is carrying. */
	int32 GetPayloadBytes() const { return Payload.Num(); }

private:
	/**
	 * Dead weight, on purpose.
	 *
	 * Not a UPROPERTY: a byte array does not need reflection, and keeping it off the property table means the
	 * garbage collector does not walk it. What is being demonstrated is a leak of objects, not a leak of
	 * references, and the two should not be muddled inside the tool that measures them.
	 */
	TArray<uint8> Payload;
};
