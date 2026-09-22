// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HeapCensusStatics.h"

#include "Engine/Engine.h"
#include "HeapCensusRecorder.h"
#include "HeapCensusSubsystem.h"
#include "Misc/StringBuilder.h"

namespace HeapCensus
{
	/** Three. The smallest number of samples at which a fitted line can disagree with the data. */
	static constexpr int32 MinSamples = 3;
}

int32 UHeapCensusStatics::MinimumSamples()
{
	return HeapCensus::MinSamples;
}

// --------------------------------------------------------------------------------------------------
// Pure logic
// --------------------------------------------------------------------------------------------------

float UHeapCensusStatics::Slope(const TArray<float>& Samples, float SampleIntervalSeconds)
{
	const int32 Num = Samples.Num();

	if (Num < HeapCensus::MinSamples || SampleIntervalSeconds <= 0.0f)
	{
		// Not a slope of zero because the class is flat - a slope of zero because there is nothing to fit.
		// EvaluateEntry keeps the two apart, and GrowthNote says which one this is.
		return 0.0f;
	}

	// Least squares through (index, count). x is the sample index rather than a timestamp, which is exact as
	// long as the samples are evenly spaced - and they are, because the census runs on a timer. The one place
	// that stops being true is a frame hitch long enough to delay a census, and the error it introduces is
	// smaller than the thing being measured.
	//
	// The x values are known in advance, so their mean and their variance are closed forms rather than a
	// second pass: mean = (n-1)/2, sum of squared deviations = n(n^2-1)/12.
	const double N = static_cast<double>(Num);
	const double MeanX = (N - 1.0) * 0.5;

	double MeanY = 0.0;
	for (const float Sample : Samples)
	{
		MeanY += static_cast<double>(Sample);
	}
	MeanY /= N;

	double Covariance = 0.0;
	for (int32 Index = 0; Index < Num; ++Index)
	{
		Covariance += (static_cast<double>(Index) - MeanX) * (static_cast<double>(Samples[Index]) - MeanY);
	}

	const double VarianceX = N * (N * N - 1.0) / 12.0;
	if (VarianceX <= 0.0)
	{
		return 0.0;
	}

	// Objects per sample, then per minute. A flat series makes the covariance exactly zero and this exactly
	// zero with it, which is the point: a box full of "+0.02/min" on an idle game reads as noise, and a tool
	// that reads as noise gets switched off.
	const double PerSample = Covariance / VarianceX;
	const double PerMinute = PerSample * (60.0 / static_cast<double>(SampleIntervalSeconds));

	return static_cast<float>(PerMinute);
}

bool UHeapCensusStatics::IsMonotonicallyGrowing(const TArray<float>& Samples)
{
	if (Samples.Num() < HeapCensus::MinSamples)
	{
		return false;
	}

	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		// Equal counts as monotonic. A leak that pauses for one census is still a leak; only a fall back
		// proves that something is giving objects up again.
		if (Samples[Index] < Samples[Index - 1])
		{
			return false;
		}
	}

	return Samples.Last() > Samples[0];
}

EHeapVerdict UHeapCensusStatics::EvaluateGrowth(float SlopePerMinute, float ThresholdPerMinute, float WarnFraction)
{
	if (ThresholdPerMinute <= 0.0f)
	{
		// Judgement switched off, which is a legitimate setting: count and measure, do not have an opinion.
		return EHeapVerdict::Ok;
	}

	// Inclusive, and tested for it. A threshold somebody typed as "120" has to mean 120, or the number on
	// the settings page and the number in the report are two different numbers.
	if (SlopePerMinute >= ThresholdPerMinute)
	{
		return EHeapVerdict::Leaking;
	}

	const float WarnAt = ThresholdPerMinute * FMath::Clamp(WarnFraction, 0.0f, 1.0f);
	if (WarnFraction > 0.0f && SlopePerMinute >= WarnAt)
	{
		return EHeapVerdict::Warn;
	}

	return EHeapVerdict::Ok;
}

EHeapVerdict UHeapCensusStatics::EvaluateEntry(const FHeapClassEntry& Entry, float ThresholdPerMinute,
	float WarnFraction, bool bRequireMonotonic)
{
	// The refusal. Under three samples there is no opinion to have, whatever the arithmetic would produce -
	// and a tool that reports a leak four seconds after a map load is a tool that gets uninstalled on the
	// first day.
	if (Entry.SampleCount < HeapCensus::MinSamples)
	{
		return EHeapVerdict::Ok;
	}

	const EHeapVerdict Verdict = EvaluateGrowth(Entry.SlopePerMinute, ThresholdPerMinute, WarnFraction);

	// A class that fell back inside the window is capped at Warn however steep the fit is. A pool filling up
	// climbs; a leak climbs and never gives anything back. This is where the two are told apart.
	if (bRequireMonotonic && Verdict == EHeapVerdict::Leaking && !Entry.bMonotonic)
	{
		return EHeapVerdict::Warn;
	}

	return Verdict;
}

FString UHeapCensusStatics::GrowthNote(const FHeapClassEntry& Entry)
{
	if (Entry.SampleCount < HeapCensus::MinSamples)
	{
		return FString::Printf(TEXT("too little data (%d of %d samples)"), Entry.SampleCount, HeapCensus::MinSamples);
	}

	if (Entry.SlopePerMinute > 0.0f && !Entry.bMonotonic)
	{
		return TEXT("climbing, but it fell back at least once - a wave, not yet a leak");
	}

	if (Entry.SlopePerMinute < 0.0f)
	{
		return TEXT("shrinking");
	}

	return FString();
}

TArray<FHeapClassEntry> UHeapCensusStatics::RankByGrowth(const TArray<FHeapClassEntry>& Entries)
{
	return RankEntries(Entries, EHeapSort::Growth);
}

TArray<FHeapClassEntry> UHeapCensusStatics::RankEntries(const TArray<FHeapClassEntry>& Entries, EHeapSort Sort)
{
	TArray<FHeapClassEntry> Ranked = Entries;

	// Every one of these orders is total - it ends in a comparison on the name, which is unique per row. Two
	// classes level on everything measurable must not be able to swap places between one census and the
	// next; a five-row list that reorders itself twice a second looks like the numbers are moving when they
	// are not.
	Ranked.Sort([Sort](const FHeapClassEntry& A, const FHeapClassEntry& B)
	{
		switch (Sort)
		{
		case EHeapSort::Count:
			if (A.Count != B.Count)
			{
				return A.Count > B.Count;
			}
			break;

		case EHeapSort::Peak:
			if (A.Peak != B.Peak)
			{
				return A.Peak > B.Peak;
			}
			break;

		case EHeapSort::Name:
			return A.ClassName.Compare(B.ClassName, ESearchCase::IgnoreCase) < 0;

		case EHeapSort::Growth:
		default:
			// Slope first, and only slope. This is the line that makes the list different from obj list.
			if (!FMath::IsNearlyEqual(A.SlopePerMinute, B.SlopePerMinute, UE_KINDA_SMALL_NUMBER))
			{
				return A.SlopePerMinute > B.SlopePerMinute;
			}
			break;
		}

		if (A.Count != B.Count)
		{
			return A.Count > B.Count;
		}

		if (A.Peak != B.Peak)
		{
			return A.Peak > B.Peak;
		}

		return A.ClassName.Compare(B.ClassName, ESearchCase::IgnoreCase) < 0;
	});

	return Ranked;
}

EHeapVerdict UHeapCensusStatics::FindWorst(const TArray<FHeapClassEntry>& Entries, FString& OutClassName,
	float& OutSlopePerMinute)
{
	OutClassName.Reset();
	OutSlopePerMinute = 0.0f;

	EHeapVerdict Worst = EHeapVerdict::Ok;

	for (const FHeapClassEntry& Entry : Entries)
	{
		// The worst verdict wins, and within a verdict the steepest slope wins. Ordering by slope alone
		// would let a fast wave outrank a slow, monotonic, genuine leak.
		const bool bBetterVerdict = static_cast<uint8>(Entry.Verdict) > static_cast<uint8>(Worst);
		const bool bSameVerdictSteeper = Entry.Verdict == Worst && Entry.SlopePerMinute > OutSlopePerMinute;

		if (bBetterVerdict || bSameVerdictSteeper)
		{
			Worst = Entry.Verdict;
			OutClassName = Entry.ClassName;
			OutSlopePerMinute = Entry.SlopePerMinute;
		}
	}

	if (OutSlopePerMinute <= 0.0f && Worst == EHeapVerdict::Ok)
	{
		OutClassName.Reset();
		OutSlopePerMinute = 0.0f;
	}

	return Worst;
}

FString UHeapCensusStatics::SummarizeWorst(const TArray<FHeapClassEntry>& Entries)
{
	if (Entries.Num() == 0)
	{
		return TEXT("no census yet - nothing has been counted.");
	}

	FString WorstClass;
	float WorstSlope = 0.0f;
	const EHeapVerdict Worst = FindWorst(Entries, WorstClass, WorstSlope);

	if (WorstClass.IsEmpty())
	{
		return TEXT("nothing is growing: every class is flat or shrinking.");
	}

	const FHeapClassEntry* Entry = Entries.FindByPredicate(
		[&WorstClass](const FHeapClassEntry& Candidate) { return Candidate.ClassName == WorstClass; });

	if (!Entry)
	{
		return TEXT("nothing is growing: every class is flat or shrinking.");
	}

	const FString Note = GrowthNote(*Entry);

	if (Worst == EHeapVerdict::Leaking)
	{
		return FString::Printf(TEXT("%s is growing: %s live, %s, and it has not fallen once in %.0f s."),
			*Entry->ClassName, *FormatCount(Entry->Count), *FormatSlope(Entry->SlopePerMinute), Entry->WindowSeconds);
	}

	if (Worst == EHeapVerdict::Warn)
	{
		return FString::Printf(TEXT("%s is climbing: %s live, %s over %.0f s%s%s."),
			*Entry->ClassName, *FormatCount(Entry->Count), *FormatSlope(Entry->SlopePerMinute), Entry->WindowSeconds,
			Note.IsEmpty() ? TEXT("") : TEXT(" - "), *Note);
	}

	return FString::Printf(TEXT("fastest growing is %s at %s, under the threshold."),
		*Entry->ClassName, *FormatSlope(Entry->SlopePerMinute));
}

bool UHeapCensusStatics::IsClassIgnored(const FString& ClassName, const TArray<FString>& IgnoredClassNames)
{
	if (ClassName.IsEmpty())
	{
		return false;
	}

	for (const FString& Ignored : IgnoredClassNames)
	{
		// An empty row in the settings array is skipped, not treated as "matches everything". A stray blank
		// line must never be able to switch the whole census off.
		if (Ignored.IsEmpty())
		{
			continue;
		}

		if (Ignored.EndsWith(TEXT("*"), ESearchCase::CaseSensitive))
		{
			const FString Prefix = Ignored.LeftChop(1);
			if (!Prefix.IsEmpty() && ClassName.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				return true;
			}

			continue;
		}

		if (ClassName.Equals(Ignored, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}

	return false;
}

FString UHeapCensusStatics::VerdictToString(EHeapVerdict Verdict)
{
	switch (Verdict)
	{
	case EHeapVerdict::Leaking:	return TEXT("Leaking");
	case EHeapVerdict::Warn:	return TEXT("Warn");
	default:					return TEXT("Ok");
	}
}

int32 UHeapCensusStatics::VerdictToExitCode(EHeapVerdict Verdict)
{
	switch (Verdict)
	{
	case EHeapVerdict::Leaking:	return 2;
	case EHeapVerdict::Warn:	return 1;
	default:					return 0;
	}
}

FString UHeapCensusStatics::FormatCount(int32 Count)
{
	// Grouped by hand rather than through the locale. This string ends up in a bug report, and a screenshot
	// from a German machine and one from an American machine have to be comparable.
	const bool bNegative = Count < 0;
	FString Digits = FString::Printf(TEXT("%lld"), FMath::Abs(static_cast<int64>(Count)));

	for (int32 Index = Digits.Len() - 3; Index > 0; Index -= 3)
	{
		Digits.InsertAt(Index, TEXT(","));
	}

	return bNegative ? FString(TEXT("-")) + Digits : Digits;
}

FString UHeapCensusStatics::FormatSlope(float SlopePerMinute)
{
	const int32 Rounded = FMath::RoundToInt(SlopePerMinute);

	if (Rounded == 0)
	{
		// Deliberately not "+0/min" and not "-0/min". Flat is flat.
		return TEXT("0/min");
	}

	return FString::Printf(TEXT("%s%s/min"), Rounded > 0 ? TEXT("+") : TEXT(""), *FormatCount(Rounded));
}

// --------------------------------------------------------------------------------------------------
// Blueprint access to the live census
// --------------------------------------------------------------------------------------------------

UHeapCensusSubsystem* UHeapCensusStatics::GetHeapCensus()
{
	return UHeapCensusSubsystem::Get();
}

FHeapCensusSummary UHeapCensusStatics::SampleHeapCensus()
{
	return FHeapCensusRecorder::Get().Sample();
}

FHeapCensusSummary UHeapCensusStatics::GetHeapCensusSummary()
{
	return FHeapCensusRecorder::Get().GetSummary();
}

TArray<FHeapClassEntry> UHeapCensusStatics::GetTopClasses(int32 N, EHeapSort Sort)
{
	return FHeapCensusRecorder::Get().GetTop(N, Sort);
}

FHeapGCStats UHeapCensusStatics::GetHeapCensusGCStats()
{
	return FHeapCensusRecorder::Get().GetGCStats();
}

void UHeapCensusStatics::ResetHeapCensus()
{
	FHeapCensusRecorder::Get().Reset();
}

void UHeapCensusStatics::SetHeapCensusCounterBoxVisible(bool bVisible)
{
	FHeapCensusRecorder::Get().SetShowCounterBox(bVisible);
}

// --------------------------------------------------------------------------------------------------
// The demonstration
// --------------------------------------------------------------------------------------------------

void UHeapCensusStatics::StartHeapCensusChurn(int32 ObjectsPerSecond)
{
	FHeapCensusRecorder::Get().StartDemo(ObjectsPerSecond, /*bHold*/ false);
}

void UHeapCensusStatics::StartHeapCensusLeak(int32 ObjectsPerSecond)
{
	FHeapCensusRecorder::Get().StartDemo(ObjectsPerSecond, /*bHold*/ true);
}

void UHeapCensusStatics::StopHeapCensusDemo(bool bCollectGarbage)
{
	FHeapCensusRecorder::Get().StopDemo(bCollectGarbage);
}

int32 UHeapCensusStatics::GetHeapCensusDemoObjectCount()
{
	return FHeapCensusRecorder::Get().GetDemoObjectCount();
}
