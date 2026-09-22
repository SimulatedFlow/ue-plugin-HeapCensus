// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HeapCensusTypes.generated.h"

/**
 * What HeapCensus thinks of a class, or of the whole census.
 *
 * The three values are also the three exit codes of Heap.Gate - 0, 1, 2 - and they mean the same three
 * things there as in LoadLens, LocaleGuard and AssetWarden, so a project that owns more than one of these
 * plugins does not have to learn a second convention.
 */
UENUM(BlueprintType)
enum class EHeapVerdict : uint8
{
	/** Nothing is climbing fast enough to worry about. Also what too little data returns - see EvaluateEntry. */
	Ok			UMETA(DisplayName = "Ok"),

	/** Climbing, but either not past the threshold or not without ever falling back. Worth a second look. */
	Warn		UMETA(DisplayName = "Warn"),

	/** Past the threshold and it never gave anything back. This is the shape of a leak. */
	Leaking		UMETA(DisplayName = "Leaking"),
};

/** How to order the class table. Growth is the default, and it is the whole point of the plugin. */
UENUM(BlueprintType)
enum class EHeapSort : uint8
{
	/** Objects per minute, biggest first. The list a leak shows up at the top of. */
	Growth		UMETA(DisplayName = "Growth"),

	/** Live objects, most first. What obj list gives you, and what a healthy big class also looks like. */
	Count		UMETA(DisplayName = "Count"),

	/** The highest count seen inside the window, most first. */
	Peak		UMETA(DisplayName = "Peak"),

	/** Alphabetical. For a dump you intend to diff against another one. */
	Name		UMETA(DisplayName = "Name"),
};

/**
 * One class, and what the census knows about it.
 *
 * There is one of these per class name, never one per object: a census of a big project touches hundreds
 * of thousands of objects and has to come back with a table a person can read.
 */
USTRUCT(BlueprintType)
struct HEAPCENSUS_API FHeapClassEntry
{
	GENERATED_BODY()

	/** The class name as the engine spells it - "StaticMeshComponent", not "/Script/Engine.StaticMeshComponent". */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	FString ClassName;

	/** Live objects of exactly this class at the last census. Not including subclasses; they get their own row. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 Count = 0;

	/** The highest count seen inside the current window. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 Peak = 0;

	/** The oldest count still in the window. Count minus this is the plain, unfitted change. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 FirstCount = 0;

	/**
	 * Objects per minute, from a least-squares line through the whole window.
	 *
	 * A line rather than "last minus first" because the last sample can land in the middle of a spawn burst
	 * and would then report a leak that is a wave. Negative means the class is shrinking.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float SlopePerMinute = 0.0f;

	/** How many samples the slope was fitted through. Under three there is no slope - see EvaluateEntry. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 SampleCount = 0;

	/** How much wall-clock time those samples span. The window the +n/min figure was measured over. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float WindowSeconds = 0.0f;

	/**
	 * True when the count never fell across the whole window and ended above where it started.
	 *
	 * This is the difference between a leak and a busy game. A pool that fills and drains has a positive
	 * slope for as long as it is filling; a leak never gives anything back. Heap.Gate returns 2 only for
	 * classes that are both over the threshold and monotonic, which is why an overnight soak can be trusted
	 * to fail for the right reason.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	bool bMonotonic = false;

	/** Ok, Warn or Leaking, from EvaluateEntry against the project's threshold. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	EHeapVerdict Verdict = EHeapVerdict::Ok;
};

/**
 * What the collection costs.
 *
 * Two figures, and the difference between them matters. Collect is the time from
 * FCoreUObjectDelegates::GetPreGarbageCollectDelegate to GetPostGarbageCollect - reachability analysis,
 * the part that holds the game thread and is felt as a hitch. Purge is what follows, up to
 * GarbageCollectComplete; with incremental purge switched on that runs in slices across several frames and
 * is not one stall, so it is reported separately instead of being added into a number people quote.
 */
USTRUCT(BlueprintType)
struct HEAPCENSUS_API FHeapGCStats
{
	GENERATED_BODY()

	/** How many collections HeapCensus has timed since it started, or since the last Reset. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 RunCount = 0;

	/** The last collection, in milliseconds. Pre-GC to post-GC: the part that holds the game thread. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float LastMilliseconds = 0.0f;

	/** The mean over every run timed so far. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float AverageMilliseconds = 0.0f;

	/** The worst single run. The number that decides whether players notice. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float WorstMilliseconds = 0.0f;

	/** The purge that followed the last collection, in milliseconds. May be spread over several frames. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float LastPurgeMilliseconds = 0.0f;

	/** The worst purge seen. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float WorstPurgeMilliseconds = 0.0f;

	/** True when the last purge did not finish in the frame it started in - so it was slices, not one stall. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	bool bLastPurgeSpannedFrames = false;

	/** Seconds between the last two collections. How often the hitch comes back. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float LastIntervalSeconds = 0.0f;

	/** The mean gap between collections. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float AverageIntervalSeconds = 0.0f;

	/** Seconds since the last collection finished, or -1 when there has not been one yet. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float SecondsSinceLastRun = -1.0f;

	/**
	 * Object-array slots freed by the last collection.
	 *
	 * Taken from the global object array before and after, so it counts slots rather than bytes - HeapCensus
	 * measures objects, and a slot freed is an object destroyed. It is the cheapest honest answer to "did
	 * that collection actually get anything back".
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 LastObjectsFreed = 0;

	/** True while a collection is in flight. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	bool bRunning = false;
};

/**
 * The whole census in one struct: the totals, the cost of the census itself, and the verdict.
 *
 * LastCensusMilliseconds is in here on purpose and is shown in the counter box on purpose. The walk over
 * the object array is the one thing this plugin adds to a frame, and a tool that hides its own price is a
 * tool that gets blamed for the hitch it was bought to find.
 */
USTRUCT(BlueprintType)
struct HEAPCENSUS_API FHeapCensusSummary
{
	GENERATED_BODY()

	/** Live UObjects at the last census, class default objects excluded. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 TotalObjects = 0;

	/** Distinct classes with at least one live object, after the ignore list. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 TrackedClasses = 0;

	/** Classes dropped by the ignore list. Shown in the box so the list can never quietly hide its effect. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 IgnoredClasses = 0;

	/** Classes counted in the total but given no row, because the table is at its cap. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 UntrackedClasses = 0;

	/** How many samples are in the window right now. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	int32 SampleCount = 0;

	/** Seconds between two censuses. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float SampleIntervalSeconds = 0.0f;

	/** How much wall-clock time the window spans, at the current sample count. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float WindowSeconds = 0.0f;

	/** The threshold every slope is judged against, in objects per minute. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float GrowthThresholdPerMinute = 0.0f;

	/** What the last walk over the object array cost, in milliseconds. The price of the measurement. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float LastCensusMilliseconds = 0.0f;

	/** The mean cost of a walk. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float AverageCensusMilliseconds = 0.0f;

	/** The worst walk. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float WorstCensusMilliseconds = 0.0f;

	/** True while the window is still filling after a map change - no slope is reported yet. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	bool bSettling = false;

	/** Seconds left of the settling window, or 0 when it is over. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float SettleSecondsRemaining = 0.0f;

	/** The worst verdict any class reached. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	EHeapVerdict Verdict = EHeapVerdict::Ok;

	/** The fastest-growing class, or empty when nothing is growing. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	FString WorstClass;

	/** That class's slope, in objects per minute. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	float WorstSlopePerMinute = 0.0f;

	/** What the collection costs. */
	UPROPERTY(BlueprintReadOnly, Category = "HeapCensus")
	FHeapGCStats GC;
};
