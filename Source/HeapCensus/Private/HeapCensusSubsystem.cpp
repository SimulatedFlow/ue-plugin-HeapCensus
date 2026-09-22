// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HeapCensusSubsystem.h"

#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GlobalRenderResources.h"
#include "HeapCensusLog.h"
#include "HeapCensusRecorder.h"
#include "HeapCensusStatics.h"
#include "Misc/StringBuilder.h"
#include "SceneTypes.h"

void UHeapCensusSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The census has been running since the module loaded. Nothing is started here - this only hangs the
	// Blueprint-facing delegate onto a census that already has history in it.
	GrowingHandle = FHeapCensusRecorder::Get().OnClassGrowing.AddUObject(
		this, &UHeapCensusSubsystem::HandleClassGrowing);
}

void UHeapCensusSubsystem::Deinitialize()
{
	// The recorder outlives this object by design, so a handle left behind here would be a dangling raw
	// pointer in a static delegate - the classic way an engine subsystem crashes on shutdown.
	FHeapCensusRecorder::Get().OnClassGrowing.Remove(GrowingHandle);
	GrowingHandle.Reset();

	Super::Deinitialize();
}

UHeapCensusSubsystem* UHeapCensusSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UHeapCensusSubsystem>() : nullptr;
}

void UHeapCensusSubsystem::HandleClassGrowing(const FString& ClassName, float SlopePerMinute, EHeapVerdict Verdict)
{
	OnClassGrowing.Broadcast(ClassName, SlopePerMinute, Verdict);
}

// --------------------------------------------------------------------------------------------------
// The census
// --------------------------------------------------------------------------------------------------

FHeapCensusSummary UHeapCensusSubsystem::Sample()
{
	return FHeapCensusRecorder::Get().Sample();
}

FHeapCensusSummary UHeapCensusSubsystem::GetSummary() const
{
	return FHeapCensusRecorder::Get().GetSummary();
}

TArray<FHeapClassEntry> UHeapCensusSubsystem::GetTop(int32 N, EHeapSort Sort) const
{
	return FHeapCensusRecorder::Get().GetTop(N, Sort);
}

FHeapGCStats UHeapCensusSubsystem::GetGCStats() const
{
	return FHeapCensusRecorder::Get().GetGCStats();
}

int32 UHeapCensusSubsystem::GetTotalObjects() const
{
	return FHeapCensusRecorder::Get().GetSummary().TotalObjects;
}

bool UHeapCensusSubsystem::IsLeaking() const
{
	return FHeapCensusRecorder::Get().GetSummary().Verdict == EHeapVerdict::Leaking;
}

void UHeapCensusSubsystem::Reset()
{
	FHeapCensusRecorder::Get().Reset();
}

bool UHeapCensusSubsystem::WriteReport(const FString& Path)
{
	return FHeapCensusRecorder::Get().WriteReport(Path);
}

void UHeapCensusSubsystem::DumpToLog()
{
	FHeapCensusRecorder::Get().DumpToLog(0, EHeapSort::Growth);
}

void UHeapCensusSubsystem::SetGrowthThreshold(float ThresholdPerMinute)
{
	FHeapCensusRecorder::Get().SetGrowthThreshold(ThresholdPerMinute);
}

float UHeapCensusSubsystem::GetGrowthThreshold() const
{
	return FHeapCensusRecorder::Get().GetGrowthThreshold();
}

void UHeapCensusSubsystem::SetShowCounterBox(bool bInShow)
{
	FHeapCensusRecorder::Get().SetShowCounterBox(bInShow);
}

bool UHeapCensusSubsystem::IsShowingCounterBox() const
{
	return FHeapCensusRecorder::Get().IsShowingCounterBox();
}

// --------------------------------------------------------------------------------------------------
// The counter box
// --------------------------------------------------------------------------------------------------

void UHeapCensusSubsystem::DrawCounterBox(UCanvas* Canvas)
{
	const FHeapCensusRecorder& Recorder = FHeapCensusRecorder::Get();

	if (!Canvas || !Recorder.IsShowingCounterBox())
	{
		return;
	}

	UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
	if (!Font)
	{
		return;
	}

	const FHeapCensusSummary& Summary = Recorder.GetSummary();
	const FHeapGCStats& GC = Summary.GC;
	const TArray<FHeapClassEntry> Ranked = Recorder.GetTop(Recorder.GetTopClassLines(), EHeapSort::Growth);

	constexpr float LineHeight = 15.0f;
	constexpr float BoxWidth = 640.0f;

	const float BoxX = static_cast<float>(Recorder.GetCounterBoxPosition().X);
	const float BoxY = static_cast<float>(Recorder.GetCounterBoxPosition().Y);

	const int32 FootnoteLines = (Summary.IgnoredClasses > 0 ? 1 : 0) + (Summary.UntrackedClasses > 0 ? 1 : 0);

	// Header, the "fastest growing" heading, the column head, the rows, the footnotes and the sentence.
	const int32 LineCount = 3 + Ranked.Num() + FootnoteLines + 1;

	FCanvasTileItem Background(
		FVector2D(BoxX - 8.0f, BoxY - 8.0f),
		GWhiteTexture,
		FVector2D(BoxWidth, LineCount * LineHeight + 16.0f),
		FLinearColor(0.0f, 0.0f, 0.0f, 0.6f));
	Background.BlendMode = SE_BLEND_Translucent;
	Canvas->DrawItem(Background);

	float LineY = BoxY;

	auto DrawLine = [&](FStringView Line, const FLinearColor& Colour)
	{
		FCanvasTextStringViewItem Item(FVector2D(BoxX, LineY), Line, Font, Colour);
		Canvas->DrawItem(Item);
		LineY += LineHeight;
	};

	const FLinearColor Good(0.55f, 0.95f, 0.55f, 1.0f);
	const FLinearColor Warn(1.0f, 0.78f, 0.30f, 1.0f);
	const FLinearColor Bad(1.0f, 0.42f, 0.38f, 1.0f);
	const FLinearColor Body(0.90f, 0.90f, 0.90f, 1.0f);
	const FLinearColor Faint(0.62f, 0.62f, 0.66f, 1.0f);

	auto ColourFor = [&](EHeapVerdict Verdict)
	{
		return Verdict == EHeapVerdict::Leaking ? Bad : Verdict == EHeapVerdict::Warn ? Warn : Good;
	};

	const FLinearColor VerdictColour = ColourFor(Summary.Verdict);

	TStringBuilder<400> Line;

	// The header, and the line a store screenshot is built around. Four numbers, and the last of them is the
	// price of the other three: what this plugin's own walk over the object array cost. A tool that hides its
	// own price is a tool that gets blamed for the hitch it was bought to find.
	Line.Reset();
	Line.Appendf(TEXT("UObjects %s"), *UHeapCensusStatics::FormatCount(Summary.TotalObjects));

	if (GC.RunCount > 0)
	{
		Line.Appendf(TEXT(" | GC last %.1f ms, worst %.1f ms, every %.0f s"),
			GC.LastMilliseconds, GC.WorstMilliseconds,
			GC.AverageIntervalSeconds > 0.0f ? GC.AverageIntervalSeconds : GC.SecondsSinceLastRun);
	}
	else
	{
		Line.Append(TEXT(" | GC not seen yet"));
	}

	Line.Appendf(TEXT(" | census %.1f ms"), Summary.LastCensusMilliseconds);
	DrawLine(Line.ToView(), VerdictColour);

	// The heading, and it is a promise: this list is ordered by slope, not by size. That single word is the
	// difference between this box and obj list, and it belongs in the picture so a screenshot cannot be
	// misread.
	Line.Reset();
	if (Summary.bSettling)
	{
		Line.Appendf(TEXT("settling after a map change: %.0f s to go - no slope yet"), Summary.SettleSecondsRemaining);
		DrawLine(Line.ToView(), Faint);
	}
	else
	{
		Line.Appendf(TEXT("fastest growing - window %.0f s, threshold %s"),
			Summary.WindowSeconds, *UHeapCensusStatics::FormatSlope(Summary.GrowthThresholdPerMinute));
		DrawLine(Line.ToView(), Body);
	}

	Line.Reset();
	Line.Appendf(TEXT("%-42s %10s %12s"), TEXT("class"), TEXT("count"), TEXT("per minute"));
	DrawLine(Line.ToView(), Faint);

	for (const FHeapClassEntry& Entry : Ranked)
	{
		Line.Reset();
		Line.Appendf(TEXT("%-42s %10s %12s"),
			*Entry.ClassName,
			*UHeapCensusStatics::FormatCount(Entry.Count),
			*UHeapCensusStatics::FormatSlope(Entry.SlopePerMinute));

		if (Entry.SampleCount < UHeapCensusStatics::MinimumSamples())
		{
			Line.Append(TEXT("  too little data"));
		}
		else if (Entry.bMonotonic && Entry.SlopePerMinute > 0.0f)
		{
			Line.Append(TEXT("  never fell back"));
		}

		// A row is coloured by its own verdict, not by the project's: red is the class somebody has to go and
		// look at, and there is no point colouring the other four rows with it.
		DrawLine(Line.ToView(), Entry.Verdict == EHeapVerdict::Ok ? Body : ColourFor(Entry.Verdict));
	}

	if (Summary.IgnoredClasses > 0)
	{
		Line.Reset();
		Line.Appendf(TEXT("%d class(es) hidden by the ignore list - their objects are still in the total above"),
			Summary.IgnoredClasses);
		DrawLine(Line.ToView(), Faint);
	}

	if (Summary.UntrackedClasses > 0)
	{
		Line.Reset();
		Line.Appendf(TEXT("%d class(es) counted but not listed - the table is at its cap"), Summary.UntrackedClasses);
		DrawLine(Line.ToView(), Faint);
	}

	// The sentence. Worth more than the table above it, because it names the thing to go and look at.
	DrawLine(UHeapCensusStatics::SummarizeWorst(Recorder.GetTop(0, EHeapSort::Growth)), VerdictColour);
}
