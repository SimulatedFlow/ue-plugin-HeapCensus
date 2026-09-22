// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "HeapCensusSettings.generated.h"

/**
 * Project-wide settings for HeapCensus.
 *
 * Project Settings > Plugins > HeapCensus, stored in DefaultGame.ini. Read once the engine is up and again
 * whenever a value changes in the editor, so a threshold you type in the Details panel is in force on the
 * next census rather than after the next restart.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "HeapCensus"))
class HEAPCENSUS_API UHeapCensusSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UHeapCensusSettings();

	//~ Begin UDeveloperSettings interface
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;
	//~ End UDeveloperSettings interface

#if WITH_EDITOR
	//~ Begin UObject interface
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
	//~ End UObject interface
#endif

	/** Convenience accessor. Never returns null. */
	static const UHeapCensusSettings& Get();

	// --------------------------------------------------------------------------------------------------
	// Census
	// --------------------------------------------------------------------------------------------------

	/**
	 * Seconds between two walks over the object array.
	 *
	 * Two, and the reason is the whole design: the walk is O(number of live objects) and on a large project
	 * it is measurable. Running it every frame would make this plugin the most expensive thing in the frame
	 * it is meant to be measuring. Two seconds is often enough that a leak shows up inside a minute and rare
	 * enough that the cost disappears into the noise - and whatever it does cost is printed in the counter
	 * box, so the trade is never hidden.
	 *
	 * Raise it on a big project, or set it to 0 to switch automatic sampling off entirely and drive the
	 * census yourself with Heap.Sample.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Census", meta = (ClampMin = "0.0", UIMax = "60.0", Units = "s"))
	float SampleIntervalSeconds = 2.0f;

	/**
	 * How many samples are kept per class. The window the slope is fitted through.
	 *
	 * Sixty at a two-second interval is two minutes of history, which is long enough that a burst of
	 * spawning does not read as a trend and short enough that a real leak is visible before somebody stops
	 * playing.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Census", meta = (ClampMin = "3", ClampMax = "1024"))
	int32 WindowSamples = 60;

	/**
	 * Seconds after a map change during which samples are thrown away instead of counted.
	 *
	 * A level coming up creates tens of thousands of objects in a second or two. That is not growth, it is a
	 * map load, and without this window every map change would report every class in the game as leaking.
	 * The counter box says "settling" while it runs, so a quiet box is never mistaken for a healthy one.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Census", meta = (ClampMin = "0.0", UIMax = "60.0", Units = "s"))
	float SettleSeconds = 5.0f;

	/**
	 * Upper limit on how many distinct classes get a row.
	 *
	 * Beyond it, further classes are still counted in the total but are no longer given a series of their
	 * own. A cap is the difference between a diagnostic tool and a leak of its own; the summary reports how
	 * many classes it swallowed so the cap can never hide.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Census", meta = (ClampMin = "16", UIMax = "8192"))
	int32 MaxTrackedClasses = 1024;

	/**
	 * Classes that never get a row.
	 *
	 * Matched against the short class name, case-insensitively. An entry ending in '*' is a prefix match, so
	 * "LinkerPlaceholder*" covers the family. The default list is short on purpose: every name on it is
	 * engine bookkeeping that a project cannot act on, and a list that hides gameplay classes would hide the
	 * leak. Whatever it drops is still counted in the object total and reported as IgnoredClasses in the
	 * counter box.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Census")
	TArray<FString> IgnoredClassNames;

	// --------------------------------------------------------------------------------------------------
	// Growth
	// --------------------------------------------------------------------------------------------------

	/**
	 * Objects per minute at or above which a class is called Leaking.
	 *
	 * A hundred and twenty is two objects a second, sustained, and no shipping game grows a single class at
	 * that rate for two minutes on purpose. Lower it once your project is clean - the useful setting is the
	 * lowest one that does not go off.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Growth", meta = (ClampMin = "0.0", UIMax = "10000.0"))
	float GrowthThresholdPerMinute = 120.0f;

	/**
	 * The fraction of the threshold at which a class becomes Warn instead of Ok.
	 *
	 * Half by default: at sixty objects a minute something is climbing and worth a look, at a hundred and
	 * twenty it has to be explained. Set it to 1.0 to remove the warning band, so anything under the
	 * threshold is simply Ok.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Growth", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float WarnFraction = 0.5f;

	/**
	 * Only fire OnClassGrowing / colour the box red for classes that never fell back inside the window.
	 *
	 * On by default. A pool filling up has a positive slope for as long as it is filling and is not a leak;
	 * requiring the count to be monotonic is what separates the two, and it is the same test Heap.Gate uses
	 * to decide between exit 1 and exit 2. Switch it off if you would rather see every climb, wave or not.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Growth")
	bool bRequireMonotonicGrowth = true;

	// --------------------------------------------------------------------------------------------------
	// Counter box
	// --------------------------------------------------------------------------------------------------

	/** Draw the counter box. Same as Heap.Show / Heap.Hide. */
	UPROPERTY(Config, EditAnywhere, Category = "Counter Box")
	bool bShowCounterBox = true;

	/**
	 * How many classes the counter box lists, fastest-growing first.
	 *
	 * Fastest growing, not biggest. That is the difference between this box and obj list, and the box says
	 * so in its own heading so a screenshot of it cannot be misread.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Counter Box", meta = (ClampMin = "0", ClampMax = "24"))
	int32 TopClassLines = 5;

	/** Top left corner of the counter box, in pixels. */
	UPROPERTY(Config, EditAnywhere, Category = "Counter Box", meta = (ClampMin = "0.0"))
	FVector2D CounterBoxPosition = FVector2D(24.0f, 90.0f);

	// --------------------------------------------------------------------------------------------------
	// Report
	// --------------------------------------------------------------------------------------------------

	/**
	 * Where Heap.Gate and Heap.Report write. Relative paths are relative to the project directory.
	 *
	 * The same file both of them write by default, so a build server and a developer end up looking at the
	 * same document.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Report")
	FString ReportPath = TEXT("Saved/HeapCensus/report.json");

	/**
	 * Write a log line whenever a class first breaks the threshold.
	 *
	 * One line per class per crossing, not one per census: a log that repeats itself every two seconds is a
	 * log people filter out, and then they filter out the one line that mattered with it.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Report")
	bool bLogGrowingClasses = true;

	// --------------------------------------------------------------------------------------------------
	// Demonstration
	// --------------------------------------------------------------------------------------------------

	/**
	 * Objects a second Heap.Leak and Heap.Churn create when no rate is given.
	 *
	 * These exist so a buyer can watch the slope react without first building a leak into their own game.
	 * What they produce is a real measurement of real objects - there is nothing staged about the number,
	 * which is exactly why it is worth having.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Demonstration", meta = (ClampMin = "1", UIMax = "5000"))
	int32 DemoObjectsPerSecond = 200;

	/**
	 * How much memory each demo object carries, in kilobytes.
	 *
	 * Objects are what HeapCensus counts, so the payload changes nothing it reports - it is here so that a
	 * leak demonstrated with this plugin also moves the numbers in Task Manager and in Memory Insights, and
	 * a sceptical buyer can check this plugin against another one.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Demonstration", meta = (ClampMin = "0", UIMax = "1024"))
	int32 DemoPayloadKilobytes = 4;
};
