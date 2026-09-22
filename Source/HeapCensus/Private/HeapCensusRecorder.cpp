// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HeapCensusRecorder.h"

#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HeapCensusDemoObject.h"
#include "HeapCensusLog.h"
#include "HeapCensusSettings.h"
#include "HeapCensusStatics.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace HeapCensusPrivate
{
	/** How often the demonstration creates a batch. Four times a second, so a wobble is visible in the box. */
	static constexpr float DemoBeatSeconds = 0.25f;

	/**
	 * How often the healthy demonstration asks for a collection.
	 *
	 * Objects that nobody references are still in the object array until a collection removes them, so a game
	 * that creates and drops objects looks like slow growth until the next collection - which by default is a
	 * minute away. Four seconds puts several full saw-teeth inside the two-minute window, which is what makes
	 * the healthy case read as a wobble around zero rather than as a ramp. It is the real behaviour of a real
	 * game with a real collection cadence, just a faster cadence.
	 */
	static constexpr float DemoCollectSeconds = 4.0f;

	/** Sanity cap on the demonstration, so a typo in the console cannot take the machine down. */
	static constexpr int32 MaxDemoObjects = 2000000;
}

// --------------------------------------------------------------------------------------------------
// FHeapClassSeries
// --------------------------------------------------------------------------------------------------

void FHeapClassSeries::Push(int32 Value, int32 Capacity)
{
	Capacity = FMath::Max(Capacity, 1);

	if (Values.Num() != Capacity)
	{
		// The window changed under us - somebody edited the setting. Keep what fits, oldest dropped first,
		// rather than throwing away history that is still valid.
		TArray<float> Existing;
		CopyChronological(Existing);

		Values.SetNumZeroed(Capacity, EAllowShrinking::Yes);
		Head = 0;
		Count = 0;

		const int32 KeepFrom = FMath::Max(0, Existing.Num() - Capacity);
		for (int32 Index = KeepFrom; Index < Existing.Num(); ++Index)
		{
			Values[Head] = static_cast<int32>(Existing[Index]);
			Head = (Head + 1) % Capacity;
			Count = FMath::Min(Count + 1, Capacity);
		}
	}

	Values[Head] = Value;
	Head = (Head + 1) % Capacity;
	Count = FMath::Min(Count + 1, Capacity);
}

void FHeapClassSeries::CopyChronological(TArray<float>& Out) const
{
	Out.Reset(Count);

	if (Count == 0 || Values.Num() == 0)
	{
		return;
	}

	// Head is where the next value goes, so the oldest live value is Count places behind it.
	const int32 Capacity = Values.Num();
	const int32 Start = (Head - Count + Capacity * 2) % Capacity;

	for (int32 Index = 0; Index < Count; ++Index)
	{
		Out.Add(static_cast<float>(Values[(Start + Index) % Capacity]));
	}
}

int32 FHeapClassSeries::Newest() const
{
	if (Count == 0 || Values.Num() == 0)
	{
		return 0;
	}

	const int32 Capacity = Values.Num();
	return Values[(Head - 1 + Capacity) % Capacity];
}

int32 FHeapClassSeries::Oldest() const
{
	if (Count == 0 || Values.Num() == 0)
	{
		return 0;
	}

	const int32 Capacity = Values.Num();
	return Values[(Head - Count + Capacity * 2) % Capacity];
}

int32 FHeapClassSeries::Peak() const
{
	int32 Highest = 0;

	if (Count == 0 || Values.Num() == 0)
	{
		return 0;
	}

	const int32 Capacity = Values.Num();
	const int32 Start = (Head - Count + Capacity * 2) % Capacity;

	for (int32 Index = 0; Index < Count; ++Index)
	{
		Highest = FMath::Max(Highest, Values[(Start + Index) % Capacity]);
	}

	return Highest;
}

void FHeapClassSeries::Reset()
{
	Values.Reset();
	Head = 0;
	Count = 0;
	bAnnounced = false;
}

// --------------------------------------------------------------------------------------------------
// Lifetime
// --------------------------------------------------------------------------------------------------

FHeapCensusRecorder& FHeapCensusRecorder::Get()
{
	// A function-local static, not a global. The module is loaded at PreDefault and the order in which
	// globals in different translation units are constructed is not something to bet a start-up hook on.
	static FHeapCensusRecorder Recorder;
	return Recorder;
}

void FHeapCensusRecorder::Install()
{
	if (bInstalled)
	{
		return;
	}

	// The collection hooks. Pre fires before the garbage-collection lock is taken; the delegate that closes
	// the measurement is GetPostGarbageCollect, which in UE 5.8 is what the pair is actually called - there
	// is no GetPostGarbageCollectDelegate. It fires after reachability analysis, which is the part that holds
	// the game thread and is felt as a hitch.
	PreGCHandle = FCoreUObjectDelegates::GetPreGarbageCollectDelegate()
		.AddRaw(this, &FHeapCensusRecorder::HandlePreGarbageCollect);

	PostGCHandle = FCoreUObjectDelegates::GetPostGarbageCollect()
		.AddRaw(this, &FHeapCensusRecorder::HandlePostGarbageCollect);

	// And the true end. With incremental purge switched on, the purge runs in slices across several frames,
	// so this can arrive well after the hitch is over. It is timed separately for exactly that reason: adding
	// it into the collection figure would inflate the one number people quote at each other in a review.
	GCCompleteHandle = FCoreUObjectDelegates::GarbageCollectComplete
		.AddRaw(this, &FHeapCensusRecorder::HandleGarbageCollectComplete);

	// A map load creates tens of thousands of objects in a second or two. That is not growth, and without a
	// settling window every map change would report every class in the game as leaking.
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld
		.AddRaw(this, &FHeapCensusRecorder::HandleWorldReady);

	// The second hook, because PostLoadMapWithWorld is not broadcast for play-in-editor. Without this one,
	// the most common way a developer will ever run this plugin is the one case the settling window misses.
	PostWorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddLambda(
		[this](UWorld* World, const UWorld::InitializationValues)
		{
			HandleWorldReady(World);
		});

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("HeapCensus"), 0.0f,
		[this](float DeltaSeconds) { return Tick(DeltaSeconds); });

	// Nothing is sampled until the settling window is over, and it starts now: engine start-up creates the
	// whole of the object graph and is not a leak.
	BeginSettling(TEXT("start-up"));

	bInstalled = true;
}

void FHeapCensusRecorder::Uninstall()
{
	if (!bInstalled)
	{
		return;
	}

	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().Remove(PreGCHandle);
	FCoreUObjectDelegates::GetPostGarbageCollect().Remove(PostGCHandle);
	FCoreUObjectDelegates::GarbageCollectComplete.Remove(GCCompleteHandle);
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	FWorldDelegates::OnPostWorldInitialization.Remove(PostWorldInitHandle);
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);

	PreGCHandle.Reset();
	PostGCHandle.Reset();
	GCCompleteHandle.Reset();
	PostLoadMapHandle.Reset();
	PostWorldInitHandle.Reset();
	TickerHandle.Reset();

	// The demonstration holds strong references. Leaving them behind while the module unloads would keep
	// objects alive whose class is about to disappear with the DLL.
	DemoHeld.Empty();
	DemoChurn.Empty();
	DemoObjectsPerSecond = 0;

	bInstalled = false;
}

void FHeapCensusRecorder::ApplySettings()
{
	const UHeapCensusSettings& Settings = UHeapCensusSettings::Get();

	SampleIntervalSeconds = FMath::Max(Settings.SampleIntervalSeconds, 0.0f);
	WindowSamples = FMath::Clamp(Settings.WindowSamples, UHeapCensusStatics::MinimumSamples(), 1024);
	SettleSeconds = FMath::Max(Settings.SettleSeconds, 0.0f);
	MaxTrackedClasses = FMath::Max(Settings.MaxTrackedClasses, 16);
	GrowthThresholdPerMinute = FMath::Max(Settings.GrowthThresholdPerMinute, 0.0f);
	WarnFraction = FMath::Clamp(Settings.WarnFraction, 0.0f, 1.0f);
	bRequireMonotonicGrowth = Settings.bRequireMonotonicGrowth;
	bShowCounterBox = Settings.bShowCounterBox;
	bLogGrowingClasses = Settings.bLogGrowingClasses;
	TopClassLines = FMath::Clamp(Settings.TopClassLines, 0, 24);
	CounterBoxPosition = Settings.CounterBoxPosition;
	ReportPath = Settings.ReportPath;
	IgnoredClassNames = Settings.IgnoredClassNames;
	DemoPayloadKilobytes = FMath::Clamp(Settings.DemoPayloadKilobytes, 0, 1024);

	bSettingsApplied = true;

	// The threshold may just have moved. Re-judge what is already in the window rather than leave a verdict
	// on screen that was reached against a threshold the project never asked for.
	BuildEntries();

	UE_LOG(LogHeapCensus, Log,
		TEXT("HeapCensus: census every %.1f s over a window of %d samples, threshold %.0f objects/min."),
		SampleIntervalSeconds, WindowSamples, GrowthThresholdPerMinute);
}

// --------------------------------------------------------------------------------------------------
// The clock
// --------------------------------------------------------------------------------------------------

bool FHeapCensusRecorder::Tick(float DeltaSeconds)
{
	TickDemo(DeltaSeconds);

	if (SettleSecondsRemaining > 0.0f)
	{
		SettleSecondsRemaining = FMath::Max(SettleSecondsRemaining - DeltaSeconds, 0.0f);
		Summary.bSettling = SettleSecondsRemaining > 0.0f;
		Summary.SettleSecondsRemaining = SettleSecondsRemaining;

		if (SettleSecondsRemaining > 0.0f)
		{
			// Still settling: no sample is taken at all. A sample taken while a level is still spawning is
			// not a wrong number, it is a number about something else.
			return true;
		}
	}

	if (SampleIntervalSeconds > 0.0f)
	{
		SecondsSinceSample += DeltaSeconds;

		if (SecondsSinceSample >= SampleIntervalSeconds)
		{
			SecondsSinceSample = 0.0f;
			Sample();
		}
	}

	if (bGateRunning)
	{
		GateSecondsRemaining -= DeltaSeconds;
		if (GateSecondsRemaining <= 0.0f)
		{
			FinishGate();
		}
	}

	// Keep the seconds-since-collection figure moving between collections, so a box that has been quiet for
	// two minutes says so rather than showing a stale gap.
	if (GCStats.RunCount > 0 && LastGCFinishedSeconds >= 0.0)
	{
		GCStats.SecondsSinceLastRun = static_cast<float>(FPlatformTime::Seconds() - LastGCFinishedSeconds);
		Summary.GC = GCStats;
	}

	return true;
}

void FHeapCensusRecorder::BeginSettling(const TCHAR* Reason)
{
	SettleSecondsRemaining = SettleSeconds;
	SecondsSinceSample = 0.0f;

	// The window goes with it. Counts from before a map change and counts from after it are counts of two
	// different games, and fitting one line through both would describe neither.
	Series.Reset();
	Entries.Reset();

	Summary.bSettling = SettleSecondsRemaining > 0.0f;
	Summary.SettleSecondsRemaining = SettleSecondsRemaining;
	Summary.Verdict = EHeapVerdict::Ok;
	Summary.WorstClass.Reset();
	Summary.WorstSlopePerMinute = 0.0f;

	UE_LOG(LogHeapCensus, Verbose, TEXT("HeapCensus: settling for %.1f s after %s."), SettleSeconds, Reason);
}

void FHeapCensusRecorder::HandleWorldReady(UWorld* World)
{
	// Editor and preview worlds are not what anybody is playing, and a census that restarted every time a
	// thumbnail was rendered would never fill its window.
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	BeginSettling(TEXT("a map change"));
}

// --------------------------------------------------------------------------------------------------
// The census
// --------------------------------------------------------------------------------------------------

int32 FHeapCensusRecorder::WalkObjectArray(TMap<UClass*, int32>& OutCounts) const
{
	OutCounts.Reset();

	int32 Total = 0;

	// THE COST OF THE MEASUREMENT.
	//
	// This loop is O(size of the global object array), and on a large project that array holds hundreds of
	// thousands of entries. It is the only thing HeapCensus adds to a frame, and it is the reason the census
	// runs on an interval rather than every frame: at sixty frames a second this walk would cost more than
	// most of the things it is meant to find. What it costs is timed by the caller and printed in the counter
	// box next to everything else, because a tool that hides its own price is a tool that gets blamed for the
	// hitch it was bought to find.
	//
	// Two decisions keep the price down. The tally is keyed by UClass* rather than by name, so nothing
	// resolves an FName or touches a string inside the loop - the names are looked up once per class
	// afterwards, and there are three orders of magnitude fewer classes than objects. And the iterator's own
	// exclusion flags do the filtering, in the engine's tight loop, rather than an if inside this one:
	// class default objects are skipped because they are constants and would only add a fixed offset to every
	// class, and Garbage objects are skipped because they are already dead and counting them would make every
	// collection look like a leak that just repaired itself.
	for (FThreadSafeObjectIterator It(UObject::StaticClass(), /*bOnlyGCedObjects*/ false,
		RF_ClassDefaultObject, EInternalObjectFlags::Garbage); It; ++It)
	{
		UObject* Object = *It;
		if (!Object)
		{
			continue;
		}

		UClass* Class = Object->GetClass();
		if (!Class)
		{
			continue;
		}

		++Total;
		++OutCounts.FindOrAdd(Class, 0);
	}

	return Total;
}

FHeapCensusSummary FHeapCensusRecorder::Sample()
{
	const double StartSeconds = FPlatformTime::Seconds();

	const int32 TotalObjects = WalkObjectArray(ScratchCounts);

	// Names are resolved here, once per class, and not inside the walk.
	TMap<FName, int32>& Named = ScratchNamed;
	Named.Reset();
	Named.Reserve(ScratchCounts.Num());

	int32 IgnoredClasses = 0;

	for (const TPair<UClass*, int32>& Pair : ScratchCounts)
	{
		const FName ClassName = Pair.Key->GetFName();

		if (UHeapCensusStatics::IsClassIgnored(ClassName.ToString(), IgnoredClassNames))
		{
			// Dropped from the table, never from the total. The number of classes dropped goes into the
			// summary and into the counter box, so the ignore list can never quietly hide its own effect.
			++IgnoredClasses;
			continue;
		}

		Named.Add(ClassName, Pair.Value);
	}

	int32 UntrackedClasses = 0;

	// Every class that already has a series gets a value, even when it has no live objects this time round.
	// Pushing the zero is what makes a class that drained show up as shrinking instead of freezing at its
	// last count and looking flat forever.
	for (TPair<FName, FHeapClassSeries>& Pair : Series)
	{
		const int32* Found = Named.Find(Pair.Key);
		Pair.Value.Push(Found ? *Found : 0, WindowSamples);
	}

	for (const TPair<FName, int32>& Pair : Named)
	{
		if (Series.Contains(Pair.Key))
		{
			continue;
		}

		if (Series.Num() >= MaxTrackedClasses)
		{
			// At the cap. Still inside the object total, simply without a row of its own - and the summary
			// says how many, so the cap can never hide.
			++UntrackedClasses;
			continue;
		}

		FHeapClassSeries& NewSeries = Series.Add(Pair.Key);
		NewSeries.Push(Pair.Value, WindowSamples);
	}

	// Classes whose whole window is zeros are gone: nothing of theirs is alive and nothing has been for the
	// length of the window. Without this the table would only ever grow.
	for (auto It = Series.CreateIterator(); It; ++It)
	{
		if (It.Value().Count >= WindowSamples && It.Value().Peak() == 0)
		{
			It.RemoveCurrent();
		}
	}

	const double CensusSeconds = FPlatformTime::Seconds() - StartSeconds;
	TotalCensusSeconds += CensusSeconds;
	++CensusCount;

	Summary.TotalObjects = TotalObjects;
	Summary.IgnoredClasses = IgnoredClasses;
	Summary.UntrackedClasses = UntrackedClasses;
	Summary.LastCensusMilliseconds = static_cast<float>(CensusSeconds * 1000.0);
	Summary.WorstCensusMilliseconds = FMath::Max(Summary.WorstCensusMilliseconds, Summary.LastCensusMilliseconds);
	Summary.AverageCensusMilliseconds = CensusCount > 0
		? static_cast<float>(TotalCensusSeconds * 1000.0 / static_cast<double>(CensusCount))
		: 0.0f;

	BuildEntries();
	AnnounceGrowth();

	return Summary;
}

void FHeapCensusRecorder::BuildEntries()
{
	Entries.Reset(Series.Num());

	TArray<float> Window;
	int32 MaxSamples = 0;

	for (const TPair<FName, FHeapClassSeries>& Pair : Series)
	{
		Pair.Value.CopyChronological(Window);

		FHeapClassEntry Entry;
		Entry.ClassName = Pair.Key.ToString();
		Entry.Count = Pair.Value.Newest();
		Entry.FirstCount = Pair.Value.Oldest();
		Entry.Peak = Pair.Value.Peak();
		Entry.SampleCount = Window.Num();
		Entry.WindowSeconds = FMath::Max(Window.Num() - 1, 0) * SampleIntervalSeconds;
		Entry.SlopePerMinute = UHeapCensusStatics::Slope(Window, SampleIntervalSeconds);
		Entry.bMonotonic = UHeapCensusStatics::IsMonotonicallyGrowing(Window);
		Entry.Verdict = UHeapCensusStatics::EvaluateEntry(Entry, GrowthThresholdPerMinute, WarnFraction,
			bRequireMonotonicGrowth);

		MaxSamples = FMath::Max(MaxSamples, Entry.SampleCount);

		Entries.Add(MoveTemp(Entry));
	}

	Summary.TrackedClasses = Entries.Num();
	Summary.SampleCount = MaxSamples;
	Summary.SampleIntervalSeconds = SampleIntervalSeconds;
	Summary.WindowSeconds = FMath::Max(MaxSamples - 1, 0) * SampleIntervalSeconds;
	Summary.GrowthThresholdPerMinute = GrowthThresholdPerMinute;
	Summary.bSettling = SettleSecondsRemaining > 0.0f;
	Summary.SettleSecondsRemaining = SettleSecondsRemaining;
	Summary.GC = GCStats;

	Summary.Verdict = UHeapCensusStatics::FindWorst(Entries, Summary.WorstClass, Summary.WorstSlopePerMinute);
}

void FHeapCensusRecorder::AnnounceGrowth()
{
	for (const FHeapClassEntry& Entry : Entries)
	{
		FHeapClassSeries* Found = Series.Find(FName(*Entry.ClassName));
		if (!Found)
		{
			continue;
		}

		if (Entry.Verdict == EHeapVerdict::Leaking)
		{
			// Once per class per episode, not once per census. A signal that repeats every two seconds is a
			// signal people filter out, and they filter out the one occurrence that mattered along with it.
			if (!Found->bAnnounced)
			{
				Found->bAnnounced = true;

				if (bLogGrowingClasses)
				{
					UE_LOG(LogHeapCensus, Warning,
						TEXT("HeapCensus: %s is growing - %d live, %+.0f objects/min over %.0f s%s."),
						*Entry.ClassName, Entry.Count, Entry.SlopePerMinute, Entry.WindowSeconds,
						Entry.bMonotonic ? TEXT(", never falling back") : TEXT(""));
				}

				OnClassGrowing.Broadcast(Entry.ClassName, Entry.SlopePerMinute, Entry.Verdict);
			}
		}
		else if (Entry.Verdict == EHeapVerdict::Ok)
		{
			// Back to normal, so the class may be announced again if it starts climbing a second time.
			Found->bAnnounced = false;
		}
	}
}

TArray<FHeapClassEntry> FHeapCensusRecorder::GetTop(int32 N, EHeapSort Sort) const
{
	TArray<FHeapClassEntry> Ranked = UHeapCensusStatics::RankEntries(Entries, Sort);

	if (N > 0 && Ranked.Num() > N)
	{
		Ranked.SetNum(N, EAllowShrinking::Yes);
	}

	return Ranked;
}

void FHeapCensusRecorder::Reset()
{
	Series.Reset();
	Entries.Reset();

	Summary = FHeapCensusSummary();
	GCStats = FHeapGCStats();

	TotalCensusSeconds = 0.0;
	CensusCount = 0;
	TotalGCMilliseconds = 0.0;
	TotalGCIntervalSeconds = 0.0;
	GCIntervalCount = 0;
	LastGCFinishedSeconds = -1.0;
	bAwaitingPurge = false;

	SecondsSinceSample = 0.0f;

	Summary.SampleIntervalSeconds = SampleIntervalSeconds;
	Summary.GrowthThresholdPerMinute = GrowthThresholdPerMinute;
}

void FHeapCensusRecorder::SetGrowthThreshold(float InThresholdPerMinute)
{
	GrowthThresholdPerMinute = FMath::Max(InThresholdPerMinute, 0.0f);
	BuildEntries();
}

// --------------------------------------------------------------------------------------------------
// What the collection costs
// --------------------------------------------------------------------------------------------------

void FHeapCensusRecorder::HandlePreGarbageCollect()
{
	GCStartSeconds = FPlatformTime::Seconds();
	GCStats.bRunning = true;

	// Claimed slots in the global object array, before and after. Slots rather than bytes, because objects
	// are what this plugin counts and a slot freed is an object destroyed. It costs one subtraction, which
	// is the right price for the answer to "did that collection actually get anything back".
	GCObjectsBefore = GUObjectArray.GetObjectArrayNumMinusAvailable();
}

void FHeapCensusRecorder::HandlePostGarbageCollect()
{
	const double Now = FPlatformTime::Seconds();

	GCPostSeconds = Now;
	GCPostFrame = GFrameCounter;

	const float Milliseconds = static_cast<float>(FMath::Max(Now - GCStartSeconds, 0.0) * 1000.0);

	++GCStats.RunCount;
	GCStats.LastMilliseconds = Milliseconds;
	GCStats.WorstMilliseconds = FMath::Max(GCStats.WorstMilliseconds, Milliseconds);

	TotalGCMilliseconds += static_cast<double>(Milliseconds);
	GCStats.AverageMilliseconds = static_cast<float>(TotalGCMilliseconds / static_cast<double>(GCStats.RunCount));

	if (LastGCFinishedSeconds >= 0.0)
	{
		const float Interval = static_cast<float>(Now - LastGCFinishedSeconds);
		GCStats.LastIntervalSeconds = Interval;

		TotalGCIntervalSeconds += static_cast<double>(Interval);
		++GCIntervalCount;
		GCStats.AverageIntervalSeconds = static_cast<float>(TotalGCIntervalSeconds / static_cast<double>(GCIntervalCount));
	}

	bAwaitingPurge = true;
	Summary.GC = GCStats;
}

void FHeapCensusRecorder::HandleGarbageCollectComplete()
{
	const double Now = FPlatformTime::Seconds();

	if (bAwaitingPurge)
	{
		GCStats.LastPurgeMilliseconds = static_cast<float>(FMath::Max(Now - GCPostSeconds, 0.0) * 1000.0);
		GCStats.WorstPurgeMilliseconds = FMath::Max(GCStats.WorstPurgeMilliseconds, GCStats.LastPurgeMilliseconds);

		// With incremental purge switched on this arrives frames later, and then the figure above is not one
		// stall but the sum of several slices. Saying which of the two it was is the difference between a
		// number and a number somebody can act on.
		GCStats.bLastPurgeSpannedFrames = GFrameCounter != GCPostFrame;
		GCStats.LastObjectsFreed = FMath::Max(GCObjectsBefore - GUObjectArray.GetObjectArrayNumMinusAvailable(), 0);

		bAwaitingPurge = false;
	}

	LastGCFinishedSeconds = Now;
	GCStats.SecondsSinceLastRun = 0.0f;
	GCStats.bRunning = false;

	Summary.GC = GCStats;
}

// --------------------------------------------------------------------------------------------------
// The demonstration
// --------------------------------------------------------------------------------------------------

void FHeapCensusRecorder::StartDemo(int32 ObjectsPerSecond, bool bHold)
{
	if (ObjectsPerSecond <= 0)
	{
		StopDemo(/*bCollectGarbage*/ true);
		return;
	}

	// Switching between the two demonstrations drops what the previous one was holding, so "healthy" really
	// is healthy and does not sit on top of the objects a previous leak left behind.
	DemoHeld.Empty();
	DemoChurn.Empty();

	DemoObjectsPerSecond = ObjectsPerSecond;
	bDemoHolds = bHold;
	DemoSpawnCredit = 0.0f;
	DemoSecondsSinceCollect = 0.0f;

	UE_LOG(LogHeapCensus, Display, TEXT("HeapCensus: demonstration running - %d %s objects a second."),
		ObjectsPerSecond, bHold ? TEXT("held (leaking)") : TEXT("released (healthy)"));
}

void FHeapCensusRecorder::StopDemo(bool bCollectGarbage)
{
	const int32 Released = DemoHeld.Num() + DemoChurn.Num();

	DemoObjectsPerSecond = 0;
	bDemoHolds = false;
	DemoSpawnCredit = 0.0f;
	DemoHeld.Empty();
	DemoChurn.Empty();

	if (bCollectGarbage && GEngine)
	{
		// Asked for rather than performed. A collection started from inside a tick - which is where a
		// Blueprint button press arrives - would destroy objects the frame is still walking. This runs it at
		// the end of the frame, and it is timed by the same hooks as every other collection, so the box shows
		// what clearing up cost.
		GEngine->ForceGarbageCollection(true);
	}

	UE_LOG(LogHeapCensus, Display, TEXT("HeapCensus: demonstration stopped, %d object(s) released%s."),
		Released, bCollectGarbage ? TEXT(", collection requested") : TEXT(""));
}

int32 FHeapCensusRecorder::GetDemoObjectCount() const
{
	return DemoHeld.Num() + DemoChurn.Num();
}

void FHeapCensusRecorder::TickDemo(float DeltaSeconds)
{
	if (DemoObjectsPerSecond <= 0)
	{
		return;
	}

	DemoSpawnCredit += DeltaSeconds * static_cast<float>(DemoObjectsPerSecond);
	DemoSecondsSinceCollect += DeltaSeconds;

	const int32 ToCreate = FMath::FloorToInt(DemoSpawnCredit);
	if (ToCreate <= 0)
	{
		return;
	}

	DemoSpawnCredit -= static_cast<float>(ToCreate);

	TArray<TStrongObjectPtr<UObject>>& Target = bDemoHolds ? DemoHeld : DemoChurn;

	if (Target.Num() + ToCreate > HeapCensusPrivate::MaxDemoObjects)
	{
		UE_LOG(LogHeapCensus, Warning,
			TEXT("HeapCensus: the demonstration has reached its cap of %d objects and has been stopped."),
			HeapCensusPrivate::MaxDemoObjects);
		StopDemo(/*bCollectGarbage*/ true);
		return;
	}

	Target.Reserve(Target.Num() + ToCreate);

	for (int32 Index = 0; Index < ToCreate; ++Index)
	{
		UHeapCensusDemoObject* Object = NewObject<UHeapCensusDemoObject>(GetTransientPackage());
		Object->SetPayloadKilobytes(DemoPayloadKilobytes);

		Target.Add(TStrongObjectPtr<UObject>(Object));
	}

	if (bDemoHolds)
	{
		return;
	}

	// The healthy half. The references go, and every few seconds a collection is asked for so the objects
	// actually leave the array - unreferenced is not the same as gone, and a game that waits for the default
	// sixty-second collection would look like slow growth for a minute at a time. What the box shows is a
	// wobble around zero, which is what an ordinary busy game looks like.
	if (DemoSecondsSinceCollect >= HeapCensusPrivate::DemoCollectSeconds)
	{
		DemoSecondsSinceCollect = 0.0f;
		DemoChurn.Reset();

		if (GEngine)
		{
			GEngine->ForceGarbageCollection(false);
		}
	}
}

// --------------------------------------------------------------------------------------------------
// Report and gate
// --------------------------------------------------------------------------------------------------

void FHeapCensusRecorder::DumpToLog(int32 TopN, EHeapSort Sort) const
{
	const TArray<FHeapClassEntry> Ranked = GetTop(TopN, Sort);

	UE_LOG(LogHeapCensus, Display,
		TEXT("--- HeapCensus: %s live UObjects in %d class(es); census %.2f ms (avg %.2f, worst %.2f) ---"),
		*UHeapCensusStatics::FormatCount(Summary.TotalObjects), Summary.TrackedClasses,
		Summary.LastCensusMilliseconds, Summary.AverageCensusMilliseconds, Summary.WorstCensusMilliseconds);

	UE_LOG(LogHeapCensus, Display,
		TEXT("    GC: %d run(s), last %.1f ms, avg %.1f ms, worst %.1f ms, every %.0f s, last purge %.1f ms%s, freed %s object(s)"),
		GCStats.RunCount, GCStats.LastMilliseconds, GCStats.AverageMilliseconds, GCStats.WorstMilliseconds,
		GCStats.AverageIntervalSeconds, GCStats.LastPurgeMilliseconds,
		GCStats.bLastPurgeSpannedFrames ? TEXT(" (over several frames)") : TEXT(""),
		*UHeapCensusStatics::FormatCount(GCStats.LastObjectsFreed));

	UE_LOG(LogHeapCensus, Display, TEXT("    %-52s %10s %10s %12s %10s %s"),
		TEXT("class"), TEXT("count"), TEXT("peak"), TEXT("per minute"), TEXT("verdict"), TEXT("note"));

	for (const FHeapClassEntry& Entry : Ranked)
	{
		UE_LOG(LogHeapCensus, Display, TEXT("    %-52s %10s %10s %12s %10s %s"),
			*Entry.ClassName,
			*UHeapCensusStatics::FormatCount(Entry.Count),
			*UHeapCensusStatics::FormatCount(Entry.Peak),
			*UHeapCensusStatics::FormatSlope(Entry.SlopePerMinute),
			*UHeapCensusStatics::VerdictToString(Entry.Verdict),
			*UHeapCensusStatics::GrowthNote(Entry));
	}

	if (Summary.IgnoredClasses > 0)
	{
		UE_LOG(LogHeapCensus, Display,
			TEXT("    %d class(es) dropped by the ignore list - their objects are still in the total."),
			Summary.IgnoredClasses);
	}

	if (Summary.UntrackedClasses > 0)
	{
		UE_LOG(LogHeapCensus, Display,
			TEXT("    %d class(es) counted but not named: the table is at its cap of %d."),
			Summary.UntrackedClasses, MaxTrackedClasses);
	}

	UE_LOG(LogHeapCensus, Display, TEXT("    Verdict %s. %s"),
		*UHeapCensusStatics::VerdictToString(Summary.Verdict),
		*UHeapCensusStatics::SummarizeWorst(Entries));

	UE_LOG(LogHeapCensus, Display,
		TEXT("    Next step: obj refs name=<ClassName> in the console names what is holding one of them - HeapCensus does not."));
}

bool FHeapCensusRecorder::WriteReport(FString Path) const
{
	if (Path.IsEmpty())
	{
		Path = ReportPath.IsEmpty() ? FString(TEXT("Saved/HeapCensus/report.json")) : ReportPath;
	}

	const FString AbsolutePath = FPaths::IsRelative(Path)
		? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / Path)
		: Path;

	const TArray<FHeapClassEntry> Ranked = UHeapCensusStatics::RankByGrowth(Entries);

	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("plugin"), TEXT("HeapCensus"));
	Writer->WriteValue(TEXT("reportVersion"), 1);
	Writer->WriteValue(TEXT("verdict"), UHeapCensusStatics::VerdictToString(Summary.Verdict));
	Writer->WriteValue(TEXT("exitCode"), UHeapCensusStatics::VerdictToExitCode(Summary.Verdict));
	Writer->WriteValue(TEXT("growthThresholdPerMinute"), Summary.GrowthThresholdPerMinute);
	Writer->WriteValue(TEXT("warnFraction"), WarnFraction);
	Writer->WriteValue(TEXT("requireMonotonicGrowth"), bRequireMonotonicGrowth);
	Writer->WriteValue(TEXT("sampleIntervalSeconds"), Summary.SampleIntervalSeconds);
	Writer->WriteValue(TEXT("windowSamples"), Summary.SampleCount);
	Writer->WriteValue(TEXT("windowSeconds"), Summary.WindowSeconds);
	Writer->WriteValue(TEXT("totalObjects"), Summary.TotalObjects);
	Writer->WriteValue(TEXT("trackedClasses"), Summary.TrackedClasses);
	Writer->WriteValue(TEXT("ignoredClasses"), Summary.IgnoredClasses);
	Writer->WriteValue(TEXT("untrackedClasses"), Summary.UntrackedClasses);
	Writer->WriteValue(TEXT("censusMillisecondsLast"), Summary.LastCensusMilliseconds);
	Writer->WriteValue(TEXT("censusMillisecondsAverage"), Summary.AverageCensusMilliseconds);
	Writer->WriteValue(TEXT("censusMillisecondsWorst"), Summary.WorstCensusMilliseconds);
	Writer->WriteValue(TEXT("worstClass"), Summary.WorstClass);
	Writer->WriteValue(TEXT("worstSlopePerMinute"), Summary.WorstSlopePerMinute);
	Writer->WriteValue(TEXT("worstFinding"), UHeapCensusStatics::SummarizeWorst(Entries));
	Writer->WriteValue(TEXT("nextStep"), TEXT("obj refs name=<ClassName> - HeapCensus counts and measures; it does not say who is holding the object."));

	Writer->WriteObjectStart(TEXT("garbageCollection"));
	Writer->WriteValue(TEXT("runs"), GCStats.RunCount);
	Writer->WriteValue(TEXT("lastMilliseconds"), GCStats.LastMilliseconds);
	Writer->WriteValue(TEXT("averageMilliseconds"), GCStats.AverageMilliseconds);
	Writer->WriteValue(TEXT("worstMilliseconds"), GCStats.WorstMilliseconds);
	Writer->WriteValue(TEXT("lastPurgeMilliseconds"), GCStats.LastPurgeMilliseconds);
	Writer->WriteValue(TEXT("worstPurgeMilliseconds"), GCStats.WorstPurgeMilliseconds);
	Writer->WriteValue(TEXT("lastPurgeSpannedFrames"), GCStats.bLastPurgeSpannedFrames);
	Writer->WriteValue(TEXT("averageIntervalSeconds"), GCStats.AverageIntervalSeconds);
	Writer->WriteValue(TEXT("lastIntervalSeconds"), GCStats.LastIntervalSeconds);
	Writer->WriteValue(TEXT("lastObjectsFreed"), GCStats.LastObjectsFreed);
	Writer->WriteObjectEnd();

	Writer->WriteArrayStart(TEXT("classes"));
	for (const FHeapClassEntry& Entry : Ranked)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("class"), Entry.ClassName);
		Writer->WriteValue(TEXT("count"), Entry.Count);
		Writer->WriteValue(TEXT("firstCount"), Entry.FirstCount);
		Writer->WriteValue(TEXT("peak"), Entry.Peak);
		Writer->WriteValue(TEXT("slopePerMinute"), Entry.SlopePerMinute);
		Writer->WriteValue(TEXT("samples"), Entry.SampleCount);
		Writer->WriteValue(TEXT("windowSeconds"), Entry.WindowSeconds);
		Writer->WriteValue(TEXT("monotonic"), Entry.bMonotonic);
		Writer->WriteValue(TEXT("verdict"), UHeapCensusStatics::VerdictToString(Entry.Verdict));
		Writer->WriteValue(TEXT("note"), UHeapCensusStatics::GrowthNote(Entry));
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	if (!FFileHelper::SaveStringToFile(Json, *AbsolutePath))
	{
		UE_LOG(LogHeapCensus, Error, TEXT("HeapCensus: could not write the report to '%s'."), *AbsolutePath);
		return false;
	}

	UE_LOG(LogHeapCensus, Display, TEXT("HeapCensus: report written to '%s'."), *AbsolutePath);
	return true;
}

void FHeapCensusRecorder::BeginGate(float Seconds, bool bExitWhenDone)
{
	if (bGateRunning)
	{
		UE_LOG(LogHeapCensus, Warning, TEXT("HeapCensus: a gate run is already in progress."));
		return;
	}

	// A gate measures its own window. Whatever the map load did before the build server said "go" is not what
	// it asked about, and leaving it in the rings would fail builds for a level that was merely large.
	Reset();
	BeginSettling(TEXT("the start of a gate run"));

	bGateRunning = true;
	bGateExitWhenDone = bExitWhenDone;
	GateSecondsRemaining = FMath::Max(Seconds, 0.0f);

	UE_LOG(LogHeapCensus, Display,
		TEXT("HeapCensus: gate running for %.0f s against a threshold of %.0f objects/min (%.0f s settling first)."),
		GateSecondsRemaining, GrowthThresholdPerMinute, SettleSeconds);
}

void FHeapCensusRecorder::FinishGate()
{
	bGateRunning = false;

	// One last census, so the verdict is about the whole window including the final seconds.
	Sample();

	const int32 ExitCode = UHeapCensusStatics::VerdictToExitCode(Summary.Verdict);

	WriteReport(FString());
	DumpToLog(TopClassLines, EHeapSort::Growth);

	UE_LOG(LogHeapCensus, Display,
		TEXT("HEAPCENSUS GATE RESULT=%s objects=%d classes=%d worst=%s slope=%.1f/min threshold=%.0f gc_worst_ms=%.1f exit=%d"),
		*UHeapCensusStatics::VerdictToString(Summary.Verdict),
		Summary.TotalObjects, Summary.TrackedClasses,
		Summary.WorstClass.IsEmpty() ? TEXT("none") : *Summary.WorstClass,
		Summary.WorstSlopePerMinute, Summary.GrowthThresholdPerMinute,
		GCStats.WorstMilliseconds, ExitCode);
	UE_LOG(LogHeapCensus, Display, TEXT("HEAPCENSUS GATE WORST=%s"), *UHeapCensusStatics::SummarizeWorst(Entries));

	if (bGateExitWhenDone)
	{
		FPlatformMisc::RequestExitWithStatus(false, static_cast<uint8>(ExitCode));
	}
}
