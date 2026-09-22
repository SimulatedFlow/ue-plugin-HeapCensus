// Copyright 2026 Silvan Teufel. All Rights Reserved.

#include "HeapCensus.h"

#include "HeapCensusLog.h"
#include "HeapCensusRecorder.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY(LogHeapCensus);

#define LOCTEXT_NAMESPACE "FHeapCensusModule"

void FHeapCensusModule::StartupModule()
{
	// The hooks go on now, at PreDefault, so the collections that happen while the first map comes up are
	// timed like every other one. They are usually the most expensive of the session.
	FHeapCensusRecorder::Get().Install();

	// The project settings are read once the engine is up. Until then the census runs on its defaults - and
	// since nothing is sampled during the settling window anyway, nothing is judged against the wrong
	// threshold in the meantime.
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddLambda([]()
	{
		FHeapCensusRecorder::Get().ApplySettings();
	});

	UE_LOG(LogHeapCensus, Log, TEXT("HeapCensus started: counting UObjects and timing the collection."));
}

void FHeapCensusModule::ShutdownModule()
{
	// FCoreDelegates lives in Core and outlives this DLL. A lambda left on it after the module has gone is a
	// call into unmapped memory the next time the engine finishes starting up.
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	FHeapCensusRecorder::Get().Uninstall();

	UE_LOG(LogHeapCensus, Log, TEXT("HeapCensus shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FHeapCensusModule, HeapCensus)
