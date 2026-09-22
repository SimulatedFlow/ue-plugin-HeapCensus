// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HeapCensusTypes.h"
#include "UObject/StrongObjectPtr.h"

class UHeapCensusDemoObject;
class UWorld;

/**
 * One class's history: a fixed-size ring of counts, oldest overwritten first.
 *
 * A ring rather than an array that grows, because there is one of these per class and a big project has
 * hundreds of them. Sixty int32 each is a table that costs a few hundred kilobytes no matter how long the
 * game runs, which is the only acceptable memory profile for something that watches for leaks.
 */
struct FHeapClassSeries
{
	/** The counts, in ring order. Use CopyChronological to read them in the order they were taken. */
	TArray<int32> Values;

	/** Where the next count goes. */
	int32 Head = 0;

	/** How many of the slots are filled. Stops at the capacity. */
	int32 Count = 0;

	/** True once this class has been announced as growing, so it is announced once and not every census. */
	bool bAnnounced = false;

	void Push(int32 Value, int32 Capacity);
	void CopyChronological(TArray<float>& Out) const;
	int32 Newest() const;
	int32 Oldest() const;
	int32 Peak() const;
	void Reset();
};

/**
 * The census itself: the walk, the ring buffers, the collection timing, the gate and the demonstration.
 *
 * A plain object rather than a UObject, and a function-local static rather than a global. The module is
 * loaded at PreDefault and the garbage-collection hooks have to be hanging from that moment - long before
 * any subsystem exists - and the order in which globals in different translation units are constructed is
 * not something to bet a start-up hook on.
 *
 * UHeapCensusSubsystem is the Blueprint-facing face of this class and owns none of the measuring.
 */
class HEAPCENSUS_API FHeapCensusRecorder
{
public:
	/** The one recorder. */
	static FHeapCensusRecorder& Get();

	// --------------------------------------------------------------------------------------------------
	// Lifetime
	// --------------------------------------------------------------------------------------------------

	/** Hang the garbage-collection hooks and start the timer. Called from the module's StartupModule. */
	void Install();

	/** Take everything back off the engine's delegates. The delegates outlive this DLL. */
	void Uninstall();

	/** Adopt the project settings. Called once the engine is up, and again on every edit in the editor. */
	void ApplySettings();

	// --------------------------------------------------------------------------------------------------
	// The census
	// --------------------------------------------------------------------------------------------------

	/** Walk the object array now, fold the counts into the rings and return the resulting summary. */
	FHeapCensusSummary Sample();

	/** The last summary, without walking anything. */
	const FHeapCensusSummary& GetSummary() const { return Summary; }

	/** Every class with a row, in the given order. Growth is the default and the point of the plugin. */
	TArray<FHeapClassEntry> GetTop(int32 N, EHeapSort Sort) const;

	/** What the collection costs. */
	const FHeapGCStats& GetGCStats() const { return GCStats; }

	/** Throw the window and the collection statistics away and start measuring again. */
	void Reset();

	/** Write the census as JSON. An empty path means the one from the project settings. */
	bool WriteReport(FString Path) const;

	/** Write the whole table to the log - every class, not just the five the counter box shows. */
	void DumpToLog(int32 TopN, EHeapSort Sort) const;

	// --------------------------------------------------------------------------------------------------
	// The gate
	// --------------------------------------------------------------------------------------------------

	/** Measure for this many seconds, write the report and (unless told not to) end the process. */
	void BeginGate(float Seconds, bool bExitWhenDone);

	/** True while a gate run is in progress. */
	bool IsGateRunning() const { return bGateRunning; }

	/** Seconds left of the gate run. */
	float GetGateSecondsRemaining() const { return GateSecondsRemaining; }

	// --------------------------------------------------------------------------------------------------
	// The demonstration
	// --------------------------------------------------------------------------------------------------

	/**
	 * Create ObjectsPerSecond demo objects a second.
	 *
	 * With bHold, every one of them is kept - the class climbs the growth table and never falls back. Without
	 * it, they are let go of again after a moment and a collection is asked for every few seconds, which is
	 * what an ordinary busy game looks like: the count wobbles and the slope stays at zero.
	 *
	 * A rate of zero or less stops the demonstration.
	 */
	void StartDemo(int32 ObjectsPerSecond, bool bHold);

	/** Stop the demonstration and drop every reference. With bCollectGarbage, ask for a collection as well. */
	void StopDemo(bool bCollectGarbage);

	/** Demo objects being held right now. */
	int32 GetDemoObjectCount() const;

	/** True while a demonstration is running, and whether it is the leaking one. */
	bool IsDemoRunning() const { return DemoObjectsPerSecond > 0; }
	bool IsDemoHolding() const { return bDemoHolds; }

	// --------------------------------------------------------------------------------------------------
	// Knobs the counter box and the console read
	// --------------------------------------------------------------------------------------------------

	void SetShowCounterBox(bool bInShow) { bShowCounterBox = bInShow; }
	bool IsShowingCounterBox() const { return bShowCounterBox; }

	void SetGrowthThreshold(float InThresholdPerMinute);
	float GetGrowthThreshold() const { return GrowthThresholdPerMinute; }

	float GetWarnFraction() const { return WarnFraction; }
	bool GetRequireMonotonicGrowth() const { return bRequireMonotonicGrowth; }
	float GetSampleIntervalSeconds() const { return SampleIntervalSeconds; }
	int32 GetWindowSamples() const { return WindowSamples; }
	int32 GetTopClassLines() const { return TopClassLines; }
	FVector2D GetCounterBoxPosition() const { return CounterBoxPosition; }
	const FString& GetReportPath() const { return ReportPath; }

	/**
	 * Fired once per class when it first reaches the Leaking verdict, not once per census.
	 *
	 * A signal that repeats every two seconds is a signal people filter out, and then they filter out the
	 * one occurrence that mattered along with it. The flag is cleared when the class drops back to Ok, so a
	 * class that leaks, is fixed and leaks again is announced twice.
	 */
	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnClassGrowingNative, const FString& /*ClassName*/,
		float /*SlopePerMinute*/, EHeapVerdict /*Verdict*/);
	FOnClassGrowingNative OnClassGrowing;

private:
	FHeapCensusRecorder() = default;

	bool Tick(float DeltaSeconds);

	/** The walk. Tallies live objects into OutCounts by class, and returns the total. */
	int32 WalkObjectArray(TMap<UClass*, int32>& OutCounts) const;

	void BuildEntries();
	void AnnounceGrowth();

	void HandlePreGarbageCollect();
	void HandlePostGarbageCollect();
	void HandleGarbageCollectComplete();
	/**
	 * A game world has arrived, from either of the two hooks that can say so.
	 *
	 * Two, because neither alone covers both cases: PostLoadMapWithWorld is not broadcast for play-in-editor,
	 * and OnPostWorldInitialization fires for editor and preview worlds as well and has to be filtered. The
	 * settling window is idempotent, so being told twice about the same map costs nothing.
	 */
	void HandleWorldReady(UWorld* World);

	void BeginSettling(const TCHAR* Reason);
	void TickDemo(float DeltaSeconds);
	void FinishGate();

	// --------------------------------------------------------------------------------------------------
	// State
	// --------------------------------------------------------------------------------------------------

	/** One ring per class, keyed by the class's own FName so no string is copied on the hot path. */
	TMap<FName, FHeapClassSeries> Series;

	/**
	 * Scratch for the walk, kept between censuses so the census does not allocate.
	 *
	 * A tool that measures allocation pressure has no business adding two map allocations every two seconds.
	 * They are reset rather than freed, so after the first census the walk touches no allocator at all.
	 */
	TMap<UClass*, int32> ScratchCounts;
	TMap<FName, int32> ScratchNamed;

	/** The table the counter box, the report and Blueprint all read. Rebuilt at the end of every census. */
	TArray<FHeapClassEntry> Entries;

	FHeapCensusSummary Summary;
	FHeapGCStats GCStats;

	/** Rolling cost of the walk itself. The price of the measurement, printed next to the measurement. */
	double TotalCensusSeconds = 0.0;
	int32 CensusCount = 0;

	float SecondsSinceSample = 0.0f;
	float SettleSecondsRemaining = 0.0f;

	// Settings, copied out of the CDO so the hot path never touches the reflection system.
	float SampleIntervalSeconds = 2.0f;
	int32 WindowSamples = 60;
	float SettleSeconds = 5.0f;
	int32 MaxTrackedClasses = 1024;
	float GrowthThresholdPerMinute = 120.0f;
	float WarnFraction = 0.5f;
	bool bRequireMonotonicGrowth = true;
	bool bShowCounterBox = true;
	bool bLogGrowingClasses = true;
	int32 TopClassLines = 5;
	FVector2D CounterBoxPosition = FVector2D(24.0f, 90.0f);
	FString ReportPath = TEXT("Saved/HeapCensus/report.json");
	TArray<FString> IgnoredClassNames;
	int32 DemoPayloadKilobytes = 4;

	// Garbage collection timing.
	double GCStartSeconds = 0.0;
	double GCPostSeconds = 0.0;
	double LastGCFinishedSeconds = -1.0;
	uint64 GCPostFrame = 0;
	int32 GCObjectsBefore = 0;
	double TotalGCMilliseconds = 0.0;
	double TotalGCIntervalSeconds = 0.0;
	int32 GCIntervalCount = 0;
	bool bAwaitingPurge = false;

	// The gate.
	bool bGateRunning = false;
	bool bGateExitWhenDone = true;
	float GateSecondsRemaining = 0.0f;

	// The demonstration.
	int32 DemoObjectsPerSecond = 0;
	bool bDemoHolds = false;
	float DemoSpawnCredit = 0.0f;
	float DemoSecondsSinceCollect = 0.0f;
	/**
	 * The references that make the leak a leak.
	 *
	 * Held as TStrongObjectPtr<UObject> rather than of the demo class, because a TStrongObjectPtr needs the
	 * complete type to destroy and this header has no business pulling in the demo object's. What is being
	 * demonstrated is a strong reference, and a strong reference to a base pointer is exactly as strong.
	 */
	TArray<TStrongObjectPtr<UObject>> DemoHeld;
	TArray<TStrongObjectPtr<UObject>> DemoChurn;

	bool bInstalled = false;
	bool bSettingsApplied = false;

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle PreGCHandle;
	FDelegateHandle PostGCHandle;
	FDelegateHandle GCCompleteHandle;
	FDelegateHandle PostLoadMapHandle;
	FDelegateHandle PostWorldInitHandle;
};
