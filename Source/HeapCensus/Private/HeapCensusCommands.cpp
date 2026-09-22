// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HAL/IConsoleManager.h"
#include "HeapCensusLog.h"
#include "HeapCensusRecorder.h"
#include "HeapCensusSettings.h"
#include "HeapCensusStatics.h"

/**
 * The console surface.
 *
 * All of it runs against the engine-wide census rather than against a world, because a leak is a thing that
 * survives a map change and the interesting question is often exactly whether the count came back down after
 * one. None of these commands is editor-only - they behave the same in play-in-editor, in a -game run and in
 * a packaged Shipping build, which is where a leak that takes forty minutes to appear actually appears.
 */
namespace HeapCensusCommands
{
	static bool ParseBool(const TArray<FString>& Args, bool bDefault)
	{
		if (Args.Num() == 0)
		{
			return bDefault;
		}

		return Args[0].ToBool() || Args[0] == TEXT("1");
	}

	static EHeapSort ParseSort(const TArray<FString>& Args, int32 FirstIndex)
	{
		for (int32 Index = FirstIndex; Index < Args.Num(); ++Index)
		{
			if (Args[Index].Equals(TEXT("count"), ESearchCase::IgnoreCase))	{ return EHeapSort::Count; }
			if (Args[Index].Equals(TEXT("peak"), ESearchCase::IgnoreCase))	{ return EHeapSort::Peak; }
			if (Args[Index].Equals(TEXT("name"), ESearchCase::IgnoreCase))	{ return EHeapSort::Name; }
			if (Args[Index].Equals(TEXT("growth"), ESearchCase::IgnoreCase))	{ return EHeapSort::Growth; }
		}

		// Growth is the default, and it is the whole point of the plugin: obj list already sorts by size.
		return EHeapSort::Growth;
	}

	static FAutoConsoleCommand GShow(
		TEXT("Heap.Show"),
		TEXT("Heap.Show [0|1] - show the on-screen counter box."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FHeapCensusRecorder::Get().SetShowCounterBox(ParseBool(Args, true));
		}));

	static FAutoConsoleCommand GHide(
		TEXT("Heap.Hide"),
		TEXT("Heap.Hide - hide the on-screen counter box."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			FHeapCensusRecorder::Get().SetShowCounterBox(false);
		}));

	static FAutoConsoleCommand GSample(
		TEXT("Heap.Sample"),
		TEXT("Heap.Sample - walk the object array now, out of turn, and print what it cost."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			const FHeapCensusSummary Summary = FHeapCensusRecorder::Get().Sample();

			// The cost of the walk is printed with the result of the walk, every time. It is the number a
			// buyer is entitled to see before they leave this running in their game.
			UE_LOG(LogHeapCensus, Display,
				TEXT("HeapCensus: %s live UObjects in %d class(es); that walk cost %.2f ms (avg %.2f, worst %.2f)."),
				*UHeapCensusStatics::FormatCount(Summary.TotalObjects), Summary.TrackedClasses,
				Summary.LastCensusMilliseconds, Summary.AverageCensusMilliseconds, Summary.WorstCensusMilliseconds);
		}));

	static FAutoConsoleCommand GDump(
		TEXT("Heap.Dump"),
		TEXT("Heap.Dump [growth|count|peak|name] - the whole table to the log, not just the rows the box shows."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FHeapCensusRecorder::Get().DumpToLog(0, ParseSort(Args, 0));
		}));

	static FAutoConsoleCommand GTop(
		TEXT("Heap.Top"),
		TEXT("Heap.Top <n> [growth|count|peak|name] - the top n classes to the log. Default 10, by growth."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 N = (Args.Num() > 0 && Args[0].IsNumeric()) ? FCString::Atoi(*Args[0]) : 10;
			FHeapCensusRecorder::Get().DumpToLog(FMath::Max(N, 1), ParseSort(Args, 1));
		}));

	static FAutoConsoleCommand GReset(
		TEXT("Heap.Reset"),
		TEXT("Heap.Reset - throw the window and the collection statistics away and start measuring again."),
		FConsoleCommandDelegate::CreateStatic([]()
		{
			FHeapCensusRecorder::Get().Reset();
			UE_LOG(LogHeapCensus, Display, TEXT("HeapCensus: census cleared."));
		}));

	static FAutoConsoleCommand GReport(
		TEXT("Heap.Report"),
		TEXT("Heap.Report [path] - write the census as JSON. Default: the project settings path."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FHeapCensusRecorder::Get().WriteReport(Args.Num() > 0 ? Args[0] : FString());
		}));

	static FAutoConsoleCommand GThreshold(
		TEXT("Heap.Threshold"),
		TEXT("Heap.Threshold <objects per minute> - the slope at which a class is called Leaking."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FHeapCensusRecorder& Recorder = FHeapCensusRecorder::Get();

			if (Args.Num() == 0)
			{
				UE_LOG(LogHeapCensus, Display, TEXT("Heap.Threshold <objects per minute>  (currently %.0f)"),
					Recorder.GetGrowthThreshold());
				return;
			}

			Recorder.SetGrowthThreshold(FCString::Atof(*Args[0]));
			UE_LOG(LogHeapCensus, Display, TEXT("HeapCensus: threshold %.0f objects/min."), Recorder.GetGrowthThreshold());
		}));

	/**
	 * The gate.
	 *
	 * Measures for the given number of seconds, fits a slope per class, writes Saved/HeapCensus/report.json
	 * and ends the process with 0 when nothing grows, 1 when something is climbing and 2 when a class both
	 * broke the threshold and never fell back across the whole soak. Those are the same three numbers
	 * LoadLens, LocaleGuard, AssetWarden and WidgetLedger return, and they mean the same three things, so a
	 * project that owns more than one of these does not have to keep two conventions in its head.
	 *
	 * This is the command that finds the case a person never will: the class that gains four hundred objects
	 * a minute for six hours while nobody is watching.
	 *
	 * -noexit measures and reports without ending the process, which is what you want when you are typing
	 * this into the console rather than running it from a build script.
	 */
	static void RunGate(const TArray<FString>& Args)
	{
		float Seconds = 300.0f;
		bool bExitWhenDone = true;

		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("-noexit"), ESearchCase::IgnoreCase))
			{
				bExitWhenDone = false;
			}
			else if (Arg.IsNumeric())
			{
				Seconds = FCString::Atof(*Arg);
			}
		}

		FHeapCensusRecorder::Get().BeginGate(Seconds, bExitWhenDone);
	}

	static FAutoConsoleCommand GGate(
		TEXT("Heap.Gate"),
		TEXT("Heap.Gate <seconds> [-noexit] - soak, then exit 0 nothing grows / 1 climbing / 2 leaking."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunGate));

	/** The same command under the plugin's full name, because that is what a build script tends to be given. */
	static FAutoConsoleCommand GGateLongName(
		TEXT("HeapCensus.Gate"),
		TEXT("HeapCensus.Gate <seconds> [-noexit] - the same command as Heap.Gate."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunGate));

	/**
	 * The demonstration, and the reason it has two halves.
	 *
	 * Heap.Leak creates objects and holds them; Heap.Churn creates the same objects at the same rate and lets
	 * go of them again. The second one is the half people skip, and without it "the number went up" proves
	 * nothing: a growth table that only ever goes up is not measuring growth, it is measuring activity.
	 */
	static FAutoConsoleCommand GLeak(
		TEXT("Heap.Leak"),
		TEXT("Heap.Leak <n> - deliberately create and HOLD n objects a second. 0 stops it."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 Rate = (Args.Num() > 0 && Args[0].IsNumeric())
				? FCString::Atoi(*Args[0])
				: UHeapCensusSettings::Get().DemoObjectsPerSecond;

			FHeapCensusRecorder::Get().StartDemo(Rate, /*bHold*/ true);
		}));

	static FAutoConsoleCommand GChurn(
		TEXT("Heap.Churn"),
		TEXT("Heap.Churn <n> - create n objects a second and RELEASE them again. The healthy control. 0 stops it."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			const int32 Rate = (Args.Num() > 0 && Args[0].IsNumeric())
				? FCString::Atoi(*Args[0])
				: UHeapCensusSettings::Get().DemoObjectsPerSecond;

			FHeapCensusRecorder::Get().StartDemo(Rate, /*bHold*/ false);
		}));

	static FAutoConsoleCommand GRelease(
		TEXT("Heap.Release"),
		TEXT("Heap.Release [0|1] - stop the demonstration, drop the references, and collect (default yes)."),
		FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
		{
			FHeapCensusRecorder::Get().StopDemo(ParseBool(Args, true));
		}));
}
