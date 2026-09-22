// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "Algo/Reverse.h"
#include "HeapCensusStatics.h"
#include "HeapCensusTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace HeapCensusTests
{
	// CommandletContext as well as EditorContext: what is checked here is arithmetic a packaged build runs,
	// and a test that only exists when somebody has the editor open is a test that will not be there on the
	// build server when it matters.
	constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::CommandletContext
		| EAutomationTestFlags::EngineFilter;

	/** TestEqual has no overload for a scoped enum, so every verdict is compared as an int32. */
	int32 AsInt(EHeapVerdict Verdict)
	{
		return static_cast<int32>(Verdict);
	}

	/** A series that starts at Start and adds Step every sample. */
	TArray<float> Ramp(float Start, float Step, int32 Count)
	{
		TArray<float> Samples;
		Samples.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			Samples.Add(Start + Step * static_cast<float>(Index));
		}

		return Samples;
	}

	FHeapClassEntry MakeEntry(const TCHAR* ClassName, int32 Count, float SlopePerMinute, int32 SampleCount = 60,
		bool bMonotonic = true, int32 Peak = 0)
	{
		FHeapClassEntry Entry;
		Entry.ClassName = ClassName;
		Entry.Count = Count;
		Entry.Peak = Peak > 0 ? Peak : Count;
		Entry.FirstCount = Count;
		Entry.SlopePerMinute = SlopePerMinute;
		Entry.SampleCount = SampleCount;
		Entry.WindowSeconds = FMath::Max(SampleCount - 1, 0) * 2.0f;
		Entry.bMonotonic = bMonotonic;
		Entry.Verdict = UHeapCensusStatics::EvaluateEntry(Entry, 120.0f, 0.5f, /*bRequireMonotonic*/ true);
		return Entry;
	}
}

//
// (1) A flat series has a slope of exactly zero.
//
// Exactly, not nearly. A counter box full of "+0.02/min" on an idle game reads as noise, and a tool that
// reads as noise is a tool people switch off in the first week.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusSlopeIsZeroOnAFlatSeriesTest,
	"HeapCensus.Logic.SlopeIsExactlyZeroOnAFlatSeries",
	HeapCensusTests::TestFlags)

bool FHeapCensusSlopeIsZeroOnAFlatSeriesTest::RunTest(const FString& Parameters)
{
	const TArray<float> Flat = HeapCensusTests::Ramp(400.0f, 0.0f, 60);

	TestEqual(TEXT("A flat series has a slope of exactly zero"),
		UHeapCensusStatics::Slope(Flat, 2.0f), 0.0f);

	// Zero objects is just as flat as four hundred objects.
	TestEqual(TEXT("A series of zeros has a slope of exactly zero"),
		UHeapCensusStatics::Slope(HeapCensusTests::Ramp(0.0f, 0.0f, 12), 2.0f), 0.0f);

	// A single spike in the middle of an otherwise flat series is not a trend either: the fit runs through
	// the middle of it and comes back out flat.
	TArray<float> Spiked = HeapCensusTests::Ramp(100.0f, 0.0f, 9);
	Spiked[4] = 900.0f;

	TestEqual(TEXT("A symmetric spike leaves the slope at zero"),
		UHeapCensusStatics::Slope(Spiked, 2.0f), 0.0f);

	return true;
}

//
// (2) A linearly rising series gives the right slope, in objects per minute.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusSlopeOnARisingSeriesTest,
	"HeapCensus.Logic.SlopeOnARisingSeriesIsObjectsPerMinute",
	HeapCensusTests::TestFlags)

bool FHeapCensusSlopeOnARisingSeriesTest::RunTest(const FString& Parameters)
{
	// Ten objects every two seconds is three hundred a minute, and the unit conversion is the half of this
	// that is easy to get wrong.
	TestEqual(TEXT("+10 per 2 s sample is +300 per minute"),
		UHeapCensusStatics::Slope(HeapCensusTests::Ramp(0.0f, 10.0f, 60), 2.0f), 300.0f, 0.01f);

	// The same series read at a different interval means a different rate. The samples do not know how far
	// apart they are; the caller has to say, and this is the test that the caller is believed.
	TestEqual(TEXT("The same series at a 1 s interval is +600 per minute"),
		UHeapCensusStatics::Slope(HeapCensusTests::Ramp(0.0f, 10.0f, 60), 1.0f), 600.0f, 0.01f);

	// Downwards works exactly the same way. A class giving objects back is the thing you want to see after a
	// collection, and a plugin that could only count upwards would not be able to show it.
	TestEqual(TEXT("A falling series has a negative slope"),
		UHeapCensusStatics::Slope(HeapCensusTests::Ramp(1000.0f, -5.0f, 30), 2.0f), -150.0f, 0.01f);

	// Where the series starts changes nothing about how fast it is climbing.
	TestEqual(TEXT("The offset does not affect the slope"),
		UHeapCensusStatics::Slope(HeapCensusTests::Ramp(50000.0f, 10.0f, 60), 2.0f), 300.0f, 0.01f);

	// A nonsense interval cannot be turned into a rate, and the honest answer is zero rather than an infinity.
	TestEqual(TEXT("A zero interval yields zero rather than a division by zero"),
		UHeapCensusStatics::Slope(HeapCensusTests::Ramp(0.0f, 10.0f, 60), 0.0f), 0.0f);

	return true;
}

//
// (3) RankByGrowth sorts by slope, not by count, and is stable on ties.
//
// This is the single test that describes what the plugin is for. obj list already ranks by size; a table
// that quietly did the same would be a worse version of a tool the engine ships with.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusRankByGrowthTest,
	"HeapCensus.Logic.RankByGrowthSortsBySlopeNotByCountAndIsStableOnTies",
	HeapCensusTests::TestFlags)

bool FHeapCensusRankByGrowthTest::RunTest(const FString& Parameters)
{
	TArray<FHeapClassEntry> Entries;
	Entries.Add(HeapCensusTests::MakeEntry(TEXT("StaticMeshComponent"), 84000, 0.0f));	// huge, flat
	Entries.Add(HeapCensusTests::MakeEntry(TEXT("LeakyThing"), 610, 420.0f));			// small, climbing hard
	Entries.Add(HeapCensusTests::MakeEntry(TEXT("Projectile"), 400, 8.0f));				// small, drifting

	const TArray<FHeapClassEntry> Ranked = UHeapCensusStatics::RankByGrowth(Entries);

	TestEqual(TEXT("The fastest-growing class comes first, however small it is"),
		Ranked[0].ClassName, FString(TEXT("LeakyThing")));
	TestEqual(TEXT("Then the next steepest"), Ranked[1].ClassName, FString(TEXT("Projectile")));
	TestEqual(TEXT("The biggest class comes last, because it is not growing"),
		Ranked[2].ClassName, FString(TEXT("StaticMeshComponent")));

	// And the same table asked for by size gives the opposite answer, which is what makes the first assertion
	// mean something.
	const TArray<FHeapClassEntry> BySize = UHeapCensusStatics::RankEntries(Entries, EHeapSort::Count);
	TestEqual(TEXT("By count, the biggest class comes first"),
		BySize[0].ClassName, FString(TEXT("StaticMeshComponent")));

	// Three rows level on every measurable value. The order has to be decided by the name and must not depend
	// on the order they arrived in - a five-row list that reorders itself twice a second is unreadable, and
	// it looks like the numbers are moving when they are not.
	TArray<FHeapClassEntry> Tied;
	Tied.Add(HeapCensusTests::MakeEntry(TEXT("Zulu"), 100, 60.0f));
	Tied.Add(HeapCensusTests::MakeEntry(TEXT("Alpha"), 100, 60.0f));
	Tied.Add(HeapCensusTests::MakeEntry(TEXT("Mike"), 100, 60.0f));

	const TArray<FHeapClassEntry> RankedTied = UHeapCensusStatics::RankByGrowth(Tied);
	TestEqual(TEXT("Ties are broken by name, first"), RankedTied[0].ClassName, FString(TEXT("Alpha")));
	TestEqual(TEXT("Ties are broken by name, second"), RankedTied[1].ClassName, FString(TEXT("Mike")));
	TestEqual(TEXT("Ties are broken by name, third"), RankedTied[2].ClassName, FString(TEXT("Zulu")));

	Algo::Reverse(Tied);
	const TArray<FHeapClassEntry> RankedReversed = UHeapCensusStatics::RankByGrowth(Tied);
	for (int32 Index = 0; Index < RankedTied.Num(); ++Index)
	{
		TestEqual(TEXT("The same rows in the other order rank identically"),
			RankedReversed[Index].ClassName, RankedTied[Index].ClassName);
	}

	// Equal slopes but different counts: the bigger one goes first, because between two classes climbing at
	// the same rate the one that is already large will hit the wall sooner.
	TArray<FHeapClassEntry> SameSlope;
	SameSlope.Add(HeapCensusTests::MakeEntry(TEXT("Small"), 10, 200.0f));
	SameSlope.Add(HeapCensusTests::MakeEntry(TEXT("Big"), 9000, 200.0f));

	const TArray<FHeapClassEntry> RankedSameSlope = UHeapCensusStatics::RankByGrowth(SameSlope);
	TestEqual(TEXT("On an equal slope the bigger class comes first"),
		RankedSameSlope[0].ClassName, FString(TEXT("Big")));

	return true;
}

//
// (4) EvaluateGrowth hits the threshold exactly.
//
// A threshold somebody typed as "120" has to mean 120, or the number on the settings page and the number in
// the report are two different numbers and every conversation about them is wasted.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusEvaluateGrowthTest,
	"HeapCensus.Logic.EvaluateGrowthHitsTheThresholdExactly",
	HeapCensusTests::TestFlags)

bool FHeapCensusEvaluateGrowthTest::RunTest(const FString& Parameters)
{
	constexpr float Threshold = 120.0f;
	constexpr float WarnFraction = 0.5f;

	TestEqual(TEXT("Exactly at the threshold is Leaking"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateGrowth(120.0f, Threshold, WarnFraction)),
		HeapCensusTests::AsInt(EHeapVerdict::Leaking));

	TestEqual(TEXT("A hair under the threshold is still only Warn"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateGrowth(119.99f, Threshold, WarnFraction)),
		HeapCensusTests::AsInt(EHeapVerdict::Warn));

	TestEqual(TEXT("Exactly at the warning fraction is Warn"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateGrowth(60.0f, Threshold, WarnFraction)),
		HeapCensusTests::AsInt(EHeapVerdict::Warn));

	TestEqual(TEXT("A hair under the warning fraction is Ok"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateGrowth(59.99f, Threshold, WarnFraction)),
		HeapCensusTests::AsInt(EHeapVerdict::Ok));

	TestEqual(TEXT("A shrinking class is Ok"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateGrowth(-500.0f, Threshold, WarnFraction)),
		HeapCensusTests::AsInt(EHeapVerdict::Ok));

	// A warning fraction of 1 removes the warning band entirely: under the threshold is simply Ok.
	TestEqual(TEXT("With a fraction of 1 there is no warning band"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateGrowth(119.0f, Threshold, 1.0f)),
		HeapCensusTests::AsInt(EHeapVerdict::Ok));

	// And a threshold of zero switches the judgement off without switching the census off.
	TestEqual(TEXT("A threshold of zero disables the verdict"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateGrowth(100000.0f, 0.0f, WarnFraction)),
		HeapCensusTests::AsInt(EHeapVerdict::Ok));

	// The exit codes the gate returns, so a build server's contract is covered by a test rather than by a
	// sentence in a readme.
	TestEqual(TEXT("Ok exits 0"), UHeapCensusStatics::VerdictToExitCode(EHeapVerdict::Ok), 0);
	TestEqual(TEXT("Warn exits 1"), UHeapCensusStatics::VerdictToExitCode(EHeapVerdict::Warn), 1);
	TestEqual(TEXT("Leaking exits 2"), UHeapCensusStatics::VerdictToExitCode(EHeapVerdict::Leaking), 2);

	return true;
}

//
// (5) Classes on the ignore list never appear.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusIgnoreListTest,
	"HeapCensus.Logic.IgnoredClassesNeverAppear",
	HeapCensusTests::TestFlags)

bool FHeapCensusIgnoreListTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Ignored = {
		TEXT("Package"),
		TEXT("ObjectRedirector"),
		TEXT("LinkerPlaceholder*"),
	};

	TestTrue(TEXT("An exact name is ignored"),
		UHeapCensusStatics::IsClassIgnored(TEXT("Package"), Ignored));

	TestTrue(TEXT("Matching is case-insensitive"),
		UHeapCensusStatics::IsClassIgnored(TEXT("package"), Ignored));

	TestTrue(TEXT("A trailing star is a prefix match"),
		UHeapCensusStatics::IsClassIgnored(TEXT("LinkerPlaceholderExportObject"), Ignored));

	// The dangerous near-miss. "Package" must not swallow "PackageMapClient", or the ignore list would hide
	// gameplay classes and the census would be quietly lying.
	TestFalse(TEXT("An exact entry does not match a longer name"),
		UHeapCensusStatics::IsClassIgnored(TEXT("PackageMapClient"), Ignored));

	TestFalse(TEXT("An unrelated class is not ignored"),
		UHeapCensusStatics::IsClassIgnored(TEXT("StaticMeshComponent"), Ignored));

	// A blank row in a settings array must not be read as "matches everything". That mistake would switch
	// the entire census off and look exactly like the plugin being broken.
	const TArray<FString> WithBlank = { FString(), TEXT("*"), TEXT("Package") };
	TestFalse(TEXT("An empty entry matches nothing"),
		UHeapCensusStatics::IsClassIgnored(TEXT("StaticMeshComponent"), WithBlank));
	TestTrue(TEXT("A real entry alongside a blank one still works"),
		UHeapCensusStatics::IsClassIgnored(TEXT("Package"), WithBlank));

	// And the filter has to survive being handed the table it filters: a ranked list must never contain a
	// class the settings said to drop.
	TArray<FHeapClassEntry> Entries;
	Entries.Add(HeapCensusTests::MakeEntry(TEXT("Package"), 900, 500.0f));
	Entries.Add(HeapCensusTests::MakeEntry(TEXT("LeakyThing"), 610, 420.0f));

	Entries.RemoveAll([&Ignored](const FHeapClassEntry& Entry)
	{
		return UHeapCensusStatics::IsClassIgnored(Entry.ClassName, Ignored);
	});

	const TArray<FHeapClassEntry> Ranked = UHeapCensusStatics::RankByGrowth(Entries);
	TestEqual(TEXT("Only the class that was not ignored is left"), Ranked.Num(), 1);
	TestEqual(TEXT("And it is the right one"), Ranked[0].ClassName, FString(TEXT("LeakyThing")));

	return true;
}

//
// (6) Too short a window says so instead of inventing a slope.
//
// Two points always describe a perfect straight line. A plugin that reported a leak four seconds after a map
// load, from two samples, would be right often enough to be believed and wrong often enough to be useless.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusTooLittleDataTest,
	"HeapCensus.Logic.TooShortAWindowSaysSoInsteadOfInventingASlope",
	HeapCensusTests::TestFlags)

bool FHeapCensusTooLittleDataTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Three samples is the minimum"), UHeapCensusStatics::MinimumSamples(), 3);

	// Two samples that would fit a line of +30000 per minute, which is the point: the arithmetic would
	// happily produce a number.
	const TArray<float> TwoSamples = { 0.0f, 1000.0f };
	TestEqual(TEXT("Two samples produce no slope at all"),
		UHeapCensusStatics::Slope(TwoSamples, 2.0f), 0.0f);

	TestFalse(TEXT("Two samples are never called monotonic"),
		UHeapCensusStatics::IsMonotonicallyGrowing(TwoSamples));

	// An entry whose slope somehow says "leaking" but which has fewer than three samples behind it is Ok,
	// and the note says why. The verdict is not allowed to outrun the data.
	FHeapClassEntry Thin = HeapCensusTests::MakeEntry(TEXT("Suspicious"), 1000, 30000.0f, /*SampleCount*/ 2);
	Thin.Verdict = UHeapCensusStatics::EvaluateEntry(Thin, 120.0f, 0.5f, true);

	TestEqual(TEXT("Under three samples the verdict is Ok whatever the slope says"),
		HeapCensusTests::AsInt(Thin.Verdict), HeapCensusTests::AsInt(EHeapVerdict::Ok));

	TestEqual(TEXT("And the note says why, rather than leaving it to be guessed"),
		UHeapCensusStatics::GrowthNote(Thin), FString(TEXT("too little data (2 of 3 samples)")));

	// One more sample and the same slope is judged for real.
	FHeapClassEntry Thick = HeapCensusTests::MakeEntry(TEXT("Suspicious"), 1000, 30000.0f, /*SampleCount*/ 3);
	TestEqual(TEXT("At three samples the verdict is reached"),
		HeapCensusTests::AsInt(Thick.Verdict), HeapCensusTests::AsInt(EHeapVerdict::Leaking));

	return true;
}

//
// (7) A class that fell back is a wave, not a leak.
//
// The one that separates this plugin from a threshold on a counter. A pool filling up climbs; a leak climbs
// and never gives anything back, and the gate only fails a build for the second.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusMonotonicTest,
	"HeapCensus.Logic.AClassThatFellBackIsAWaveNotALeak",
	HeapCensusTests::TestFlags)

bool FHeapCensusMonotonicTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("A steadily rising series is monotonic"),
		UHeapCensusStatics::IsMonotonicallyGrowing(HeapCensusTests::Ramp(0.0f, 10.0f, 10)));

	// A pause is not a fall. A leak that stops for one census is still a leak.
	const TArray<float> WithPlateau = { 10.0f, 20.0f, 20.0f, 20.0f, 30.0f };
	TestTrue(TEXT("A plateau in the middle is still monotonic"),
		UHeapCensusStatics::IsMonotonicallyGrowing(WithPlateau));

	// One step down anywhere is enough. Something gave objects back, so something can.
	const TArray<float> WithDip = { 10.0f, 20.0f, 19.0f, 40.0f, 60.0f };
	TestFalse(TEXT("A single fall back breaks it"),
		UHeapCensusStatics::IsMonotonicallyGrowing(WithDip));

	const TArray<float> Flat = HeapCensusTests::Ramp(400.0f, 0.0f, 10);
	TestFalse(TEXT("A flat series is not growing"), UHeapCensusStatics::IsMonotonicallyGrowing(Flat));

	// And the consequence: the same steep slope is Leaking when it never fell back and only Warn when it did.
	const FHeapClassEntry Leak = HeapCensusTests::MakeEntry(TEXT("Leak"), 5000, 400.0f, 60, /*bMonotonic*/ true);
	const FHeapClassEntry Wave = HeapCensusTests::MakeEntry(TEXT("Wave"), 5000, 400.0f, 60, /*bMonotonic*/ false);

	TestEqual(TEXT("Steep and never falling back is Leaking"),
		HeapCensusTests::AsInt(Leak.Verdict), HeapCensusTests::AsInt(EHeapVerdict::Leaking));
	TestEqual(TEXT("The same slope with a fall back in it is capped at Warn"),
		HeapCensusTests::AsInt(Wave.Verdict), HeapCensusTests::AsInt(EHeapVerdict::Warn));

	// Unless the project has asked for every climb to count, which is a legitimate setting.
	TestEqual(TEXT("Without the monotonic requirement the wave is Leaking too"),
		HeapCensusTests::AsInt(UHeapCensusStatics::EvaluateEntry(Wave, 120.0f, 0.5f, /*bRequireMonotonic*/ false)),
		HeapCensusTests::AsInt(EHeapVerdict::Leaking));

	return true;
}

//
// (8) The counter box never prints an empty line, and never prints a locale.
//
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHeapCensusFormattingTest,
	"HeapCensus.Logic.FormattingIsStableAndNeverEmpty",
	HeapCensusTests::TestFlags)

bool FHeapCensusFormattingTest::RunTest(const FString& Parameters)
{
	// Grouped by hand rather than through the machine's locale, so a screenshot from a German machine and one
	// from an American machine are comparable.
	TestEqual(TEXT("Six figures are grouped"), UHeapCensusStatics::FormatCount(214882), FString(TEXT("214,882")));
	TestEqual(TEXT("Three figures are not"), UHeapCensusStatics::FormatCount(400), FString(TEXT("400")));
	TestEqual(TEXT("Exactly four figures group once"), UHeapCensusStatics::FormatCount(1000), FString(TEXT("1,000")));
	TestEqual(TEXT("Zero is zero"), UHeapCensusStatics::FormatCount(0), FString(TEXT("0")));
	TestEqual(TEXT("Negatives keep their sign"), UHeapCensusStatics::FormatCount(-12345), FString(TEXT("-12,345")));

	// The sign is always visible on a slope, because "1204/min" and "-1204/min" would otherwise be one glance
	// apart and mean opposite things.
	TestEqual(TEXT("A positive slope is signed"), UHeapCensusStatics::FormatSlope(1204.0f), FString(TEXT("+1,204/min")));
	TestEqual(TEXT("A negative slope is signed"), UHeapCensusStatics::FormatSlope(-38.0f), FString(TEXT("-38/min")));
	TestEqual(TEXT("Flat is flat, with no sign at all"), UHeapCensusStatics::FormatSlope(0.0f), FString(TEXT("0/min")));
	TestEqual(TEXT("A slope that rounds to zero is flat too"), UHeapCensusStatics::FormatSlope(0.4f), FString(TEXT("0/min")));

	// An empty table gets a sentence, never an empty string: a blank line at the bottom of the counter box
	// looks like a bug in the tool, and somebody will report it as one.
	TestFalse(TEXT("An empty table still produces a sentence"),
		UHeapCensusStatics::SummarizeWorst(TArray<FHeapClassEntry>()).IsEmpty());

	TArray<FHeapClassEntry> Flat;
	Flat.Add(HeapCensusTests::MakeEntry(TEXT("StaticMeshComponent"), 84000, 0.0f));
	TestFalse(TEXT("A table with nothing growing still produces a sentence"),
		UHeapCensusStatics::SummarizeWorst(Flat).IsEmpty());

	TArray<FHeapClassEntry> Leaking;
	Leaking.Add(HeapCensusTests::MakeEntry(TEXT("StaticMeshComponent"), 84000, 0.0f));
	Leaking.Add(HeapCensusTests::MakeEntry(TEXT("LeakyThing"), 4812, 1204.0f));

	const FString Sentence = UHeapCensusStatics::SummarizeWorst(Leaking);
	TestTrue(TEXT("The sentence names the growing class, not the big one"), Sentence.Contains(TEXT("LeakyThing")));
	TestFalse(TEXT("The sentence does not name the big flat one"), Sentence.Contains(TEXT("StaticMeshComponent")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
