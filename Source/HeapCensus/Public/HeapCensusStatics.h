// Copyright 2026 Silvan Teufel. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HeapCensusTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "HeapCensusStatics.generated.h"

class UHeapCensusSubsystem;

/**
 * HeapCensus's arithmetic, and the way into it from Blueprint.
 *
 * Everything in the first half of this class is static and needs no world, no engine and no objects at all.
 * Fitting a line through a series, ranking a table by growth rather than by size, judging a slope against a
 * threshold and refusing to invent a slope from two samples are the four places this plugin can be quietly
 * wrong, and none of them needs a running game to be tested. That is the whole reason they live here
 * instead of inside the census.
 */
UCLASS()
class HEAPCENSUS_API UHeapCensusStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// --------------------------------------------------------------------------------------------------
	// Pure logic
	// --------------------------------------------------------------------------------------------------

	/**
	 * The fewest samples HeapCensus will fit a line through. Three.
	 *
	 * Two points always describe a perfect straight line, and a perfect straight line through two samples
	 * taken two seconds apart is how a tool reports a leak because somebody opened a menu. Three is the
	 * smallest number at which the fit can disagree with the data, and disagreeing with the data is the only
	 * thing a regression is for.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static int32 MinimumSamples();

	/**
	 * Objects per minute, from a least-squares line through the series.
	 *
	 * Samples are counts in chronological order, SampleIntervalSeconds apart. A flat series returns exactly
	 * zero - not almost zero - because a box full of "+0.02/min" on an idle game reads as noise and gets
	 * switched off. A series shorter than MinimumSamples() returns zero as well: see EvaluateEntry for why
	 * that is honest rather than convenient.
	 *
	 * @param Samples                 Counts, oldest first.
	 * @param SampleIntervalSeconds   Wall-clock seconds between two samples. Zero or less returns 0.
	 * @return                        Slope in objects per minute. Negative when the class is shrinking.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static float Slope(const TArray<float>& Samples, float SampleIntervalSeconds = 2.0f);

	/**
	 * True when the series never fell and ended above where it started.
	 *
	 * The test that separates a leak from a busy game: a pool filling up climbs and then gives it all back,
	 * a leak only ever climbs. Equal neighbours count as monotonic - a leak that pauses for one census is
	 * still a leak. A series shorter than MinimumSamples() is never monotonic.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static bool IsMonotonicallyGrowing(const TArray<float>& Samples);

	/**
	 * Judge a slope against the threshold.
	 *
	 * At or above the threshold is Leaking. At or above WarnFraction of it is Warn. Anything else, including
	 * every negative slope, is Ok. With the defaults - threshold 120, fraction 0.5 - that is: 59.9 Ok,
	 * 60 Warn, 119.9 Warn, 120 Leaking.
	 *
	 * The boundary is inclusive on purpose and is tested for it. A threshold somebody typed as "120" has to
	 * mean 120, or the number in the settings page and the number in the report are two different numbers.
	 *
	 * A threshold of zero or less disables the judgement entirely and returns Ok - it is how you switch the
	 * verdict off without switching the census off.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static EHeapVerdict EvaluateGrowth(float SlopePerMinute, float ThresholdPerMinute = 120.0f, float WarnFraction = 0.5f);

	/**
	 * Judge a whole entry - the slope, the number of samples behind it and whether it ever fell back.
	 *
	 * This is EvaluateGrowth plus the two refusals that keep the plugin honest:
	 *
	 * Under MinimumSamples() the answer is Ok, whatever the slope says, because there is not yet enough data
	 * to have an opinion. GrowthNote() says so in words. A tool that reports a leak four seconds after a map
	 * load is a tool that gets uninstalled on the first day.
	 *
	 * And with bRequireMonotonic, a class that fell back at any point inside the window is capped at Warn
	 * however steep the fit is. That is the difference between a pool filling and a leak, and it is the same
	 * test Heap.Gate uses to decide between exit 1 and exit 2.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static EHeapVerdict EvaluateEntry(const FHeapClassEntry& Entry, float ThresholdPerMinute = 120.0f,
		float WarnFraction = 0.5f, bool bRequireMonotonic = true);

	/**
	 * Why an entry got the verdict it got, in words. Empty when there is nothing to explain.
	 *
	 * "too little data (2 of 3 samples)" is the important one: it is what stands where an invented slope
	 * would otherwise be.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static FString GrowthNote(const FHeapClassEntry& Entry);

	/**
	 * Rank by growth: steepest slope first.
	 *
	 * By slope, never by count. That single decision is the difference between this list and obj list, and
	 * it is what the counter box's "fastest growing" heading promises.
	 *
	 * Ties are broken by count, then by peak, then by name, so the order is total: two classes level on
	 * everything measurable must not be able to swap places between one census and the next. A five-row list
	 * that reorders itself while somebody is reading it looks like the numbers are moving when they are not.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static TArray<FHeapClassEntry> RankByGrowth(const TArray<FHeapClassEntry>& Entries);

	/** Rank by growth, count, peak or name. Every order is total, for the reason given on RankByGrowth. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static TArray<FHeapClassEntry> RankEntries(const TArray<FHeapClassEntry>& Entries, EHeapSort Sort = EHeapSort::Growth);

	/**
	 * The single biggest finding, in a sentence a person can read out loud.
	 *
	 * "HeapCensusDemoObject is growing: 4,812 live, +1,204/min, and it has not fallen once in 120 s" is
	 * worth more than the table above it, because it names the thing to go and look at. An empty table gets
	 * a clean sentence, never an empty string: a blank line at the bottom of the counter box looks like a
	 * bug in the tool, and somebody will report it as one.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static FString SummarizeWorst(const TArray<FHeapClassEntry>& Entries);

	/**
	 * Whether a class name is covered by the ignore list.
	 *
	 * Case-insensitive. An exact name match, or a prefix match when the entry ends in '*'. Empty entries are
	 * skipped rather than treated as "matches everything" - a stray blank row in a settings array must not
	 * silently switch the whole census off.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static bool IsClassIgnored(const FString& ClassName, const TArray<FString>& IgnoredClassNames);

	/** "Ok", "Warn" or "Leaking". Used by the log line, the report and the counter box. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static FString VerdictToString(EHeapVerdict Verdict);

	/** The exit code Heap.Gate returns for a verdict: 0 Ok, 1 Warn, 2 Leaking. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static int32 VerdictToExitCode(EHeapVerdict Verdict);

	/**
	 * 214882 becomes "214,882".
	 *
	 * Grouped with commas, not with the machine's locale, because the counter box ends up in a bug report
	 * and a screenshot from a German machine and one from an American machine have to be comparable.
	 */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static FString FormatCount(int32 Count);

	/** "+1,204/min", "-38/min", or "0/min" for a flat class. Signed, so the direction is never in doubt. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Logic")
	static FString FormatSlope(float SlopePerMinute);

	/** The worst verdict in a table, and the class that earned it. Both out parameters may come back empty. */
	static EHeapVerdict FindWorst(const TArray<FHeapClassEntry>& Entries, FString& OutClassName, float& OutSlopePerMinute);

	// --------------------------------------------------------------------------------------------------
	// Blueprint access to the live census
	// --------------------------------------------------------------------------------------------------

	/** The engine-wide HeapCensus subsystem, or null before the engine is up. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	static UHeapCensusSubsystem* GetHeapCensus();

	/** Walk the object array now, out of turn, and return the resulting summary. */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	static FHeapCensusSummary SampleHeapCensus();

	/** The totals, the cost of the census and the verdict. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	static FHeapCensusSummary GetHeapCensusSummary();

	/** The N classes at the top of the given order. N of 0 or less returns the whole table. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	static TArray<FHeapClassEntry> GetTopClasses(int32 N = 5, EHeapSort Sort = EHeapSort::Growth);

	/** What the collection costs. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus")
	static FHeapGCStats GetHeapCensusGCStats();

	/** Throw the window away and start measuring again. The collection statistics are cleared too. */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	static void ResetHeapCensus();

	/** Show or hide the counter box. Same as Heap.Show / Heap.Hide. */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus")
	static void SetHeapCensusCounterBoxVisible(bool bVisible);

	// --------------------------------------------------------------------------------------------------
	// The demonstration
	// --------------------------------------------------------------------------------------------------

	/**
	 * The healthy game: create ObjectsPerSecond demo objects a second and let go of them again.
	 *
	 * The count wobbles, the slope stays at zero. This is the control, and it is the half of the
	 * demonstration people skip - without it, "the number went up" proves nothing.
	 *
	 * Zero or less stops the demonstration. Same as Heap.Churn.
	 */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus|Demo")
	static void StartHeapCensusChurn(int32 ObjectsPerSecond = 200);

	/**
	 * The leak: create ObjectsPerSecond demo objects a second and hold every one of them.
	 *
	 * Same objects, same rate, one difference - a strong reference is kept. The class climbs the growth
	 * table within a census or two and never falls back. Same as Heap.Leak.
	 */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus|Demo")
	static void StartHeapCensusLeak(int32 ObjectsPerSecond = 200);

	/**
	 * Stop the demonstration, drop the references, and optionally collect right away.
	 *
	 * With bCollectGarbage the count falls back in the next census and the collection that did it is timed
	 * like any other, so the box shows what clearing up cost. That is the third button, and it is the one
	 * that proves the first two were measurements rather than a drawing.
	 */
	UFUNCTION(BlueprintCallable, Category = "HeapCensus|Demo")
	static void StopHeapCensusDemo(bool bCollectGarbage = true);

	/** Live demo objects being held right now. Zero unless a leak is running. */
	UFUNCTION(BlueprintPure, Category = "HeapCensus|Demo")
	static int32 GetHeapCensusDemoObjectCount();
};
